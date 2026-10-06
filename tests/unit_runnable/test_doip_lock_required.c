// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_doip_lock_required.c
 *
 * MODULE UNDER TEST: transport/doip/doip_server.c (PRODUCTION-configuration
 *                     lock-callback requirement).
 *
 * [EDS#358]
 *
 * PURPOSE:
 *   Prove the PRODUCTION behaviour of eds_doip_server_run(): it must refuse
 *   to run (UDS_STATUS_ERR_NOT_INITIALIZED) if eds_doip_set_lock_callbacks()
 *   was never called with both callbacks non-NULL, and it must NOT refuse a
 *   correctly-configured build. This behaviour only activates when
 *   EDS_BUILD_IS_PRODUCTION == 1, which cannot be reached from the default
 *   dev-configuration build used by test_doip_server.c. This file is
 *   therefore compiled as its own, separate test binary with
 *   -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0, same mechanism as
 *   test_trng_fail_closed.c.
 *
 *   The mirror DEVELOPMENT-configuration regression guard — proving that
 *   the default build still runs unprotected (today's behaviour,
 *   unchanged) — lives in test_doip_server.c and is not duplicated here.
 *
 * TEST CASES:
 *   TC-DLR-001  Production build, no lock callbacks registered ->
 *               UDS_STATUS_ERR_NOT_INITIALIZED, and tcp_listen() is never
 *               called (proves the guard fires before any real work, not
 *               just that the end result happens to be an error).
 *   TC-DLR-002  Production build, lock callbacks registered -> the new
 *               guard does not fire; the function proceeds to call
 *               tcp_listen() (which this test's mock fails deliberately,
 *               so the function returns UDS_STATUS_ERR_PLATFORM without
 *               blocking in the accept loop). Proves the fix does not
 *               break a correctly-configured production build.
 *
 * VERIFIED (run-014): with the EDS#358 guard temporarily reverted,
 * TC-DLR-001 does not report a clean assertion failure -- it hangs,
 * because eds_doip_server_run()'s accept loop retries forever on a
 * failing tcp_accept() by design (its own "never returns in normal
 * operation" contract). build_tests.sh's own per-test timeout catches
 * this and reports it as FAIL (0 cases) via the run-013 zero-cases rule,
 * rather than hanging the whole suite or silently passing -- confirmed
 * directly, twice, before this file's mocks were redesigned to count
 * rather than assert-inside-the-loop (an earlier draft called
 * TEST_FAIL_MESSAGE from mock_tcp_accept, which does not unwind the
 * current test in this harness -- see the mock comment below -- and
 * produced an unbounded failure-message loop instead of a clean FAIL).
 * Restored and re-verified passing (2/2) immediately after.
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "doip_server.h"
#include "uds_server.h"
#include "uds_types.h"

/* =============================================================================
 * [EDS#358] Compile-time proof this TU is actually built in the production
 * configuration -- same rationale as test_trng_fail_closed.c's identical
 * guard: without this, a broken build-flag pipeline would silently compile
 * this file in DEVELOPMENT mode instead, and TC-DLR-001 would then fail for
 * the wrong reason (or TC-DLR-002 would pass vacuously), making this test
 * worthless without ever failing loudly about why.
 * ============================================================================= */
#if defined(CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY)
#  if CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY
#    error "[EDS#358] test_doip_lock_required.c must be built with " \
           "CONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 (forces " \
           "EDS_BUILD_IS_PRODUCTION=1), not a non-zero value -- see the " \
           "extra_flags_for_test() entry in build_tests.sh."
#  endif
#else
#  error "[EDS#358] test_doip_lock_required.c must be built with " \
         "-DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 to force " \
         "EDS_BUILD_IS_PRODUCTION=1 -- see the extra_flags_for_test() " \
         "entry in build_tests.sh."
#endif

ZTEST_SUITE(test_doip_lock_required, NULL, NULL, NULL, NULL, NULL);

/* --------------------------------------------------------------------------
 * Minimal mock platform ops -- only tcp_listen is ever reached by either
 * test case under the fix; every other op would only be called from
 * inside the accept loop, which neither test case reaches.
 *
 * These count calls rather than calling TEST_FAIL_MESSAGE/zassert directly,
 * deliberately: this harness's assertion macros record a failure but do
 * NOT unwind the current test function (no setjmp/longjmp abort here,
 * unlike upstream Unity's default). eds_doip_server_run()'s accept loop
 * retries forever on a non-OK tcp_accept() (by design -- see its own
 * "Timeout or transient error — keep listening" comment), so an assertion
 * macro called from inside that loop would never stop the loop, hanging
 * the test instead of failing it. Counting and asserting on the count
 * *after* eds_doip_server_run() returns is the safe version of the same
 * check -- confirmed the hard way: an earlier draft of this file called
 * TEST_FAIL_MESSAGE directly from mock_tcp_accept, reverted the §EDS#358
 * fix to prove the test would fail without it (per this repo's run-014
 * rule), and got an unbounded failure-message loop instead of a clean
 * FAIL, exactly because of this.
 * -------------------------------------------------------------------------- */

static int g_mock_listen_calls;
static int g_mock_listen_rc; /* return value for tcp_listen */
static int g_mock_accept_calls;
static int g_mock_send_calls;
static int g_mock_recv_calls;

static int mock_tcp_listen(uint16_t port, void **server_ctx)
{
    (void)port;
    *server_ctx = (void *)(uintptr_t)0xDEADBEEFU;
    g_mock_listen_calls++;
    return g_mock_listen_rc;
}

static int mock_tcp_accept(void *server_ctx, void **conn_ctx, uint32_t timeout_ms)
{
    (void)server_ctx; (void)timeout_ms;
    *conn_ctx = NULL;
    g_mock_accept_calls++;
    return -1; /* "no connection yet" -- matches a real poll timeout */
}

static int mock_tcp_send(void *conn_ctx, const uint8_t *data, size_t len)
{
    (void)conn_ctx; (void)data; (void)len;
    g_mock_send_calls++;
    return -1;
}

static int mock_tcp_recv(void *conn_ctx, uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    (void)conn_ctx; (void)buf; (void)buf_len; (void)timeout_ms;
    g_mock_recv_calls++;
    return 0;
}

static void mock_tcp_close(void *conn_ctx)
{
    (void)conn_ctx;
}

static void mock_tcp_server_close(void *server_ctx)
{
    (void)server_ctx;
}

static const eds_doip_platform_ops_t g_mock_ops = {
    .tcp_listen        = mock_tcp_listen,
    .tcp_accept        = mock_tcp_accept,
    .tcp_send          = mock_tcp_send,
    .tcp_recv          = mock_tcp_recv,
    .tcp_close         = mock_tcp_close,
    .tcp_server_close  = mock_tcp_server_close,
};

/* --------------------------------------------------------------------------
 * Dummy lock callbacks -- never actually invoked by either test case
 * (both return before reaching the dispatch call); counted for the same
 * reason the platform-ops mocks above are, not asserted-on-call.
 * -------------------------------------------------------------------------- */

static int g_dummy_lock_calls;
static int g_dummy_unlock_calls;

static void dummy_lock(void)
{
    g_dummy_lock_calls++;
}

static void dummy_unlock(void)
{
    g_dummy_unlock_calls++;
}

/* --------------------------------------------------------------------------
 * TC-DLR-001: no lock callbacks registered -> production build refuses.
 * -------------------------------------------------------------------------- */
ZTEST(test_doip_lock_required, tc001_no_locks_production_refuses)
{
    doip_server_state_t s;
    uds_server_ctx_t    uds_ctx;
    uds_status_t        rc;

    (void)memset(&uds_ctx, 0, sizeof(uds_ctx));

    rc = eds_doip_register_platform(&g_mock_ops);
    zassert_equal(rc, UDS_STATUS_OK, "register_platform failed");

    eds_doip_set_lock_callbacks(NULL, NULL); /* explicit: this test's precondition */

    rc = eds_doip_server_init(&s, 0xE400U);
    zassert_equal(rc, UDS_STATUS_OK, "server_init failed");

    g_mock_listen_calls = 0;
    g_mock_listen_rc    = 0; /* would succeed, if ever reached */
    g_mock_accept_calls = 0;
    g_mock_send_calls   = 0;
    g_mock_recv_calls   = 0;

    rc = eds_doip_server_run(&s, &uds_ctx, DOIP_PORT);
    zassert_equal(rc, UDS_STATUS_ERR_NOT_INITIALIZED,
        "production build with no lock callbacks registered must refuse to run");
    zassert_equal(g_mock_listen_calls, 0,
        "the guard must fire before tcp_listen() -- proves it short-circuits "
        "rather than merely erroring out later");
    zassert_equal(g_mock_accept_calls, 0,
        "must never reach the accept loop at all");
    zassert_equal(g_mock_send_calls, 0, "no frame can be sent with no connection");
    zassert_equal(g_mock_recv_calls, 0, "no frame can be received with no connection");
}

/* --------------------------------------------------------------------------
 * TC-DLR-002: lock callbacks registered -> guard does not fire.
 * -------------------------------------------------------------------------- */
ZTEST(test_doip_lock_required, tc002_locks_registered_production_proceeds)
{
    doip_server_state_t s;
    uds_server_ctx_t    uds_ctx;
    uds_status_t        rc;

    (void)memset(&uds_ctx, 0, sizeof(uds_ctx));

    rc = eds_doip_register_platform(&g_mock_ops);
    zassert_equal(rc, UDS_STATUS_OK, "register_platform failed");

    eds_doip_set_lock_callbacks(dummy_lock, dummy_unlock);

    rc = eds_doip_server_init(&s, 0xE400U);
    zassert_equal(rc, UDS_STATUS_OK, "server_init failed");

    g_mock_listen_calls = 0;
    g_mock_listen_rc    = -1; /* deliberate failure: returns before the accept loop */
    g_mock_accept_calls = 0;
    g_mock_send_calls   = 0;
    g_mock_recv_calls   = 0;
    g_dummy_lock_calls  = 0;
    g_dummy_unlock_calls = 0;

    rc = eds_doip_server_run(&s, &uds_ctx, DOIP_PORT);
    zassert_equal(rc, UDS_STATUS_ERR_PLATFORM,
        "a correctly-configured production build must reach tcp_listen(), "
        "not be refused by the EDS#358 guard");
    zassert_equal(g_mock_listen_calls, 1,
        "tcp_listen() must have been called exactly once -- proves the "
        "guard let a correctly-configured build proceed");
    zassert_equal(g_mock_accept_calls, 0,
        "tcp_listen() failed, so the accept loop must never have started");
    zassert_equal(g_mock_send_calls, 0, "no frame can be sent with no connection");
    zassert_equal(g_mock_recv_calls, 0, "no frame can be received with no connection");
    zassert_equal(g_dummy_lock_calls, 0,
        "no request was ever dispatched, so the lock callback itself must "
        "never have been invoked -- only its registration matters here");
    zassert_equal(g_dummy_unlock_calls, 0,
        "same as above, for the unlock callback");

    eds_doip_set_lock_callbacks(NULL, NULL); /* leave clean for any later test */
}

/* run_all_tests shim */
extern void test_doip_lock_required__tc001_no_locks_production_refuses(void);
extern void test_doip_lock_required__tc002_locks_registered_production_proceeds(void);

void run_all_tests(void)
{
    RUN_TEST(test_doip_lock_required__tc001_no_locks_production_refuses);
    RUN_TEST(test_doip_lock_required__tc002_locks_registered_production_proceeds);
}
