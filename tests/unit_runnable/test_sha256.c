// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_sha256.c
 *
 * MODULE UNDER TEST: platform/uds_sha256.c — streaming SHA-256 (FIPS 180-4).
 *
 * [#232 Phase 2]
 *
 * PURPOSE:
 *   Prove the streaming SHA-256 primitive the Phase 2 DFU image policy will
 *   use to digest a firmware image as it arrives over 0x36 TransferData.
 *   Two properties matter and both are load-bearing:
 *
 *     1. CORRECTNESS against the published FIPS 180-4 / NIST test vectors.
 *        An implementation that is merely self-consistent would happily
 *        agree with itself and disagree with the signing tool, so the
 *        expected digests below are the standard values, not values
 *        captured from this implementation's own output.
 *
 *     2. CHUNK-SPLIT INDEPENDENCE.  The digest must depend only on the
 *        concatenation of the bytes fed to update(), never on how they were
 *        split across calls — because the split is the tester's choice of
 *        0x36 block length, not the ECU's.  A partial-block bug here would
 *        pass every single-call vector test and then reject every real
 *        image whose size is not a multiple of 64.
 *
 * TEST CASES:
 *   Published vectors
 *     TC-SHA256-001  Empty string
 *                    -> e3b0c442...b7852b855
 *     TC-SHA256-002  "abc" (FIPS 180-4 one-block vector)
 *                    -> ba7816bf...f20015ad
 *     TC-SHA256-003  "abcdbcde...nopq" (FIPS 180-4 two-block, 448-bit)
 *                    -> 248d6a61...19db06c1
 *     TC-SHA256-004  The 896-bit NIST vector ("abcdefghbcdefghi...nopqrstu")
 *                    -> cf5b16a7...7afee9d1
 *     TC-SHA256-005  One million 'a', fed one byte at a time
 *                    -> cdc76e5c...c7112cd0
 *
 *   Padding-boundary lengths (where a partial-block bug hides)
 *     TC-SHA256-010  Lengths 55, 56, 57, 63, 64, 65 and 120 each digest to
 *                    their reference value.  55/56 straddle the point at
 *                    which the 8-byte length field stops fitting in the
 *                    final block; 63/64/65 straddle the block boundary.
 *
 *   Streaming equivalence
 *     TC-SHA256-020  A 211-byte message digests identically when fed in one
 *                    call, in 211 one-byte calls, and in irregular chunks —
 *                    and all three equal the independently published
 *                    reference digest for that message.
 *     TC-SHA256-021  Zero-length update() calls interleaved anywhere in the
 *                    stream change nothing.
 *
 *   Defensive behaviour
 *     TC-SHA256-030  NULL ctx to init/update/final is a no-op, and NULL
 *                    digest to final() leaves the caller's buffer untouched.
 *     TC-SHA256-031  A NULL data pointer with len > 0 is refused, and the
 *                    resulting digest is the one for the bytes that WERE
 *                    absorbed — i.e. it cannot match the full message, so
 *                    the Phase 2 comparison fails closed.
 *     TC-SHA256-032  Re-init after final() yields a fresh, correct digest
 *                    (the context is reusable, but only via init()).
 *
 *   Sensitivity
 *     TC-SHA256-040  A single-bit change anywhere in the message changes the
 *                    digest.  This is the test that would catch a stubbed or
 *                    short-circuited implementation.
 *
 * REFERENCE VALUES:
 *   TC-SHA256-001..005 are the published FIPS 180-4 / NIST CSRC example
 *   digests.  The digests for the synthetic messages used by TC-SHA256-010
 *   and TC-SHA256-020 (byte i = (i * 7 + 3) mod 256) were produced by an
 *   independent SHA-256 implementation, not by this one.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "uds_sha256.h"

/* ==========================================================================
 * Helpers
 * ========================================================================== */

/** Length of a 64-hex-character digest string, terminator excluded. */
#define HEX_DIGEST_CHARS   (UDS_SHA256_DIGEST_LEN * 2U)

/**
 * @brief Convert a 64-character lowercase hex string into 32 bytes.
 *
 * Keeps the expected values in the test readable as the published strings
 * rather than as brace-initialiser byte soup, which is where transcription
 * errors hide.
 */
static void hex_to_bytes(const char *hex, uint8_t out[UDS_SHA256_DIGEST_LEN])
{
    size_t i;

    for (i = 0U; i < (size_t)UDS_SHA256_DIGEST_LEN; i++) {
        size_t  j;
        uint8_t v = 0U;

        for (j = 0U; j < 2U; j++) {
            char    c = hex[(i * 2U) + j];
            uint8_t n;

            if ((c >= '0') && (c <= '9')) {
                n = (uint8_t)((uint8_t)c - (uint8_t)'0');
            } else {
                n = (uint8_t)(((uint8_t)c - (uint8_t)'a') + 10U);
            }
            v = (uint8_t)((uint8_t)(v << 4U) | n);
        }
        out[i] = v;
    }
}

/** Digest a whole buffer in one update() call. */
static void digest_once(const uint8_t *data, size_t len,
                        uint8_t out[UDS_SHA256_DIGEST_LEN])
{
    uds_sha256_ctx_t ctx;

    uds_sha256_init(&ctx);
    uds_sha256_update(&ctx, data, len);
    uds_sha256_final(&ctx, out);
}

/** Digest a NUL-terminated string in one update() call. */
static void digest_str(const char *s, uint8_t out[UDS_SHA256_DIGEST_LEN])
{
    digest_once((const uint8_t *)s, strlen(s), out);
}

/** Assert that digesting `s` in one call yields the published hex digest. */
static void expect_str_digest(const char *s, const char *expect_hex)
{
    uint8_t got[UDS_SHA256_DIGEST_LEN];
    uint8_t want[UDS_SHA256_DIGEST_LEN];

    digest_str(s, got);
    hex_to_bytes(expect_hex, want);
    zassert_mem_equal(want, got, UDS_SHA256_DIGEST_LEN, "");
}

/**
 * @brief Fill `buf` with the deterministic synthetic message byte pattern.
 *
 * byte i = (i * 7 + 3) mod 256.  Chosen so that every byte differs from its
 * index and the pattern has period 256, which makes an off-by-one in the
 * partial-block copy show up as a wrong digest rather than as an accidental
 * match.
 */
static void fill_pattern(uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0U; i < len; i++) {
        buf[i] = (uint8_t)(((i * 7U) + 3U) & 0xFFU);
    }
}

/** Assert that the n-byte synthetic message digests to `expect_hex`. */
static void expect_pattern_digest(size_t n, const char *expect_hex)
{
    uint8_t msg[256];
    uint8_t got[UDS_SHA256_DIGEST_LEN];
    uint8_t want[UDS_SHA256_DIGEST_LEN];

    zassert_true(n <= sizeof(msg), "");
    fill_pattern(msg, n);
    digest_once(msg, n, got);
    hex_to_bytes(expect_hex, want);
    zassert_mem_equal(want, got, UDS_SHA256_DIGEST_LEN, "");
}

/* ==========================================================================
 * Published vectors
 * ========================================================================== */

/* TC-SHA256-001: the empty message. */
ZTEST(sha256, test_vector_empty_string)
{
    expect_str_digest(
        "",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

/* TC-SHA256-002: FIPS 180-4 Appendix B.1, the one-block "abc" vector. */
ZTEST(sha256, test_vector_abc)
{
    expect_str_digest(
        "abc",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

/* TC-SHA256-003: FIPS 180-4 Appendix B.2, the 448-bit two-block vector. */
ZTEST(sha256, test_vector_two_block)
{
    expect_str_digest(
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

/* TC-SHA256-004: the 896-bit NIST vector — 112 bytes, so the padding needs a
 * whole extra block. */
ZTEST(sha256, test_vector_896_bit)
{
    expect_str_digest(
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

/* TC-SHA256-005: FIPS 180-4 Appendix B.3 — one million 'a'.  Fed ONE BYTE AT
 * A TIME on purpose: this simultaneously proves the published long-message
 * vector and that 1,000,000 consecutive single-byte update() calls produce
 * the same result as a bulk feed, which is the degenerate case of the 0x36
 * streaming path. */
ZTEST(sha256, test_vector_million_a_one_byte_at_a_time)
{
    uds_sha256_ctx_t ctx;
    uint8_t          got[UDS_SHA256_DIGEST_LEN];
    uint8_t          want[UDS_SHA256_DIGEST_LEN];
    uint8_t          a = (uint8_t)'a';
    uint32_t         i;

    uds_sha256_init(&ctx);
    for (i = 0U; i < 1000000U; i++) {
        uds_sha256_update(&ctx, &a, 1U);
    }
    uds_sha256_final(&ctx, got);

    hex_to_bytes(
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        want);
    zassert_mem_equal(want, got, UDS_SHA256_DIGEST_LEN, "");
}

/* ==========================================================================
 * Padding-boundary lengths
 * ========================================================================== */

/* TC-SHA256-010: the lengths at which FIPS 180-4 padding changes shape.
 * 55 is the largest message whose padding still fits in one block; 56 is the
 * first that needs a second; 63/64/65 straddle the block boundary itself. */
ZTEST(sha256, test_padding_boundary_lengths)
{
    expect_pattern_digest(
        55U, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b");
    expect_pattern_digest(
        56U, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27");
    expect_pattern_digest(
        57U, "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73");
    expect_pattern_digest(
        63U, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055");
    expect_pattern_digest(
        64U, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241");
    expect_pattern_digest(
        65U, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e");
    expect_pattern_digest(
        119U, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e");
    expect_pattern_digest(
        120U, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5");
    expect_pattern_digest(
        128U, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6");
}

/* ==========================================================================
 * Streaming equivalence
 * ========================================================================== */

/** Length of the streaming-equivalence message: >3 blocks, not a multiple
 *  of 64, and not a multiple of any chunk size used below. */
#define STREAM_MSG_LEN   (211U)

/** Independently computed digest of the 211-byte synthetic message. */
#define STREAM_MSG_HEX \
    "3a8e3e87376a17164eecf3a64bfe051413a8dbb47658a0b29fc127583775fa95"

/* TC-SHA256-020: one call vs. 211 one-byte calls vs. irregular chunks.
 * All three must equal each other AND the independent reference digest —
 * equality among themselves alone would be satisfied by an implementation
 * that ignored its input entirely. */
ZTEST(sha256, test_streaming_equivalence)
{
    uint8_t          msg[STREAM_MSG_LEN];
    uint8_t          d_once[UDS_SHA256_DIGEST_LEN];
    uint8_t          d_bytewise[UDS_SHA256_DIGEST_LEN];
    uint8_t          d_irregular[UDS_SHA256_DIGEST_LEN];
    uint8_t          want[UDS_SHA256_DIGEST_LEN];
    uds_sha256_ctx_t ctx;
    size_t           i;
    size_t           offset;
    /* Chunk sizes deliberately mix sub-block, exactly-one-block and
     * multi-block feeds, and sum to more than the message so the final
     * chunk is clamped to the remainder. */
    static const size_t k_chunks[] = { 1U, 63U, 64U, 2U, 65U, 7U, 100U };

    fill_pattern(msg, (size_t)STREAM_MSG_LEN);
    hex_to_bytes(STREAM_MSG_HEX, want);

    /* (a) one call */
    digest_once(msg, (size_t)STREAM_MSG_LEN, d_once);

    /* (b) 211 single-byte calls */
    uds_sha256_init(&ctx);
    for (i = 0U; i < (size_t)STREAM_MSG_LEN; i++) {
        uds_sha256_update(&ctx, &msg[i], 1U);
    }
    uds_sha256_final(&ctx, d_bytewise);

    /* (c) irregular chunks */
    uds_sha256_init(&ctx);
    offset = 0U;
    i      = 0U;
    while (offset < (size_t)STREAM_MSG_LEN) {
        size_t want_chunk = k_chunks[i % (sizeof(k_chunks) / sizeof(k_chunks[0]))];
        size_t remaining  = (size_t)STREAM_MSG_LEN - offset;
        size_t take       = (want_chunk < remaining) ? want_chunk : remaining;

        uds_sha256_update(&ctx, &msg[offset], take);
        offset += take;
        i++;
    }
    uds_sha256_final(&ctx, d_irregular);

    zassert_mem_equal(want, d_once, UDS_SHA256_DIGEST_LEN, "");
    zassert_mem_equal(want, d_bytewise, UDS_SHA256_DIGEST_LEN, "");
    zassert_mem_equal(want, d_irregular, UDS_SHA256_DIGEST_LEN, "");
}

/* TC-SHA256-021: zero-length updates are transparent.  A 0x36 block that
 * carried no payload bytes must not perturb the running digest. */
ZTEST(sha256, test_zero_length_updates_are_transparent)
{
    uint8_t          msg[STREAM_MSG_LEN];
    uint8_t          got[UDS_SHA256_DIGEST_LEN];
    uint8_t          want[UDS_SHA256_DIGEST_LEN];
    uds_sha256_ctx_t ctx;
    size_t           i;

    fill_pattern(msg, (size_t)STREAM_MSG_LEN);
    hex_to_bytes(STREAM_MSG_HEX, want);

    uds_sha256_init(&ctx);
    uds_sha256_update(&ctx, msg, 0U);          /* before anything          */
    uds_sha256_update(&ctx, NULL, 0U);         /* NULL is legal when len=0 */
    for (i = 0U; i < (size_t)STREAM_MSG_LEN; i++) {
        uds_sha256_update(&ctx, &msg[i], 1U);
        uds_sha256_update(&ctx, msg, 0U);      /* interleaved              */
    }
    uds_sha256_update(&ctx, msg, 0U);          /* after everything         */
    uds_sha256_final(&ctx, got);

    zassert_mem_equal(want, got, UDS_SHA256_DIGEST_LEN, "");
}

/* ==========================================================================
 * Defensive behaviour
 * ========================================================================== */

/* TC-SHA256-030: NULL arguments are no-ops, not faults. */
ZTEST(sha256, test_null_arguments_are_no_ops)
{
    uds_sha256_ctx_t ctx;
    uint8_t          digest[UDS_SHA256_DIGEST_LEN];
    uint8_t          want[UDS_SHA256_DIGEST_LEN];
    uint8_t          canary[UDS_SHA256_DIGEST_LEN];
    size_t           i;

    /* NULL ctx must not fault on any of the three entry points. */
    uds_sha256_init(NULL);
    uds_sha256_update(NULL, (const uint8_t *)"abc", 3U);
    uds_sha256_final(NULL, digest);

    /* NULL digest must leave the caller's buffer alone. */
    for (i = 0U; i < (size_t)UDS_SHA256_DIGEST_LEN; i++) {
        canary[i] = (uint8_t)0xA5U;
        digest[i] = (uint8_t)0xA5U;
    }
    uds_sha256_init(&ctx);
    uds_sha256_update(&ctx, (const uint8_t *)"abc", 3U);
    uds_sha256_final(&ctx, NULL);
    zassert_mem_equal(canary, digest, UDS_SHA256_DIGEST_LEN, "");

    /* And the module still works afterwards. */
    digest_str("abc", digest);
    hex_to_bytes(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        want);
    zassert_mem_equal(want, digest, UDS_SHA256_DIGEST_LEN, "");
}

/* TC-SHA256-031: NULL data with a non-zero length is refused.  The chunk is
 * NOT absorbed, so the digest is the one for "ab" — provably not the digest
 * for "abc".  That is the fail-closed direction: a dropped chunk can only
 * make a good image mismatch, never make a bad one match. */
ZTEST(sha256, test_null_data_with_nonzero_len_is_refused)
{
    uds_sha256_ctx_t ctx;
    uint8_t          got[UDS_SHA256_DIGEST_LEN];
    uint8_t          d_ab[UDS_SHA256_DIGEST_LEN];
    uint8_t          d_abc[UDS_SHA256_DIGEST_LEN];

    digest_str("ab", d_ab);
    digest_str("abc", d_abc);

    uds_sha256_init(&ctx);
    uds_sha256_update(&ctx, (const uint8_t *)"ab", 2U);
    uds_sha256_update(&ctx, NULL, 1U);   /* dropped */
    uds_sha256_final(&ctx, got);

    zassert_mem_equal(d_ab, got, UDS_SHA256_DIGEST_LEN, "");
    zassert_true(memcmp(d_abc, got, (size_t)UDS_SHA256_DIGEST_LEN) != 0, "");
}

/* TC-SHA256-032: a context is reusable after final(), but only via init(). */
ZTEST(sha256, test_context_reuse_after_reinit)
{
    uds_sha256_ctx_t ctx;
    uint8_t          first[UDS_SHA256_DIGEST_LEN];
    uint8_t          second[UDS_SHA256_DIGEST_LEN];
    uint8_t          want[UDS_SHA256_DIGEST_LEN];

    uds_sha256_init(&ctx);
    uds_sha256_update(&ctx, (const uint8_t *)"abc", 3U);
    uds_sha256_final(&ctx, first);

    uds_sha256_init(&ctx);
    uds_sha256_update(&ctx, (const uint8_t *)"abc", 3U);
    uds_sha256_final(&ctx, second);

    hex_to_bytes(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        want);
    zassert_mem_equal(want, first, UDS_SHA256_DIGEST_LEN, "");
    zassert_mem_equal(want, second, UDS_SHA256_DIGEST_LEN, "");
}

/* ==========================================================================
 * Sensitivity
 * ========================================================================== */

/* TC-SHA256-040: flipping one bit of the message changes the digest.  Run at
 * three positions — first byte, a middle byte and the last byte — so that a
 * hypothetical implementation which ignored its ragged head or tail would be
 * caught. */
ZTEST(sha256, test_single_bit_change_changes_digest)
{
    uint8_t msg[STREAM_MSG_LEN];
    uint8_t base[UDS_SHA256_DIGEST_LEN];
    uint8_t mutated[UDS_SHA256_DIGEST_LEN];
    size_t  positions[3];
    size_t  p;

    fill_pattern(msg, (size_t)STREAM_MSG_LEN);
    digest_once(msg, (size_t)STREAM_MSG_LEN, base);

    positions[0] = 0U;
    positions[1] = (size_t)STREAM_MSG_LEN / 2U;
    positions[2] = (size_t)STREAM_MSG_LEN - 1U;

    for (p = 0U; p < 3U; p++) {
        size_t idx = positions[p];

        msg[idx] ^= (uint8_t)0x01U;
        digest_once(msg, (size_t)STREAM_MSG_LEN, mutated);
        zassert_true(memcmp(base, mutated, (size_t)UDS_SHA256_DIGEST_LEN) != 0, "");
        msg[idx] ^= (uint8_t)0x01U;   /* restore */
    }

    /* Restoring every bit must return the original digest. */
    digest_once(msg, (size_t)STREAM_MSG_LEN, mutated);
    zassert_mem_equal(base, mutated, UDS_SHA256_DIGEST_LEN, "");
}

/* ==========================================================================
 * run_all_tests — required by tests/runner/test_main.c
 * ========================================================================== */

void run_all_tests(void)
{
    /* --- Published FIPS 180-4 / NIST vectors --- */
    RUN_TEST(sha256__test_vector_empty_string);
    RUN_TEST(sha256__test_vector_abc);
    RUN_TEST(sha256__test_vector_two_block);
    RUN_TEST(sha256__test_vector_896_bit);
    RUN_TEST(sha256__test_vector_million_a_one_byte_at_a_time);

    /* --- Padding boundaries --- */
    RUN_TEST(sha256__test_padding_boundary_lengths);

    /* --- Streaming equivalence --- */
    RUN_TEST(sha256__test_streaming_equivalence);
    RUN_TEST(sha256__test_zero_length_updates_are_transparent);

    /* --- Defensive behaviour --- */
    RUN_TEST(sha256__test_null_arguments_are_no_ops);
    RUN_TEST(sha256__test_null_data_with_nonzero_len_is_refused);
    RUN_TEST(sha256__test_context_reuse_after_reinit);

    /* --- Sensitivity --- */
    RUN_TEST(sha256__test_single_bit_change_changes_digest);
}
