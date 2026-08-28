// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_trng_fail_closed.c
 *
 * MODULE UNDER TEST: core/uds_security_algo.c (PRODUCTION-configuration
 *                     entropy behaviour), core/uds_security.c (end-to-end).
 *
 * [SEC-TRNG-FAILCLOSED-01]
 *
 * PURPOSE:
 *   Prove the PRODUCTION (fail-closed) entropy behaviour of
 *   core/uds_security_algo.c: when the registered TRNG callback fails (or
 *   none is registered), a production build must refuse the seed request
 *   with UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE instead of silently falling
 *   back to the deterministic software LFSR.
 *
 *   This behaviour only activates when ALGO_ENTROPY_FAIL_CLOSED == 1 in
 *   core/uds_security_algo.c, which cannot be reached from the default
 *   dev-configuration build used by every other test module (see
 *   build_tests.sh). This file is therefore compiled as its own, separate
 *   test binary with -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0, which forces
 *   the production path (see the compile-time guard below).
 *
 *   The mirror DEVELOPMENT-configuration regression guard — proving the
 *   pre-existing LFSR fallback is byte-for-byte unchanged — lives in
 *   tests/unit_runnable/test_phase5_security_algo.c.
 *
 * TEST CASES:
 *   TC-TFC-001  No RNG callback registered -> ERR_SEC_SEED_UNAVAILABLE;
 *               out_seed_len untouched; seed buffer untouched (sentinel
 *               intact); platform_violations +1; last_violation_code ==
 *               ERR_SEC_SEED_UNAVAILABLE.
 *   TC-TFC-002  Registered callback returns ERR_PLATFORM (mid-session
 *               hardware fault) -> same as TC-TFC-001, PLUS
 *               get_trng_fallback_count() == 1 and the seed buffer contains
 *               no LFSR output (sentinel intact, buffer is scrubbed to
 *               zero by the implementation -- not sentinel and not LFSR
 *               garbage).
 *   TC-TFC-003  Registered callback succeeds -> normal seed issued,
 *               UDS_STATUS_OK. Proves fail-closed does not break the good
 *               production path.
 *   TC-TFC-004  End-to-end through uds_security: ctx configured with
 *               seed_generate_cb = uds_security_algo_generate_seed, a
 *               failing TRNG registered, request_seed() call returns
 *               UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE and no seed bytes are
 *               emitted into the caller's buffer.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_security_algo.h"
#include "uds_security.h"
#include "uds_safety.h"
#include "uds_types.h"

/* =============================================================================
 * [SEC-TRNG-FAILCLOSED-01] Compile-time proof this TU is actually built in
 * the production (fail-closed) configuration.
 *
 * ALGO_ENTROPY_FAIL_CLOSED itself is private to uds_security_algo.c, so it
 * cannot be inspected directly from this file. Instead, this re-derives the
 * host-build half of that same condition from CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY
 * (see the full three-case derivation and rationale in
 * core/uds_security_algo.c, near ALGO_ENTROPY_FAIL_CLOSED) and #errors out if
 * it does not hold. Without this guard, a broken build-flag pipeline (e.g.
 * build_tests.sh losing the -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 flag for
 * this test) would silently compile this file in DEVELOPMENT mode instead,
 * and every assertion below would happen to still pass against the LFSR
 * fallback path -- making this test worthless without ever failing.
 * ============================================================================= */
#if defined(CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY)
#  if CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY
#    error "[SEC-TRNG-FAILCLOSED-01] test_trng_fail_closed.c must be built " \
           "with CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 (forces the production " \
           "fail-closed entropy gate), not a non-zero value -- see the " \
           "extra_flags_for_test() entry in build_tests.sh."
#  endif
#else
#  error "[SEC-TRNG-FAILCLOSED-01] test_trng_fail_closed.c must be built " \
         "with -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 to force the " \
         "production fail-closed entropy gate -- see the " \
         "extra_flags_for_test() entry in build_tests.sh."
#endif

ZTEST_SUITE(test_trng_fail_closed, NULL, NULL, NULL, NULL, NULL);

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/** Sentinel byte pattern used to prove a buffer was never touched. */
#define SENTINEL_BYTE ((uint8_t)0x5AU)

static void fill_sentinel(uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        buf[i] = SENTINEL_BYTE;
    }
}

static bool buf_is_sentinel(const uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0U; i < len; i++) {
        if (buf[i] != SENTINEL_BYTE) {
            return false;
        }
    }
    return true;
}

static uds_status_t stub_rng_cb_always_fails(uint8_t *buf, uint8_t len)
{
    (void)buf;
    (void)len;
    return UDS_STATUS_ERR_PLATFORM;
}

static uds_status_t stub_rng_cb_working(uint8_t *buf, uint8_t len)
{
    uint8_t i;
    for (i = 0U; i < len; i++) {
        buf[i] = (uint8_t)(0x11U * (i + 1U));
    }
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * TC-TFC-001: no RNG callback registered -> hard refusal.
 * -------------------------------------------------------------------------- */
ZTEST(test_trng_fail_closed, tc001_no_rng_cb_production_refuses)
{
    uint8_t seed[UDS_ALGO_SEED_LEN];
    uint8_t out_len = SENTINEL_BYTE;
    uds_status_t rc;
    const uds_safety_ctx_t *ctx;

    uds_security_algo_reset();
    (void)uds_safety_init();
    (void)uds_safety_reset_counters();

    ctx = uds_safety_get_ctx();
    zassert_not_null(ctx, "safety ctx must be available");
    zassert_equal(ctx->platform_violations, 0U,
        "platform_violations must start at 0 after reset_counters()");

    fill_sentinel(seed, sizeof(seed));

    rc = uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
        "production build: no TRNG registered must refuse, not fall back to LFSR");
    zassert_equal(out_len, SENTINEL_BYTE, "*out_seed_len must be left untouched");
    zassert_true(buf_is_sentinel(seed, sizeof(seed)),
        "seed buffer must be left untouched -- no seed was ever issued");

    zassert_equal(ctx->platform_violations, 1U,
        "exactly one platform violation must be recorded");
    zassert_equal(ctx->last_violation_code, UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
        "production hard refusal must record SEED_UNAVAILABLE, not PLATFORM");
}

/* --------------------------------------------------------------------------
 * TC-TFC-002: registered callback fails at runtime -> hard refusal, LFSR
 * never runs.
 * -------------------------------------------------------------------------- */
ZTEST(test_trng_fail_closed, tc002_failing_rng_cb_production_refuses)
{
    uint8_t seed[UDS_ALGO_SEED_LEN];
    uint8_t out_len = SENTINEL_BYTE;
    uds_status_t rc;
    const uds_safety_ctx_t *ctx;
    uint32_t fallback_before;

    uds_security_algo_reset();
    (void)uds_safety_init();
    (void)uds_safety_reset_counters();
    uds_security_algo_set_rng_cb(stub_rng_cb_always_fails);

    ctx = uds_safety_get_ctx();
    zassert_not_null(ctx, "safety ctx must be available");
    zassert_equal(ctx->platform_violations, 0U,
        "platform_violations must start at 0 after reset_counters()");
    /*
     * uds_security_algo_get_trng_fallback_count() is a module-global counter
     * outside uds_safety_ctx_t with no reset hook, so it is still asserted
     * on a delta across this call.
     */
    fallback_before = uds_security_algo_get_trng_fallback_count();

    fill_sentinel(seed, sizeof(seed));

    rc = uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
        "production build: failing TRNG must refuse, not fall back to LFSR");
    zassert_equal(out_len, SENTINEL_BYTE, "*out_seed_len must be left untouched");
    /*
     * The caller-visible seed_buf is untouched on this path (only the
     * internal nonce buffer inside generate_seed/algo_get_random is
     * scrubbed -- it never reaches the caller). The sentinel pattern must
     * therefore still be intact, proving no LFSR output (or anything else)
     * was ever written into it.
     */
    zassert_true(buf_is_sentinel(seed, sizeof(seed)),
        "seed buffer must be left untouched -- must contain NO LFSR output");

    zassert_equal(uds_security_algo_get_trng_fallback_count(), fallback_before + 1U,
        "exactly one new TRNG call failure must be counted");
    zassert_equal(ctx->platform_violations, 1U,
        "exactly one platform violation must be recorded");
    zassert_equal(ctx->last_violation_code, UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
        "production hard refusal must record SEED_UNAVAILABLE, not PLATFORM");

    uds_security_algo_set_rng_cb(NULL);
}

/* --------------------------------------------------------------------------
 * TC-TFC-003: working TRNG -> fail-closed does not break the good path.
 * -------------------------------------------------------------------------- */
ZTEST(test_trng_fail_closed, tc003_working_rng_cb_production_ok)
{
    uint8_t seed[UDS_ALGO_SEED_LEN];
    uint8_t out_len = 0U;
    uds_status_t rc;

    uds_security_algo_reset();
    uds_security_algo_set_rng_cb(stub_rng_cb_working);

    rc = uds_security_algo_generate_seed(0x01U, seed, sizeof(seed), &out_len);
    zassert_equal(rc, UDS_STATUS_OK, "working TRNG must still succeed in production build");
    zassert_equal(out_len, (uint8_t)UDS_ALGO_SEED_LEN, "seed length must be full UDS_ALGO_SEED_LEN");
    zassert_equal(seed[UDS_ALGO_SEED_NONCE_OFFSET], (uint8_t)0x11U,
        "nonce must come from the working TRNG stub");

    uds_security_algo_set_rng_cb(NULL);
}

/* --------------------------------------------------------------------------
 * TC-TFC-004: end-to-end through uds_security -- request_seed() propagates
 * the production refusal and never emits seed bytes.
 * -------------------------------------------------------------------------- */
ZTEST(test_trng_fail_closed, tc004_end_to_end_uds_security_refuses)
{
    uds_security_ctx_t ctx;
    uint8_t seed[UDS_SECURITY_SEED_LEN];
    uint8_t out_len = SENTINEL_BYTE;
    uds_status_t rc;
    static const uds_security_cfg_t k_cfg = {
        .max_attempts     = 3U,
        .lockout_ms       = 10000U,
        .key_validate_cb  = uds_security_algo_validate_key,
        .seed_generate_cb = uds_security_algo_generate_seed,
    };

    (void)memset(&ctx, 0, sizeof(ctx));
    uds_security_algo_reset();
    uds_security_algo_set_rng_cb(stub_rng_cb_always_fails);

    rc = uds_security_init(&ctx, &k_cfg);
    zassert_equal(rc, UDS_STATUS_OK, "security ctx init must succeed");

    fill_sentinel(seed, sizeof(seed));

    rc = uds_security_request_seed(&ctx, UDS_SEC_LEVEL_1_SEED, seed,
                                    (uint8_t)UDS_SECURITY_SEED_LEN, &out_len);
    zassert_equal(rc, UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
        "end-to-end: production TRNG failure must surface as SEED_UNAVAILABLE");
    zassert_equal(out_len, SENTINEL_BYTE, "*out_seed_len must be left untouched");
    zassert_true(buf_is_sentinel(seed, sizeof(seed)),
        "no seed bytes may ever reach the caller's buffer on this path");
    zassert_false(ctx.seed_pending, "seed_pending must not be left set on refusal");

    uds_security_algo_set_rng_cb(NULL);
}

/* run_all_tests shim */
extern void test_trng_fail_closed__tc001_no_rng_cb_production_refuses(void);
extern void test_trng_fail_closed__tc002_failing_rng_cb_production_refuses(void);
extern void test_trng_fail_closed__tc003_working_rng_cb_production_ok(void);
extern void test_trng_fail_closed__tc004_end_to_end_uds_security_refuses(void);

void run_all_tests(void)
{
    RUN_TEST(test_trng_fail_closed__tc001_no_rng_cb_production_refuses);
    RUN_TEST(test_trng_fail_closed__tc002_failing_rng_cb_production_refuses);
    RUN_TEST(test_trng_fail_closed__tc003_working_rng_cb_production_ok);
    RUN_TEST(test_trng_fail_closed__tc004_end_to_end_uds_security_refuses);
}
