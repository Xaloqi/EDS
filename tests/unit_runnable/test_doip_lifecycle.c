// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_doip_lifecycle.c
 *
 * MODULE UNDER TEST: transport/doip/doip_server.c — eds_doip_server_run()
 *
 * PURPOSE:
 *   Connection-lifecycle behaviour of the DoIP server loop itself, which
 *   test_doip_server (frame handling via doip_handle_frame()) and
 *   test_doip_conformance_vectors (frame-level vectors) never enter.
 *
 *   The loop never returns in normal operation. Each test scripts the
 *   platform ops: mock_tcp_recv() plays back a list of data / timeout /
 *   error events, and mock_tcp_accept() longjmp()s back into the test once
 *   the scripted connection has closed (or after a scripted number of idle
 *   accept polls). Simulated time is the sum of the timeout_ms of every
 *   recv that timed out, which is exactly what the server sees on target,
 *   plus g_ms_per_data_read for every read that returned data. The
 *   uptime_ms op returns it (EDS#412).
 *
 *   Inactivity (EDS#400):
 *     - test_doip_idle_gap_holds_session
 *     - test_doip_general_inactivity_closes_at_iso_default
 *     - test_doip_initial_inactivity_without_routing_activation
 *     - test_doip_connection_idle_budget_bounds_lifetime
 *   Guards that must not regress (EDS#193, EDS#218, dead peers):
 *     - test_doip_partial_frame_stall_still_closes_fast
 *     - test_doip_hard_error_while_idle_closes_immediately
 *     - test_doip_payload_trickle_still_capped
 *   Connection lifetime (EDS#412, replaces the EDS#218 frame cap):
 *     - test_doip_long_download_not_cut_off
 *     - test_doip_busy_client_loses_slot_at_lifetime
 *   Response-pending continuation (EDS#423):
 *     - test_doip_pending_without_callback_sends_only_0x78
 *     - test_doip_pending_callback_completes_exchange
 *     - test_doip_pending_phase_is_time_bounded
 *   ECUReset performed by the server (EDS#434):
 *     - test_doip_ecu_reset_performed_after_response
 *     - test_doip_ecu_reset_suppressed_response_still_resets
 *     - test_doip_no_reset_without_ecu_reset_request
 *   Return to DEFAULT on disconnect (SEC-SESS-COMMRESTORE-01):
 *     - test_doip_disconnect_restores_comm_and_dtc_setting
 *   Supervision check-in (EDS#399):
 *     - test_doip_checkin_while_idle_without_client
 *     - test_doip_checkin_while_connected_idle
 *     - test_doip_checkin_cleared_is_never_called
 *
 * FRAMEWORK: Zephyr Ztest (shim used for host builds — see tests/runner/)
 * BUILT BY : build_tests.sh
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#define EDS_MSG_BUF_MAX_STACK_BYTES 8192
#include "doip_server.h"
#include "uds_server.h"
#include "uds_session.h"
#include "services.h"   /* uds_service_0x11_handler() [EDS#434] */
#include "uds_comm_control.h"   /* [SEC-SESS-COMMRESTORE-01] */
#include "uds_types.h"

#define TEST_ECU_ADDR     (0x0E00U)
#define TEST_TESTER_ADDR  (0x0E80U)

/* Stand-in for a negative errno a real binding returns on a dead socket
 * (ECONNRESET on Linux/Zephyr). Deliberately not DOIP_TCP_RECV_TIMEOUT. */
#define TEST_RECV_HARD_ERROR (-104)

/* The in-tree Unity has no *_MESSAGE variants. These name the defect on
 * failure; like every TEST_ASSERT_* here they return from the enclosing
 * function, so they are only used in test bodies. */
#define EXPECT_EQ_MSG(expected, actual, msg) do { \
    if ((long long)(expected) != (long long)(actual)) { \
        char _b[320]; \
        (void)snprintf(_b, sizeof(_b), "%s: expected %lld, got %lld (" #actual ")", \
                       (msg), (long long)(expected), (long long)(actual)); \
        TEST_FAIL_MESSAGE(_b); \
        return; \
    } \
} while (0)

#define EXPECT_TRUE_MSG(cond, msg) do { \
    if (!(cond)) { TEST_FAIL_MESSAGE(msg); return; } \
} while (0)

/* =========================================================================
 * Scripted platform ops
 * ========================================================================= */

typedef enum {
    STEP_DATA,     /* return bytes from data[], possibly across several reads */
    STEP_TIMEOUT,  /* return DOIP_TCP_RECV_TIMEOUT (no data within timeout_ms) */
    STEP_ERROR     /* return TEST_RECV_HARD_ERROR (socket is dead) */
} step_kind_t;

typedef struct {
    step_kind_t    kind;
    const uint8_t *data;
    size_t         len;
    uint32_t       repeat;   /* STEP_TIMEOUT / STEP_ERROR: times to repeat */
    size_t         chunk;    /* STEP_DATA: max bytes per read, 0 = no cap */
} recv_step_t;

#define MAX_STEPS 64U

static recv_step_t g_steps[MAX_STEPS];
static uint32_t    g_step_count;
static uint32_t    g_step_idx;
static size_t      g_step_off;      /* bytes consumed of current DATA step */
static uint32_t    g_step_rep;      /* repeats consumed of current step */

static jmp_buf     g_server_exit;
static uint32_t    g_accept_timeouts;      /* idle accept polls before conn */
static bool        g_conn_given;
static uint32_t    g_accept_calls;

/* Observations — counted here, asserted only in the test body. */
static uint32_t    g_close_calls;
static bool        g_script_done_at_close;  /* true: peer closed it, not us */
static uint32_t    g_sim_idle_ms;            /* sum of timed-out recv waits */
static uint32_t    g_sim_now_ms;             /* uptime_ms op: the simulated clock */
static uint32_t    g_ms_per_data_read;       /* clock advance per data read */
static uint32_t    g_conn_open_ms;           /* clock at accept */
static uint32_t    g_sim_now_ms_at_close;
static uint32_t    g_sim_idle_ms_at_close;
static uint32_t    g_recv_calls;
static uint32_t    g_last_recv_timeout_ms;
static uint32_t    g_checkins;
static uint32_t    g_tp_calls;
static uds_session_type_t g_session_at_last_tp;

static bool script_done(void)
{
    return g_step_idx >= g_step_count;
}

static void script_reset(void)
{
    memset(g_steps, 0, sizeof(g_steps));
    g_step_count = 0U;
    g_step_idx   = 0U;
    g_step_off   = 0U;
    g_step_rep   = 0U;
}

static void add_step(step_kind_t kind, const uint8_t *data, size_t len, uint32_t repeat)
{
    if (g_step_count >= MAX_STEPS) {
        TEST_FAIL_MESSAGE("test script too long");
        return;
    }
    g_steps[g_step_count].kind   = kind;
    g_steps[g_step_count].data   = data;
    g_steps[g_step_count].len    = len;
    g_steps[g_step_count].repeat = repeat;
    g_step_count++;
}

static void add_data(const uint8_t *data, size_t len)  { add_step(STEP_DATA, data, len, 1U); }
static void add_timeouts(uint32_t n)                   { add_step(STEP_TIMEOUT, NULL, 0U, n); }
static void add_error(void)                            { add_step(STEP_ERROR, NULL, 0U, 1U); }

static int mock_tcp_listen(uint16_t port, void **server_ctx)
{
    (void)port;
    *server_ctx = (void *)(uintptr_t)0xDEADBEEFU;
    return 0;
}

static int mock_tcp_accept(void *server_ctx, void **conn_ctx, uint32_t timeout_ms)
{
    (void)server_ctx; (void)timeout_ms;
    g_accept_calls++;
    if (g_accept_timeouts > 0U) {
        g_accept_timeouts--;
        return -11; /* poll timeout, as zephyr_tcp_accept() reports it */
    }
    if (g_conn_given || (g_step_count == 0U)) {
        /* Scenario over: leave eds_doip_server_run() for good. */
        longjmp(g_server_exit, 1);
    }
    g_conn_given = true;
    g_conn_open_ms = g_sim_now_ms;
    *conn_ctx = (void *)(uintptr_t)0xCAFEBABEU;
    return 0;
}

/* [EDS#423] First 3 bytes of every UDS response the server sends, in order. */
#define MAX_RESPS 64U
static uint8_t  g_resp[MAX_RESPS][3];
static uint32_t g_resp_count;

static int mock_tcp_send(void *conn_ctx, const uint8_t *data, size_t len)
{
    (void)conn_ctx;
    /* A DiagnosticMessage (0x8001) from us: header, 4 address bytes, UDS. */
    if ((len >= (size_t)(DOIP_HEADER_LEN + 5U)) &&
        (data[2] == 0x80U) && (data[3] == 0x01U) && (g_resp_count < MAX_RESPS)) {
        const uint8_t *uds = &data[DOIP_HEADER_LEN + 4U];
        size_t ulen = len - (size_t)(DOIP_HEADER_LEN + 4U);
        g_resp[g_resp_count][0] = uds[0];
        g_resp[g_resp_count][1] = (ulen > 1U) ? uds[1] : 0U;
        g_resp[g_resp_count][2] = (ulen > 2U) ? uds[2] : 0U;
        g_resp_count++;
    }
    return (int)len;
}

static int mock_tcp_recv(void *conn_ctx, uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    (void)conn_ctx;
    g_recv_calls++;
    g_last_recv_timeout_ms = timeout_ms;

    if (script_done()) {
        return 0; /* peer closed cleanly */
    }

    recv_step_t *st = &g_steps[g_step_idx];
    switch (st->kind) {
    case STEP_DATA: {
        size_t left = st->len - g_step_off;
        size_t n    = (left < buf_len) ? left : buf_len;
        if ((st->chunk > 0U) && (n > st->chunk)) {
            n = st->chunk;
        }
        memcpy(buf, &st->data[g_step_off], n);
        g_step_off += n;
        g_sim_now_ms += g_ms_per_data_read;
        if (g_step_off >= st->len) {
            g_step_idx++;
            g_step_off = 0U;
        }
        return (int)n;
    }
    case STEP_TIMEOUT:
        g_sim_idle_ms += timeout_ms;
        g_sim_now_ms  += timeout_ms;
        g_step_rep++;
        if (g_step_rep >= st->repeat) {
            g_step_idx++;
            g_step_rep = 0U;
        }
        return DOIP_TCP_RECV_TIMEOUT;
    case STEP_ERROR:
    default:
        g_step_idx++;
        g_step_rep = 0U;
        return TEST_RECV_HARD_ERROR;
    }
}

static void mock_tcp_close(void *conn_ctx)
{
    (void)conn_ctx;
    g_close_calls++;
    g_script_done_at_close  = script_done();
    g_sim_idle_ms_at_close  = g_sim_idle_ms;
    g_sim_now_ms_at_close   = g_sim_now_ms;
}

static void mock_tcp_server_close(void *server_ctx)
{
    (void)server_ctx;
}

static uint32_t mock_uptime_ms(void)
{
    return g_sim_now_ms;
}

/* [EDS#434] ecu_reset op: records instead of resetting, then returns (as
 * for an unsupported type), so the server carries on with the script. */
static uint32_t g_reset_calls;
static uint8_t  g_reset_type;
static uint32_t g_resp_count_at_reset;
static bool     g_locked_at_reset;
static bool     g_locked;

static void mock_ecu_reset(uint8_t reset_type);

static const eds_doip_platform_ops_t g_mock_ops = {
    .tcp_listen        = mock_tcp_listen,
    .tcp_accept        = mock_tcp_accept,
    .tcp_send          = mock_tcp_send,
    .tcp_recv          = mock_tcp_recv,
    .tcp_close         = mock_tcp_close,
    .tcp_server_close  = mock_tcp_server_close,
    .uptime_ms         = mock_uptime_ms,
    .ecu_reset         = mock_ecu_reset,
};

static void checkin_cb(void)
{
    g_checkins++;
}

/* =========================================================================
 * UDS context: one TesterPresent handler that records the session it ran in
 * ========================================================================= */

static uds_session_ctx_t   g_session_ctx;
static uds_security_ctx_t  g_security_ctx;
static uds_server_ctx_t    g_uds_ctx;

static uds_status_t handler_tester_present(uds_server_ctx_t *ctx,
                                            const uds_msg_buf_t *req,
                                            uds_msg_buf_t *resp)
{
    (void)req;
    g_tp_calls++;
    (void)uds_session_get_active(ctx->cfg.session_ctx, &g_session_at_last_tp);
    resp->data[0] = 0x7EU;
    resp->data[1] = 0x00U;
    resp->length  = 2U;
    return UDS_STATUS_OK;
}

static bool stub_key_validate(uint8_t level, const uint8_t *seed, uint8_t seed_len,
                               const uint8_t *key, uint8_t key_len)
{
    (void)level; (void)seed; (void)seed_len; (void)key; (void)key_len;
    return true;
}

static uds_status_t stub_seed_gen(uint8_t level, uint8_t *seed_buf,
                                   uint8_t seed_buf_len, uint8_t *out_seed_len)
{
    (void)level; (void)seed_buf_len;
    seed_buf[0] = 0xAAU;
    *out_seed_len = 1U;
    return UDS_STATUS_OK;
}

/* [EDS#423] A service that answers 7F 22 78 and finishes later, as
 * RequestDownload's chunked erase does. */
static uds_status_t handler_pending_0x22(uds_server_ctx_t *ctx,
                                          const uds_msg_buf_t *req,
                                          uds_msg_buf_t *resp)
{
    (void)ctx; (void)req;
    resp->data[0] = 0x7FU;
    resp->data[1] = 0x22U;
    resp->data[2] = 0x78U;
    resp->length  = 3U;
    return UDS_STATUS_OK;
}

/* [EDS#423] Scripted continuation: g_pending_left more 0x78 frames, then
 * the final 62 response; or 0x78 forever when g_pending_forever. Each call
 * advances the simulated clock by g_pending_step_ms (one erase step). */
static uint32_t g_pending_calls;
static uint32_t g_pending_left;
static bool     g_pending_forever;
static uint32_t g_pending_step_ms;
static uint32_t g_pending_calls_unlocked;
static uint32_t g_checkins_at_pending;

static void mock_ecu_reset(uint8_t reset_type)
{
    g_reset_calls++;
    g_reset_type          = reset_type;
    g_resp_count_at_reset = g_resp_count;
    g_locked_at_reset     = g_locked;
}

static void lock_cb(void)   { g_locked = true; }
static void unlock_cb(void) { g_locked = false; }

static bool pending_cb(uds_msg_buf_t *out)
{
    g_pending_calls++;
    if (!g_locked) {
        g_pending_calls_unlocked++;
    }
    g_checkins_at_pending = g_checkins;
    g_sim_now_ms += g_pending_step_ms;
    /* A server that never bounds the phase would loop here forever: leave
     * the server instead, so the test fails on the call count, not a hang. */
    if (g_pending_forever && (g_pending_calls > 1000U)) {
        longjmp(g_server_exit, 1);
    }
    out->data[0] = 0x7FU;
    out->data[1] = 0x22U;
    out->data[2] = 0x78U;
    out->length  = 3U;
    if (!g_pending_forever) {
        if (g_pending_left == 0U) {
            out->data[0] = 0x62U;
            out->data[1] = 0xF1U;
            out->data[2] = 0x90U;
        } else {
            g_pending_left--;
        }
    }
    return true;
}

static uds_server_ctx_t *uds_ctx(void)
{
    static bool done;
    if (!done) {
        static const uds_service_entry_t table[] = {
            { 0x3EU, handler_tester_present, false },
            { 0x22U, handler_pending_0x22, false },
            { 0x11U, uds_service_0x11_handler, true },   /* [EDS#434] */
        };
        static const uds_security_cfg_t sec_cfg = {
            .max_attempts     = 3U,
            .lockout_ms       = 10000U,
            .key_validate_cb  = stub_key_validate,
            .seed_generate_cb = stub_seed_gen,
        };
        uds_server_cfg_t srv_cfg = {
            .p2_server_max_ms      = 50U,
            .p2_star_server_max_ms = 5000U,
            .session_ctx           = &g_session_ctx,
            .security_ctx          = &g_security_ctx,
            .service_table         = table,
            .service_table_count   = 3U,
            .access_table          = NULL,
            .access_table_count    = 0U,
        };
        (void)uds_session_init(&g_session_ctx, 5000U);
        (void)uds_security_init(&g_security_ctx, &sec_cfg);
        (void)uds_server_init(&g_uds_ctx, &srv_cfg);
        done = true;
    }
    return &g_uds_ctx;
}

/* =========================================================================
 * Frames
 * ========================================================================= */

static const uint8_t k_routing_activation[] = {
    0x02U, 0xFDU, 0x00U, 0x05U, 0x00U, 0x00U, 0x00U, 0x07U,
    (uint8_t)(TEST_TESTER_ADDR >> 8U), (uint8_t)(TEST_TESTER_ADDR & 0xFFU),
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};

/* ReadDataByIdentifier F190, answered through the pending handler above. */
static const uint8_t k_rdbi_pending[] = {
    0x02U, 0xFDU, 0x80U, 0x01U, 0x00U, 0x00U, 0x00U, 0x07U,
    (uint8_t)(TEST_TESTER_ADDR >> 8U), (uint8_t)(TEST_TESTER_ADDR & 0xFFU),
    (uint8_t)(TEST_ECU_ADDR >> 8U),    (uint8_t)(TEST_ECU_ADDR & 0xFFU),
    0x22U, 0xF1U, 0x90U,
};

/* [EDS#434] ECUReset hardReset, and the same with the suppress bit. */
static const uint8_t k_ecu_reset_hard[] = {
    0x02U, 0xFDU, 0x80U, 0x01U, 0x00U, 0x00U, 0x00U, 0x06U,
    (uint8_t)(TEST_TESTER_ADDR >> 8U), (uint8_t)(TEST_TESTER_ADDR & 0xFFU),
    (uint8_t)(TEST_ECU_ADDR >> 8U),    (uint8_t)(TEST_ECU_ADDR & 0xFFU),
    0x11U, 0x01U,
};
static const uint8_t k_ecu_reset_hard_suppressed[] = {
    0x02U, 0xFDU, 0x80U, 0x01U, 0x00U, 0x00U, 0x00U, 0x06U,
    (uint8_t)(TEST_TESTER_ADDR >> 8U), (uint8_t)(TEST_TESTER_ADDR & 0xFFU),
    (uint8_t)(TEST_ECU_ADDR >> 8U),    (uint8_t)(TEST_ECU_ADDR & 0xFFU),
    0x11U, 0x81U,
};

static const uint8_t k_tester_present[] = {
    0x02U, 0xFDU, 0x80U, 0x01U, 0x00U, 0x00U, 0x00U, 0x06U,
    (uint8_t)(TEST_TESTER_ADDR >> 8U), (uint8_t)(TEST_TESTER_ADDR & 0xFFU),
    (uint8_t)(TEST_ECU_ADDR >> 8U),    (uint8_t)(TEST_ECU_ADDR & 0xFFU),
    0x3EU, 0x00U,
};

/* DiagnosticMessage header announcing a 300-byte payload — only the
 * header; the payload is fed one byte per read (EDS#193 trickle). */
static const uint8_t k_diag_hdr_300[] = {
    0x02U, 0xFDU, 0x80U, 0x01U, 0x00U, 0x00U, 0x01U, 0x2CU,
};

/* =========================================================================
 * Harness
 * ========================================================================= */

static doip_server_state_t g_doip;

static void reset_observations(void)
{
    script_reset();
    g_accept_timeouts       = 0U;
    g_conn_given            = false;
    g_accept_calls          = 0U;
    g_close_calls           = 0U;
    g_script_done_at_close  = false;
    g_sim_idle_ms           = 0U;
    g_sim_idle_ms_at_close  = 0U;
    g_sim_now_ms            = 0U;
    g_ms_per_data_read      = 0U;
    g_conn_open_ms          = 0U;
    g_sim_now_ms_at_close   = 0U;
    g_recv_calls            = 0U;
    g_last_recv_timeout_ms  = 0U;
    g_checkins              = 0U;
    g_tp_calls              = 0U;
    g_session_at_last_tp    = UDS_SESSION_DEFAULT;

    TEST_ASSERT_EQUAL(UDS_STATUS_OK, eds_doip_register_platform(&g_mock_ops));
    eds_doip_set_checkin_callback(NULL);
    eds_doip_set_pending_callback(NULL);
    eds_doip_set_lock_callbacks(NULL, NULL);
    g_resp_count             = 0U;
    g_pending_calls          = 0U;
    g_pending_left           = 0U;
    g_pending_forever        = false;
    g_pending_step_ms        = 0U;
    g_locked                 = false;
    g_pending_calls_unlocked = 0U;
    g_checkins_at_pending    = 0U;
    g_reset_calls            = 0U;
    g_reset_type             = 0U;
    g_resp_count_at_reset    = 0U;
    g_locked_at_reset        = false;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, eds_doip_server_init(&g_doip, TEST_ECU_ADDR));

    /* Every scenario starts in the extended session so a test can see
     * whether a disconnect's EDS#191 reset ran in between two requests. */
    (void)uds_session_transition(uds_ctx()->cfg.session_ctx, UDS_SESSION_DEFAULT);
    (void)uds_session_transition(uds_ctx()->cfg.session_ctx, UDS_SESSION_EXTENDED);
}

/* Run the server until the scripted scenario has played out. */
static void run_server(void)
{
    if (setjmp(g_server_exit) == 0) {
        (void)eds_doip_server_run(&g_doip, uds_ctx(), DOIP_PORT);
        TEST_FAIL_MESSAGE("eds_doip_server_run() returned unexpectedly");
    }
}

/* =========================================================================
 * Inactivity (EDS#400)
 * ========================================================================= */

/* A tester pausing 3 s between two requests — the TesterPresent keep-alive
 * pattern — keeps its connection, and with it its session. */
ZTEST(doip_lifecycle_suite, test_doip_idle_gap_holds_session)
{
    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_tester_present, sizeof(k_tester_present));
    add_timeouts(3000U / DOIP_IDLE_POLL_MS);
    add_data(k_tester_present, sizeof(k_tester_present));
    run_server();

    EXPECT_EQ_MSG(2U, g_tp_calls,
        "TesterPresent after a 3 s idle gap was never dispatched: "
        "the server dropped an idle connection (EDS#400)");
    EXPECT_TRUE_MSG(g_session_at_last_tp == UDS_SESSION_EXTENDED,
        "session was reset across the idle gap (EDS#400)");
    EXPECT_TRUE_MSG(g_script_done_at_close,
        "server closed the connection before the peer did");
}

/* A connection silent after routing activation is closed after ISO 13400-2
 * T_TCP_General_Inactivity (5 min by default) — not before, and not never. */
ZTEST(doip_lifecycle_suite, test_doip_general_inactivity_closes_at_iso_default)
{
    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_timeouts(2U * DOIP_GENERAL_INACTIVITY_POLLS);
    run_server();

    TEST_ASSERT_EQUAL_UINT32(1U, g_close_calls);
    EXPECT_TRUE_MSG(!g_script_done_at_close,
        "silent connection was never closed for inactivity");
    EXPECT_EQ_MSG(
        DOIP_GENERAL_INACTIVITY_POLLS * DOIP_IDLE_POLL_MS, g_sim_idle_ms_at_close,
        "idle connection closed at the wrong time (EDS#400)");
    EXPECT_TRUE_MSG(g_sim_idle_ms_at_close >= 300000U,
        "default general inactivity is below the ISO 13400-2 5 min default");
}

/* A client that connects and never activates routing is released after
 * ISO 13400-2 T_TCP_Initial_Inactivity (2 s) — the fast release the old
 * 150 ms read timeout gave every idle connection is kept for this case. */
ZTEST(doip_lifecycle_suite, test_doip_initial_inactivity_without_routing_activation)
{
    reset_observations();
    add_timeouts(100U);
    run_server();

    TEST_ASSERT_EQUAL_UINT32(1U, g_close_calls);
    EXPECT_EQ_MSG(2000U, g_sim_idle_ms_at_close,
        "connection without routing activation not released at 2 s");
}

/* Total idleness per connection is bounded, so a client sending one frame
 * just inside every inactivity window still cannot hold the single serial
 * slot indefinitely. */
ZTEST(doip_lifecycle_suite, test_doip_connection_idle_budget_bounds_lifetime)
{
    const uint32_t gap = DOIP_GENERAL_INACTIVITY_POLLS - 1U;
    const uint32_t gaps_needed =
        (DOIP_MAX_CONNECTION_IDLE_POLLS + gap - 1U) / gap;

    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    for (uint32_t i = 0U; i < (gaps_needed + 2U); i++) {
        add_timeouts(gap);
        add_data(k_tester_present, sizeof(k_tester_present));
    }
    run_server();

    EXPECT_TRUE_MSG(!g_script_done_at_close,
        "slow-but-live client held the connection past the idle budget");
    TEST_ASSERT_EQUAL_UINT32(DOIP_MAX_CONNECTION_IDLE_POLLS * DOIP_IDLE_POLL_MS,
                             g_sim_idle_ms_at_close);
    TEST_ASSERT_EQUAL_UINT32(gaps_needed - 1U, g_tp_calls);
}

/* =========================================================================
 * Guards that must not regress
 * ========================================================================= */

/* Once a frame has started, a stall still ends the connection on the first
 * DOIP_TCP_RECV_TIMEOUT_MS read — the long idle wait applies only between
 * frames. */
ZTEST(doip_lifecycle_suite, test_doip_partial_frame_stall_still_closes_fast)
{
    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_tester_present, 3U);
    add_timeouts(10U);
    run_server();

    TEST_ASSERT_EQUAL_UINT32(1U, g_close_calls);
    TEST_ASSERT_EQUAL_UINT32(DOIP_TCP_RECV_TIMEOUT_MS, g_sim_idle_ms_at_close);
    TEST_ASSERT_EQUAL_UINT32(DOIP_TCP_RECV_TIMEOUT_MS, g_last_recv_timeout_ms);
}

/* A dead socket is not mistaken for a quiet one: a hard recv error while
 * waiting for the next frame closes at once instead of burning idle polls. */
ZTEST(doip_lifecycle_suite, test_doip_hard_error_while_idle_closes_immediately)
{
    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_error();
    add_data(k_tester_present, sizeof(k_tester_present));
    run_server();

    TEST_ASSERT_EQUAL_UINT32(1U, g_close_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_tp_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_sim_idle_ms_at_close);
}

/* [EDS#193] A payload trickled one byte per read still hits the read-attempt
 * cap and is never dispatched. */
ZTEST(doip_lifecycle_suite, test_doip_payload_trickle_still_capped)
{
    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_diag_hdr_300, sizeof(k_diag_hdr_300));
    static const uint8_t payload[300];
    add_data(payload, sizeof(payload));
    g_steps[g_step_count - 1U].chunk = 1U;
    run_server();

    TEST_ASSERT_EQUAL_UINT32(1U, g_close_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_tp_calls);
    TEST_ASSERT_FALSE(g_script_done_at_close);
}

/* =========================================================================
 * Connection lifetime (EDS#412)
 * ========================================================================= */

/* Enough TesterPresent frames for either test below. The old cap counted
 * frames whatever the SID, so each stands in for a TransferData block. */
#define LIFETIME_TEST_MAX_FRAMES (2100U)
static uint8_t g_many_tp[LIFETIME_TEST_MAX_FRAMES * sizeof(k_tester_present)];

static void add_tester_presents(uint32_t n)
{
    for (uint32_t i = 0U; i < n; i++) {
        memcpy(&g_many_tp[i * sizeof(k_tester_present)], k_tester_present,
               sizeof(k_tester_present));
    }
    add_data(g_many_tp, (size_t)n * sizeof(k_tester_present));
}

/* A 2 MB download (2000 x 1 KB blocks) finishes on one connection, with
 * its session intact. The EDS#218 frame cap closed the connection after 500
 * frames, ~0.5 MB, and reset session and security with it. */
ZTEST(doip_lifecycle_suite, test_doip_long_download_not_cut_off)
{
    const uint32_t blocks = 2000U;

    reset_observations();
    g_ms_per_data_read = 5U;    /* 10 ms per frame: 20 s for the download */
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_tester_presents(blocks);
    run_server();

    EXPECT_EQ_MSG(blocks, g_tp_calls,
        "download cut off: the server closed the connection after a count of "
        "frames, not a time (EDS#412)");
    EXPECT_TRUE_MSG(g_script_done_at_close,
        "server closed a 20 s download before the tester did (EDS#412)");
    EXPECT_TRUE_MSG(g_session_at_last_tp == UDS_SESSION_EXTENDED,
        "session was reset during the download (EDS#412)");
    EXPECT_EQ_MSG(0U, g_doip.connections_lifetime_capped,
        "a 20 s connection was counted as lifetime-capped");
}

/* [EDS#218] The protection the frame cap gave must survive its removal: a
 * client sending a frame every 900 ms is never idle, so no idle bound ever
 * fires, and only the connection lifetime frees the slot. The clock starts
 * just below UINT32_MAX and wraps mid-connection. */
ZTEST(doip_lifecycle_suite, test_doip_busy_client_loses_slot_at_lifetime)
{
    const uint32_t frame_ms = 900U;
    const uint32_t frames   = (DOIP_MAX_CONNECTION_LIFETIME_MS / frame_ms) + 50U;

    EXPECT_TRUE_MSG(frames <= LIFETIME_TEST_MAX_FRAMES,
        "test buffer too small for DOIP_MAX_CONNECTION_LIFETIME_MS");
    reset_observations();
    g_sim_now_ms       = 0xFFF00000U;   /* wraps ~17 min into the connection */
    g_ms_per_data_read = frame_ms / 2U; /* header read + payload read */
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_tester_presents(frames);
    run_server();

    const uint32_t age = g_sim_now_ms_at_close - g_conn_open_ms;

    EXPECT_TRUE_MSG(!g_script_done_at_close,
        "a never-idle client held the single connection slot past "
        "DOIP_MAX_CONNECTION_LIFETIME_MS (EDS#218 protection lost)");
    EXPECT_EQ_MSG(0U, g_sim_idle_ms,
        "client went idle: the test no longer isolates the lifetime bound");
    EXPECT_TRUE_MSG(age >= DOIP_MAX_CONNECTION_LIFETIME_MS,
        "connection closed before DOIP_MAX_CONNECTION_LIFETIME_MS");
    EXPECT_TRUE_MSG(age < (DOIP_MAX_CONNECTION_LIFETIME_MS + frame_ms),
        "connection outlived DOIP_MAX_CONNECTION_LIFETIME_MS by more than one frame");
    EXPECT_EQ_MSG(1U, g_doip.connections_lifetime_capped,
        "lifetime close not counted in connections_lifetime_capped");
}

/* =========================================================================
 * Response-pending continuation (EDS#423)
 * ========================================================================= */

/* No continuation registered: the server sends the 7F 22 78 and nothing
 * more, as before, and goes on serving the connection. */
ZTEST(doip_lifecycle_suite, test_doip_pending_without_callback_sends_only_0x78)
{
    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_rdbi_pending, sizeof(k_rdbi_pending));
    add_data(k_tester_present, sizeof(k_tester_present));
    run_server();

    EXPECT_EQ_MSG(2U, g_resp_count, "unexpected response count without a continuation");
    EXPECT_EQ_MSG(0x78U, g_resp[0][2], "first response is not the 0x78");
    EXPECT_EQ_MSG(0x7EU, g_resp[1][0], "connection not served after the 0x78");
}

/* With a continuation: the 0x78 from dispatch, two more from the
 * continuation, then the final response, in that order, each continuation
 * call made inside the lock pair and after a check-in. Before EDS#423 the
 * tester got the first 0x78 and never a final response. */
ZTEST(doip_lifecycle_suite, test_doip_pending_callback_completes_exchange)
{
    reset_observations();
    eds_doip_set_lock_callbacks(lock_cb, unlock_cb);
    eds_doip_set_checkin_callback(checkin_cb);
    eds_doip_set_pending_callback(pending_cb);
    g_pending_left = 2U;
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_rdbi_pending, sizeof(k_rdbi_pending));
    add_data(k_tester_present, sizeof(k_tester_present));
    run_server();

    EXPECT_EQ_MSG(3U, g_pending_calls, "continuation not driven to the final response");
    EXPECT_EQ_MSG(5U, g_resp_count, "wrong number of responses sent");
    EXPECT_EQ_MSG(0x78U, g_resp[0][2], "response 1 is not 0x78");
    EXPECT_EQ_MSG(0x78U, g_resp[1][2], "response 2 is not 0x78");
    EXPECT_EQ_MSG(0x78U, g_resp[2][2], "response 3 is not 0x78");
    EXPECT_EQ_MSG(0x62U, g_resp[3][0], "final response missing (EDS#423)");
    EXPECT_EQ_MSG(0x7EU, g_resp[4][0], "connection not served after the exchange");
    EXPECT_EQ_MSG(0U, g_pending_calls_unlocked,
        "continuation called outside the EDS#353 lock pair");
    EXPECT_TRUE_MSG(g_checkins_at_pending > 0U,
        "no supervision check-in during the pending phase");
}

/* A continuation that never finishes is cut off at DOIP_MAX_PENDING_MS and
 * the connection closed, so a stuck job cannot hold the slot. */
ZTEST(doip_lifecycle_suite, test_doip_pending_phase_is_time_bounded)
{
    const uint32_t step_ms = 4000U;

    reset_observations();
    eds_doip_set_pending_callback(pending_cb);
    g_pending_forever = true;
    g_pending_step_ms = step_ms;
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_rdbi_pending, sizeof(k_rdbi_pending));
    add_data(k_tester_present, sizeof(k_tester_present));
    run_server();

    EXPECT_TRUE_MSG(!g_script_done_at_close,
        "stuck response-pending phase never ended (EDS#423)");
    EXPECT_EQ_MSG(DOIP_MAX_PENDING_MS / step_ms, g_pending_calls,
        "pending phase not bounded at DOIP_MAX_PENDING_MS");
}

/* =========================================================================
 * Supervision check-in (EDS#399)
 * ========================================================================= */

/* No client at all: every accept poll wake is a check-in, so an idle
 * server never looks hung to a supervisor. */
ZTEST(doip_lifecycle_suite, test_doip_checkin_while_idle_without_client)
{
    reset_observations();
    eds_doip_set_checkin_callback(checkin_cb);
    g_accept_timeouts = 5U;
    run_server();

    EXPECT_EQ_MSG(5U, g_checkins,
        "idle accept polls did not check in (EDS#399)");
}

/* A connected client that is quiet between requests: every idle poll is a
 * check-in, so the supervisor's window only needs to cover one poll. */
ZTEST(doip_lifecycle_suite, test_doip_checkin_while_connected_idle)
{
    reset_observations();
    eds_doip_set_checkin_callback(checkin_cb);
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_timeouts(10U);
    run_server();

    /* At least one check-in per recv return. */
    EXPECT_TRUE_MSG(g_checkins >= g_recv_calls,
        "connected idle polls did not check in (EDS#399)");
}

/* NULL clears the callback; the loop runs exactly as without one. */
ZTEST(doip_lifecycle_suite, test_doip_checkin_cleared_is_never_called)
{
    reset_observations();
    eds_doip_set_checkin_callback(checkin_cb);
    eds_doip_set_checkin_callback(NULL);
    g_accept_timeouts = 3U;
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_timeouts(3U);
    add_data(k_tester_present, sizeof(k_tester_present));
    run_server();

    TEST_ASSERT_EQUAL_UINT32(0U, g_checkins);
    TEST_ASSERT_EQUAL_UINT32(1U, g_tp_calls);
}

/* =========================================================================
 * ECUReset performed by the server (EDS#434)
 * ========================================================================= */

/* 11 01: the server sends 51 01, then calls ecu_reset with the type, outside
 * the lock pair. Before EDS#434 nothing called it and the ECU kept running. */
ZTEST(doip_lifecycle_suite, test_doip_ecu_reset_performed_after_response)
{
    reset_observations();
    eds_doip_set_lock_callbacks(lock_cb, unlock_cb);
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_ecu_reset_hard, sizeof(k_ecu_reset_hard));
    add_data(k_tester_present, sizeof(k_tester_present));
    run_server();

    EXPECT_EQ_MSG(1U, g_reset_calls,
        "ECUReset answered but never performed: ecu_reset op not called (EDS#434)");
    EXPECT_EQ_MSG(0x01U, g_reset_type, "ecu_reset got the wrong reset type");
    EXPECT_EQ_MSG(0x51U, g_resp[0][0], "positive response 51 missing");
    EXPECT_EQ_MSG(1U, g_resp_count_at_reset,
        "reset performed before the 51 01 response was sent");
    EXPECT_TRUE_MSG(!g_locked_at_reset, "ecu_reset called while holding the lock pair");
}

/* 11 81: no response is sent, and the reset is still performed. */
ZTEST(doip_lifecycle_suite, test_doip_ecu_reset_suppressed_response_still_resets)
{
    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_ecu_reset_hard_suppressed, sizeof(k_ecu_reset_hard_suppressed));
    add_data(k_tester_present, sizeof(k_tester_present));
    run_server();

    EXPECT_EQ_MSG(1U, g_reset_calls,
        "suppressed ECUReset never performed (EDS#434)");
    EXPECT_EQ_MSG(0x01U, g_reset_type, "suppress bit leaked into the reset type");
    EXPECT_EQ_MSG(0U, g_resp_count_at_reset, "a response was sent for 11 81");
    EXPECT_EQ_MSG(0x7EU, g_resp[0][0], "connection not served after the reset returned");
}

/* Ordinary requests never reach ecu_reset. */
ZTEST(doip_lifecycle_suite, test_doip_no_reset_without_ecu_reset_request)
{
    reset_observations();
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_tester_present, sizeof(k_tester_present));
    add_data(k_rdbi_pending, sizeof(k_rdbi_pending));
    run_server();

    EXPECT_EQ_MSG(0U, g_reset_calls, "ecu_reset called without an ECUReset request");
}

/* [SEC-SESS-COMMRESTORE-01] A dropped connection returns the session to
 * DEFAULT, so CommunicationControl and ControlDTCSetting must be reset too
 * (ISO 14229-1:2026 §9.2.1 d)), or the ECU stays silent with DTC recording
 * off after the tester disappears. */
ZTEST(doip_lifecycle_suite, test_doip_disconnect_restores_comm_and_dtc_setting)
{
    uds_comm_mode_t        m = UDS_COMM_MODE_DISABLE_RX_TX;
    uds_dtc_setting_mode_t d = UDS_DTC_SETTING_OFF;

    reset_observations();
    uds_comm_control_test_reset();
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_comm_control_init(NULL));
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_comm_control_apply(0x03U, 0x03U));
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, uds_comm_control_set_dtc_setting(0x02U));
    add_data(k_routing_activation, sizeof(k_routing_activation));
    add_data(k_tester_present, sizeof(k_tester_present));
    add_error();
    run_server();

    (void)uds_comm_control_get_mode(&m);
    (void)uds_comm_control_get_dtc_setting(&d);
    EXPECT_EQ_MSG(UDS_COMM_MODE_ENABLE_RX_TX, m,
        "communication left disabled after DoIP disconnect (SEC-SESS-COMMRESTORE-01)");
    EXPECT_EQ_MSG(UDS_DTC_SETTING_ON, d,
        "DTC setting left off after DoIP disconnect (SEC-SESS-COMMRESTORE-01)");
}

ZTEST_SUITE(doip_lifecycle_suite, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * Unity runner infrastructure
 * ========================================================================= */

void setUp(void)    { }
void tearDown(void) { }

void run_all_tests(void)
{
    RUN_TEST(doip_lifecycle_suite__test_doip_idle_gap_holds_session);
    RUN_TEST(doip_lifecycle_suite__test_doip_general_inactivity_closes_at_iso_default);
    RUN_TEST(doip_lifecycle_suite__test_doip_initial_inactivity_without_routing_activation);
    RUN_TEST(doip_lifecycle_suite__test_doip_connection_idle_budget_bounds_lifetime);
    RUN_TEST(doip_lifecycle_suite__test_doip_partial_frame_stall_still_closes_fast);
    RUN_TEST(doip_lifecycle_suite__test_doip_hard_error_while_idle_closes_immediately);
    RUN_TEST(doip_lifecycle_suite__test_doip_payload_trickle_still_capped);
    RUN_TEST(doip_lifecycle_suite__test_doip_long_download_not_cut_off);
    RUN_TEST(doip_lifecycle_suite__test_doip_busy_client_loses_slot_at_lifetime);
    RUN_TEST(doip_lifecycle_suite__test_doip_pending_without_callback_sends_only_0x78);
    RUN_TEST(doip_lifecycle_suite__test_doip_pending_callback_completes_exchange);
    RUN_TEST(doip_lifecycle_suite__test_doip_pending_phase_is_time_bounded);
    RUN_TEST(doip_lifecycle_suite__test_doip_ecu_reset_performed_after_response);
    RUN_TEST(doip_lifecycle_suite__test_doip_ecu_reset_suppressed_response_still_resets);
    RUN_TEST(doip_lifecycle_suite__test_doip_no_reset_without_ecu_reset_request);
    RUN_TEST(doip_lifecycle_suite__test_doip_disconnect_restores_comm_and_dtc_setting);
    RUN_TEST(doip_lifecycle_suite__test_doip_checkin_while_idle_without_client);
    RUN_TEST(doip_lifecycle_suite__test_doip_checkin_while_connected_idle);
    RUN_TEST(doip_lifecycle_suite__test_doip_checkin_cleared_is_never_called);
}
