// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_phase5_security_algo.c
 *
 * MODULE UNDER TEST: core/uds_security_algo.c  (Phase 1 - AES-CMAC + TRNG)
 *                    core/uds_aes_cmac.c        (AES-128 + CMAC primitive)
 *
 * PHASE 1 - Production Security Hardening [P1-SEC]
 *   Updated from Phase 5 XOR-based tests to cover:
 *     - AES-CMAC key derivation properties
 *     - TRNG callback integration
 *     - OEM derive callback override (pluggable interface)
 *     - Per-level key injection (uds_security_algo_set_level_key)
 *     - All existing structural tests (seed format, sequence, replay, NULLs)
 *
 * TEST CASES:
 *   TC-ALGO-001  reset() clears sequence counter
 *   TC-ALGO-002  generate_seed() returns UDS_ALGO_SEED_LEN bytes
 *   TC-ALGO-003  Seed byte [SEQ_OFFSET] encodes lower 8 bits of counter
 *   TC-ALGO-004  Sequence counter increments on every seed request
 *   TC-ALGO-005  Sequence byte wrap: full counter never reaches 0x0000
 *   TC-ALGO-006  Level-1 correct key accepted (AES-CMAC derivation)
 *   TC-ALGO-007  Level-2 correct key accepted (AES-CMAC derivation)
 *   TC-ALGO-008  Level-1 and Level-2 keys differ for the same seed
 *   TC-ALGO-009  validate_key() accepts correctly derived key
 *   TC-ALGO-010  validate_key() rejects bit-flipped key
 *   TC-ALGO-011  Replay protection: advanced counter rejects old seed
 *   TC-ALGO-012  Fresh seed with correct key is accepted
 *   TC-ALGO-013  Level-1 key rejected when validating at Level-2
 *   TC-ALGO-014  Level-2 key rejected when validating at Level-1
 *   TC-ALGO-015  NULL seed_buf to generate_seed -> ERR_NULL_PTR
 *   TC-ALGO-016  NULL out_seed_len to generate_seed -> ERR_NULL_PTR
 *   TC-ALGO-017  seed_buf_len too small -> ERR_INVALID_PARAM
 *   TC-ALGO-018  NULL seed to derive_key -> ERR_NULL_PTR
 *   TC-ALGO-019  NULL key_out to derive_key -> ERR_NULL_PTR
 *   TC-ALGO-020  Unknown security level in derive_key -> ERR_INVALID_PARAM
 *   TC-ALGO-021  NULL seed in validate_key -> false
 *   TC-ALGO-022  NULL key in validate_key -> false
 *   TC-ALGO-023  Consecutive seeds differ in nonce field
 *   TC-ALGO-024  get_sequence() reflects the internal counter
 *   TC-ALGO-025  TRNG callback overrides LFSR when registered
 *   TC-ALGO-026  OEM derive callback override replaces AES-CMAC
 *   TC-ALGO-027  OEM derive callback can be cleared (restored to AES-CMAC)
 *   TC-ALGO-028  Per-level key injection changes derivation result
 *   TC-ALGO-029  Key injection with NULL pointer returns ERR_NULL_PTR
 *   TC-ALGO-030  Key injection with unknown level returns ERR_INVALID_PARAM
 *   TC-ALGO-031  Seed embeds correct security_level byte at LEVEL_OFFSET
 *   TC-ALGO-032  AES-128 known-answer test (NIST FIPS 197 Appendix B)
 *   TC-ALGO-033  AES-CMAC known-answer test (RFC 4493 D.1 empty message)
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_security_algo.h"
#include "uds_aes_cmac.h"
#include "uds_types.h"

ZTEST_SUITE(test_phase5_security_algo, NULL, NULL, NULL, NULL, NULL);

/* Helpers */
static void gen_seed(uint8_t level, uint8_t *seed_out)
{
    uint8_t len = 0U;
    uds_status_t rc = uds_security_algo_generate_seed(level, seed_out,
                                                        (uint8_t)UDS_ALGO_SEED_LEN, &len);
    zassert_equal(rc, UDS_STATUS_OK, "gen_seed must return OK");
    zassert_equal(len, (uint8_t)UDS_ALGO_SEED_LEN, "seed length must be UDS_ALGO_SEED_LEN");
}
static void derive(uint8_t level, const uint8_t *seed, uint8_t *key_out)
{
    uds_status_t rc = uds_security_algo_derive_key(level, seed, key_out);
    zassert_equal(rc, UDS_STATUS_OK, "derive_key must return OK");
}
static uint8_t seed_seq_lo(const uint8_t *seed) { return seed[UDS_ALGO_SEED_SEQ_OFFSET]; }

ZTEST(test_phase5_security_algo, tc001_reset_clears_sequence)
{
    uds_security_algo_reset();
    zassert_equal(uds_security_algo_get_sequence(), (uint16_t)0U, "seq must be 0 after reset");
}
ZTEST(test_phase5_security_algo, tc002_generate_seed_length)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t len = 0U;
    uds_status_t rc = uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &len);
    zassert_equal(rc, UDS_STATUS_OK, "must be OK");
    zassert_equal(len, (uint8_t)UDS_ALGO_SEED_LEN, "len must be UDS_ALGO_SEED_LEN");
}
ZTEST(test_phase5_security_algo, tc003_seed_encodes_sequence)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; gen_seed(0x01U, seed);
    uint8_t m = (uint8_t)(uds_security_algo_get_sequence() & 0xFFU);
    zassert_equal(seed_seq_lo(seed), m, "seq byte must match lower 8 bits of counter");
}
ZTEST(test_phase5_security_algo, tc004_sequence_increments)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; gen_seed(0x01U, seed);
    uint16_t s1 = uds_security_algo_get_sequence(); gen_seed(0x01U, seed);
    uint16_t s2 = uds_security_algo_get_sequence();
    zassert_equal(s2, (uint16_t)(s1 + 1U), "counter must increment by 1");
}
ZTEST(test_phase5_security_algo, tc005_sequence_never_zero)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t len;
    uint16_t i;
    for (i = 0U; i < 300U; i++) {
        (void)uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &len);
        zassert_not_equal(uds_security_algo_get_sequence(), (uint16_t)0U,
            "full counter must never be 0x0000");
    }
}
ZTEST(test_phase5_security_algo, tc006_level1_key_correct)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t key[UDS_ALGO_KEY_LEN];
    gen_seed(0x01U, seed); derive(0x02U, seed, key);
    zassert_true(uds_security_algo_validate_key(0x02U, seed, UDS_ALGO_SEED_LEN, key, UDS_ALGO_KEY_LEN),
        "Level-1 AES-CMAC key must validate");
}
ZTEST(test_phase5_security_algo, tc007_level2_key_correct)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t key[UDS_ALGO_KEY_LEN];
    gen_seed(0x03U, seed); derive(0x04U, seed, key);
    zassert_true(uds_security_algo_validate_key(0x04U, seed, UDS_ALGO_SEED_LEN, key, UDS_ALGO_KEY_LEN),
        "Level-2 AES-CMAC key must validate");
}
ZTEST(test_phase5_security_algo, tc008_level1_level2_keys_differ)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; gen_seed(0x01U, seed);
    uint8_t k1[UDS_ALGO_KEY_LEN]; uint8_t k2[UDS_ALGO_KEY_LEN];
    (void)uds_security_algo_derive_key(0x02U, seed, k1);
    (void)uds_security_algo_derive_key(0x04U, seed, k2);
    zassert_false(memcmp(k1, k2, UDS_ALGO_KEY_LEN) == 0, "L1 and L2 keys must differ");
}
ZTEST(test_phase5_security_algo, tc009_validate_accepts_correct_key)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t key[UDS_ALGO_KEY_LEN];
    gen_seed(0x01U, seed); derive(0x02U, seed, key);
    zassert_true(uds_security_algo_validate_key(0x02U, seed, UDS_ALGO_SEED_LEN, key, UDS_ALGO_KEY_LEN),
        "correct key must be accepted");
}
ZTEST(test_phase5_security_algo, tc010_validate_rejects_wrong_key)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t key[UDS_ALGO_KEY_LEN];
    gen_seed(0x01U, seed); derive(0x02U, seed, key);
    key[0] ^= 0x01U;
    zassert_false(uds_security_algo_validate_key(0x02U, seed, UDS_ALGO_SEED_LEN, key, UDS_ALGO_KEY_LEN),
        "corrupted key must be rejected");
}
ZTEST(test_phase5_security_algo, tc011_replay_rejected)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t key[UDS_ALGO_KEY_LEN]; uint8_t ns[UDS_ALGO_SEED_LEN];
    gen_seed(0x01U, seed); derive(0x02U, seed, key);
    zassert_true(uds_security_algo_validate_key(0x02U, seed, UDS_ALGO_SEED_LEN, key, UDS_ALGO_KEY_LEN), "first must pass");
    gen_seed(0x01U, ns);
    zassert_false(uds_security_algo_validate_key(0x02U, seed, UDS_ALGO_SEED_LEN, key, UDS_ALGO_KEY_LEN),
        "replayed pair must be rejected");
}
ZTEST(test_phase5_security_algo, tc012_fresh_seed_accepted)
{
    uds_security_algo_reset();
    uint8_t os[UDS_ALGO_SEED_LEN]; uint8_t ok[UDS_ALGO_KEY_LEN];
    uint8_t ns[UDS_ALGO_SEED_LEN]; uint8_t nk[UDS_ALGO_KEY_LEN];
    gen_seed(0x01U, os); derive(0x02U, os, ok);
    (void)uds_security_algo_validate_key(0x02U, os, UDS_ALGO_SEED_LEN, ok, UDS_ALGO_KEY_LEN);
    gen_seed(0x01U, ns); derive(0x02U, ns, nk);
    zassert_true(uds_security_algo_validate_key(0x02U, ns, UDS_ALGO_SEED_LEN, nk, UDS_ALGO_KEY_LEN),
        "fresh seed+key must be accepted");
}
ZTEST(test_phase5_security_algo, tc013_level1_key_rejected_at_level2)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t kl1[UDS_ALGO_KEY_LEN];
    gen_seed(0x03U, seed); derive(0x02U, seed, kl1);
    zassert_false(uds_security_algo_validate_key(0x04U, seed, UDS_ALGO_SEED_LEN, kl1, UDS_ALGO_KEY_LEN),
        "L1 key must be rejected at L2");
}
ZTEST(test_phase5_security_algo, tc014_level2_key_rejected_at_level1)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t kl2[UDS_ALGO_KEY_LEN];
    gen_seed(0x01U, seed); derive(0x04U, seed, kl2);
    zassert_false(uds_security_algo_validate_key(0x02U, seed, UDS_ALGO_SEED_LEN, kl2, UDS_ALGO_KEY_LEN),
        "L2 key must be rejected at L1");
}
ZTEST(test_phase5_security_algo, tc015_generate_null_seed_buf)
{
    uds_security_algo_reset();
    uint8_t len = 0U;
    zassert_equal(uds_security_algo_generate_seed(0x01U, NULL, 8U, &len),
        UDS_STATUS_ERR_NULL_PTR, "NULL seed_buf must return ERR_NULL_PTR");
}
ZTEST(test_phase5_security_algo, tc016_generate_null_out_len)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN];
    zassert_equal(uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), NULL),
        UDS_STATUS_ERR_NULL_PTR, "NULL out_len must return ERR_NULL_PTR");
}
ZTEST(test_phase5_security_algo, tc017_generate_buf_too_small)
{
    uds_security_algo_reset();
    uint8_t seed[2]; uint8_t len = 0U;
    zassert_equal(uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &len),
        UDS_STATUS_ERR_INVALID_PARAM, "small buf must return ERR_INVALID_PARAM");
}
ZTEST(test_phase5_security_algo, tc018_derive_null_seed)
{
    uint8_t key[UDS_ALGO_KEY_LEN];
    zassert_equal(uds_security_algo_derive_key(0x02U, NULL, key),
        UDS_STATUS_ERR_NULL_PTR, "NULL seed must return ERR_NULL_PTR");
}
ZTEST(test_phase5_security_algo, tc019_derive_null_key_out)
{
    uint8_t seed[UDS_ALGO_SEED_LEN] = {0};
    zassert_equal(uds_security_algo_derive_key(0x02U, seed, NULL),
        UDS_STATUS_ERR_NULL_PTR, "NULL key_out must return ERR_NULL_PTR");
}
ZTEST(test_phase5_security_algo, tc020_derive_unknown_level)
{
    uint8_t seed[UDS_ALGO_SEED_LEN] = {0}; uint8_t key[UDS_ALGO_KEY_LEN];
    zassert_equal(uds_security_algo_derive_key(0x06U, seed, key),
        UDS_STATUS_ERR_INVALID_PARAM, "unknown level must return ERR_INVALID_PARAM");
}
ZTEST(test_phase5_security_algo, tc021_validate_null_seed)
{
    uint8_t key[UDS_ALGO_KEY_LEN] = {0};
    zassert_false(uds_security_algo_validate_key(0x02U, NULL, 8U, key, 4U), "NULL seed must return false");
}
ZTEST(test_phase5_security_algo, tc022_validate_null_key)
{
    uint8_t seed[UDS_ALGO_SEED_LEN] = {0};
    zassert_false(uds_security_algo_validate_key(0x02U, seed, 8U, NULL, 4U), "NULL key must return false");
}
ZTEST(test_phase5_security_algo, tc023_consecutive_seeds_differ_in_nonce)
{
    uds_security_algo_reset();
    uint8_t s1[UDS_ALGO_SEED_LEN]; uint8_t s2[UDS_ALGO_SEED_LEN];
    gen_seed(0x01U, s1); gen_seed(0x01U, s2);
    zassert_false(memcmp(&s1[UDS_ALGO_SEED_NONCE_OFFSET], &s2[UDS_ALGO_SEED_NONCE_OFFSET],
        UDS_ALGO_SEED_NONCE_LEN) == 0, "consecutive nonce fields must differ");
}
ZTEST(test_phase5_security_algo, tc024_get_sequence_reflects_counter)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t len;
    (void)uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &len);
    (void)uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &len);
    (void)uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &len);
    zassert_equal(uds_security_algo_get_sequence(), (uint16_t)3U, "must return 3");
}
static uds_status_t stub_rng_cb(uint8_t *buf, uint8_t len)
{
    uint8_t i; for (i = 0U; i < len; i++) { buf[i] = 0xA5U; } return UDS_STATUS_OK;
}
ZTEST(test_phase5_security_algo, tc025_trng_callback_overrides_lfsr)
{
    uds_security_algo_reset(); uds_security_algo_set_rng_cb(stub_rng_cb);
    uint8_t seed[UDS_ALGO_SEED_LEN]; uint8_t len;
    (void)uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &len);
    uint8_t i;
    for (i = 0U; i < (uint8_t)UDS_ALGO_SEED_NONCE_LEN; i++) {
        zassert_equal(seed[UDS_ALGO_SEED_NONCE_OFFSET + i], (uint8_t)0xA5U, "TRNG stub must fill nonce");
    }
    uds_security_algo_set_rng_cb(NULL);
}
static uds_status_t stub_derive_cb(uint8_t sl, const uint8_t *s, uint8_t *ko)
{
    (void)sl; (void)s;
    ko[0]=0xDEU; ko[1]=0xADU; ko[2]=0xBEU; ko[3]=0xEFU;
    return UDS_STATUS_OK;
}
ZTEST(test_phase5_security_algo, tc026_oem_derive_callback_overrides_aes_cmac)
{
    uds_security_algo_reset(); uds_security_algo_set_derive_cb(stub_derive_cb);
    uint8_t seed[UDS_ALGO_SEED_LEN]; gen_seed(0x01U, seed);
    uint8_t key[UDS_ALGO_KEY_LEN];
    zassert_equal(uds_security_algo_derive_key(0x02U, seed, key), UDS_STATUS_OK, "must be OK");
    zassert_equal(key[0], 0xDEU, "byte 0 must be 0xDE");
    zassert_equal(key[1], 0xADU, "byte 1 must be 0xAD");
    zassert_equal(key[2], 0xBEU, "byte 2 must be 0xBE");
    zassert_equal(key[3], 0xEFU, "byte 3 must be 0xEF");
    zassert_true(uds_security_algo_validate_key(0x02U, seed, UDS_ALGO_SEED_LEN, key, UDS_ALGO_KEY_LEN),
        "OEM-derived key must validate");
    uds_security_algo_set_derive_cb(NULL);
}
ZTEST(test_phase5_security_algo, tc027_oem_derive_callback_clearable)
{
    uds_security_algo_reset(); uds_security_algo_set_derive_cb(stub_derive_cb);
    uint8_t seed[UDS_ALGO_SEED_LEN]; gen_seed(0x01U, seed);
    uint8_t oem_key[UDS_ALGO_KEY_LEN];
    (void)uds_security_algo_derive_key(0x02U, seed, oem_key);
    uds_security_algo_set_derive_cb(NULL);
    uint8_t cmac_key[UDS_ALGO_KEY_LEN];
    (void)uds_security_algo_derive_key(0x02U, seed, cmac_key);
    zassert_false(memcmp(oem_key, cmac_key, UDS_ALGO_KEY_LEN) == 0, "AES-CMAC must differ from OEM stub");
}
ZTEST(test_phase5_security_algo, tc028_key_injection_changes_derivation)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; gen_seed(0x01U, seed);
    uint8_t kdef[UDS_ALGO_KEY_LEN];
    (void)uds_security_algo_derive_key(0x02U, seed, kdef);
    static const uint8_t nk[16U] = {
        0xFFU,0xFEU,0xFDU,0xFCU,0xFBU,0xFAU,0xF9U,0xF8U,
        0xF7U,0xF6U,0xF5U,0xF4U,0xF3U,0xF2U,0xF1U,0xF0U
    };
    zassert_equal(uds_security_algo_set_level_key(0x01U, nk), UDS_STATUS_OK, "inject must succeed");
    uint8_t kinj[UDS_ALGO_KEY_LEN];
    (void)uds_security_algo_derive_key(0x02U, seed, kinj);
    zassert_false(memcmp(kdef, kinj, UDS_ALGO_KEY_LEN) == 0, "injected key must change derivation");
    uds_security_algo_reset();
}
ZTEST(test_phase5_security_algo, tc029_key_injection_null_ptr)
{
    uds_security_algo_reset();
    zassert_equal(uds_security_algo_set_level_key(0x01U, NULL),
        UDS_STATUS_ERR_NULL_PTR, "NULL key must return ERR_NULL_PTR");
}
ZTEST(test_phase5_security_algo, tc030_key_injection_unknown_level)
{
    uds_security_algo_reset();
    static const uint8_t dk[16U] = {0};
    zassert_equal(uds_security_algo_set_level_key(0x09U, dk),
        UDS_STATUS_ERR_INVALID_PARAM, "unknown level must return ERR_INVALID_PARAM");
}
ZTEST(test_phase5_security_algo, tc031_seed_embeds_security_level)
{
    uds_security_algo_reset();
    uint8_t seed[UDS_ALGO_SEED_LEN]; gen_seed(0x03U, seed);
    zassert_equal(seed[UDS_ALGO_SEED_LEVEL_OFFSET], (uint8_t)0x03U, "level byte must be 0x03");
}
/* NIST FIPS 197 Appendix B */
ZTEST(test_phase5_security_algo, tc032_aes128_known_answer)
{
    static const uint8_t key[16U] = {
        0x2BU,0x7EU,0x15U,0x16U,0x28U,0xAEU,0xD2U,0xA6U,
        0xABU,0xF7U,0x15U,0x88U,0x09U,0xCFU,0x4FU,0x3CU
    };
    static const uint8_t pt[16U] = {
        0x32U,0x43U,0xF6U,0xA8U,0x88U,0x5AU,0x30U,0x8DU,
        0x31U,0x31U,0x98U,0xA2U,0xE0U,0x37U,0x07U,0x34U
    };
    static const uint8_t exp[16U] = {
        0x39U,0x25U,0x84U,0x1DU,0x02U,0xDCU,0x09U,0xFBU,
        0xDCU,0x11U,0x85U,0x97U,0x19U,0x6AU,0x0BU,0x32U
    };
    uint8_t ct[16U];
    uds_aes128_encrypt_block(key, pt, ct);
    zassert_mem_equal(ct, exp, 16U, "FIPS 197 Appendix B AES-128 KAT failed");
}
/* RFC 4493 D.1 empty message */
ZTEST(test_phase5_security_algo, tc033_aes_cmac_rfc4493_empty)
{
    static const uint8_t key[16U] = {
        0x2BU,0x7EU,0x15U,0x16U,0x28U,0xAEU,0xD2U,0xA6U,
        0xABU,0xF7U,0x15U,0x88U,0x09U,0xCFU,0x4FU,0x3CU
    };
    static const uint8_t exp[16U] = {
        0xBBU,0x1DU,0x69U,0x29U,0xE9U,0x59U,0x37U,0x28U,
        0x7FU,0xA3U,0x7DU,0x12U,0x9BU,0x75U,0x67U,0x46U
    };
    uint8_t mac[16U];
    zassert_equal(uds_aes_cmac(key, NULL, 0U, mac), 0, "CMAC must return 0");
    zassert_mem_equal(mac, exp, 16U, "RFC 4493 D.1 empty CMAC KAT failed");
}

/* run_all_tests shim */
extern void test_phase5_security_algo__tc001_reset_clears_sequence(void);
extern void test_phase5_security_algo__tc006_level1_key_correct(void);
extern void test_phase5_security_algo__tc026_oem_derive_callback_overrides_aes_cmac(void);
extern void test_phase5_security_algo__tc032_aes128_known_answer(void);
extern void test_phase5_security_algo__tc033_aes_cmac_rfc4493_empty(void);

void run_all_tests(void)
{
    RUN_TEST(test_phase5_security_algo__tc001_reset_clears_sequence);
    RUN_TEST(test_phase5_security_algo__tc006_level1_key_correct);
    RUN_TEST(test_phase5_security_algo__tc026_oem_derive_callback_overrides_aes_cmac);
    RUN_TEST(test_phase5_security_algo__tc032_aes128_known_answer);
    RUN_TEST(test_phase5_security_algo__tc033_aes_cmac_rfc4493_empty);
}
