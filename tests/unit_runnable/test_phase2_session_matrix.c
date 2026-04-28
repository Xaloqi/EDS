// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_phase2_session_matrix.c
 *
 * PURPOSE: Phase-2 regression tests — ISO 14229-1 session transition matrix
 *          and session change notification callbacks.
 *
 * TEST CASES:
 *   TC-SESS-001  DEFAULT → EXTENDED           OK
 *   TC-SESS-002  DEFAULT → PROGRAMMING        OK
 *   TC-SESS-003  DEFAULT → SAFETY_SYSTEM      OK
 *   TC-SESS-004  PROGRAMMING → PROGRAMMING    OK (refresh)
 *   TC-SESS-005  PROGRAMMING → EXTENDED       ERR_SESSION_TRANSITION
 *   TC-SESS-006  EXTENDED → PROGRAMMING       OK
 *   TC-SESS-007  EXTENDED → SAFETY_SYSTEM     ERR_SESSION_TRANSITION
 *   TC-SESS-008  ANY → DEFAULT                OK (unconditional)
 *   TC-SESS-009  Callback fires on transition
 *   TC-SESS-010  Callback fires on S3 timeout
 *   TC-SESS-011  NULL deregisters callback
 *   TC-SESS-012  SAFETY_SYSTEM → SAFETY_SYS   OK (refresh)
 *   TC-SESS-013  SAFETY_SYSTEM → EXTENDED     ERR_SESSION_TRANSITION
 *
 * FRAMEWORK: Zephyr Ztest (host shim)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdbool.h>
#include "uds_session.h"
#include "uds_types.h"

static uds_session_type_t g_cb_old;
static uds_session_type_t g_cb_new;
static uint32_t           g_cb_count;

static void sess_cb(uds_session_type_t old_s, uds_session_type_t new_s)
{
    g_cb_old = old_s; g_cb_new = new_s; g_cb_count++;
}

static uds_session_ctx_t g_ctx;

static void setup(void)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    uds_session_init(&g_ctx, 5000U);
    g_cb_old = UDS_SESSION_DEFAULT;
    g_cb_new = UDS_SESSION_DEFAULT;
    g_cb_count = 0U;
}

ZTEST_SUITE(test_phase2_session_matrix, NULL, NULL, NULL, NULL, NULL);

ZTEST(test_phase2_session_matrix, tc001_default_to_extended)
{
    setup();
    zassert_equal(UDS_STATUS_OK, uds_session_transition(&g_ctx, UDS_SESSION_EXTENDED), "");
}

ZTEST(test_phase2_session_matrix, tc002_default_to_programming)
{
    setup();
    zassert_equal(UDS_STATUS_OK, uds_session_transition(&g_ctx, UDS_SESSION_PROGRAMMING), "");
}

ZTEST(test_phase2_session_matrix, tc003_default_to_safety)
{
    setup();
    zassert_equal(UDS_STATUS_OK, uds_session_transition(&g_ctx, UDS_SESSION_SAFETY_SYSTEM), "");
}

ZTEST(test_phase2_session_matrix, tc004_programming_refresh)
{
    setup();
    uds_session_transition(&g_ctx, UDS_SESSION_PROGRAMMING);
    zassert_equal(UDS_STATUS_OK, uds_session_transition(&g_ctx, UDS_SESSION_PROGRAMMING), "");
}

ZTEST(test_phase2_session_matrix, tc005_programming_to_extended_rejected)
{
    setup();
    uds_session_transition(&g_ctx, UDS_SESSION_PROGRAMMING);
    zassert_equal(UDS_STATUS_ERR_SESSION_TRANSITION,
        uds_session_transition(&g_ctx, UDS_SESSION_EXTENDED),
        "Programming to Extended must be rejected");
}

ZTEST(test_phase2_session_matrix, tc006_extended_to_programming)
{
    setup();
    uds_session_transition(&g_ctx, UDS_SESSION_EXTENDED);
    zassert_equal(UDS_STATUS_OK, uds_session_transition(&g_ctx, UDS_SESSION_PROGRAMMING), "");
}

ZTEST(test_phase2_session_matrix, tc007_extended_to_safety_rejected)
{
    setup();
    uds_session_transition(&g_ctx, UDS_SESSION_EXTENDED);
    zassert_equal(UDS_STATUS_ERR_SESSION_TRANSITION,
        uds_session_transition(&g_ctx, UDS_SESSION_SAFETY_SYSTEM),
        "Extended to SafetySystem must be rejected");
}

ZTEST(test_phase2_session_matrix, tc008_any_to_default)
{
    setup();
    uds_session_transition(&g_ctx, UDS_SESSION_PROGRAMMING);
    zassert_equal(UDS_STATUS_OK, uds_session_transition(&g_ctx, UDS_SESSION_DEFAULT), "");
}

ZTEST(test_phase2_session_matrix, tc009_callback_on_transition)
{
    setup();
    uds_session_register_change_cb(&g_ctx, sess_cb);
    uds_session_transition(&g_ctx, UDS_SESSION_EXTENDED);
    zassert_equal(1U, g_cb_count, "Callback fires once");
    zassert_equal(UDS_SESSION_DEFAULT,  g_cb_old, "Old = DEFAULT");
    zassert_equal(UDS_SESSION_EXTENDED, g_cb_new, "New = EXTENDED");
}

ZTEST(test_phase2_session_matrix, tc010_callback_on_s3_timeout)
{
    setup();
    memset(&g_ctx, 0, sizeof(g_ctx));
    uds_session_init(&g_ctx, 3U); /* 3ms timeout */
    uds_session_register_change_cb(&g_ctx, sess_cb);
    uds_session_transition(&g_ctx, UDS_SESSION_EXTENDED);
    g_cb_count = 0U; /* reset counter after transition */

    uds_status_t rc = UDS_STATUS_OK;
    for (int i = 0; i <= 5; i++) {
        rc = uds_session_tick_1ms(&g_ctx);
        if (rc != UDS_STATUS_OK) break;
    }
    zassert_equal(UDS_STATUS_ERR_SESSION_TIMEOUT, rc, "Timeout must occur");
    zassert_equal(1U, g_cb_count, "Callback fires on timeout");
    zassert_equal(UDS_SESSION_DEFAULT, g_cb_new, "Returns to DEFAULT");
}

ZTEST(test_phase2_session_matrix, tc011_deregister_callback)
{
    setup();
    uds_session_register_change_cb(&g_ctx, sess_cb);
    uds_session_register_change_cb(&g_ctx, NULL);
    uds_session_transition(&g_ctx, UDS_SESSION_EXTENDED);
    zassert_equal(0U, g_cb_count, "Deregistered callback must not fire");
}

ZTEST(test_phase2_session_matrix, tc012_safety_system_refresh)
{
    setup();
    uds_session_transition(&g_ctx, UDS_SESSION_SAFETY_SYSTEM);
    zassert_equal(UDS_STATUS_OK, uds_session_transition(&g_ctx, UDS_SESSION_SAFETY_SYSTEM), "");
}

ZTEST(test_phase2_session_matrix, tc013_safety_to_extended_rejected)
{
    setup();
    uds_session_transition(&g_ctx, UDS_SESSION_SAFETY_SYSTEM);
    zassert_equal(UDS_STATUS_ERR_SESSION_TRANSITION,
        uds_session_transition(&g_ctx, UDS_SESSION_EXTENDED),
        "SafetySystem to Extended must be rejected");
}

extern void test_phase2_session_matrix__tc001_default_to_extended(void);
extern void test_phase2_session_matrix__tc002_default_to_programming(void);
extern void test_phase2_session_matrix__tc003_default_to_safety(void);
extern void test_phase2_session_matrix__tc004_programming_refresh(void);
extern void test_phase2_session_matrix__tc005_programming_to_extended_rejected(void);
extern void test_phase2_session_matrix__tc006_extended_to_programming(void);
extern void test_phase2_session_matrix__tc007_extended_to_safety_rejected(void);
extern void test_phase2_session_matrix__tc008_any_to_default(void);
extern void test_phase2_session_matrix__tc009_callback_on_transition(void);
extern void test_phase2_session_matrix__tc010_callback_on_s3_timeout(void);
extern void test_phase2_session_matrix__tc011_deregister_callback(void);
extern void test_phase2_session_matrix__tc012_safety_system_refresh(void);
extern void test_phase2_session_matrix__tc013_safety_to_extended_rejected(void);

void run_all_tests(void)
{
    RUN_TEST(test_phase2_session_matrix__tc001_default_to_extended);
    RUN_TEST(test_phase2_session_matrix__tc002_default_to_programming);
    RUN_TEST(test_phase2_session_matrix__tc003_default_to_safety);
    RUN_TEST(test_phase2_session_matrix__tc004_programming_refresh);
    RUN_TEST(test_phase2_session_matrix__tc005_programming_to_extended_rejected);
    RUN_TEST(test_phase2_session_matrix__tc006_extended_to_programming);
    RUN_TEST(test_phase2_session_matrix__tc007_extended_to_safety_rejected);
    RUN_TEST(test_phase2_session_matrix__tc008_any_to_default);
    RUN_TEST(test_phase2_session_matrix__tc009_callback_on_transition);
    RUN_TEST(test_phase2_session_matrix__tc010_callback_on_s3_timeout);
    RUN_TEST(test_phase2_session_matrix__tc011_deregister_callback);
    RUN_TEST(test_phase2_session_matrix__tc012_safety_system_refresh);
    RUN_TEST(test_phase2_session_matrix__tc013_safety_to_extended_rejected);
}
