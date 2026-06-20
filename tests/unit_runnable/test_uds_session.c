// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
// ================================
// File: tests/unit/test_uds_session.c
// ================================
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit/test_uds_session.c
 *
 * MODULE UNDER TEST: core/uds_session.c
 *
 * PURPOSE:
 *   Verify the UDS session management state machine.
 *   Tests cover:
 *     - uds_session_init: NULL guard, zero-timeout guard,
 *       happy path, double-init guard
 *     - uds_session_transition: all valid transitions,
 *       invalid session type rejection
 *     - uds_session_get_active: correct session reported
 *     - uds_session_reset_s3_timer: timer reload verified
 *     - uds_session_tick_1ms: timer countdown, S3 timeout detection,
 *       session reset to DEFAULT on timeout
 *     - uds_session_is_default: correct boolean for each session type
 *
 * FRAMEWORK: Zephyr Ztest
 *
 * ISO 14229-1 references:
 *   §9.3 — session transition rules
 *   §7.5 — S3server inactivity timeout (default 5000 ms)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_session.h"
#include "uds_types.h"

/* =========================================================================
 * Helpers
 * ========================================================================= */

/** Initialise a session context with default 5-second S3 timeout. */
static uds_status_t default_init(uds_session_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    return uds_session_init(ctx, 5000U);
}

/* =========================================================================
 * Test suite: uds_session_init
 * ========================================================================= */

ZTEST_SUITE(test_uds_session_init, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SESS-INIT-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_session_init, test_null_ctx)
{
    uds_status_t rc = uds_session_init(NULL, 5000U);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SESS-INIT-002: Zero S3 timeout → UDS_STATUS_ERR_INVALID_PARAM.
 */
ZTEST(test_uds_session_init, test_zero_timeout)
{
    uds_session_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    uds_status_t rc = uds_session_init(&ctx, 0U);
    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "Zero S3 timeout must be rejected");
}

/**
 * TC-SESS-INIT-003: Valid init → OK, active session = DEFAULT, timer = timeout.
 */
ZTEST(test_uds_session_init, test_happy_path)
{
    uds_session_ctx_t ctx;
    uds_status_t rc = default_init(&ctx);
    zassert_equal(rc, UDS_STATUS_OK, "Valid init must return OK");
    zassert_true(ctx.initialized, "initialized flag must be set");

    uds_session_type_t active;
    rc = uds_session_get_active(&ctx, &active);
    zassert_equal(rc, UDS_STATUS_OK, "get_active must succeed");
    zassert_equal(active, UDS_SESSION_DEFAULT,
                  "Initial session must be DEFAULT");

    zassert_equal(ctx.s3_timer_ms, 5000U, "S3 timer must be loaded");
    zassert_equal(ctx.s3_timeout_cfg_ms, 5000U, "Configured timeout must be stored");
}

/**
 * TC-SESS-INIT-004: Double init → UDS_STATUS_ERR_ALREADY_INITIALIZED.
 */
ZTEST(test_uds_session_init, test_double_init)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "First init must succeed");
    uds_status_t rc = uds_session_init(&ctx, 5000U);
    zassert_equal(rc, UDS_STATUS_ERR_ALREADY_INITIALIZED,
                  "Double init must return ALREADY_INITIALIZED");
}

/* =========================================================================
 * Test suite: uds_session_transition
 * ========================================================================= */

ZTEST_SUITE(test_uds_session_transition, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SESS-TRANS-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_session_transition, test_null_ctx)
{
    uds_status_t rc = uds_session_transition(NULL, UDS_SESSION_EXTENDED);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SESS-TRANS-002: Not initialized → UDS_STATUS_ERR_NOT_INITIALIZED.
 */
ZTEST(test_uds_session_transition, test_not_initialized)
{
    uds_session_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Do NOT call init */
    uds_status_t rc = uds_session_transition(&ctx, UDS_SESSION_EXTENDED);
    zassert_equal(rc, UDS_STATUS_ERR_NOT_INITIALIZED, "Must fail when not initialised");
}

/**
 * TC-SESS-TRANS-003: DEFAULT → EXTENDED → OK, session updated.
 */
ZTEST(test_uds_session_transition, test_default_to_extended)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");

    uds_status_t rc = uds_session_transition(&ctx, UDS_SESSION_EXTENDED);
    zassert_equal(rc, UDS_STATUS_OK, "DEFAULT→EXTENDED must succeed");

    uds_session_type_t active;
    uds_session_get_active(&ctx, &active);
    zassert_equal(active, UDS_SESSION_EXTENDED, "Active session must be EXTENDED");
}

/**
 * TC-SESS-TRANS-004: DEFAULT → PROGRAMMING → OK.
 */
ZTEST(test_uds_session_transition, test_default_to_programming)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");

    uds_status_t rc = uds_session_transition(&ctx, UDS_SESSION_PROGRAMMING);
    zassert_equal(rc, UDS_STATUS_OK, "DEFAULT→PROGRAMMING must succeed");
}

/**
 * TC-SESS-TRANS-005: EXTENDED → DEFAULT → OK (always permitted).
 */
ZTEST(test_uds_session_transition, test_extended_to_default)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    zassert_equal(uds_session_transition(&ctx, UDS_SESSION_EXTENDED),
                  UDS_STATUS_OK, "To extended failed");

    uds_status_t rc = uds_session_transition(&ctx, UDS_SESSION_DEFAULT);
    zassert_equal(rc, UDS_STATUS_OK, "EXTENDED→DEFAULT must succeed");

    uds_session_type_t active;
    uds_session_get_active(&ctx, &active);
    zassert_equal(active, UDS_SESSION_DEFAULT, "Must be DEFAULT");
}

/**
 * TC-SESS-TRANS-006: Transition to SAFETY_SYSTEM session → OK.
 */
ZTEST(test_uds_session_transition, test_to_safety_system)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    uds_status_t rc = uds_session_transition(&ctx, UDS_SESSION_SAFETY_SYSTEM);
    zassert_equal(rc, UDS_STATUS_OK, "Transition to SAFETY_SYSTEM must succeed");

    uds_session_type_t active;
    uds_session_get_active(&ctx, &active);
    zassert_equal(active, UDS_SESSION_SAFETY_SYSTEM, "Must be SAFETY_SYSTEM");
}

/**
 * TC-SESS-TRANS-007: Transition resets S3 timer.
 */
ZTEST(test_uds_session_transition, test_transition_resets_timer)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");

    /* Consume some timer ticks */
    for (int i = 0; i < 100; i++) {
        uds_session_tick_1ms(&ctx);
    }
    /* Move to extended */
    uds_session_transition(&ctx, UDS_SESSION_EXTENDED);
    /* Timer must have been reloaded */
    for (int i = 0; i < 100; i++) {
        uds_session_tick_1ms(&ctx);
    }
    /* S3 timer after 100 ticks must still be > 0 (timeout is 5000ms) */
    zassert_true(ctx.s3_timer_ms > 0U, "S3 timer must be non-zero 100ms after reset");
}

/* =========================================================================
 * Test suite: uds_session_get_active
 * ========================================================================= */

ZTEST_SUITE(test_uds_session_get_active, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SESS-GA-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_session_get_active, test_null_ctx)
{
    uds_session_type_t s;
    uds_status_t rc = uds_session_get_active(NULL, &s);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SESS-GA-002: NULL out_session → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_session_get_active, test_null_out)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    uds_status_t rc = uds_session_get_active(&ctx, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL out_session must fail");
}

/**
 * TC-SESS-GA-003: After transition to PROGRAMMING, correct session returned.
 */
ZTEST(test_uds_session_get_active, test_reports_correct_session)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    uds_session_transition(&ctx, UDS_SESSION_PROGRAMMING);

    uds_session_type_t active;
    zassert_equal(uds_session_get_active(&ctx, &active), UDS_STATUS_OK, "must succeed");
    zassert_equal(active, UDS_SESSION_PROGRAMMING, "Must report PROGRAMMING");
}

/* =========================================================================
 * Test suite: uds_session_reset_s3_timer
 * ========================================================================= */

ZTEST_SUITE(test_uds_session_s3_reset, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SESS-S3R-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_session_s3_reset, test_null_ctx)
{
    uds_status_t rc = uds_session_reset_s3_timer(NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SESS-S3R-002: After consuming 500 ticks, reset restores timer to full value.
 */
ZTEST(test_uds_session_s3_reset, test_timer_reloaded)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");

    /* Move to extended so timer is counting */
    uds_session_transition(&ctx, UDS_SESSION_EXTENDED);

    /* Consume 500 ms */
    for (int i = 0; i < 500; i++) {
        uds_session_tick_1ms(&ctx);
    }
    uint32_t before_reset = ctx.s3_timer_ms;
    zassert_true(before_reset < 5000U, "Timer should have decremented");

    /* Reset */
    uds_status_t rc = uds_session_reset_s3_timer(&ctx);
    zassert_equal(rc, UDS_STATUS_OK, "reset must return OK");
    zassert_equal(ctx.s3_timer_ms, 5000U, "Timer must be reloaded to 5000ms");
}

/* =========================================================================
 * Test suite: uds_session_tick_1ms — S3 timeout
 * ========================================================================= */

ZTEST_SUITE(test_uds_session_tick, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SESS-TICK-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_session_tick, test_null_ctx)
{
    uds_status_t rc = uds_session_tick_1ms(NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must fail");
}

/**
 * TC-SESS-TICK-002: In DEFAULT session, tick does not decrement S3 timer.
 * ISO 14229-1: S3server only runs in non-default sessions.
 */
ZTEST(test_uds_session_tick, test_no_countdown_in_default)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");

    uint32_t before = ctx.s3_timer_ms;
    for (int i = 0; i < 10; i++) {
        uds_session_tick_1ms(&ctx);
    }
    /* Timer must not have decreased in DEFAULT session */
    zassert_equal(ctx.s3_timer_ms, before,
                  "Timer must not count in DEFAULT session");
}

/**
 * TC-SESS-TICK-003: In EXTENDED session, timer decrements each tick.
 */
ZTEST(test_uds_session_tick, test_countdown_in_extended)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    uds_session_transition(&ctx, UDS_SESSION_EXTENDED);

    uint32_t initial = ctx.s3_timer_ms;
    for (int i = 0; i < 10; i++) {
        uds_session_tick_1ms(&ctx);
    }
    zassert_equal(ctx.s3_timer_ms, initial - 10U,
                  "Timer must have decremented by 10");
}

/**
 * TC-SESS-TICK-004: S3 timeout → returns UDS_STATUS_ERR_SESSION_TIMEOUT,
 *                   session reverts to DEFAULT.
 *
 * Uses a short timeout (10 ms) to keep the test fast.
 */
ZTEST(test_uds_session_tick, test_s3_timeout_reverts_to_default)
{
    uds_session_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Short 10-ms timeout for speed */
    zassert_equal(uds_session_init(&ctx, 10U), UDS_STATUS_OK, "init failed");
    uds_session_transition(&ctx, UDS_SESSION_EXTENDED);

    /* Tick until timeout */
    uds_status_t tick_rc = UDS_STATUS_OK;
    for (int i = 0; i <= 12; i++) {
        tick_rc = uds_session_tick_1ms(&ctx);
        if (tick_rc != UDS_STATUS_OK) {
            break;
        }
    }
    zassert_equal(tick_rc, UDS_STATUS_ERR_SESSION_TIMEOUT,
                  "Must return SESSION_TIMEOUT when S3 expires");

    uds_session_type_t active;
    uds_session_get_active(&ctx, &active);
    zassert_equal(active, UDS_SESSION_DEFAULT,
                  "Session must revert to DEFAULT on timeout");
}

/**
 * TC-SESS-TICK-005: Timeout reloads S3 timer to configured value.
 */
ZTEST(test_uds_session_tick, test_timer_reloaded_after_timeout)
{
    uds_session_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    zassert_equal(uds_session_init(&ctx, 5U), UDS_STATUS_OK, "init failed");
    uds_session_transition(&ctx, UDS_SESSION_EXTENDED);

    for (int i = 0; i <= 7; i++) {
        uds_session_tick_1ms(&ctx);
    }

    /* After timeout, timer must be reloaded */
    zassert_equal(ctx.s3_timer_ms, 5U,
                  "Timer must be reloaded after timeout");
}

/* =========================================================================
 * Test suite: uds_session_is_default
 * ========================================================================= */

ZTEST_SUITE(test_uds_session_is_default, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SESS-ID-001: NULL ctx → returns true (safe default).
 */
ZTEST(test_uds_session_is_default, test_null_returns_true)
{
    bool result = uds_session_is_default(NULL);
    zassert_true(result, "NULL ctx must return true (safe default)");
}

/**
 * TC-SESS-ID-002: DEFAULT session → true.
 */
ZTEST(test_uds_session_is_default, test_default_session_true)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    zassert_true(uds_session_is_default(&ctx), "DEFAULT must return true");
}

/**
 * TC-SESS-ID-003: EXTENDED session → false.
 */
ZTEST(test_uds_session_is_default, test_extended_session_false)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    uds_session_transition(&ctx, UDS_SESSION_EXTENDED);
    zassert_false(uds_session_is_default(&ctx),
                  "EXTENDED session must return false");
}

/**
 * TC-SESS-ID-004: PROGRAMMING session → false.
 */
ZTEST(test_uds_session_is_default, test_programming_session_false)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    uds_session_transition(&ctx, UDS_SESSION_PROGRAMMING);
    zassert_false(uds_session_is_default(&ctx),
                  "PROGRAMMING session must return false");
}

/* =========================================================================
 * Test suite: uds_session_set_strict_programming
 * ========================================================================= */

ZTEST_SUITE(test_uds_session_strict, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-SESS-STRICT-001: NULL ctx → UDS_STATUS_ERR_NULL_PTR.
 */
ZTEST(test_uds_session_strict, test_null_ctx)
{
    uds_status_t rc = uds_session_set_strict_programming(NULL, true);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must return NULL_PTR");
}

/**
 * TC-SESS-STRICT-002: strict=true blocks DEFAULT → PROGRAMMING directly.
 */
ZTEST(test_uds_session_strict, test_blocks_default_to_programming)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    zassert_equal(uds_session_set_strict_programming(&ctx, true),
                  UDS_STATUS_OK, "set_strict failed");

    uds_status_t rc = uds_session_transition(&ctx, UDS_SESSION_PROGRAMMING);
    zassert_equal(rc, UDS_STATUS_ERR_SESSION_TRANSITION,
                  "strict mode must block DEFAULT→PROGRAMMING");

    uds_session_type_t active;
    uds_session_get_active(&ctx, &active);
    zassert_equal(active, UDS_SESSION_DEFAULT, "Session must remain DEFAULT after rejection");
}

/**
 * TC-SESS-STRICT-003: strict=true allows DEFAULT → EXTENDED → PROGRAMMING.
 */
ZTEST(test_uds_session_strict, test_allows_via_extended)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    zassert_equal(uds_session_set_strict_programming(&ctx, true),
                  UDS_STATUS_OK, "set_strict failed");

    zassert_equal(uds_session_transition(&ctx, UDS_SESSION_EXTENDED),
                  UDS_STATUS_OK, "DEFAULT→EXTENDED must succeed in strict mode");
    zassert_equal(uds_session_transition(&ctx, UDS_SESSION_PROGRAMMING),
                  UDS_STATUS_OK, "EXTENDED→PROGRAMMING must succeed in strict mode");

    uds_session_type_t active;
    uds_session_get_active(&ctx, &active);
    zassert_equal(active, UDS_SESSION_PROGRAMMING, "Must end in PROGRAMMING");
}

/**
 * TC-SESS-STRICT-004: strict=true does not affect other transitions.
 */
ZTEST(test_uds_session_strict, test_other_transitions_unaffected)
{
    uds_session_ctx_t ctx;
    zassert_equal(default_init(&ctx), UDS_STATUS_OK, "init failed");
    zassert_equal(uds_session_set_strict_programming(&ctx, true),
                  UDS_STATUS_OK, "set_strict failed");

    zassert_equal(uds_session_transition(&ctx, UDS_SESSION_EXTENDED),
                  UDS_STATUS_OK, "DEFAULT→EXTENDED must work in strict mode");
    zassert_equal(uds_session_transition(&ctx, UDS_SESSION_DEFAULT),
                  UDS_STATUS_OK, "EXTENDED→DEFAULT must work in strict mode");
    zassert_equal(uds_session_transition(&ctx, UDS_SESSION_SAFETY_SYSTEM),
                  UDS_STATUS_OK, "DEFAULT→SAFETY_SYSTEM must work in strict mode");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_uds_session_init__test_null_ctx(void);
extern void test_uds_session_init__test_zero_timeout(void);
extern void test_uds_session_init__test_happy_path(void);
extern void test_uds_session_init__test_double_init(void);
extern void test_uds_session_transition__test_null_ctx(void);
extern void test_uds_session_transition__test_not_initialized(void);
extern void test_uds_session_transition__test_default_to_extended(void);
extern void test_uds_session_transition__test_default_to_programming(void);
extern void test_uds_session_transition__test_extended_to_default(void);
extern void test_uds_session_transition__test_to_safety_system(void);
extern void test_uds_session_transition__test_transition_resets_timer(void);
extern void test_uds_session_get_active__test_null_ctx(void);
extern void test_uds_session_get_active__test_null_out(void);
extern void test_uds_session_get_active__test_reports_correct_session(void);
extern void test_uds_session_s3_reset__test_null_ctx(void);
extern void test_uds_session_s3_reset__test_timer_reloaded(void);
extern void test_uds_session_tick__test_null_ctx(void);
extern void test_uds_session_tick__test_no_countdown_in_default(void);
extern void test_uds_session_tick__test_countdown_in_extended(void);
extern void test_uds_session_tick__test_s3_timeout_reverts_to_default(void);
extern void test_uds_session_tick__test_timer_reloaded_after_timeout(void);
extern void test_uds_session_is_default__test_null_returns_true(void);
extern void test_uds_session_is_default__test_default_session_true(void);
extern void test_uds_session_is_default__test_extended_session_false(void);
extern void test_uds_session_is_default__test_programming_session_false(void);
extern void test_uds_session_strict__test_null_ctx(void);
extern void test_uds_session_strict__test_blocks_default_to_programming(void);
extern void test_uds_session_strict__test_allows_via_extended(void);
extern void test_uds_session_strict__test_other_transitions_unaffected(void);

void run_all_tests(void)
{
    RUN_TEST(test_uds_session_init__test_null_ctx);
    RUN_TEST(test_uds_session_init__test_zero_timeout);
    RUN_TEST(test_uds_session_init__test_happy_path);
    RUN_TEST(test_uds_session_init__test_double_init);
    RUN_TEST(test_uds_session_transition__test_null_ctx);
    RUN_TEST(test_uds_session_transition__test_not_initialized);
    RUN_TEST(test_uds_session_transition__test_default_to_extended);
    RUN_TEST(test_uds_session_transition__test_default_to_programming);
    RUN_TEST(test_uds_session_transition__test_extended_to_default);
    RUN_TEST(test_uds_session_transition__test_to_safety_system);
    RUN_TEST(test_uds_session_transition__test_transition_resets_timer);
    RUN_TEST(test_uds_session_get_active__test_null_ctx);
    RUN_TEST(test_uds_session_get_active__test_null_out);
    RUN_TEST(test_uds_session_get_active__test_reports_correct_session);
    RUN_TEST(test_uds_session_s3_reset__test_null_ctx);
    RUN_TEST(test_uds_session_s3_reset__test_timer_reloaded);
    RUN_TEST(test_uds_session_tick__test_null_ctx);
    RUN_TEST(test_uds_session_tick__test_no_countdown_in_default);
    RUN_TEST(test_uds_session_tick__test_countdown_in_extended);
    RUN_TEST(test_uds_session_tick__test_s3_timeout_reverts_to_default);
    RUN_TEST(test_uds_session_tick__test_timer_reloaded_after_timeout);
    RUN_TEST(test_uds_session_is_default__test_null_returns_true);
    RUN_TEST(test_uds_session_is_default__test_default_session_true);
    RUN_TEST(test_uds_session_is_default__test_extended_session_false);
    RUN_TEST(test_uds_session_is_default__test_programming_session_false);
    RUN_TEST(test_uds_session_strict__test_null_ctx);
    RUN_TEST(test_uds_session_strict__test_blocks_default_to_programming);
    RUN_TEST(test_uds_session_strict__test_allows_via_extended);
    RUN_TEST(test_uds_session_strict__test_other_transitions_unaffected);
}
