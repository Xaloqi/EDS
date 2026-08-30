// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_uds_acl_permissive_opt_in.c
 *
 * MODULE UNDER TEST: core/uds_access_table.c / core/uds_server.c, compiled
 *                     with the permissive opt-in switch active.
 *
 * [#113] Regression test for issue #113's second requirement: prove that
 * UDS_ACL_ALLOW_UNLISTED_SERVICES=1 restores the EXACT pre-#113 behaviour
 * for a service_id absent from the active access table — i.e. the opt-in
 * genuinely works, not just that the fail-closed default (proven by
 * tests/unit_runnable/test_phase5_access_table.c's TC-ACL-022/026-029)
 * exists.
 *
 * This only activates when UDS_ACL_ALLOW_UNLISTED_SERVICES == 1, which
 * cannot be reached from the default host build used by every other test
 * module (see build_tests.sh / tests/CMakeLists.txt). This file is
 * therefore compiled as its own, separate test binary with
 * -DUDS_ACL_ALLOW_UNLISTED_SERVICES=1 — mirroring the existing
 * test_trng_fail_closed.c pattern for CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0.
 *
 * TEST CASES:
 *   TC-ACLP-001  Compile-time proof this TU is actually built with the
 *                opt-in active (UDS_ACL_ALLOW_UNLISTED_SERVICES == 1).
 *   TC-ACLP-002  uds_access_table_enforce(NULL, ...) -> UDS_STATUS_OK with
 *                the opt-in active (was DENY by default; see TC-ACL-022).
 *   TC-ACLP-003  Fake SID 0x99 (no ACL entry), end-to-end lookup+enforce
 *                against the default table, DEFAULT session -> ALLOWED.
 *   TC-ACLP-004  End-to-end through uds_server_process_request(): a
 *                registered service handler with NO row in the supplied
 *                access_table is still DISPATCHED (handler runs) when the
 *                opt-in is active — proves the restore reaches the real
 *                production dispatch path in core/uds_server.c, not just
 *                the uds_access_table.c library functions in isolation.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "uds_access_table.h"
#include "uds_security.h"
#include "uds_session.h"
#include "uds_server.h"
#include "uds_types.h"

/* =============================================================================
 * [#113] Compile-time proof this TU is actually built with the permissive
 * opt-in active. Without this guard, a broken build-flag pipeline (e.g.
 * build_tests.sh losing the -DUDS_ACL_ALLOW_UNLISTED_SERVICES=1 flag for
 * this test) would silently compile this file against the fail-closed
 * default instead, and every assertion below would simply fail loudly —
 * this guard turns that into an unambiguous compile-time error instead of
 * a confusing runtime failure.
 * ============================================================================= */
#if !UDS_ACL_ALLOW_UNLISTED_SERVICES
#  error "[#113] test_uds_acl_permissive_opt_in.c must be built with " \
         "-DUDS_ACL_ALLOW_UNLISTED_SERVICES=1 -- see the " \
         "extra_flags_for_test() entry in build_tests.sh / the DEFINES " \
         "on this target in tests/CMakeLists.txt."
#endif

ZTEST_SUITE(test_uds_acl_permissive_opt_in, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-ACLP-001: sanity — the macro really is 1 in this translation unit.
 */
ZTEST(test_uds_acl_permissive_opt_in, tc001_opt_in_macro_is_active)
{
    zassert_equal(UDS_ACL_ALLOW_UNLISTED_SERVICES, 1,
        "this test binary must be compiled with UDS_ACL_ALLOW_UNLISTED_SERVICES=1");
}

/**
 * TC-ACLP-002: enforce(NULL, ...) restores the pre-#113 OK/"no restriction"
 * result when the opt-in is active. Mirrors TC-ACL-022 in
 * test_phase5_access_table.c, which proves the opposite (DENY) in the
 * default build.
 */
ZTEST(test_uds_acl_permissive_opt_in, tc002_enforce_null_entry_allowed)
{
    uds_security_ctx_t sec_ctx;
    (void)memset(&sec_ctx, 0, sizeof(sec_ctx));
    uds_status_t rc = uds_access_table_enforce(NULL, &sec_ctx);
    zassert_equal(rc, UDS_STATUS_OK,
        "enforce with NULL entry must be OK when UDS_ACL_ALLOW_UNLISTED_SERVICES=1");
}

/**
 * TC-ACLP-003: fake SID 0x99 (no ACL entry in the default table),
 * end-to-end lookup + enforce, DEFAULT session -> ALLOWED. This is the same
 * scenario TC-ACL-026 in test_phase5_access_table.c proves is DENIED in the
 * default build.
 */
ZTEST(test_uds_acl_permissive_opt_in, tc003_fake_sid_0x99_allowed_default)
{
    const uds_access_entry_t *entry = NULL;
    uds_status_t rc = uds_access_table_lookup(
        uds_access_table_get_default(),
        (uint8_t)UDS_ACCESS_TABLE_DEFAULT_COUNT,
        (uint8_t)0x99U, UDS_SESSION_DEFAULT, &entry);
    zassert_equal(rc, UDS_STATUS_OK, "lookup must return OK");
    zassert_is_null(entry, "SID 0x99 must still be absent from the default table");

    rc = uds_access_table_enforce(entry, NULL);
    zassert_equal(rc, UDS_STATUS_OK,
        "SID 0x99 (no ACL entry) must be ALLOWED with the permissive opt-in active");
}

/* =========================================================================
 * TC-ACLP-004: end-to-end through uds_server_process_request().
 * ========================================================================= */

static uds_status_t stub_handler_ok(uds_server_ctx_t *ctx,
                                     const uds_msg_buf_t *req,
                                     uds_msg_buf_t *resp)
{
    (void)ctx; (void)req;
    resp->data[0] = 0xD9U; /* 0x99 + 0x40 positive-response offset */
    resp->data[1] = 0x00U;
    resp->length  = 2U;
    return UDS_STATUS_OK;
}

/* Artificial SID 0x99 — registered as a service, but deliberately given NO
 * row in g_acl_table below, mirroring exactly the #113 scenario: a service
 * handler exists with no corresponding ACL entry. */
static const uds_service_entry_t g_svc_table[] = {
    { 0x99U, stub_handler_ok, false },
};
#define SVC_TABLE_COUNT (1U)

/* Empty ACL table: zero entries, so SID 0x99 can never match a row. With
 * the opt-in active this must still permit dispatch (see file header). */
static const uds_access_entry_t g_acl_table[1] = {{ 0 }};
#define ACL_TABLE_COUNT (0U) /* count=0 -> lookup() always returns NULL */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

static uds_msg_buf_t make_req(uint8_t sid)
{
    uds_msg_buf_t req;
    (void)memset(&req, 0, sizeof(req));
    req.data[0] = sid;
    req.length  = 1U;
    return req;
}

ZTEST(test_uds_acl_permissive_opt_in, tc004_end_to_end_dispatch_allowed)
{
    (void)memset(&g_sess, 0, sizeof(g_sess));
    (void)memset(&g_sec,  0, sizeof(g_sec));
    (void)memset(&g_srv,  0, sizeof(g_srv));
    uds_session_init(&g_sess, 5000U);

    static const uds_security_cfg_t sec_cfg = {
        .max_attempts = 3U,
        .lockout_ms   = 100U,
    };
    uds_security_init(&g_sec, &sec_cfg);

    const uds_server_cfg_t cfg = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_svc_table,
        .service_table_count   = (uint8_t)SVC_TABLE_COUNT,
        .access_table           = g_acl_table,
        .access_table_count     = (uint8_t)ACL_TABLE_COUNT,
    };
    zassert_equal(uds_server_init(&g_srv, &cfg), UDS_STATUS_OK, "init failed");

    uds_msg_buf_t req  = make_req(0x99U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_server_process_request(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "process_request must return OK");
    zassert_equal(resp.data[0], 0xD9U,
        "SID 0x99 (no ACL row) must reach stub_handler_ok with the "
        "permissive opt-in active -- a negative response (0x7F) here would "
        "mean the opt-in did not actually restore dispatch");
    zassert_equal(resp.length, 2U, "response length mismatch");
}

/* run_all_tests shim */
extern void test_uds_acl_permissive_opt_in__tc001_opt_in_macro_is_active(void);
extern void test_uds_acl_permissive_opt_in__tc002_enforce_null_entry_allowed(void);
extern void test_uds_acl_permissive_opt_in__tc003_fake_sid_0x99_allowed_default(void);
extern void test_uds_acl_permissive_opt_in__tc004_end_to_end_dispatch_allowed(void);

void run_all_tests(void)
{
    RUN_TEST(test_uds_acl_permissive_opt_in__tc001_opt_in_macro_is_active);
    RUN_TEST(test_uds_acl_permissive_opt_in__tc002_enforce_null_entry_allowed);
    RUN_TEST(test_uds_acl_permissive_opt_in__tc003_fake_sid_0x99_allowed_default);
    RUN_TEST(test_uds_acl_permissive_opt_in__tc004_end_to_end_dispatch_allowed);
}
