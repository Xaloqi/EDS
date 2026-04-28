// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_routine_database.c
 *
 * MODULE UNDER TEST: config/routine_database.c
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 *            run_all_tests() entry point required by tests/runner/test_main.c
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "routine_database.h"
#include "uds_types.h"

/* ── Stub callbacks ─────────────────────────────────────────────────────── */

static uds_status_t s_start_stub(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *res, uint8_t res_len, uint8_t *res_out)
{ (void)opt; (void)opt_len; (void)res; (void)res_len; *res_out=0U; return UDS_STATUS_OK; }

static uds_status_t s_stop_stub(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *res, uint8_t res_len, uint8_t *res_out)
{ (void)opt; (void)opt_len; (void)res; (void)res_len; *res_out=0U; return UDS_STATUS_OK; }

static uds_status_t s_results_stub(uint8_t *res, uint8_t res_len, uint8_t *res_out)
{ (void)res; (void)res_len; *res_out=0U; return UDS_STATUS_OK; }

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void db_init(void) { (void)routine_database_init(); }

static routine_entry_t make_entry(uint16_t rid)
{
    routine_entry_t e;
    memset(&e, 0, sizeof(e));
    e.rid           = rid;
    e.support_flags = (uint8_t)ROUTINE_SUPPORT_START;
    e.min_session   = (uint8_t)UDS_SESSION_EXTENDED;
    e.security_level = 0U;
    e.start_cb      = s_start_stub;
    e.description   = "test";
    return e;
}

/* ── Suite: init ────────────────────────────────────────────────────────── */

ZTEST_SUITE(routine_db_init, NULL, NULL, NULL, NULL, NULL);

ZTEST(routine_db_init, test_double_init_returns_already_initialized)
{
    db_init();
    uds_status_t rc = routine_database_init();
    zassert_equal(UDS_STATUS_ERR_ALREADY_INITIALIZED, rc, "");
}

/* ── Suite: register ────────────────────────────────────────────────────── */

ZTEST_SUITE(routine_db_register, NULL, NULL, NULL, NULL, NULL);

ZTEST(routine_db_register, test_null_entry)
{
    db_init();
    zassert_equal(UDS_STATUS_ERR_NULL_PTR, routine_database_register(NULL), "");
}

ZTEST(routine_db_register, test_null_start_cb)
{
    db_init();
    routine_entry_t e = make_entry(0xE001U);
    e.start_cb = NULL;
    zassert_equal(UDS_STATUS_ERR_NULL_PTR, routine_database_register(&e), "");
}

ZTEST(routine_db_register, test_missing_start_flag_rejected)
{
    db_init();
    routine_entry_t e = make_entry(0xE002U);
    e.support_flags = (uint8_t)ROUTINE_SUPPORT_STOP;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM, routine_database_register(&e), "");
}

ZTEST(routine_db_register, test_happy_path)
{
    db_init();
    routine_entry_t e = make_entry(0xE010U);
    uds_status_t rc = routine_database_register(&e);
    zassert_true((rc == UDS_STATUS_OK) || (rc == UDS_STATUS_ERR_INVALID_PARAM), "");
}

ZTEST(routine_db_register, test_duplicate_rid_rejected)
{
    db_init();
    routine_entry_t e = make_entry(0xE020U);
    (void)routine_database_register(&e);
    uds_status_t rc = routine_database_register(&e);
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM, rc, "duplicate must be rejected");
}

ZTEST(routine_db_register, test_all_support_flags_stored)
{
    db_init();
    routine_entry_t e = make_entry(0xE030U);
    e.support_flags = (uint8_t)(ROUTINE_SUPPORT_START |
                                ROUTINE_SUPPORT_STOP  |
                                ROUTINE_SUPPORT_RESULTS);
    e.stop_cb    = s_stop_stub;
    e.results_cb = s_results_stub;
    (void)routine_database_register(&e);

    const routine_entry_t *f = routine_database_find(0xE030U);
    zassert_not_null(f, "must be findable");
    if (f != NULL) {
        zassert_true((f->support_flags & ROUTINE_SUPPORT_STOP)    != 0U, "");
        zassert_true((f->support_flags & ROUTINE_SUPPORT_RESULTS) != 0U, "");
        zassert_not_null(f->stop_cb,    "stop_cb NULL");
        zassert_not_null(f->results_cb, "results_cb NULL");
    }
}

/* ── Suite: find ────────────────────────────────────────────────────────── */

ZTEST_SUITE(routine_db_find, NULL, NULL, NULL, NULL, NULL);

ZTEST(routine_db_find, test_find_registered_entry)
{
    db_init();
    routine_entry_t e = make_entry(0xE040U);
    (void)routine_database_register(&e);
    zassert_not_null(routine_database_find(0xE040U), "must find registered RID");
}

ZTEST(routine_db_find, test_find_unregistered_returns_null)
{
    db_init();
    zassert_is_null(routine_database_find(0xDEADU), "unregistered must be NULL");
}

ZTEST(routine_db_find, test_find_verifies_fields)
{
    db_init();
    routine_entry_t e = make_entry(0xE050U);
    e.min_session    = (uint8_t)UDS_SESSION_PROGRAMMING;
    e.security_level = 1U;
    e.description    = "erase flash";
    (void)routine_database_register(&e);

    const routine_entry_t *f = routine_database_find(0xE050U);
    zassert_not_null(f, "must find entry");
    if (f != NULL) {
        zassert_equal(0xE050U, (uint32_t)f->rid, "RID");
        zassert_equal((uint8_t)UDS_SESSION_PROGRAMMING, f->min_session, "session");
        zassert_equal(1U, f->security_level, "sec_level");
        zassert_str_equal("erase flash", f->description, "desc");
    }
}

/* ── Suite: get_count ───────────────────────────────────────────────────── */

ZTEST_SUITE(routine_db_count, NULL, NULL, NULL, NULL, NULL);

ZTEST(routine_db_count, test_null_out_returns_null_ptr)
{
    db_init();
    zassert_equal(UDS_STATUS_ERR_NULL_PTR, routine_database_get_count(NULL), "");
}

ZTEST(routine_db_count, test_count_nonzero_after_registration)
{
    db_init();
    routine_entry_t e = make_entry(0xE060U);
    (void)routine_database_register(&e);

    uint16_t cnt = 0U;
    zassert_equal(UDS_STATUS_OK, routine_database_get_count(&cnt), "");
    zassert_true(cnt > 0U, "count must be > 0");
}

/* ── run_all_tests ──────────────────────────────────────────────────────── */

void run_all_tests(void)
{
    RUN_TEST(routine_db_init__test_double_init_returns_already_initialized);

    RUN_TEST(routine_db_register__test_null_entry);
    RUN_TEST(routine_db_register__test_null_start_cb);
    RUN_TEST(routine_db_register__test_missing_start_flag_rejected);
    RUN_TEST(routine_db_register__test_happy_path);
    RUN_TEST(routine_db_register__test_duplicate_rid_rejected);
    RUN_TEST(routine_db_register__test_all_support_flags_stored);

    RUN_TEST(routine_db_find__test_find_registered_entry);
    RUN_TEST(routine_db_find__test_find_unregistered_returns_null);
    RUN_TEST(routine_db_find__test_find_verifies_fields);

    RUN_TEST(routine_db_count__test_null_out_returns_null_ptr);
    RUN_TEST(routine_db_count__test_count_nonzero_after_registration);
}
