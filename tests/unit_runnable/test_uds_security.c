// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
// ================================
// File: tests/unit/test_uds_security.c
// ================================
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit/test_uds_security.c
 *
 * MODULE UNDER TEST: core/uds_security.c
 *
 * PURPOSE:
 *   Verify the UDS security access state machine (ISO 14229-1 SID 0x27).
 *   Tests cover:
 *     - uds_security_init: NULL/invalid-param guards, happy path, double-init
 *     - uds_security_request_seed: locked/unlocked already checks,
 *       seed generation, lockout rejection
 *     - uds_security_send_key: valid key acceptance, invalid key rejection,
 *       attempt counter increment, lockout engagement after MAX_ATTEMPTS
 *     - uds_security_is_unlocked: level query after unlock/lock
 *     - uds_security_reset: clears level, preserves lockout
 *     - uds_security_tick_1ms: lockout countdown, lockout expiry
 *
 * SECURITY MODEL (from uds_security.c):
 *   - Odd sub-function = RequestSeed (0x01, 0x03, ...)
 *   - Even sub-function = SendKey (0x02, 0x04, ...)
 *   - active_level stores the SEED sub-function value on unlock
 *     (not the KEY sub-function)
 *   - Already-unlocked at a level returns all-zero seed per ISO 14229-1
 *   - After MAX_ATTEMPTS (3) wrong keys, locked_out = true for LOCKOUT_MS
 *
 * KEY ALGORITHM (test stub): xor seed bytes with 0xAA.
 *
 * FRAMEWORK: Zephyr Ztest
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_security.h"
#include "uds_types.h"

/* =========================================================================
 * Test key algorithm stub
 * ========================================================================= */

/** XOR-0xAA algorithm — simplest possible stub for testing. */
static uds_status_t test_seed_generate(
    uint8_t  security_level,
    uint8_t *seed_buf,
    uint8_t  seed_buf_len,
    uint8_t *out_seed_len)
{
    (void)security_level;
    if ((seed_buf == NULL) || (out_seed_len == NULL)) {
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
    }
    uint8_t len = (seed_buf_len < UDS_SECURITY_SEED_LEN)
                ? seed_buf_len
                : (uint8_t)UDS_SECURITY_SEED_LEN;
    for (uint8_t i = 0U; i < len; i++) {
        seed_buf[i] = (uint8_t)(0x10U + i);  /* Deterministic: 0x10,0x11,0x12,0x13 */
    }
    *out_seed_len = len;
    return UDS_STATUS_OK;
}

static bool test_key_validate(
    uint8_t        security_level,
    const uint8_t *seed,
    uint8_t        seed_len,
    const uint8_t *key,
    uint8_t        key_len)
{
    /* [P1-SEC FIX] seed_len (8) != key_len (4) — accept seed_len >= key_len */
    (void)security_level;
    if ((seed == NULL) || (key == NULL)) {
        return false;
    }
    if ((seed_len == 0U) || (key_len == 0U) || (seed_len < key_len)) {
        return false;
    }
    /* Valid key = first key_len seed bytes XOR 0xAA */
    for (uint8_t i = 0U; i < key_len; i++) {
        if (key[i] != (uint8_t)(seed[i] ^ 0xAAU)) {
            return false;
        }
    }
    return true;
}

/** Produce the correct key for the current seed.
 * [P1-SEC FIX] key = UDS_SECURITY_KEY_LEN (4) bytes; seed may be 8 bytes. */
static void compute_correct_key(const uint8_t *seed, uint8_t seed_len,
                                 uint8_t *key_out)
{
    uint8_t klen = (seed_len < (uint8_t)UDS_SECURITY_KEY_LEN)
                   ? seed_len : (uint8_t)UDS_SECURITY_KEY_LEN;
    for (uint8_t i = 0U; i < klen; i++) {
        key_out[i] = (uint8_t)(seed[i] ^ 0xAAU);
    }
}

/* =========================================================================
 * Helper: initialise a fresh security context
 * ========================================================================= */

static uds_status_t default_sec_init(uds_security_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    static const uds_security_cfg_t k_cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,  /* Short for test speed */
        .key_validate_cb  = test_key_validate,
        .seed_generate_cb = test_seed_generate,
    };
    return uds_security_init(ctx, &k_cfg);
}

/** Request seed at level 1, get the seed bytes out. */
static uds_status_t do_seed_request(uds_security_ctx_t *ctx,
                                     uint8_t *seed_out,
                                     uint8_t *seed_len_out)
{
    return uds_security_request_seed(ctx, UDS_SEC_LEVEL_1_SEED,
                                     seed_out, UDS_SECURITY_SEED_LEN,
                                     seed_len_out);
}

/** Full unlock sequence for level 1: request seed then send correct key. */
static uds_status_t do_full_unlock(uds_security_ctx_t *ctx)
{
    uint8_t seed[UDS_SECURITY_SEED_LEN];
    uint8_t seed_len = 0U;
    uds_status_t rc = do_seed_request(ctx, seed, &seed_len);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }
    /* [P1-SEC FIX] key buffer = KEY_LEN (4); send KEY_LEN bytes */
    uint8_t key[UDS_SECURITY_KEY_LEN];
    compute_correct_key(seed, seed_len, key);
    return uds_security_send_key(ctx, UDS_SEC_LEVEL_1_KEY, key,
                                 (uint8_t)UDS_SECURITY_KEY_LEN);
}

/* =========================================================================
 * Test suite: uds_security_init
 * ========================================================================= */

ZTEST_SUITE(test_uds_security_init, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SEC-INIT-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_security_init, test_null_ctx)
{
    static const uds_security_cfg_t cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 10000U,
        .key_validate_cb  = test_key_validate,
        .seed_generate_cb = test_seed_generate,
    };
    zassert_equal(uds_security_init(NULL, &cfg), UDS_STATUS_ERR_NULL_PTR,
                  "NULL ctx must fail");
}

/**
 * TC-SEC-INIT-002: NULL cfg → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_security_init, test_null_cfg)
{
    uds_security_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(uds_security_init(&ctx, NULL), UDS_STATUS_ERR_NULL_PTR,
                  "NULL cfg must fail");
}

/**
 * TC-SEC-INIT-003: NULL key_validate_cb → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_uds_security_init, test_null_key_cb)
{
    uds_security_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    static const uds_security_cfg_t cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 10000U,
        .key_validate_cb  = NULL,   /* Missing */
        .seed_generate_cb = test_seed_generate,
    };
    zassert_equal(uds_security_init(&ctx, &cfg), UDS_STATUS_ERR_INVALID_PARAM,
                  "NULL key_validate_cb must be rejected");
}

/**
 * TC-SEC-INIT-004: NULL seed_generate_cb → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_uds_security_init, test_null_seed_cb)
{
    uds_security_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    static const uds_security_cfg_t cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 10000U,
        .key_validate_cb  = test_key_validate,
        .seed_generate_cb = NULL,   /* Missing */
    };
    zassert_equal(uds_security_init(&ctx, &cfg), UDS_STATUS_ERR_INVALID_PARAM,
                  "NULL seed_generate_cb must be rejected");
}

/**
 * TC-SEC-INIT-005: Happy path → OK, initial state is locked, level = UNLOCKED.
 */
ZTEST(test_uds_security_init, test_happy_path)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init must succeed");
    zassert_true(ctx.initialized, "initialized must be set");
    zassert_equal(ctx.active_level, (uint8_t)UDS_SEC_LEVEL_UNLOCKED,
                  "initial level must be UNLOCKED (0)");
    zassert_false(ctx.locked_out, "must not be locked out initially");
    zassert_equal(ctx.failed_attempts, 0U, "no failed attempts initially");
}

/**
 * TC-SEC-INIT-006: Double init → UDS_STATUS_ERR_ALREADY_INITIALIZED.
 */
ZTEST(test_uds_security_init, test_double_init)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "first init must succeed");
    /* Call init again WITHOUT zeroing ctx — must detect already-initialized */
    static const uds_security_cfg_t k_cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = test_key_validate,
        .seed_generate_cb = test_seed_generate,
    };
    zassert_equal(uds_security_init(&ctx, &k_cfg), UDS_STATUS_ERR_ALREADY_INITIALIZED,
                  "double init must fail");
}

/* =========================================================================
 * Test suite: uds_security_request_seed
 * ========================================================================= */

ZTEST_SUITE(test_uds_security_seed, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SEC-SEED-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_security_seed, test_null_ctx)
{
    uint8_t seed[4]; uint8_t len;
    zassert_equal(uds_security_request_seed(NULL, 0x01U, seed, 4U, &len),
                  UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SEC-SEED-002: NULL seed_buf → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_security_seed, test_null_seed_buf)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");
    uint8_t len;
    zassert_equal(uds_security_request_seed(&ctx, 0x01U, NULL, 4U, &len),
                  UDS_STATUS_ERR_NULL_PTR, "NULL seed_buf must fail");
}

/**
 * TC-SEC-SEED-003: Valid seed request → OK, seed is non-zero.
 */
ZTEST(test_uds_security_seed, test_seed_generated)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t seed[UDS_SECURITY_SEED_LEN] = { 0 };
    uint8_t seed_len = 0U;
    uds_status_t rc = do_seed_request(&ctx, seed, &seed_len);
    zassert_equal(rc, UDS_STATUS_OK, "Seed request must succeed");
    zassert_equal(seed_len, (uint8_t)UDS_SECURITY_SEED_LEN, "Seed length mismatch");
    zassert_true(ctx.seed_pending, "seed_pending must be set after seed request");

    /* Our test generator produces deterministic non-zero seeds */
    zassert_equal(seed[0], 0x10U, "seed[0] mismatch (test generator)");
    zassert_equal(seed[1], 0x11U, "seed[1] mismatch");
}

/**
 * TC-SEC-SEED-004: Level already unlocked → returns all-zero seed per ISO 14229-1.
 */
ZTEST(test_uds_security_seed, test_already_unlocked_returns_zero_seed)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    /* Unlock level 1 */
    zassert_equal(do_full_unlock(&ctx), UDS_STATUS_OK, "Unlock must succeed");

    /* Request seed again for the same level */
    uint8_t seed[UDS_SECURITY_SEED_LEN];
    memset(seed, 0xFFU, sizeof(seed));
    uint8_t seed_len = 0U;
    uds_status_t rc = do_seed_request(&ctx, seed, &seed_len);
    zassert_equal(rc, UDS_STATUS_OK, "Seed request for unlocked level must succeed");

    /* All bytes must be zero (already unlocked indicator) */
    for (uint8_t i = 0U; i < seed_len; i++) {
        zassert_equal(seed[i], 0x00U,
                      "Seed must be zero for already-unlocked level");
    }
}

/**
 * TC-SEC-SEED-005: Request seed while locked out → UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED.
 */
ZTEST(test_uds_security_seed, test_lockout_blocks_seed_request)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    /* Trigger lockout with 3 bad keys */
    uint8_t seed[UDS_SECURITY_SEED_LEN];
    uint8_t seed_len = 0U;
    do_seed_request(&ctx, seed, &seed_len);

    uint8_t bad_key[UDS_SECURITY_KEY_LEN]  /* [P1-SEC FIX] key=4 bytes */ = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    for (int i = 0; i < 3; i++) {
        do_seed_request(&ctx, seed, &seed_len);
        uds_security_send_key(&ctx, UDS_SEC_LEVEL_1_KEY, bad_key, (uint8_t)UDS_SECURITY_KEY_LEN);
    }
    zassert_true(ctx.locked_out, "Must be locked out after 3 bad keys");

    /* Now seed request must fail */
    uds_status_t rc = do_seed_request(&ctx, seed, &seed_len);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED,
                  "Seed request during lockout must fail");
}

/* =========================================================================
 * Test suite: uds_security_send_key
 * ========================================================================= */

ZTEST_SUITE(test_uds_security_send_key, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SEC-KEY-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_security_send_key, test_null_ctx)
{
    uint8_t key[4] = { 0 };
    zassert_equal(uds_security_send_key(NULL, 0x02U, key, 4U),
                  UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SEC-KEY-002: NULL key → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_security_send_key, test_null_key)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");
    uint8_t seed[4]; uint8_t seed_len;
    do_seed_request(&ctx, seed, &seed_len);
    zassert_equal(uds_security_send_key(&ctx, UDS_SEC_LEVEL_1_KEY, NULL, 4U),
                  UDS_STATUS_ERR_NULL_PTR, "NULL key must fail");
}

/**
 * TC-SEC-KEY-003: Key without prior seed request
 *                 → UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE (ISO 14229-1: seed must be
 *                   requested before a key can be submitted; maps to NRC 0x24).
 */
ZTEST(test_uds_security_send_key, test_key_without_seed)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");
    uint8_t key[4] = { 0xBAU, 0xBBU, 0xBCU, 0xBDU };
    uds_status_t rc = uds_security_send_key(&ctx, UDS_SEC_LEVEL_1_KEY, key, 4U);
    /* ISO 14229-1 §10.4.1: key submitted with no pending seed → ERR_SEC_SEED_UNAVAILABLE.
     * uds_server maps this to NRC 0x24 (requestSequenceError). */
    zassert_equal(rc, UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
                  "Key without prior seed must return SEC_SEED_UNAVAILABLE");
}

/**
 * TC-SEC-KEY-004: Correct key → OK, level unlocked.
 */
ZTEST(test_uds_security_send_key, test_correct_key_unlocks)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");
    zassert_equal(do_full_unlock(&ctx), UDS_STATUS_OK, "Full unlock must succeed");

    bool unlocked = false;
    uds_security_is_unlocked(&ctx, UDS_SEC_LEVEL_1_SEED, &unlocked);
    zassert_true(unlocked, "Level must be unlocked after correct key");
    zassert_equal(ctx.failed_attempts, 0U, "Failed attempts must be 0 after unlock");
}

/**
 * TC-SEC-KEY-005: Wrong key → UDS_STATUS_ERR_SEC_INVALID_KEY, counter increments.
 */
ZTEST(test_uds_security_send_key, test_wrong_key_increments_counter)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");
    uint8_t seed[UDS_SECURITY_SEED_LEN]; uint8_t seed_len;
    do_seed_request(&ctx, seed, &seed_len);

    uint8_t wrong_key[UDS_SECURITY_KEY_LEN]  /* [P1-SEC FIX] key=4 bytes */ = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };
    uds_status_t rc = uds_security_send_key(&ctx, UDS_SEC_LEVEL_1_KEY,
                                             wrong_key, seed_len);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_INVALID_KEY,
                  "Wrong key must return ERR_SEC_INVALID_KEY");
    zassert_equal(ctx.failed_attempts, 1U, "Failed attempt counter must be 1");
}

/**
 * TC-SEC-KEY-006: 3 wrong keys → lockout engaged.
 *
 * Validates REQ from uds_security_cfg_t.max_attempts (= 3).
 */
ZTEST(test_uds_security_send_key, test_lockout_after_max_attempts)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t seed[UDS_SECURITY_SEED_LEN];
    uint8_t seed_len = 0U;
    uint8_t wrong_key[UDS_SECURITY_KEY_LEN]  /* [P1-SEC FIX] key=4 bytes */ = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };

    for (int attempt = 1; attempt <= 3; attempt++) {
        /* Re-request seed before each key attempt */
        uds_status_t sr = do_seed_request(&ctx, seed, &seed_len);
        if (ctx.locked_out) {
            break;  /* Already locked — stop */
        }
        zassert_equal(sr, UDS_STATUS_OK, "Seed request must succeed");
        uds_security_send_key(&ctx, UDS_SEC_LEVEL_1_KEY, wrong_key, (uint8_t)UDS_SECURITY_KEY_LEN);
    }
    zassert_true(ctx.locked_out,
                 "Must be locked out after 3 consecutive wrong keys");

    /* 4th attempt must fail immediately */
    uds_status_t rc = do_seed_request(&ctx, seed, &seed_len);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED,
                  "Request during lockout must return ATTEMPT_EXCEEDED");
}

/**
 * TC-SEC-KEY-007: [P4-SEC-01] Key for wrong level → REQUEST_OUT_OF_RANGE,
 *                 seed consumed, failed_attempts incremented.
 *
 * Seeds Level 1, then submits a Level 2 key.  Before the fix the early
 * return left seed_pending = true and failed_attempts = 0, allowing
 * unbounded level probing without triggering lockout.
 */
ZTEST(test_uds_security_send_key, test_wrong_level_key_consumed_as_failed_attempt)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t seed[UDS_SECURITY_SEED_LEN]; uint8_t seed_len;
    do_seed_request(&ctx, seed, &seed_len); /* seed pending at level 1 */

    uint8_t key[UDS_SECURITY_KEY_LEN] = { 0x01U, 0x02U, 0x03U, 0x04U };
    /* Submit Level-2 key while Level-1 seed is pending → mismatch */
    uds_status_t rc = uds_security_send_key(&ctx, UDS_SEC_LEVEL_2_KEY,
                                             key, (uint8_t)UDS_SECURITY_KEY_LEN);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "[P4-SEC-01] Level mismatch must return REQUEST_OUT_OF_RANGE");
    zassert_false(ctx.seed_pending,
                  "[P4-SEC-01] Seed must be consumed on level mismatch");
    zassert_equal(ctx.failed_attempts, 1U,
                  "[P4-SEC-01] Failed attempt must be counted on level mismatch");
}

/**
 * TC-SEC-KEY-008: [P4-SEC-01] Three wrong-level submissions → lockout engaged.
 *
 * An attacker probing level/sub-function pairings must trip the lockout
 * counter just like incorrect key values do.
 */
ZTEST(test_uds_security_send_key, test_wrong_level_triggers_lockout)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    uint8_t seed[UDS_SECURITY_SEED_LEN];
    uint8_t seed_len = 0U;
    uint8_t key[UDS_SECURITY_KEY_LEN] = { 0x01U, 0x02U, 0x03U, 0x04U };

    for (int attempt = 1; attempt <= 3; attempt++) {
        uds_status_t sr = do_seed_request(&ctx, seed, &seed_len);
        if (ctx.locked_out) {
            break;
        }
        zassert_equal(sr, UDS_STATUS_OK, "Seed request must succeed");
        uds_security_send_key(&ctx, UDS_SEC_LEVEL_2_KEY,
                              key, (uint8_t)UDS_SECURITY_KEY_LEN);
    }

    zassert_true(ctx.locked_out,
                 "[P4-SEC-01] Must be locked out after 3 wrong-level submissions");

    uds_status_t rc = do_seed_request(&ctx, seed, &seed_len);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED,
                  "Seed request after wrong-level lockout must return ATTEMPT_EXCEEDED");
}

/* =========================================================================
 * Test suite: uds_security_is_unlocked
 * ========================================================================= */

ZTEST_SUITE(test_uds_security_is_unlocked, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SEC-IU-001: After fresh init, level 1 seed = NOT unlocked.
 */
ZTEST(test_uds_security_is_unlocked, test_initially_locked)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");
    bool unlocked = true;
    zassert_equal(uds_security_is_unlocked(&ctx, UDS_SEC_LEVEL_1_SEED, &unlocked),
                  UDS_STATUS_OK, "is_unlocked must succeed");
    zassert_false(unlocked, "Level must be locked initially");
}

/**
 * TC-SEC-IU-002: After full unlock, level 1 seed IS unlocked.
 */
ZTEST(test_uds_security_is_unlocked, test_unlocked_after_correct_key)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");
    zassert_equal(do_full_unlock(&ctx), UDS_STATUS_OK, "unlock failed");
    bool unlocked = false;
    uds_security_is_unlocked(&ctx, UDS_SEC_LEVEL_1_SEED, &unlocked);
    zassert_true(unlocked, "Level must be unlocked");
}

/* =========================================================================
 * Test suite: uds_security_reset
 * ========================================================================= */

ZTEST_SUITE(test_uds_security_reset, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SEC-RST-001: Reset clears active_level back to UNLOCKED sentinel (0).
 */
ZTEST(test_uds_security_reset, test_reset_clears_level)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");
    zassert_equal(do_full_unlock(&ctx), UDS_STATUS_OK, "unlock failed");

    zassert_equal(uds_security_reset(&ctx), UDS_STATUS_OK, "reset must succeed");
    zassert_equal(ctx.active_level, (uint8_t)UDS_SEC_LEVEL_UNLOCKED,
                  "active_level must be cleared to UNLOCKED");
    zassert_false(ctx.seed_pending, "seed_pending must be cleared");
}

/**
 * TC-SEC-RST-002: Reset does NOT clear lockout (per implementation comment).
 */
ZTEST(test_uds_security_reset, test_reset_preserves_lockout)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    /* Trigger lockout */
    uint8_t seed[UDS_SECURITY_SEED_LEN]; uint8_t seed_len;
    uint8_t bad_key[UDS_SECURITY_KEY_LEN]  /* [P1-SEC FIX] key=4 bytes */ = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
    for (int i = 0; i < 3; i++) {
        do_seed_request(&ctx, seed, &seed_len);
        if (ctx.locked_out) break;
        uds_security_send_key(&ctx, UDS_SEC_LEVEL_1_KEY, bad_key, (uint8_t)UDS_SECURITY_KEY_LEN);
    }
    zassert_true(ctx.locked_out, "Must be locked out");

    uds_security_reset(&ctx);
    /* Lockout intentionally preserved across session reset per uds_security.c */
    zassert_true(ctx.locked_out, "Lockout must survive security reset");
}

/* =========================================================================
 * Test suite: uds_security_tick_1ms — lockout expiry
 * ========================================================================= */

ZTEST_SUITE(test_uds_security_tick, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SEC-TICK-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_security_tick, test_null_ctx)
{
    zassert_equal(uds_security_tick_1ms(NULL), UDS_STATUS_ERR_NULL_PTR,
                  "NULL ctx must fail");
}

/**
 * TC-SEC-TICK-002: Lockout expires after lockout_ms ticks.
 *
 * Uses lockout_ms = 100 (set in default_sec_init for test speed).
 */
ZTEST(test_uds_security_tick, test_lockout_expires)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    /* Trigger lockout */
    uint8_t seed[UDS_SECURITY_SEED_LEN]; uint8_t seed_len;
    uint8_t bad_key[UDS_SECURITY_KEY_LEN]  /* [P1-SEC FIX] key=4 bytes */ = { 0xFFU, 0xFFU, 0xFFU, 0xFFU };
    for (int i = 0; i < 3; i++) {
        do_seed_request(&ctx, seed, &seed_len);
        if (ctx.locked_out) break;
        uds_security_send_key(&ctx, UDS_SEC_LEVEL_1_KEY, bad_key, (uint8_t)UDS_SECURITY_KEY_LEN);
    }
    zassert_true(ctx.locked_out, "Must be locked out");

    /* Tick through lockout period (100 ms + margin) */
    for (int i = 0; i <= 102; i++) {
        uds_security_tick_1ms(&ctx);
    }
    zassert_false(ctx.locked_out, "Lockout must expire after lockout_ms ticks");

    /* Should now be able to request seed again */
    uds_status_t rc = do_seed_request(&ctx, seed, &seed_len);
    zassert_equal(rc, UDS_STATUS_OK,
                  "Seed request must succeed after lockout expires");
}

/**
 * TC-SEC-TICK-003: Timer does not underflow below zero.
 */
ZTEST(test_uds_security_tick, test_no_underflow_when_not_locked)
{
    uds_security_ctx_t ctx;
    zassert_equal(default_sec_init(&ctx), UDS_STATUS_OK, "init failed");

    /* Tick many times with no lockout active */
    for (int i = 0; i < 200; i++) {
        uds_status_t rc = uds_security_tick_1ms(&ctx);
        zassert_equal(rc, UDS_STATUS_OK, "Tick must return OK when not locked");
    }
    zassert_false(ctx.locked_out, "Must remain unlocked");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_uds_security_init__test_null_ctx(void);
extern void test_uds_security_init__test_null_cfg(void);
extern void test_uds_security_init__test_null_key_cb(void);
extern void test_uds_security_init__test_null_seed_cb(void);
extern void test_uds_security_init__test_happy_path(void);
extern void test_uds_security_init__test_double_init(void);
extern void test_uds_security_seed__test_null_ctx(void);
extern void test_uds_security_seed__test_null_seed_buf(void);
extern void test_uds_security_seed__test_seed_generated(void);
extern void test_uds_security_seed__test_already_unlocked_returns_zero_seed(void);
extern void test_uds_security_seed__test_lockout_blocks_seed_request(void);
extern void test_uds_security_send_key__test_null_ctx(void);
extern void test_uds_security_send_key__test_null_key(void);
extern void test_uds_security_send_key__test_key_without_seed(void);
extern void test_uds_security_send_key__test_correct_key_unlocks(void);
extern void test_uds_security_send_key__test_wrong_key_increments_counter(void);
extern void test_uds_security_send_key__test_lockout_after_max_attempts(void);
extern void test_uds_security_is_unlocked__test_initially_locked(void);
extern void test_uds_security_is_unlocked__test_unlocked_after_correct_key(void);
extern void test_uds_security_reset__test_reset_clears_level(void);
extern void test_uds_security_reset__test_reset_preserves_lockout(void);
extern void test_uds_security_tick__test_null_ctx(void);
extern void test_uds_security_tick__test_lockout_expires(void);
extern void test_uds_security_tick__test_no_underflow_when_not_locked(void);

void run_all_tests(void)
{
    RUN_TEST(test_uds_security_init__test_null_ctx);
    RUN_TEST(test_uds_security_init__test_null_cfg);
    RUN_TEST(test_uds_security_init__test_null_key_cb);
    RUN_TEST(test_uds_security_init__test_null_seed_cb);
    RUN_TEST(test_uds_security_init__test_happy_path);
    RUN_TEST(test_uds_security_init__test_double_init);
    RUN_TEST(test_uds_security_seed__test_null_ctx);
    RUN_TEST(test_uds_security_seed__test_null_seed_buf);
    RUN_TEST(test_uds_security_seed__test_seed_generated);
    RUN_TEST(test_uds_security_seed__test_already_unlocked_returns_zero_seed);
    RUN_TEST(test_uds_security_seed__test_lockout_blocks_seed_request);
    RUN_TEST(test_uds_security_send_key__test_null_ctx);
    RUN_TEST(test_uds_security_send_key__test_null_key);
    RUN_TEST(test_uds_security_send_key__test_key_without_seed);
    RUN_TEST(test_uds_security_send_key__test_correct_key_unlocks);
    RUN_TEST(test_uds_security_send_key__test_wrong_key_increments_counter);
    RUN_TEST(test_uds_security_send_key__test_lockout_after_max_attempts);
    RUN_TEST(test_uds_security_is_unlocked__test_initially_locked);
    RUN_TEST(test_uds_security_is_unlocked__test_unlocked_after_correct_key);
    RUN_TEST(test_uds_security_reset__test_reset_clears_level);
    RUN_TEST(test_uds_security_reset__test_reset_preserves_lockout);
    RUN_TEST(test_uds_security_tick__test_null_ctx);
    RUN_TEST(test_uds_security_tick__test_lockout_expires);
    RUN_TEST(test_uds_security_tick__test_no_underflow_when_not_locked);
}
