// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_doip_discovery.c
 *
 * MODULE UNDER TEST: transport/doip/doip_server.c — vehicle discovery over
 *                    UDP (EDS#392)
 *
 * PURPOSE:
 *   Two layers.
 *   1. doip_handle_udp_datagram(), byte-exact: vehicle identification
 *      (0x0001/0x0002/0x0003 -> 0x0004), entity status (0x4001 -> 0x4002),
 *      power mode (0x4003 -> 0x4004), the generic header NACK table, the
 *      response types that are never answered, and the reply rate cap.
 *   2. eds_doip_server_run() on a simulated clock: the announcement
 *      schedule (ISO 13400-2 A_DoIP_Announce_*), both opt-outs, answers
 *      served inside the idle waits, EDS#400 idle accounting unchanged
 *      under UDP wakes, the per-wake datagram bound, and a failed socket
 *      leaving the server TCP-only.
 *
 *   wait_readable() is the simulated clock: it advances to the next
 *   scripted event (a datagram arriving, a connection pending) or by its
 *   whole timeout, and leaves the server (longjmp) at g_end_ms. Sends are
 *   recorded and counted here and asserted only in test bodies (zassert in
 *   a callback does not abort the test).
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
#include "uds_types.h"

#define TEST_ECU_ADDR     (0x0E00U)

#define SERVER_CTX ((void *)(uintptr_t)0xDEADBEEFU)
#define CONN_CTX   ((void *)(uintptr_t)0xCAFEBABEU)
#define UDP_CTX    ((void *)(uintptr_t)0x0DD0D0D0U)

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
 * Simulated network
 * ========================================================================= */

#define MAX_DGRAMS 32U
#define MAX_SENDS  64U

typedef struct {
    uint32_t at_ms;
    uint8_t  data[80];
    uint32_t len;
    eds_doip_udp_peer_t peer;
} dgram_t;

typedef struct {
    uint32_t at_ms;
    eds_doip_udp_peer_t peer;
    uint8_t  data[64];
    uint32_t len;
} sent_t;

static dgram_t  g_rx[MAX_DGRAMS];
static uint32_t g_rx_count;
static uint32_t g_rx_next;          /* next datagram not yet read */

static sent_t   g_sent[MAX_SENDS];
static uint32_t g_sent_count;

static uint32_t g_now_ms;
static uint32_t g_end_ms;
static jmp_buf  g_server_exit;

static bool     g_udp_open_fails;
static uint32_t g_udp_open_calls;
static uint32_t g_wait_calls;
static uint32_t g_recvfrom_since_wait;
static uint32_t g_recvfrom_max_per_wake;
static uint32_t g_checkins;

/* TCP: a connection becomes pending at g_conn_at_ms (UINT32_MAX: never). */
static uint32_t g_conn_at_ms;
static bool     g_conn_given;
static uint32_t g_tcp_close_at_ms;
static uint32_t g_tcp_accept_calls;

static uint8_t  g_eid[DOIP_EID_LEN] = { 0x02U, 0x80U, 0xE1U, 0x00U, 0x00U, 0x01U };

static void net_reset(void)
{
    memset(g_rx, 0, sizeof(g_rx));
    memset(g_sent, 0, sizeof(g_sent));
    g_rx_count = 0U;
    g_rx_next = 0U;
    g_sent_count = 0U;
    g_now_ms = 0U;
    g_end_ms = 0U;
    g_udp_open_fails = false;
    g_udp_open_calls = 0U;
    g_wait_calls = 0U;
    g_recvfrom_since_wait = 0U;
    g_recvfrom_max_per_wake = 0U;
    g_checkins = 0U;
    g_conn_at_ms = UINT32_MAX;
    g_conn_given = false;
    g_tcp_close_at_ms = 0U;
    g_tcp_accept_calls = 0U;
}

static void add_dgram(uint32_t at_ms, const uint8_t *data, uint32_t len)
{
    if (g_rx_count >= MAX_DGRAMS) {
        return;
    }
    g_rx[g_rx_count].at_ms = at_ms;
    memcpy(g_rx[g_rx_count].data, data, len);
    g_rx[g_rx_count].len = len;
    g_rx[g_rx_count].peer.addr = 0xC0000202U;   /* 192.0.2.2 */
    g_rx[g_rx_count].peer.port = 50123U;
    g_rx_count++;
}

static bool udp_ready(void)
{
    return (g_rx_next < g_rx_count) && (g_rx[g_rx_next].at_ms <= g_now_ms);
}

static bool tcp_ready(void *tcp_ctx)
{
    /* Only a pending connection is modelled; a connection never has data. */
    return (tcp_ctx == SERVER_CTX) && !g_conn_given && (g_conn_at_ms <= g_now_ms);
}

static int mock_tcp_listen(uint16_t port, void **server_ctx)
{
    (void)port;
    *server_ctx = SERVER_CTX;
    return 0;
}

static int mock_tcp_accept(void *server_ctx, void **conn_ctx, uint32_t timeout_ms)
{
    g_tcp_accept_calls++;
    if (tcp_ready(server_ctx)) {
        g_conn_given = true;
        *conn_ctx = CONN_CTX;
        return 0;
    }
    /* Only reached without UDP: plain poll timeout on the clock. */
    g_now_ms += timeout_ms;
    if (g_now_ms >= g_end_ms) {
        longjmp(g_server_exit, 1);
    }
    return -11;
}

static int mock_tcp_send(void *conn_ctx, const uint8_t *data, size_t len)
{
    (void)conn_ctx; (void)data;
    return (int)len;
}

static int mock_tcp_recv(void *conn_ctx, uint8_t *buf, size_t buf_len, uint32_t timeout_ms)
{
    (void)conn_ctx; (void)buf; (void)buf_len;
    g_now_ms += timeout_ms;
    return DOIP_TCP_RECV_TIMEOUT;
}

static void mock_tcp_close(void *conn_ctx)
{
    (void)conn_ctx;
    g_tcp_close_at_ms = g_now_ms;
}

static void mock_tcp_server_close(void *server_ctx) { (void)server_ctx; }
static uint32_t mock_uptime_ms(void) { return g_now_ms; }
static void mock_ecu_reset(uint8_t reset_type) { (void)reset_type; }

static int mock_udp_open(uint16_t port, void **udp_ctx)
{
    g_udp_open_calls++;
    if (g_udp_open_fails || (port != DOIP_PORT)) {
        return -12;
    }
    *udp_ctx = UDP_CTX;
    return 0;
}

static int mock_udp_recvfrom(void *udp_ctx, uint8_t *buf, size_t buf_len,
                             eds_doip_udp_peer_t *peer)
{
    (void)udp_ctx;
    if (!udp_ready()) {
        return DOIP_UDP_NO_DATA;
    }
    g_recvfrom_since_wait++;
    if (g_recvfrom_since_wait > g_recvfrom_max_per_wake) {
        g_recvfrom_max_per_wake = g_recvfrom_since_wait;
    }
    dgram_t *d = &g_rx[g_rx_next++];
    uint32_t n = (d->len < (uint32_t)buf_len) ? d->len : (uint32_t)buf_len;
    memcpy(buf, d->data, n);
    *peer = d->peer;
    return (int)n;
}

static int mock_udp_sendto(void *udp_ctx, const eds_doip_udp_peer_t *peer,
                           const uint8_t *data, size_t len)
{
    (void)udp_ctx;
    if (g_sent_count < MAX_SENDS) {
        sent_t *s = &g_sent[g_sent_count];
        s->at_ms = g_now_ms;
        s->peer  = *peer;
        s->len   = (uint32_t)len;
        memcpy(s->data, data, (len < sizeof(s->data)) ? len : sizeof(s->data));
    }
    g_sent_count++;
    return (int)len;
}

static void mock_udp_close(void *udp_ctx) { (void)udp_ctx; }

static int mock_wait_readable(void *tcp_ctx, void *udp_ctx, uint32_t timeout_ms)
{
    (void)udp_ctx;
    g_wait_calls++;
    g_recvfrom_since_wait = 0U;
    if (g_now_ms >= g_end_ms) {
        longjmp(g_server_exit, 1);
    }
    /* Next event time: a datagram arriving or a connection pending. */
    uint32_t next = UINT32_MAX;
    if (g_rx_next < g_rx_count) {
        next = g_rx[g_rx_next].at_ms;
    }
    if ((tcp_ctx == SERVER_CTX) && !g_conn_given && (g_conn_at_ms < next)) {
        next = g_conn_at_ms;
    }
    if (next < g_now_ms) {
        next = g_now_ms;
    }
    if ((next != UINT32_MAX) && ((next - g_now_ms) <= timeout_ms)) {
        g_now_ms = next;
    } else {
        g_now_ms += timeout_ms;
    }
    unsigned mask = 0U;
    if (udp_ready()) {
        mask |= DOIP_READY_UDP;
    }
    if (tcp_ready(tcp_ctx)) {
        mask |= DOIP_READY_TCP;
    }
    return (int)mask;
}

static int mock_get_eid(uint8_t eid[DOIP_EID_LEN])
{
    memcpy(eid, g_eid, DOIP_EID_LEN);
    return 0;
}

static const eds_doip_platform_ops_t g_ops_udp = {
    .tcp_listen       = mock_tcp_listen,
    .tcp_accept       = mock_tcp_accept,
    .tcp_send         = mock_tcp_send,
    .tcp_recv         = mock_tcp_recv,
    .tcp_close        = mock_tcp_close,
    .tcp_server_close = mock_tcp_server_close,
    .uptime_ms        = mock_uptime_ms,
    .ecu_reset        = mock_ecu_reset,
    .udp_open         = mock_udp_open,
    .udp_recvfrom     = mock_udp_recvfrom,
    .udp_sendto       = mock_udp_sendto,
    .udp_close        = mock_udp_close,
    .wait_readable    = mock_wait_readable,
    .get_eid          = mock_get_eid,
};

/* =========================================================================
 * UDS context and VIN source
 * ========================================================================= */

static uds_session_ctx_t   g_session_ctx;
static uds_security_ctx_t  g_security_ctx;
static uds_server_ctx_t    g_uds_ctx;

static uds_status_t handler_tester_present(uds_server_ctx_t *ctx,
                                            const uds_msg_buf_t *req,
                                            uds_msg_buf_t *resp)
{
    (void)ctx; (void)req;
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

static uds_server_ctx_t *uds_ctx(void)
{
    static bool done;
    if (!done) {
        static const uds_service_entry_t table[] = {
            { 0x3EU, handler_tester_present, false },
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
            .service_table_count   = 1U,
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

static const uint8_t k_vin[DOIP_VIN_LEN] = {
    'W', 'X', 'L', 'Q', 'I', '0', '0', '0', '0', '0', '0', '0', '0', '0', '3', '9', '2',
};

static bool     g_locked;
static uint32_t g_vin_calls;
static uint32_t g_vin_calls_unlocked;
static uds_server_ctx_t *g_vin_ctx_seen;
static uds_status_t g_vin_rc;   /* what the "safety wrapper" returns */

static void lock_cb(void)   { g_locked = true; }
static void unlock_cb(void) { g_locked = false; }

static uds_status_t vin_cb(uds_server_ctx_t *ctx, uint8_t vin[DOIP_VIN_LEN])
{
    g_vin_calls++;
    if (!g_locked) {
        g_vin_calls_unlocked++;
    }
    g_vin_ctx_seen = ctx;
    if (g_vin_rc == UDS_STATUS_OK) {
        memcpy(vin, k_vin, DOIP_VIN_LEN);
    } else {
        /* A refusing wrapper may leave junk; the server must not use it. */
        memset(vin, 0xA5, DOIP_VIN_LEN);
    }
    return g_vin_rc;
}

static uint8_t g_power_mode = DOIP_POWER_MODE_NOT_READY;
static uint8_t power_mode_cb(void) { return g_power_mode; }

/* Announce re-request from the check-in callback, once the clock passes
 * g_rerequest_at_ms (0: never). Stands in for a network event callback. */
static uint32_t g_rerequest_at_ms;
static void checkin_cb(void)
{
    g_checkins++;
    if ((g_rerequest_at_ms != 0U) && (g_now_ms >= g_rerequest_at_ms)) {
        g_rerequest_at_ms = 0U;
        eds_doip_announce_request();
    }
}

/* =========================================================================
 * Harness
 * ========================================================================= */

static doip_server_state_t g_doip;
static const eds_doip_udp_peer_t k_peer = { 0xC0000202U, 50123U };

static eds_doip_discovery_cfg_t default_cfg(void)
{
    eds_doip_discovery_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled  = true;
    cfg.announce = true;
    cfg.get_vin  = vin_cb;
    cfg.further_action = DOIP_FURTHER_ACTION_NONE;
    return cfg;
}

static void reset_all(void)
{
    net_reset();
    g_locked = false;
    g_vin_calls = 0U;
    g_vin_calls_unlocked = 0U;
    g_vin_ctx_seen = NULL;
    g_vin_rc = UDS_STATUS_OK;
    g_power_mode = DOIP_POWER_MODE_NOT_READY;
    g_rerequest_at_ms = 0U;
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, eds_doip_register_platform(&g_ops_udp));
    eds_doip_set_lock_callbacks(lock_cb, unlock_cb);
    eds_doip_set_checkin_callback(checkin_cb);
    eds_doip_set_pending_callback(NULL);
    eds_doip_discovery_cfg_t cfg = default_cfg();
    eds_doip_set_discovery(&cfg);
    TEST_ASSERT_EQUAL(UDS_STATUS_OK, eds_doip_server_init(&g_doip, TEST_ECU_ADDR));
    (void)uds_ctx();
}

/* Handler-level tests: a server state with its UDP socket "open". */
static void handler_setup(void)
{
    reset_all();
    g_doip.udp_ctx = UDP_CTX;
    g_now_ms = 10000U;
    g_doip.udp_rate_window_ms = g_now_ms;
}

static void feed(const uint8_t *data, uint32_t len)
{
    TEST_ASSERT_EQUAL(UDS_STATUS_OK,
        doip_handle_udp_datagram(&g_doip, uds_ctx(), data, len, &k_peer));
}

static void run_server(void)
{
    if (setjmp(g_server_exit) == 0) {
        (void)eds_doip_server_run(&g_doip, uds_ctx(), DOIP_PORT);
        TEST_FAIL_MESSAGE("eds_doip_server_run() returned unexpectedly");
    }
}

static uint16_t sent_type(uint32_t i)
{
    return (uint16_t)(((uint16_t)g_sent[i].data[2] << 8U) | g_sent[i].data[3]);
}

/* =========================================================================
 * Requests
 * ========================================================================= */

static const uint8_t k_vir[]      = { 0x02U, 0xFDU, 0x00U, 0x01U, 0, 0, 0, 0 };
static const uint8_t k_vir_ff[]   = { 0xFFU, 0x00U, 0x00U, 0x01U, 0, 0, 0, 0 };
static const uint8_t k_entity[]   = { 0x02U, 0xFDU, 0x40U, 0x01U, 0, 0, 0, 0 };
static const uint8_t k_power[]    = { 0x02U, 0xFDU, 0x40U, 0x03U, 0, 0, 0, 0 };

static void make_vir_eid(uint8_t out[14], const uint8_t eid[DOIP_EID_LEN])
{
    const uint8_t hdr[8] = { 0x02U, 0xFDU, 0x00U, 0x02U, 0, 0, 0, 6U };
    memcpy(out, hdr, 8U);
    memcpy(&out[8], eid, DOIP_EID_LEN);
}

static void make_vir_vin(uint8_t out[25], const uint8_t vin[DOIP_VIN_LEN])
{
    const uint8_t hdr[8] = { 0x02U, 0xFDU, 0x00U, 0x03U, 0, 0, 0, 17U };
    memcpy(out, hdr, 8U);
    memcpy(&out[8], vin, DOIP_VIN_LEN);
}

/* =========================================================================
 * 1. Vehicle identification
 * ========================================================================= */

/* 0x0001 -> 0x0004, byte-exact, to the requester's own address and port. */
ZTEST(doip_discovery_suite, test_vir_answered_byte_exact)
{
    handler_setup();
    feed(k_vir, sizeof(k_vir));

    EXPECT_EQ_MSG(1U, g_sent_count, "VIR not answered exactly once");
    uint8_t expected[40] = { 0x02U, 0xFDU, 0x00U, 0x04U, 0U, 0U, 0U, 32U };
    memcpy(&expected[8], k_vin, DOIP_VIN_LEN);
    expected[25] = 0x0EU; expected[26] = 0x00U;            /* LA */
    memcpy(&expected[27], g_eid, DOIP_EID_LEN);            /* EID */
    memcpy(&expected[33], g_eid, DOIP_GID_LEN);            /* GID = EID */
    expected[39] = DOIP_FURTHER_ACTION_NONE;
    EXPECT_EQ_MSG(40U, g_sent[0].len, "VAM length (VIN available: no sync byte)");
    TEST_ASSERT_EQUAL_MEMORY(expected, g_sent[0].data, 40U);
    EXPECT_EQ_MSG(k_peer.addr, g_sent[0].peer.addr, "reply not sent to requester address");
    EXPECT_EQ_MSG(k_peer.port, g_sent[0].peer.port, "reply not sent to requester port");
    EXPECT_EQ_MSG(1U, g_vin_calls, "VIN not read for the response");
    EXPECT_EQ_MSG(0U, g_vin_calls_unlocked, "VIN read outside the UDS lock pair");
    EXPECT_TRUE_MSG(g_vin_ctx_seen == uds_ctx(), "VIN callback did not get the server's UDS ctx");
}

/* The CDA sends 0xFF/0x00; the reply carries the server's own version. */
ZTEST(doip_discovery_suite, test_vir_default_version_accepted)
{
    handler_setup();
    feed(k_vir_ff, sizeof(k_vir_ff));
    EXPECT_EQ_MSG(1U, g_sent_count, "VIR with default version 0xFF not answered");
    EXPECT_EQ_MSG(DOIP_PT_VEHICLE_ANNOUNCE, sent_type(0), "wrong reply type");
    EXPECT_EQ_MSG(DOIP_VERSION, g_sent[0].data[0], "reply must carry 0x02, not 0xFF");
    EXPECT_EQ_MSG(DOIP_VERSION_INV, g_sent[0].data[1], "reply inverse version");
}

/* No VIN callback: VIN is the invalidity value and sync status says
 * incomplete (33-byte form). */
ZTEST(doip_discovery_suite, test_vir_without_vin_source_reports_not_available)
{
    handler_setup();
    eds_doip_discovery_cfg_t cfg = default_cfg();
    cfg.get_vin = NULL;
    eds_doip_set_discovery(&cfg);
    feed(k_vir, sizeof(k_vir));

    EXPECT_EQ_MSG(1U, g_sent_count, "VIR not answered without a VIN source");
    EXPECT_EQ_MSG(41U, g_sent[0].len, "VIN not available must add the sync status byte");
    EXPECT_EQ_MSG(33U, g_sent[0].data[7], "payload length field");
    uint8_t zeros[DOIP_VIN_LEN] = { 0U };
    TEST_ASSERT_EQUAL_MEMORY(zeros, &g_sent[0].data[8], DOIP_VIN_LEN);
    EXPECT_EQ_MSG(DOIP_SYNC_STATUS_INCOMPLETE, g_sent[0].data[40], "sync status");
}

/* The safety wrapper refuses (DID restricted): same as not available, and
 * whatever the callback left in the buffer is not sent. */
ZTEST(doip_discovery_suite, test_vir_restricted_vin_reports_not_available)
{
    handler_setup();
    g_vin_rc = UDS_STATUS_ERR_SESSION_INVALID;
    feed(k_vir, sizeof(k_vir));

    EXPECT_EQ_MSG(1U, g_sent_count, "VIR not answered when the VIN is restricted");
    EXPECT_EQ_MSG(41U, g_sent[0].len, "restricted VIN must add the sync status byte");
    uint8_t zeros[DOIP_VIN_LEN] = { 0U };
    TEST_ASSERT_EQUAL_MEMORY(zeros, &g_sent[0].data[8], DOIP_VIN_LEN);
    EXPECT_EQ_MSG(DOIP_SYNC_STATUS_INCOMPLETE, g_sent[0].data[40], "sync status");
    EXPECT_EQ_MSG(0U, g_vin_calls_unlocked, "VIN read outside the UDS lock pair");
}

ZTEST(doip_discovery_suite, test_vir_by_eid_answers_only_on_match)
{
    uint8_t req[14];
    uint8_t other[DOIP_EID_LEN] = { 0x02U, 0x80U, 0xE1U, 0x00U, 0x00U, 0x02U };

    handler_setup();
    make_vir_eid(req, other);
    feed(req, sizeof(req));
    EXPECT_EQ_MSG(0U, g_sent_count, "VIR with a foreign EID must get no answer at all");

    make_vir_eid(req, g_eid);
    feed(req, sizeof(req));
    EXPECT_EQ_MSG(1U, g_sent_count, "VIR with our EID not answered");
    EXPECT_EQ_MSG(DOIP_PT_VEHICLE_ANNOUNCE, sent_type(0), "wrong reply type");
}

ZTEST(doip_discovery_suite, test_vir_by_vin_answers_only_on_match)
{
    uint8_t req[25];
    uint8_t other[DOIP_VIN_LEN];

    handler_setup();
    memcpy(other, k_vin, DOIP_VIN_LEN);
    other[16] = '3';
    make_vir_vin(req, other);
    feed(req, sizeof(req));
    EXPECT_EQ_MSG(0U, g_sent_count, "VIR with a foreign VIN must get no answer at all");

    make_vir_vin(req, k_vin);
    feed(req, sizeof(req));
    EXPECT_EQ_MSG(1U, g_sent_count, "VIR with our VIN not answered");
}

/* A VIN that is not available never matches, not even an all-zero request. */
ZTEST(doip_discovery_suite, test_vir_by_vin_never_matches_unavailable_vin)
{
    uint8_t req[25];
    uint8_t zeros[DOIP_VIN_LEN] = { 0U };

    handler_setup();
    g_vin_rc = UDS_STATUS_ERR_SESSION_INVALID;
    make_vir_vin(req, zeros);
    feed(req, sizeof(req));
    EXPECT_EQ_MSG(0U, g_sent_count, "an unavailable VIN matched a VIN request");
}

/* EID override, GID and further action from the configuration. */
ZTEST(doip_discovery_suite, test_vir_fields_from_configuration)
{
    const uint8_t eid[DOIP_EID_LEN] = { 1U, 2U, 3U, 4U, 5U, 6U };
    const uint8_t gid[DOIP_GID_LEN] = { 9U, 8U, 7U, 6U, 5U, 4U };

    handler_setup();
    eds_doip_discovery_cfg_t cfg = default_cfg();
    cfg.eid_set = true;
    memcpy(cfg.eid, eid, DOIP_EID_LEN);
    cfg.gid_set = true;
    memcpy(cfg.gid, gid, DOIP_GID_LEN);
    cfg.further_action = 0x10U;
    eds_doip_set_discovery(&cfg);
    feed(k_vir, sizeof(k_vir));

    EXPECT_EQ_MSG(1U, g_sent_count, "VIR not answered");
    TEST_ASSERT_EQUAL_MEMORY(eid, &g_sent[0].data[27], DOIP_EID_LEN);
    TEST_ASSERT_EQUAL_MEMORY(gid, &g_sent[0].data[33], DOIP_GID_LEN);
    EXPECT_EQ_MSG(0x10U, g_sent[0].data[39], "further action byte");
}

/* =========================================================================
 * Entity status and power mode
 * ========================================================================= */

ZTEST(doip_discovery_suite, test_entity_status_reports_slot)
{
    const uint8_t idle[15] = {
        0x02U, 0xFDU, 0x40U, 0x02U, 0U, 0U, 0U, 7U,
        DOIP_NODE_TYPE_NODE, 1U, 0U, 0x00U, 0x00U, 0x10U, 0x00U,
    };

    handler_setup();
    feed(k_entity, sizeof(k_entity));
    EXPECT_EQ_MSG(1U, g_sent_count, "entity status not answered");
    EXPECT_EQ_MSG(15U, g_sent[0].len, "entity status length");
    TEST_ASSERT_EQUAL_MEMORY(idle, g_sent[0].data, 15U);

    g_doip.conn_ctx = CONN_CTX;   /* a tester holds the slot */
    feed(k_entity, sizeof(k_entity));
    EXPECT_EQ_MSG(2U, g_sent_count, "entity status not answered");
    EXPECT_EQ_MSG(1U, g_sent[1].data[10], "open sockets must be 1 while a tester is connected");
    g_doip.conn_ctx = NULL;
}

ZTEST(doip_discovery_suite, test_power_mode_default_ready_and_callback)
{
    handler_setup();
    feed(k_power, sizeof(k_power));
    EXPECT_EQ_MSG(1U, g_sent_count, "power mode not answered");
    EXPECT_EQ_MSG(DOIP_PT_POWER_MODE_RESP, sent_type(0), "wrong reply type");
    EXPECT_EQ_MSG(9U, g_sent[0].len, "power mode length");
    EXPECT_EQ_MSG(DOIP_POWER_MODE_READY, g_sent[0].data[8], "default must be ready");

    eds_doip_discovery_cfg_t cfg = default_cfg();
    cfg.get_power_mode = power_mode_cb;
    eds_doip_set_discovery(&cfg);
    feed(k_power, sizeof(k_power));
    EXPECT_EQ_MSG(DOIP_POWER_MODE_NOT_READY, g_sent[1].data[8], "callback value not reported");
}

/* =========================================================================
 * Generic header NACK
 * ========================================================================= */

static uint8_t nack_for(const uint8_t *data, uint32_t len)
{
    uint32_t before = g_sent_count;
    (void)doip_handle_udp_datagram(&g_doip, uds_ctx(), data, len, &k_peer);
    if ((g_sent_count != before + 1U) || (sent_type(before) != DOIP_PT_GENERIC_NACK) ||
        (g_sent[before].len != 9U)) {
        return 0xEEU; /* not exactly one generic NACK */
    }
    return g_sent[before].data[8];
}

ZTEST(doip_discovery_suite, test_generic_nack_table)
{
    handler_setup();

    const uint8_t bad_inverse[] = { 0x02U, 0xFCU, 0x00U, 0x01U, 0, 0, 0, 0 };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_INCORRECT_PATTERN, nack_for(bad_inverse, 8U), "bad inverse");

    const uint8_t v3[] = { 0x03U, 0xFCU, 0x00U, 0x01U, 0, 0, 0, 0 };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_INCORRECT_PATTERN, nack_for(v3, 8U), "unsupported version");

    /* 0xFF is the default only for vehicle identification requests. */
    const uint8_t ff_entity[] = { 0xFFU, 0x00U, 0x40U, 0x01U, 0, 0, 0, 0 };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_INCORRECT_PATTERN, nack_for(ff_entity, 8U), "0xFF on 0x4001");

    const uint8_t tcp_only[] = { 0x02U, 0xFDU, 0x80U, 0x01U, 0, 0, 0, 0 };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_UNKNOWN_TYPE, nack_for(tcp_only, 8U), "0x8001 on UDP");

    const uint8_t unknown[] = { 0x02U, 0xFDU, 0x12U, 0x34U, 0, 0, 0, 0 };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_UNKNOWN_TYPE, nack_for(unknown, 8U), "unknown type");

    const uint8_t huge[] = { 0x02U, 0xFDU, 0x00U, 0x03U, 0, 0, 0x01U, 0x00U };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_MSG_TOO_LARGE, nack_for(huge, 8U), "payload length 256");

    const uint8_t vir_len1[] = { 0x02U, 0xFDU, 0x00U, 0x01U, 0, 0, 0, 1U, 0xAAU };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_INVALID_LENGTH, nack_for(vir_len1, 9U), "VIR with payload");

    /* Header says 6, datagram carries 3. */
    const uint8_t eid_short[] = { 0x02U, 0xFDU, 0x00U, 0x02U, 0, 0, 0, 6U, 1U, 2U, 3U };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_INVALID_LENGTH, nack_for(eid_short, 11U), "truncated EID");

    /* Trailing bytes after the declared payload. */
    const uint8_t vir_trailing[] = { 0x02U, 0xFDU, 0x00U, 0x01U, 0, 0, 0, 0, 0xAAU };
    EXPECT_EQ_MSG(DOIP_HDR_NACK_INVALID_LENGTH, nack_for(vir_trailing, 9U), "trailing bytes");

    uint32_t before = g_sent_count;
    feed(k_vir, 7U);
    EXPECT_EQ_MSG(before, g_sent_count, "a datagram shorter than a header must get no reply");
}

/* Response types are never answered, even malformed: no NACK loops between
 * two entities, and our own broadcast echo is ignored. */
ZTEST(doip_discovery_suite, test_response_types_never_answered)
{
    const uint16_t types[] = {
        DOIP_PT_GENERIC_NACK, DOIP_PT_VEHICLE_ANNOUNCE,
        DOIP_PT_ENTITY_STATUS_RESP, DOIP_PT_POWER_MODE_RESP,
    };

    handler_setup();
    for (uint32_t i = 0U; i < 4U; i++) {
        uint8_t ok[9]  = { 0x02U, 0xFDU, (uint8_t)(types[i] >> 8U), (uint8_t)types[i],
                           0, 0, 0, 1U, 0x00U };
        uint8_t bad[9] = { 0x07U, 0x07U, (uint8_t)(types[i] >> 8U), (uint8_t)types[i],
                           0, 0, 0, 1U, 0x00U };
        feed(ok, sizeof(ok));
        feed(bad, sizeof(bad));
    }
    EXPECT_EQ_MSG(0U, g_sent_count, "a response-type datagram was answered");
}

/* TCP stays strict: 0xFF is not a valid version on a connection. */
ZTEST(doip_discovery_suite, test_tcp_header_still_rejects_default_version)
{
    uint16_t type = 0U;
    uint32_t len = 0U;
    EXPECT_EQ_MSG(UDS_STATUS_ERR_TP_FRAME_INVALID,
                  doip_parse_header(k_vir_ff, &type, &len),
                  "doip_parse_header() (TCP) accepted version 0xFF");
}

/* =========================================================================
 * Reply rate cap
 * ========================================================================= */

ZTEST(doip_discovery_suite, test_reply_rate_cap)
{
    handler_setup();
    for (uint32_t i = 0U; i < (DOIP_UDP_MAX_REPLIES_PER_S + 4U); i++) {
        feed(k_vir, sizeof(k_vir));
    }
    EXPECT_EQ_MSG(DOIP_UDP_MAX_REPLIES_PER_S, g_sent_count, "replies above the per-second cap");
    EXPECT_EQ_MSG(4U, g_doip.udp_dropped, "dropped datagrams not counted");

    g_now_ms += 1000U;
    feed(k_vir, sizeof(k_vir));
    EXPECT_EQ_MSG(DOIP_UDP_MAX_REPLIES_PER_S + 1U, g_sent_count,
                  "cap did not reset in the next second");
}

/* NACKs count against the cap too: a malformed flood is not a reflector. */
ZTEST(doip_discovery_suite, test_rate_cap_covers_nacks)
{
    const uint8_t unknown[] = { 0x02U, 0xFDU, 0x12U, 0x34U, 0, 0, 0, 0 };

    handler_setup();
    for (uint32_t i = 0U; i < (DOIP_UDP_MAX_REPLIES_PER_S * 2U); i++) {
        feed(unknown, sizeof(unknown));
    }
    EXPECT_EQ_MSG(DOIP_UDP_MAX_REPLIES_PER_S, g_sent_count, "NACKs above the per-second cap");
}

/* =========================================================================
 * Registration
 * ========================================================================= */

ZTEST(doip_discovery_suite, test_partial_udp_ops_refused)
{
    eds_doip_platform_ops_t ops = g_ops_udp;
    ops.wait_readable = NULL;
    EXPECT_EQ_MSG(UDS_STATUS_ERR_NULL_PTR, eds_doip_register_platform(&ops),
                  "a partial UDP op set was accepted");

    ops = g_ops_udp;
    ops.udp_open = NULL; ops.udp_recvfrom = NULL; ops.udp_sendto = NULL;
    ops.udp_close = NULL; ops.wait_readable = NULL; ops.get_eid = NULL;
    EXPECT_EQ_MSG(UDS_STATUS_OK, eds_doip_register_platform(&ops),
                  "TCP-only ops (no UDP) refused");
    EXPECT_EQ_MSG(UDS_STATUS_OK, eds_doip_register_platform(&g_ops_udp),
                  "full UDP op set refused");
}

/* =========================================================================
 * 2. The server loop
 * ========================================================================= */

/* ISO 13400-2 defaults: 3 announcements, 500 ms apart, the first within
 * 0-500 ms; to 255.255.255.255:13400. A re-request starts another burst. */
ZTEST(doip_discovery_suite, test_announcements_at_start_and_on_request)
{
    reset_all();
    g_end_ms = 20000U;
    g_rerequest_at_ms = 6000U;
    run_server();

    EXPECT_EQ_MSG(1U, g_udp_open_calls, "UDP socket not opened");
    EXPECT_EQ_MSG(6U, g_sent_count, "expected two bursts of 3 announcements");
    for (uint32_t i = 0U; i < 6U; i++) {
        EXPECT_EQ_MSG(DOIP_PT_VEHICLE_ANNOUNCE, sent_type(i), "announcement type");
        EXPECT_EQ_MSG(DOIP_UDP_BROADCAST_ADDR, g_sent[i].peer.addr, "not broadcast");
        EXPECT_EQ_MSG(DOIP_PORT, g_sent[i].peer.port, "not to port 13400");
    }
    EXPECT_TRUE_MSG(g_sent[0].at_ms <= DOIP_ANNOUNCE_WAIT_MAX_MS,
                    "first announcement later than A_DoIP_Announce_Wait");
    EXPECT_EQ_MSG(DOIP_ANNOUNCE_INTERVAL_MS, g_sent[1].at_ms - g_sent[0].at_ms, "interval 1-2");
    EXPECT_EQ_MSG(DOIP_ANNOUNCE_INTERVAL_MS, g_sent[2].at_ms - g_sent[1].at_ms, "interval 2-3");
    EXPECT_TRUE_MSG(g_sent[3].at_ms >= 6000U, "second burst before the request");
    EXPECT_TRUE_MSG(g_sent[3].at_ms <= 6000U + DOIP_ANNOUNCE_WAIT_MAX_MS + DOIP_ACCEPT_POLL_TIMEOUT_MS,
                    "second burst too late");
    EXPECT_EQ_MSG(DOIP_ANNOUNCE_INTERVAL_MS, g_sent[4].at_ms - g_sent[3].at_ms, "interval 4-5");
    EXPECT_EQ_MSG(6U, g_doip.announcements_sent, "announcement counter");
}

/* announce = false: no broadcasts, requests still answered. */
ZTEST(doip_discovery_suite, test_announce_opt_out)
{
    reset_all();
    eds_doip_discovery_cfg_t cfg = default_cfg();
    cfg.announce = false;
    eds_doip_set_discovery(&cfg);
    add_dgram(1234U, k_vir, sizeof(k_vir));
    g_end_ms = 8000U;
    g_rerequest_at_ms = 3000U;   /* ignored while announcements are off */
    run_server();

    EXPECT_EQ_MSG(1U, g_sent_count, "with announce off only the VIR answer is sent");
    EXPECT_EQ_MSG(k_peer.port, g_sent[0].peer.port, "the one send is the unicast answer");
}

/* enabled = false: no UDP socket, no multiplexed wait, the TCP calls of
 * before EDS#392. */
ZTEST(doip_discovery_suite, test_discovery_opt_out_is_tcp_only)
{
    reset_all();
    eds_doip_discovery_cfg_t cfg = default_cfg();
    cfg.enabled = false;
    eds_doip_set_discovery(&cfg);
    add_dgram(100U, k_vir, sizeof(k_vir));
    g_end_ms = 3U * DOIP_ACCEPT_POLL_TIMEOUT_MS;
    run_server();

    EXPECT_EQ_MSG(0U, g_udp_open_calls, "UDP socket opened with discovery disabled");
    EXPECT_EQ_MSG(0U, g_wait_calls, "wait_readable used with discovery disabled");
    EXPECT_EQ_MSG(0U, g_sent_count, "UDP sent with discovery disabled");
    EXPECT_EQ_MSG(3U, g_tcp_accept_calls, "accept polls changed with discovery disabled");
}

/* A failed socket leaves the server TCP-only, counted, not stopped. */
ZTEST(doip_discovery_suite, test_udp_open_failure_leaves_tcp_only)
{
    reset_all();
    g_udp_open_fails = true;
    g_end_ms = 2U * DOIP_ACCEPT_POLL_TIMEOUT_MS;
    run_server();

    EXPECT_EQ_MSG(1U, g_doip.udp_open_failed, "open failure not counted");
    EXPECT_EQ_MSG(0U, g_wait_calls, "wait_readable used without a UDP socket");
    EXPECT_EQ_MSG(2U, g_tcp_accept_calls, "server did not keep accepting");
}

/* A VIR arriving while the server idles in accept is answered when it
 * arrives, not at the end of the 5 s accept poll. */
ZTEST(doip_discovery_suite, test_vir_answered_during_accept_wait)
{
    reset_all();
    eds_doip_discovery_cfg_t cfg = default_cfg();
    cfg.announce = false;
    eds_doip_set_discovery(&cfg);
    add_dgram(1234U, k_vir_ff, sizeof(k_vir_ff));
    g_end_ms = 6000U;
    run_server();

    EXPECT_EQ_MSG(1U, g_sent_count, "VIR during accept wait not answered");
    EXPECT_EQ_MSG(1234U, g_sent[0].at_ms, "VIR answered late");
    EXPECT_TRUE_MSG(g_checkins >= 2U, "no check-in on the UDP wake");
}

/* EDS#400 unchanged: a connection without routing activation is still
 * closed after T_TCP_Initial_Inactivity (2 s at the default poll), measured
 * on the clock, however many UDP wakes fall inside the polls. */
ZTEST(doip_discovery_suite, test_idle_accounting_unchanged_by_udp_wakes)
{
    reset_all();
    eds_doip_discovery_cfg_t cfg = default_cfg();
    cfg.announce = false;
    eds_doip_set_discovery(&cfg);
    g_conn_at_ms = 0U;
    add_dgram(300U, k_vir, sizeof(k_vir));
    add_dgram(1200U, k_entity, sizeof(k_entity));
    add_dgram(1700U, k_power, sizeof(k_power));
    g_end_ms = 2500U;
    run_server();

    EXPECT_EQ_MSG(DOIP_INITIAL_INACTIVITY_POLLS * DOIP_IDLE_POLL_MS, g_tcp_close_at_ms,
                  "UDP wakes changed when an idle connection is closed (EDS#400)");
    EXPECT_EQ_MSG(3U, g_sent_count, "datagrams during a connection not answered");
    EXPECT_EQ_MSG(1U, g_sent[1].data[10], "entity status must show the open connection");
}

/* A flood is read at most DOIP_UDP_MAX_PER_WAKE datagrams per wake, so TCP
 * and the check-in get a look in between. */
ZTEST(doip_discovery_suite, test_per_wake_bound_under_flood)
{
    reset_all();
    eds_doip_discovery_cfg_t cfg = default_cfg();
    cfg.announce = false;
    eds_doip_set_discovery(&cfg);
    for (uint32_t i = 0U; i < 12U; i++) {
        add_dgram(100U, k_vir, sizeof(k_vir));
    }
    g_end_ms = 2000U;
    run_server();

    EXPECT_EQ_MSG(DOIP_UDP_MAX_PER_WAKE, g_recvfrom_max_per_wake,
                  "more datagrams read in one wake than DOIP_UDP_MAX_PER_WAKE");
    EXPECT_EQ_MSG(12U, g_doip.udp_received, "flood not drained across wakes");
    EXPECT_EQ_MSG(12U, g_sent_count, "flood below the rate cap not answered");
}

ZTEST_SUITE(doip_discovery_suite, NULL, NULL, NULL, NULL, NULL);

/* =========================================================================
 * Unity runner infrastructure
 * ========================================================================= */

void setUp(void)    { }
void tearDown(void) { }

void run_all_tests(void)
{
    RUN_TEST(doip_discovery_suite__test_vir_answered_byte_exact);
    RUN_TEST(doip_discovery_suite__test_vir_default_version_accepted);
    RUN_TEST(doip_discovery_suite__test_vir_without_vin_source_reports_not_available);
    RUN_TEST(doip_discovery_suite__test_vir_restricted_vin_reports_not_available);
    RUN_TEST(doip_discovery_suite__test_vir_by_eid_answers_only_on_match);
    RUN_TEST(doip_discovery_suite__test_vir_by_vin_answers_only_on_match);
    RUN_TEST(doip_discovery_suite__test_vir_by_vin_never_matches_unavailable_vin);
    RUN_TEST(doip_discovery_suite__test_vir_fields_from_configuration);
    RUN_TEST(doip_discovery_suite__test_entity_status_reports_slot);
    RUN_TEST(doip_discovery_suite__test_power_mode_default_ready_and_callback);
    RUN_TEST(doip_discovery_suite__test_generic_nack_table);
    RUN_TEST(doip_discovery_suite__test_response_types_never_answered);
    RUN_TEST(doip_discovery_suite__test_tcp_header_still_rejects_default_version);
    RUN_TEST(doip_discovery_suite__test_reply_rate_cap);
    RUN_TEST(doip_discovery_suite__test_rate_cap_covers_nacks);
    RUN_TEST(doip_discovery_suite__test_partial_udp_ops_refused);
    RUN_TEST(doip_discovery_suite__test_announcements_at_start_and_on_request);
    RUN_TEST(doip_discovery_suite__test_announce_opt_out);
    RUN_TEST(doip_discovery_suite__test_discovery_opt_out_is_tcp_only);
    RUN_TEST(doip_discovery_suite__test_udp_open_failure_leaves_tcp_only);
    RUN_TEST(doip_discovery_suite__test_vir_answered_during_accept_wait);
    RUN_TEST(doip_discovery_suite__test_idle_accounting_unchanged_by_udp_wakes);
    RUN_TEST(doip_discovery_suite__test_per_wake_bound_under_flood);
}
