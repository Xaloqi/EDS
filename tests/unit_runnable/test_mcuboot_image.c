// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_mcuboot_image.c
 *
 * MODULE UNDER TEST: platform/uds_mcuboot_image.c — MCUboot image header and
 *                    TLV trailer parser.
 *
 * [#232 Phase 2]
 *
 * PURPOSE:
 *   Prove the defensive parser the Phase 2 DFU image policy will point at a
 *   freshly downloaded firmware image.  Its input is attacker-influenced by
 *   construction — the bytes arrived over 0x36 TransferData from whoever is
 *   plugged into the OBD port — so the tests are weighted towards what the
 *   parser REFUSES, not what it accepts.
 *
 *   Every buffer here is built byte by byte by the little-endian pack
 *   helpers below rather than by memcpy-ing a host struct.  That is
 *   deliberate: if the test built its fixtures the same way the parser reads
 *   them, a shared endianness or layout mistake would cancel out and both
 *   sides would agree on the wrong format.  The helpers encode the wire
 *   format as documented in platform/uds_mcuboot_image.h, independently.
 *
 * TEST CASES:
 *   Header — platform/uds_mcuboot_image.c::uds_mcuboot_parse_header()
 *     TC-MCUB-001  Valid 32-byte header: every one of the nine decoded
 *                  fields matches the value packed into the fixture,
 *                  including the 16-bit revision and 32-bit build number
 *                  that straddle the version sub-record.
 *     TC-MCUB-002  Wrong magic                      -> ERR_BAD_MAGIC
 *     TC-MCUB-003  Buffer shorter than 32 bytes     -> ERR_BUFFER_TOO_SMALL
 *                  (checked at 0 and at 31 bytes, i.e. one byte short)
 *     TC-MCUB-004  IMAGE_MAGIC_V1 (the legacy pre-1.0 format)
 *                                                   -> ERR_BAD_MAGIC
 *                  Documented decision: v1 is rejected, not adapted to.
 *     TC-MCUB-005  NULL buf / NULL out              -> ERR_NULL_ARG
 *     TC-MCUB-006  ih_hdr_size < 32                 -> ERR_BAD_LENGTH;
 *                  ih_hdr_size > 32 (padded header) is ACCEPTED.
 *     TC-MCUB-007  A buffer longer than 32 bytes parses; the excess is
 *                  ignored and *out is untouched on every error path.
 *
 *   TLV iteration — uds_mcuboot_tlv_iter_begin() / _next()
 *     TC-MCUB-010  Area holding a SHA256 TLV (32-byte value) followed by an
 *                  ECDSA_SIG TLV: both are found, in order, with correct
 *                  type and len, and the value bytes recovered at
 *                  value_offset match what was packed.  Iteration then ends
 *                  cleanly with status OK.
 *     TC-MCUB-011  Empty area (just the 4-byte info record, it_tlv_tot == 4)
 *                  -> begin() OK, first next() reports clean end, status OK.
 *     TC-MCUB-012  Protected-area magic iterated with the matching
 *                  expected_info_magic -> OK; the same area presented as a
 *                  regular area -> ERR_BAD_MAGIC.
 *     TC-MCUB-013  A zero-length TLV value is a legitimate entry.
 *
 *   TLV malformed input — the fail-closed half
 *     TC-MCUB-020  A TLV whose it_len claims more bytes than remain
 *                  -> next() returns false with status ERR_BAD_LENGTH, NOT a
 *                  clean end, and *out is untouched.  This is the single
 *                  most important case in the file: conflating it with
 *                  "done" would let an attacker delete an image's signature
 *                  TLV by truncating the trailer.
 *     TC-MCUB-021  Trailing bytes too few for a 4-byte TLV header (1, 2 and
 *                  3 stray bytes) -> ERR_BAD_LENGTH, not a clean end.
 *     TC-MCUB-022  Wrong it_magic in the info record -> ERR_BAD_MAGIC.
 *     TC-MCUB-023  it_tlv_tot != the area_len passed in -> ERR_BAD_LENGTH,
 *                  in both directions (declared larger and declared
 *                  smaller).
 *     TC-MCUB-024  it_tlv_tot < 4 (an area smaller than its own record)
 *                  -> ERR_BAD_LENGTH.
 *     TC-MCUB-025  area_len < 4                     -> ERR_BUFFER_TOO_SMALL
 *     TC-MCUB-026  NULL it / NULL area / NULL out / NULL status handled
 *                  without fault; ERR_NULL_ARG where a status can be
 *                  reported.
 *     TC-MCUB-027  A maximal it_len (0xFFFF) in a small area is refused and
 *                  reads nothing beyond the area.  The fixture buffer is
 *                  sized EXACTLY to the declared area with nothing after it,
 *                  so any over-read is a stack-buffer-overflow under ASan on
 *                  the sanitized build (build_tests.sh --sanitize, which CI
 *                  runs as the 'sanitizers' job).
 *     TC-MCUB-028  After a malformed entry the iterator stays refusing —
 *                  a caller that ignores the status and loops again does
 *                  not fall through into a clean end.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "uds_mcuboot_image.h"

/* ==========================================================================
 * Little-endian pack helpers
 *
 * Written independently of the parser's decoders on purpose — see the file
 * header.  Each returns the number of bytes written so fixtures can be
 * built by simple accumulation.
 * ========================================================================== */

/** Store a 16-bit value little-endian and return 2. */
static size_t pack_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v % 256U);
    dst[1] = (uint8_t)((v / 256U) % 256U);
    return 2U;
}

/** Store a 32-bit value little-endian and return 4. */
static size_t pack_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v % 256U);
    dst[1] = (uint8_t)((v / 256U) % 256U);
    dst[2] = (uint8_t)((v / 65536U) % 256U);
    dst[3] = (uint8_t)((v / 16777216U) % 256U);
    return 4U;
}

/* ==========================================================================
 * Header fixture
 * ========================================================================== */

/* Distinctive field values — every one differs from every other so that a
 * swapped-offset bug cannot produce a passing decode. */
#define FIX_LOAD_ADDR         (0x08020000UL)
#define FIX_HDR_SIZE          (32U)
#define FIX_PROTECT_TLV_SIZE  (0x0048U)
#define FIX_IMG_SIZE          (0x0001ABCDUL)
#define FIX_FLAGS             (UDS_MCUBOOT_F_ENCRYPTED_AES256)
#define FIX_VER_MAJOR         (1U)
#define FIX_VER_MINOR         (10U)
#define FIX_VER_REVISION      (0x1234U)
#define FIX_VER_BUILD_NUM     (0xDEADBEEFUL)

/**
 * @brief Build a 32-byte image header into buf, with a caller-chosen magic
 *        and ih_hdr_size so the failure cases can reuse the same fixture.
 */
static void build_header(uint8_t buf[UDS_MCUBOOT_HEADER_SIZE],
                         uint32_t magic,
                         uint16_t hdr_size)
{
    size_t o = 0U;

    (void)memset(buf, 0, (size_t)UDS_MCUBOOT_HEADER_SIZE);
    o += pack_le32(&buf[o], magic);
    o += pack_le32(&buf[o], (uint32_t)FIX_LOAD_ADDR);
    o += pack_le16(&buf[o], hdr_size);
    o += pack_le16(&buf[o], (uint16_t)FIX_PROTECT_TLV_SIZE);
    o += pack_le32(&buf[o], (uint32_t)FIX_IMG_SIZE);
    o += pack_le32(&buf[o], (uint32_t)FIX_FLAGS);
    buf[o] = (uint8_t)FIX_VER_MAJOR;
    o += 1U;
    buf[o] = (uint8_t)FIX_VER_MINOR;
    o += 1U;
    o += pack_le16(&buf[o], (uint16_t)FIX_VER_REVISION);
    o += pack_le32(&buf[o], (uint32_t)FIX_VER_BUILD_NUM);
    /* _pad1 at offset 28 is left zero; the parser must ignore it. */
    o += pack_le32(&buf[o], 0xFFFFFFFFUL);
    zassert_equal((size_t)UDS_MCUBOOT_HEADER_SIZE, o, "");
}

/* ==========================================================================
 * TLV area fixtures
 * ========================================================================== */

/** Working buffer for hand-built TLV areas. */
#define TLV_BUF_MAX   (256U)

/** Recognisable SHA256 TLV value: 32 bytes, byte i = 0xC0 + i. */
static void fill_hash_value(uint8_t *dst)
{
    size_t i;

    for (i = 0U; i < (size_t)UDS_MCUBOOT_HASH_LEN; i++) {
        dst[i] = (uint8_t)(0xC0U + i);
    }
}

/** Recognisable ECDSA signature value: 8 bytes, byte i = 0x50 + i. */
#define FIX_SIG_LEN   (8U)
static void fill_sig_value(uint8_t *dst)
{
    size_t i;

    for (i = 0U; i < (size_t)FIX_SIG_LEN; i++) {
        dst[i] = (uint8_t)(0x50U + i);
    }
}

/**
 * @brief Build a TLV area holding a SHA256 TLV then an ECDSA_SIG TLV.
 *
 * @param[out] buf       Destination, at least TLV_BUF_MAX bytes.
 * @param[in]  magic     it_magic to write into the info record.
 * @param[in]  tot_over  Value added to the true total when writing
 *                       it_tlv_tot, so a mismatch can be injected.  0 for a
 *                       consistent area.
 * @return Number of bytes actually written (the TRUE area length).
 */
static size_t build_two_tlv_area(uint8_t *buf, uint16_t magic, int32_t tot_over)
{
    size_t o;
    size_t total;

    /* True area size: info(4) + hdr(4)+32 + hdr(4)+8 */
    total = (size_t)UDS_MCUBOOT_TLV_INFO_SIZE
          + ((size_t)UDS_MCUBOOT_TLV_HDR_SIZE + (size_t)UDS_MCUBOOT_HASH_LEN)
          + ((size_t)UDS_MCUBOOT_TLV_HDR_SIZE + (size_t)FIX_SIG_LEN);

    o  = 0U;
    o += pack_le16(&buf[o], magic);
    o += pack_le16(&buf[o], (uint16_t)((int32_t)total + tot_over));

    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_SHA256);
    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_HASH_LEN);
    fill_hash_value(&buf[o]);
    o += (size_t)UDS_MCUBOOT_HASH_LEN;

    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_ECDSA_SIG);
    o += pack_le16(&buf[o], (uint16_t)FIX_SIG_LEN);
    fill_sig_value(&buf[o]);
    o += (size_t)FIX_SIG_LEN;

    return o;
}

/**
 * @brief Build an area holding a single TLV whose it_len is a lie.
 *
 * The area declares (and is passed as) exactly enough room for the info
 * record, one TLV header and `real_value_len` value bytes, but the TLV
 * header claims `claimed_len`.
 *
 * @return The true area length.
 */
static size_t build_lying_tlv_area(uint8_t *buf,
                                   uint16_t claimed_len,
                                   size_t   real_value_len)
{
    size_t o;
    size_t total;

    total = (size_t)UDS_MCUBOOT_TLV_INFO_SIZE
          + (size_t)UDS_MCUBOOT_TLV_HDR_SIZE
          + real_value_len;

    o  = 0U;
    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC);
    o += pack_le16(&buf[o], (uint16_t)total);
    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_ECDSA_SIG);
    o += pack_le16(&buf[o], claimed_len);
    (void)memset(&buf[o], 0x5AU, real_value_len);
    o += real_value_len;

    return o;
}

/* ==========================================================================
 * Header tests
 * ========================================================================== */

/* TC-MCUB-001: every field of a valid header decodes correctly. */
ZTEST(mcuboot_image, test_valid_header_decodes_every_field)
{
    uint8_t              buf[UDS_MCUBOOT_HEADER_SIZE];
    uds_mcuboot_header_t h;

    (void)memset(&h, 0, sizeof(h));
    build_header(buf, (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC, (uint16_t)FIX_HDR_SIZE);

    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");

    zassert_equal((uint32_t)UDS_MCUBOOT_IMAGE_MAGIC, h.magic, "");
    zassert_equal((uint32_t)FIX_LOAD_ADDR,           h.load_addr, "");
    zassert_equal((uint16_t)FIX_HDR_SIZE,            h.hdr_size, "");
    zassert_equal((uint16_t)FIX_PROTECT_TLV_SIZE,    h.protect_tlv_size, "");
    zassert_equal((uint32_t)FIX_IMG_SIZE,            h.img_size, "");
    zassert_equal((uint32_t)FIX_FLAGS,               h.flags, "");
    zassert_equal((uint8_t)FIX_VER_MAJOR,            h.ver_major, "");
    zassert_equal((uint8_t)FIX_VER_MINOR,            h.ver_minor, "");
    zassert_equal((uint16_t)FIX_VER_REVISION,        h.ver_revision, "");
    zassert_equal((uint32_t)FIX_VER_BUILD_NUM,       h.ver_build_num, "");
}

/* TC-MCUB-002: wrong magic is refused. */
ZTEST(mcuboot_image, test_header_wrong_magic_rejected)
{
    uint8_t              buf[UDS_MCUBOOT_HEADER_SIZE];
    uds_mcuboot_header_t h;

    build_header(buf, 0xDEADBEEFUL, (uint16_t)FIX_HDR_SIZE);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");

    /* An all-zero buffer — an erased flash page — is not a header either. */
    (void)memset(buf, 0, sizeof(buf));
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");

    /* Nor is an all-0xFF buffer — the other erased-flash pattern. */
    (void)memset(buf, 0xFF, sizeof(buf));
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");
}

/* TC-MCUB-003: a buffer shorter than the fixed header is refused BEFORE the
 * magic is looked at, so a short buffer can never be read past its end. */
ZTEST(mcuboot_image, test_header_buffer_too_small)
{
    uint8_t              buf[UDS_MCUBOOT_HEADER_SIZE];
    uds_mcuboot_header_t h;

    build_header(buf, (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC, (uint16_t)FIX_HDR_SIZE);

    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL,
                  uds_mcuboot_parse_header(buf, 0U, &h), "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL,
                  uds_mcuboot_parse_header(buf, 1U, &h), "");
    /* One byte short — the classic off-by-one. */
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL,
                  uds_mcuboot_parse_header(buf,
                                           (size_t)UDS_MCUBOOT_HEADER_SIZE - 1U,
                                           &h), "");
    /* Exactly 32 is enough. */
    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_parse_header(buf,
                                           (size_t)UDS_MCUBOOT_HEADER_SIZE,
                                           &h), "");
}

/* TC-MCUB-004: the legacy v1 magic is refused exactly like random bytes.
 * Documented decision — this repo's reference policy targets current MCUboot
 * only, and silently decoding a v1 header with v2 field offsets would be a
 * fail-OPEN misread. */
ZTEST(mcuboot_image, test_header_v1_magic_rejected)
{
    uint8_t              buf[UDS_MCUBOOT_HEADER_SIZE];
    uds_mcuboot_header_t h;

    build_header(buf, (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC_V1, (uint16_t)FIX_HDR_SIZE);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");

    /* Sanity: v1 and current magic differ only in the low nibble, so this
     * test really does exercise an exact comparison. */
    zassert_true((uint32_t)UDS_MCUBOOT_IMAGE_MAGIC !=
                 (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC_V1, "");
}

/* TC-MCUB-005: NULL arguments. */
ZTEST(mcuboot_image, test_header_null_args)
{
    uint8_t              buf[UDS_MCUBOOT_HEADER_SIZE];
    uds_mcuboot_header_t h;

    build_header(buf, (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC, (uint16_t)FIX_HDR_SIZE);

    zassert_equal(UDS_MCUBOOT_PARSE_ERR_NULL_ARG,
                  uds_mcuboot_parse_header(NULL, sizeof(buf), &h), "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_NULL_ARG,
                  uds_mcuboot_parse_header(buf, sizeof(buf), NULL), "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_NULL_ARG,
                  uds_mcuboot_parse_header(NULL, 0U, NULL), "");
}

/* TC-MCUB-006: ih_hdr_size below the fixed size is structurally impossible;
 * above it is a legitimately padded header. */
ZTEST(mcuboot_image, test_header_hdr_size_bounds)
{
    uint8_t              buf[UDS_MCUBOOT_HEADER_SIZE];
    uds_mcuboot_header_t h;

    build_header(buf, (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC, 0U);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");

    build_header(buf, (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC,
                 (uint16_t)((uint16_t)UDS_MCUBOOT_HEADER_SIZE - 1U));
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");

    /* A padded header (0x200 is what a Cortex-M vector-table alignment
     * typically produces) is accepted and reported verbatim. */
    build_header(buf, (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC, 0x0200U);
    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");
    zassert_equal((uint16_t)0x0200U, h.hdr_size, "");
}

/* TC-MCUB-007: a longer buffer parses (the excess is ignored), and *out is
 * left untouched on every error path. */
ZTEST(mcuboot_image, test_header_excess_bytes_and_out_untouched_on_error)
{
    uint8_t              buf[UDS_MCUBOOT_HEADER_SIZE + 64U];
    uds_mcuboot_header_t h;

    (void)memset(buf, 0xA5, sizeof(buf));
    build_header(buf, (uint32_t)UDS_MCUBOOT_IMAGE_MAGIC, (uint16_t)FIX_HDR_SIZE);

    (void)memset(&h, 0, sizeof(h));
    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");
    zassert_equal((uint32_t)FIX_IMG_SIZE, h.img_size, "");

    /* Now corrupt the magic and confirm h keeps its previous contents
     * rather than being half-overwritten. */
    buf[0] ^= (uint8_t)0x01U;
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_parse_header(buf, sizeof(buf), &h), "");
    zassert_equal((uint32_t)FIX_IMG_SIZE, h.img_size, "");
    zassert_equal((uint32_t)UDS_MCUBOOT_IMAGE_MAGIC, h.magic, "");
}

/* ==========================================================================
 * TLV iteration tests
 * ========================================================================== */

/* TC-MCUB-010: SHA256 then ECDSA_SIG, both recovered with correct values. */
ZTEST(mcuboot_image, test_tlv_iterate_sha256_then_signature)
{
    uint8_t                    buf[TLV_BUF_MAX];
    uint8_t                    want_hash[UDS_MCUBOOT_HASH_LEN];
    uint8_t                    want_sig[FIX_SIG_LEN];
    uds_mcuboot_tlv_iter_t     it;
    uds_mcuboot_tlv_t          e;
    uds_mcuboot_parse_status_t st = UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
    size_t                     area_len;

    area_len = build_two_tlv_area(buf, (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC, 0);
    fill_hash_value(want_hash);
    fill_sig_value(want_sig);

    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    /* Entry 1: SHA256. */
    zassert_true(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");
    zassert_equal((uint16_t)UDS_MCUBOOT_TLV_SHA256, e.type, "");
    zassert_equal((uint16_t)UDS_MCUBOOT_HASH_LEN, e.len, "");
    zassert_equal((size_t)8U, e.value_offset, "");   /* info(4) + tlv hdr(4) */
    zassert_mem_equal(want_hash, &buf[e.value_offset], UDS_MCUBOOT_HASH_LEN, "");

    /* Entry 2: ECDSA_SIG. */
    zassert_true(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");
    zassert_equal((uint16_t)UDS_MCUBOOT_TLV_ECDSA_SIG, e.type, "");
    zassert_equal((uint16_t)FIX_SIG_LEN, e.len, "");
    zassert_equal((size_t)44U, e.value_offset, "");  /* 8 + 32 + 4 */
    zassert_mem_equal(want_sig, &buf[e.value_offset], FIX_SIG_LEN, "");

    /* Clean end: false return, status OK. */
    st = UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");

    /* And it stays cleanly ended if the caller asks again. */
    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");
}

/* TC-MCUB-011: an empty area is valid and ends cleanly on the first call. */
ZTEST(mcuboot_image, test_tlv_empty_area_ends_cleanly)
{
    uint8_t                    buf[UDS_MCUBOOT_TLV_INFO_SIZE];
    uds_mcuboot_tlv_iter_t     it;
    uds_mcuboot_tlv_t          e;
    uds_mcuboot_parse_status_t st = UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
    size_t                     o  = 0U;

    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC);
    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_INFO_SIZE);
    zassert_equal((size_t)UDS_MCUBOOT_TLV_INFO_SIZE, o, "");

    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, buf, o,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");
    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");
}

/* TC-MCUB-012: the protected area is a distinct area, not a variant to be
 * accepted interchangeably. */
ZTEST(mcuboot_image, test_tlv_protected_area_magic_is_distinct)
{
    uint8_t                buf[TLV_BUF_MAX];
    uds_mcuboot_tlv_iter_t it;
    size_t                 area_len;

    area_len = build_two_tlv_area(buf, (uint16_t)UDS_MCUBOOT_TLV_PROT_INFO_MAGIC, 0);

    /* Iterated as the protected area: fine. */
    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_PROT_INFO_MAGIC),
                  "");

    /* Presented as the regular area: refused. */
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    /* And the converse direction too. */
    area_len = build_two_tlv_area(buf, (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC, 0);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_PROT_INFO_MAGIC),
                  "");
}

/* TC-MCUB-013: a zero-length TLV value is a legitimate entry, not an end. */
ZTEST(mcuboot_image, test_tlv_zero_length_value_is_an_entry)
{
    uint8_t                    buf[TLV_BUF_MAX];
    uds_mcuboot_tlv_iter_t     it;
    uds_mcuboot_tlv_t          e;
    uds_mcuboot_parse_status_t st = UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH;
    size_t                     o  = 0U;
    size_t                     total;

    total = (size_t)UDS_MCUBOOT_TLV_INFO_SIZE + (size_t)UDS_MCUBOOT_TLV_HDR_SIZE;
    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC);
    o += pack_le16(&buf[o], (uint16_t)total);
    o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_BOOT_RECORD);
    o += pack_le16(&buf[o], 0U);
    zassert_equal(total, o, "");

    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, buf, o,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");
    zassert_true(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");
    zassert_equal((uint16_t)UDS_MCUBOOT_TLV_BOOT_RECORD, e.type, "");
    zassert_equal((uint16_t)0U, e.len, "");
    zassert_equal((size_t)8U, e.value_offset, "");

    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");
}

/* ==========================================================================
 * TLV malformed-input tests — the fail-closed half
 * ========================================================================== */

/* TC-MCUB-020: an it_len longer than the area has left is MALFORMED, and is
 * reported as such rather than as a clean end. */
ZTEST(mcuboot_image, test_tlv_value_len_beyond_area_is_malformed_not_end)
{
    uint8_t                    buf[TLV_BUF_MAX];
    uds_mcuboot_tlv_iter_t     it;
    uds_mcuboot_tlv_t          e;
    uds_mcuboot_parse_status_t st;
    size_t                     area_len;

    /* Room for 8 value bytes; the header claims 9 — one byte over. */
    area_len = build_lying_tlv_area(buf, 9U, 8U);

    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    (void)memset(&e, 0xEE, sizeof(e));
    st = UDS_MCUBOOT_PARSE_OK;
    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    /* The whole point: NOT a clean end. */
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH, st, "");
    zassert_not_equal(UDS_MCUBOOT_PARSE_OK, st, "");
    /* *out untouched. */
    zassert_equal((uint16_t)0xEEEEU, e.type, "");

    /* Exactly-fitting is accepted — proving the check is `>` and not `>=`. */
    area_len = build_lying_tlv_area(buf, 8U, 8U);
    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");
    zassert_true(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");
    zassert_equal((uint16_t)8U, e.len, "");
    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_OK, st, "");
}

/* TC-MCUB-021: 1, 2 or 3 bytes left over cannot hold a TLV header. */
ZTEST(mcuboot_image, test_tlv_trailing_stray_bytes_are_malformed)
{
    uint8_t                    buf[TLV_BUF_MAX];
    uds_mcuboot_tlv_iter_t     it;
    uds_mcuboot_tlv_t          e;
    uds_mcuboot_parse_status_t st;
    size_t                     stray;

    for (stray = 1U; stray <= 3U; stray++) {
        size_t total = (size_t)UDS_MCUBOOT_TLV_INFO_SIZE + stray;
        size_t o     = 0U;

        o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC);
        o += pack_le16(&buf[o], (uint16_t)total);
        (void)memset(&buf[o], 0x7EU, stray);
        o += stray;

        zassert_equal(UDS_MCUBOOT_PARSE_OK,
                      uds_mcuboot_tlv_iter_begin(&it, buf, o,
                                                 (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                      "");
        st = UDS_MCUBOOT_PARSE_OK;
        zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
        zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH, st, "");
    }
}

/* TC-MCUB-022: wrong it_magic in the info record. */
ZTEST(mcuboot_image, test_tlv_wrong_info_magic_rejected)
{
    uint8_t                buf[TLV_BUF_MAX];
    uds_mcuboot_tlv_iter_t it;
    size_t                 area_len;

    area_len = build_two_tlv_area(buf, 0x1234U, 0);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    /* An erased-flash area is not a TLV area. */
    (void)memset(buf, 0xFF, (size_t)UDS_MCUBOOT_TLV_INFO_SIZE);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_MAGIC,
                  uds_mcuboot_tlv_iter_begin(&it, buf,
                                             (size_t)UDS_MCUBOOT_TLV_INFO_SIZE,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");
}

/* TC-MCUB-023: it_tlv_tot must equal the area_len the caller passes, in both
 * directions.  The documented precondition is what stops an iteration
 * running off one area into the next. */
ZTEST(mcuboot_image, test_tlv_tot_must_match_area_len)
{
    uint8_t                buf[TLV_BUF_MAX];
    uds_mcuboot_tlv_iter_t it;
    size_t                 area_len;

    /* Record declares 4 MORE bytes than the caller passed. */
    area_len = build_two_tlv_area(buf, (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC, 4);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    /* Record declares 4 FEWER. */
    area_len = build_two_tlv_area(buf, (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC, -4);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    /* Consistent record, but the caller passes a different length. */
    area_len = build_two_tlv_area(buf, (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC, 0);
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len + 1U,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len - 1U,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");
}

/* TC-MCUB-024: it_tlv_tot below its own record size. */
ZTEST(mcuboot_image, test_tlv_tot_below_info_size_rejected)
{
    uint8_t                buf[UDS_MCUBOOT_TLV_INFO_SIZE];
    uds_mcuboot_tlv_iter_t it;
    uint16_t               tot;

    for (tot = 0U; tot < (uint16_t)UDS_MCUBOOT_TLV_INFO_SIZE; tot++) {
        size_t o = 0U;

        o += pack_le16(&buf[o], (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC);
        o += pack_le16(&buf[o], tot);
        zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH,
                      uds_mcuboot_tlv_iter_begin(&it, buf, o,
                                                 (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                      "");
    }
}

/* TC-MCUB-025: an area buffer too small to hold the info record. */
ZTEST(mcuboot_image, test_tlv_area_shorter_than_info_record)
{
    uint8_t                buf[UDS_MCUBOOT_TLV_INFO_SIZE];
    uds_mcuboot_tlv_iter_t it;
    size_t                 n;

    (void)pack_le16(&buf[0], (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC);
    (void)pack_le16(&buf[2], (uint16_t)UDS_MCUBOOT_TLV_INFO_SIZE);

    for (n = 0U; n < (size_t)UDS_MCUBOOT_TLV_INFO_SIZE; n++) {
        zassert_equal(UDS_MCUBOOT_PARSE_ERR_BUFFER_TOO_SMALL,
                      uds_mcuboot_tlv_iter_begin(&it, buf, n,
                                                 (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                      "");
    }
}

/* TC-MCUB-026: NULL arguments on both iterator entry points. */
ZTEST(mcuboot_image, test_tlv_null_args)
{
    uint8_t                    buf[TLV_BUF_MAX];
    uds_mcuboot_tlv_iter_t     it;
    uds_mcuboot_tlv_t          e;
    uds_mcuboot_parse_status_t st;
    size_t                     area_len;

    area_len = build_two_tlv_area(buf, (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC, 0);

    zassert_equal(UDS_MCUBOOT_PARSE_ERR_NULL_ARG,
                  uds_mcuboot_tlv_iter_begin(NULL, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_NULL_ARG,
                  uds_mcuboot_tlv_iter_begin(&it, NULL, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    /* NULL status: nothing to report into, so just refuse without faulting. */
    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, NULL), "");

    st = UDS_MCUBOOT_PARSE_OK;
    zassert_false(uds_mcuboot_tlv_iter_next(NULL, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_NULL_ARG, st, "");

    st = UDS_MCUBOOT_PARSE_OK;
    zassert_false(uds_mcuboot_tlv_iter_next(&it, NULL, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_NULL_ARG, st, "");

    /* A zeroed iterator (area == NULL) is refused rather than dereferenced. */
    (void)memset(&it, 0, sizeof(it));
    st = UDS_MCUBOOT_PARSE_OK;
    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_NULL_ARG, st, "");
}

/* TC-MCUB-027: a maximal it_len is refused, and nothing beyond the area is
 * read.  The fixture buffer is sized exactly to the declared area, so any
 * over-read is a stack-buffer-overflow under ASan, which CI runs via
 * build_tests.sh --sanitize. */
ZTEST(mcuboot_image, test_tlv_maximal_len_reads_nothing_out_of_bounds)
{
    /* Exactly sized: info(4) + tlv hdr(4) + 2 value bytes. */
    uint8_t                    tight[10];
    uds_mcuboot_tlv_iter_t     it;
    uds_mcuboot_tlv_t          e;
    uds_mcuboot_parse_status_t st;
    size_t                     o = 0U;

    o += pack_le16(&tight[o], (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC);
    o += pack_le16(&tight[o], (uint16_t)sizeof(tight));
    o += pack_le16(&tight[o], (uint16_t)UDS_MCUBOOT_TLV_ECDSA_SIG);
    o += pack_le16(&tight[o], 0xFFFFU);        /* claims 65535 value bytes */
    tight[o] = 0x11U;
    o += 1U;
    tight[o] = 0x22U;
    o += 1U;
    zassert_equal(sizeof(tight), o, "");

    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, tight, sizeof(tight),
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");
    st = UDS_MCUBOOT_PARSE_OK;
    zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
    zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH, st, "");
}

/* TC-MCUB-028: once malformed, the iterator keeps refusing.  A caller that
 * ignores the status and loops must not eventually be told "clean end". */
ZTEST(mcuboot_image, test_tlv_malformed_state_is_sticky)
{
    uint8_t                    buf[TLV_BUF_MAX];
    uds_mcuboot_tlv_iter_t     it;
    uds_mcuboot_tlv_t          e;
    uds_mcuboot_parse_status_t st;
    size_t                     area_len;
    uint32_t                   i;

    area_len = build_lying_tlv_area(buf, 0xFF00U, 8U);
    zassert_equal(UDS_MCUBOOT_PARSE_OK,
                  uds_mcuboot_tlv_iter_begin(&it, buf, area_len,
                                             (uint16_t)UDS_MCUBOOT_TLV_INFO_MAGIC),
                  "");

    for (i = 0U; i < 5U; i++) {
        st = UDS_MCUBOOT_PARSE_OK;
        zassert_false(uds_mcuboot_tlv_iter_next(&it, &e, &st), "");
        zassert_equal(UDS_MCUBOOT_PARSE_ERR_BAD_LENGTH, st, "");
    }
}

/* ==========================================================================
 * run_all_tests — required by tests/runner/test_main.c
 * ========================================================================== */

void run_all_tests(void)
{
    /* --- image header --- */
    RUN_TEST(mcuboot_image__test_valid_header_decodes_every_field);
    RUN_TEST(mcuboot_image__test_header_wrong_magic_rejected);
    RUN_TEST(mcuboot_image__test_header_buffer_too_small);
    RUN_TEST(mcuboot_image__test_header_v1_magic_rejected);
    RUN_TEST(mcuboot_image__test_header_null_args);
    RUN_TEST(mcuboot_image__test_header_hdr_size_bounds);
    RUN_TEST(mcuboot_image__test_header_excess_bytes_and_out_untouched_on_error);

    /* --- TLV iteration, well-formed --- */
    RUN_TEST(mcuboot_image__test_tlv_iterate_sha256_then_signature);
    RUN_TEST(mcuboot_image__test_tlv_empty_area_ends_cleanly);
    RUN_TEST(mcuboot_image__test_tlv_protected_area_magic_is_distinct);
    RUN_TEST(mcuboot_image__test_tlv_zero_length_value_is_an_entry);

    /* --- TLV iteration, malformed --- */
    RUN_TEST(mcuboot_image__test_tlv_value_len_beyond_area_is_malformed_not_end);
    RUN_TEST(mcuboot_image__test_tlv_trailing_stray_bytes_are_malformed);
    RUN_TEST(mcuboot_image__test_tlv_wrong_info_magic_rejected);
    RUN_TEST(mcuboot_image__test_tlv_tot_must_match_area_len);
    RUN_TEST(mcuboot_image__test_tlv_tot_below_info_size_rejected);
    RUN_TEST(mcuboot_image__test_tlv_area_shorter_than_info_record);
    RUN_TEST(mcuboot_image__test_tlv_null_args);
    RUN_TEST(mcuboot_image__test_tlv_maximal_len_reads_nothing_out_of_bounds);
    RUN_TEST(mcuboot_image__test_tlv_malformed_state_is_sticky);
}
