// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * TEST SUITE: test_phase3_nvm_security.c
 *
 * PURPOSE: Verify that the security failed-attempt counter and lockout timer
 *          persist across simulated power-cycles using the NVM store mock.
 *
 * COVERS:
 *   [P3-SEC-01] uds_security_nvm_load / uds_security_nvm_save callbacks
 *   [P3-SEC-01] Counter restored in uds_security_init() from NVM
 *   [P3-SEC-01] Counter incremented eagerly on each failed attempt
 *   [P3-SEC-01] Lockout timer residual persisted on lockout engage
 *   [P3-SEC-01] Lockout active on reboot when residual > 0
 *   [P3-SEC-01] Counter cleared in NVM on successful unlock
 *   [P3-SEC-01] Counter clamped to max_attempts on corrupted NVM
 *   nvm_store_mock: write / read / erase / deinit cycle
 *
 * TEST COUNT: 14
 * =============================================================================
 */

#include "unity.h"
#include "uds_security.h"
#include "uds_security_nvm.h"
#include "nvm_store.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Declarations for test-internal mock helpers (defined in nvm_store_mock.c)
 * -------------------------------------------------------------------------- */
extern void nvm_mock_reset(void);
extern void nvm_mock_deinit(void);

/* --------------------------------------------------------------------------
 * Minimal key-validation and seed-generation stubs
 * -------------------------------------------------------------------------- */

static bool s_key_valid = true;   /* controlled by tests */

static bool stub_key_validate(
    uint8_t        level,
    const uint8_t *seed,
    uint8_t        seed_len,
    const uint8_t *key,
    uint8_t        key_len)
{
    (void)level; (void)seed; (void)seed_len; (void)key; (void)key_len;
    return s_key_valid;
}

static uds_status_t stub_seed_generate(
    uint8_t  level,
    uint8_t *seed_buf,
    uint8_t  seed_buf_len,
    uint8_t *out_seed_len)
{
    /* [P1-SEC FIX] fill UDS_SECURITY_SEED_LEN (8) bytes */
    (void)level;
    if (seed_buf_len < (uint8_t)UDS_SECURITY_SEED_LEN) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }
    seed_buf[0] = 0xAAU; seed_buf[1] = 0xBBU;
    seed_buf[2] = 0xCCU; seed_buf[3] = 0xDDU;
    seed_buf[4] = 0x11U; seed_buf[5] = 0x22U;
    seed_buf[6] = 0x33U; seed_buf[7] = 0x44U;
    *out_seed_len = (uint8_t)UDS_SECURITY_SEED_LEN;
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Helper: build a security config with NVM callbacks wired
 * -------------------------------------------------------------------------- */
static uds_security_cfg_t make_cfg(void)
{
    uds_security_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_attempts     = 3U;
    cfg.lockout_ms       = 10000U;
    cfg.key_validate_cb  = stub_key_validate;
    cfg.seed_generate_cb = stub_seed_generate;
    cfg.nvm_load_cb      = uds_security_nvm_load;
    cfg.nvm_save_cb      = uds_security_nvm_save;
    return cfg;
}

/* Helper: do one seed+key cycle and return the send_key status */
static uds_status_t do_attempt(uds_security_ctx_t *ctx, bool key_valid)
{
    /* [P1-SEC FIX] seed = 8 bytes, key = 4 bytes (KEY_LEN) */
    uint8_t seed[UDS_SECURITY_SEED_LEN];
    uint8_t seed_len;
    uint8_t key[UDS_SECURITY_KEY_LEN] = {0x01U, 0x02U, 0x03U, 0x04U};
    s_key_valid = key_valid;
    uds_security_request_seed(ctx, 0x01U, seed, sizeof(seed), &seed_len);
    return uds_security_send_key(ctx, 0x02U, key, (uint8_t)UDS_SECURITY_KEY_LEN);
}

/* --------------------------------------------------------------------------
 * setUp / tearDown
 * -------------------------------------------------------------------------- */

void setUp(void)
{
    /* Full RAM-mock reset before every test. */
    nvm_mock_reset();
    /* Re-initialize fresh NVM store. */
    nvm_store_init(NULL);
    s_key_valid = true;
}

void tearDown(void) {}

/* --------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------- */

/* 1. First boot — no NVM data — counter starts at 0 */
void test_first_boot_counter_zero(void)
{
    uds_security_ctx_t ctx;
    uds_security_cfg_t cfg = make_cfg();
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_security_init(&ctx, &cfg));
    TEST_ASSERT_EQUAL_UINT8(0U, ctx.failed_attempts);
    TEST_ASSERT_FALSE(ctx.locked_out);
}

/* 2. Failed attempt counter is written to NVM immediately */
void test_failed_attempt_persisted_eagerly(void)
{
    uds_security_ctx_t ctx;
    uds_security_cfg_t cfg = make_cfg();
    memset(&ctx, 0, sizeof(ctx));
    uds_security_init(&ctx, &cfg);

    /* One failed attempt */
    do_attempt(&ctx, false);
    TEST_ASSERT_EQUAL_UINT8(1U, ctx.failed_attempts);

    /* Verify NVM holds the counter */
    uint8_t  nvm_attempts = 0U;
    uint32_t nvm_lockout  = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_security_nvm_load(&nvm_attempts, &nvm_lockout));
    TEST_ASSERT_EQUAL_UINT8(1U, nvm_attempts);
    TEST_ASSERT_EQUAL_UINT32(0U, nvm_lockout);
}

/* 3. Counter restored correctly after simulated power-cycle */
void test_counter_restored_across_power_cycle(void)
{
    /* Power-on #1: accumulate 2 failed attempts */
    {
        uds_security_ctx_t ctx;
        uds_security_cfg_t cfg = make_cfg();
        memset(&ctx, 0, sizeof(ctx));
        uds_security_init(&ctx, &cfg);
        do_attempt(&ctx, false);
        do_attempt(&ctx, false);
        TEST_ASSERT_EQUAL_UINT8(2U, ctx.failed_attempts);
    }

    /* Simulate power-cycle: deinit NVM (keeps records), re-init driver */
    nvm_mock_deinit();
    nvm_store_init(NULL);

    /* Power-on #2: counter should be restored to 2 */
    {
        uds_security_ctx_t ctx;
        uds_security_cfg_t cfg = make_cfg();
        memset(&ctx, 0, sizeof(ctx));
        TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_security_init(&ctx, &cfg));
        TEST_ASSERT_EQUAL_UINT8(2U, ctx.failed_attempts);
        TEST_ASSERT_FALSE(ctx.locked_out);
    }
}

/* 4. Lockout engages on 3rd consecutive fail */
void test_lockout_on_third_failure(void)
{
    uds_security_ctx_t ctx;
    uds_security_cfg_t cfg = make_cfg();
    memset(&ctx, 0, sizeof(ctx));
    uds_security_init(&ctx, &cfg);

    do_attempt(&ctx, false);
    do_attempt(&ctx, false);
    uds_status_t rc = do_attempt(&ctx, false);

    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED, rc);
    TEST_ASSERT_TRUE(ctx.locked_out);
}

/* 5. Lockout timer residual is persisted to NVM on lockout */
void test_lockout_timer_persisted(void)
{
    uds_security_ctx_t ctx;
    uds_security_cfg_t cfg = make_cfg();
    memset(&ctx, 0, sizeof(ctx));
    uds_security_init(&ctx, &cfg);

    do_attempt(&ctx, false);
    do_attempt(&ctx, false);
    do_attempt(&ctx, false);  /* triggers lockout */

    TEST_ASSERT_TRUE(ctx.locked_out);

    uint8_t  nvm_attempts = 0U;
    uint32_t nvm_lockout  = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_security_nvm_load(&nvm_attempts, &nvm_lockout));
    /* Attempts reset to 0 when lockout engages; lockout_ms = configured duration */
    TEST_ASSERT_EQUAL_UINT8(0U, nvm_attempts);
    TEST_ASSERT_EQUAL_UINT32(10000U, nvm_lockout);
}

/* 6. Lockout is active on reboot when residual > 0 in NVM */
void test_lockout_active_after_power_cycle(void)
{
    /* Power-on #1: trigger lockout */
    {
        uds_security_ctx_t ctx;
        uds_security_cfg_t cfg = make_cfg();
        memset(&ctx, 0, sizeof(ctx));
        uds_security_init(&ctx, &cfg);
        do_attempt(&ctx, false);
        do_attempt(&ctx, false);
        do_attempt(&ctx, false);
        TEST_ASSERT_TRUE(ctx.locked_out);
    }

    /* Power-cycle */
    nvm_mock_deinit();
    nvm_store_init(NULL);

    /* Power-on #2: must still be locked out */
    {
        uds_security_ctx_t ctx;
        uds_security_cfg_t cfg = make_cfg();
        memset(&ctx, 0, sizeof(ctx));
        uds_security_init(&ctx, &cfg);

        TEST_ASSERT_TRUE(ctx.locked_out);
        TEST_ASSERT_EQUAL_UINT32(10000U, ctx.lockout_timer_ms);

        /* Seed request must be rejected while locked out */
        uint8_t seed[UDS_SECURITY_SEED_LEN]; uint8_t seed_len;
        uds_status_t rc = uds_security_request_seed(
            &ctx, 0x01U, seed, sizeof(seed), &seed_len);
        TEST_ASSERT_EQUAL(UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED, rc);
    }
}

/* 7. Successful unlock clears the counter in NVM */
void test_successful_unlock_clears_nvm_counter(void)
{
    uds_security_ctx_t ctx;
    uds_security_cfg_t cfg = make_cfg();
    memset(&ctx, 0, sizeof(ctx));
    uds_security_init(&ctx, &cfg);

    do_attempt(&ctx, false);  /* fail: counter = 1 in NVM */
    do_attempt(&ctx, true);   /* success: counter should be 0 in NVM */

    TEST_ASSERT_EQUAL_UINT8(0U, ctx.failed_attempts);

    uint8_t  nvm_attempts = 99U;
    uint32_t nvm_lockout  = 99U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_security_nvm_load(&nvm_attempts, &nvm_lockout));
    TEST_ASSERT_EQUAL_UINT8(0U, nvm_attempts);
    TEST_ASSERT_EQUAL_UINT32(0U, nvm_lockout);
}

/* 8. Counter clamped to max_attempts if NVM holds corrupt value */
void test_corrupt_nvm_counter_clamped_to_max(void)
{
    /* Write an absurdly large counter to NVM directly */
    uint8_t corrupt = 200U;
    nvm_store_write(NVM_KEY_SEC_ATTEMPT_CTR, &corrupt, sizeof(corrupt));

    uds_security_ctx_t ctx;
    uds_security_cfg_t cfg = make_cfg();
    memset(&ctx, 0, sizeof(ctx));
    uds_security_init(&ctx, &cfg);

    /* Must be clamped to max_attempts (3) not 200 */
    TEST_ASSERT_EQUAL_UINT8(cfg.max_attempts, ctx.failed_attempts);
}

/* 9. Without NVM callbacks, counter starts zero every boot (legacy behavior) */
void test_no_nvm_callbacks_counter_zero_each_boot(void)
{
    uds_security_cfg_t cfg = make_cfg();
    cfg.nvm_load_cb = NULL;
    cfg.nvm_save_cb = NULL;

    /* Power-on #1: fail twice */
    {
        uds_security_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
        uds_security_init(&ctx, &cfg);
        do_attempt(&ctx, false);
        do_attempt(&ctx, false);
    }

    nvm_mock_deinit();
    nvm_store_init(NULL);

    /* Power-on #2: counter must be 0 (not 2) because no NVM callbacks */
    {
        uds_security_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
        uds_security_init(&ctx, &cfg);
        TEST_ASSERT_EQUAL_UINT8(0U, ctx.failed_attempts);
        TEST_ASSERT_FALSE(ctx.locked_out);
    }
}

/* 10. uds_security_nvm_clear removes both NVM keys */
void test_nvm_clear_removes_keys(void)
{
    /* Write some data first */
    uint8_t attempts = 2U;
    uint32_t lockout = 5000U;
    nvm_store_write(NVM_KEY_SEC_ATTEMPT_CTR, &attempts, sizeof(attempts));
    nvm_store_write(NVM_KEY_SEC_LOCKOUT_MS,  &lockout,  sizeof(lockout));

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_security_nvm_clear());

    /* Both records should now be absent */
    uint8_t  out_a = 0U;
    uint32_t out_l = 0U;
    uds_status_t rc = uds_security_nvm_load(&out_a, &out_l);
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_DID_NOT_FOUND, rc);
}

/* 11. nvm_store_write then deinit then re-init then read — data survives */
void test_nvm_mock_data_survives_deinit_reinit(void)
{
    uint32_t val = 0xDEADBEEFUL;
    nvm_store_write(NVM_KEY_LIFECYCLE_CNT, &val, sizeof(val));

    nvm_mock_deinit();
    nvm_store_init(NULL);

    uint32_t readback = 0U;
    size_t   len      = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK,
        nvm_store_read(NVM_KEY_LIFECYCLE_CNT, &readback, sizeof(readback), &len));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFUL, readback);
    TEST_ASSERT_EQUAL_INT((int)sizeof(uint32_t),(int) len);
}

/* 12. nvm_store_erase_all wipes all records */
void test_nvm_erase_all_wipes_records(void)
{
    uint8_t  a = 2U;
    uint32_t l = 9999U;
    nvm_store_write(NVM_KEY_SEC_ATTEMPT_CTR, &a, sizeof(a));
    nvm_store_write(NVM_KEY_SEC_LOCKOUT_MS,  &l, sizeof(l));

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, nvm_store_erase_all());

    uint8_t readback = 0U;
    uds_status_t rc = nvm_store_read(NVM_KEY_SEC_ATTEMPT_CTR, &readback, sizeof(readback), NULL);
    TEST_ASSERT_EQUAL(UDS_STATUS_ERR_DID_NOT_FOUND, rc);
}

/* 13. nvm_store_delete is idempotent (deleting non-existent key = OK) */
void test_nvm_delete_idempotent(void)
{
    /* Delete a key that was never written */
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, nvm_store_delete(NVM_KEY_SEC_ATTEMPT_CTR));
    /* Delete again — must still be OK */
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, nvm_store_delete(NVM_KEY_SEC_ATTEMPT_CTR));
}

/* 14. Lockout timer ticks down and restores unlocked state; NVM not re-read */
void test_lockout_expires_after_tick(void)
{
    uds_security_ctx_t ctx;
    uds_security_cfg_t cfg = make_cfg();
    cfg.lockout_ms = 3U;  /* very short lockout for test */
    memset(&ctx, 0, sizeof(ctx));
    uds_security_init(&ctx, &cfg);

    do_attempt(&ctx, false);
    do_attempt(&ctx, false);
    do_attempt(&ctx, false);  /* lockout with 3 ms residual */
    TEST_ASSERT_TRUE(ctx.locked_out);

    /* Tick down exactly 3 ms */
    uds_security_tick_1ms(&ctx);
    uds_security_tick_1ms(&ctx);
    uds_security_tick_1ms(&ctx);

    TEST_ASSERT_FALSE(ctx.locked_out);

    /* Should now be able to request a seed again */
    uint8_t seed[UDS_SECURITY_SEED_LEN]; uint8_t seed_len;
    uds_status_t rc = uds_security_request_seed(
        &ctx, 0x01U, seed, sizeof(seed), &seed_len);
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, rc);
}

/* --------------------------------------------------------------------------
 * Test runner
 * -------------------------------------------------------------------------- */
void run_all_tests(void)
{
    RUN_TEST(test_first_boot_counter_zero);
    RUN_TEST(test_failed_attempt_persisted_eagerly);
    RUN_TEST(test_counter_restored_across_power_cycle);
    RUN_TEST(test_lockout_on_third_failure);
    RUN_TEST(test_lockout_timer_persisted);
    RUN_TEST(test_lockout_active_after_power_cycle);
    RUN_TEST(test_successful_unlock_clears_nvm_counter);
    RUN_TEST(test_corrupt_nvm_counter_clamped_to_max);
    RUN_TEST(test_no_nvm_callbacks_counter_zero_each_boot);
    RUN_TEST(test_nvm_clear_removes_keys);
    RUN_TEST(test_nvm_mock_data_survives_deinit_reinit);
    RUN_TEST(test_nvm_erase_all_wipes_records);
    RUN_TEST(test_nvm_delete_idempotent);
    RUN_TEST(test_lockout_expires_after_tick);
}
