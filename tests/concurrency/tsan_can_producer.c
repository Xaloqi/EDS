// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test (ThreadSanitizer job, one thread)
 * FILE: tests/concurrency/tsan_can_producer.c
 * MODULE UNDER TEST: examples/dual_transport_ecu/src/can_producer.c
 *
 * PURPOSE: [EDS#197] The CAN producer's contracts with the executor
 *   (core/uds_exec.h), deterministically: the test steps the producer and
 *   the executor itself, and plays the tester on a mock CAN bus.
 *     1. Contract 2: while a request is posted (the executor has not run it
 *        yet), the producer keeps draining RX (EDS#558: a whole multi-frame
 *        request queued at once is taken in one step), and a second complete
 *        physical request is answered 7F sid 21 by the producer; the posted
 *        request and response buffers are not written. A functional one is
 *        dropped.
 *     2. Contract 1: the response is handed to ISO-TP, then the slot is
 *        released, and the next request posts.
 *     3. Routing: a job's frames go out on the ISO-TP channel, a periodic
 *        message as a UUDT frame on its own CAN ID.
 *   Prints "tsan_can_producer: ... ok" and exits 0, or names the failed check.
 *
 * CI: the concurrency-tsan job (EDS#197 J1).
 * =============================================================================
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uds_exec.h"
#include "isotp.h"
#include "can_transport.h"
#include "services.h"
#include "did_database.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_periodic.h"
/* The example's CAN producer itself (run_tsan.sh builds one file). */
#include "../../examples/dual_transport_ecu/src/can_producer.c"

#define CAN_RX_ID   (0x7E0U)
#define CAN_FUNC_ID (0x7DFU)
#define CAN_TX_ID   (0x7E8U)
#define CAN_PER_ID  (0x5E8U)
#define SRC_CAN     (0U)

static int g_fail;
#define CHECK(c, what) \
    do { if (!(c)) { printf("FAIL: %s (line %d)\n", (what), __LINE__); g_fail = 1; } } while (0)

/* ---- executor with fake bindings: runs only when the test says ---------- */

static uint32_t g_now = 1000U;
static uint32_t f_now(void)   { return g_now; }
static void     f_enter(void) { }
static void     f_exit(void)  { }

/* ---- mock CAN bus ------------------------------------------------------- */

static uds_can_frame_t s_rxq[8];
static uint32_t        s_rxq_n;
static uds_can_frame_t s_tx[16];     /* frames the ECU sent */
static uint32_t        s_tx_n;

static void bus_send(uint32_t id, const uint8_t *d, uint8_t n)
{
    uds_can_frame_t *f = &s_rxq[s_rxq_n++];

    (void)memset(f, 0, sizeof(*f));
    f->id  = id;
    f->dlc = 8U;
    (void)memcpy(f->data, d, n);
}

static uds_status_t m_tx(can_transport_t *self, const uds_can_frame_t *f)
{
    (void)self;
    if (s_tx_n < 16U) {
        s_tx[s_tx_n++] = *f;
    }
    return UDS_STATUS_OK;
}

static uds_status_t m_rx(can_transport_t *self, uds_can_frame_t *out, bool *ready)
{
    uint32_t i;

    (void)self;
    *ready = (s_rxq_n > 0U);
    if (*ready) {
        *out = s_rxq[0];
        for (i = 1U; i < s_rxq_n; i++) {
            s_rxq[i - 1U] = s_rxq[i];
        }
        s_rxq_n--;
    }
    return UDS_STATUS_OK;
}

static uds_status_t m_status(can_transport_t *self, bool *bus_off)
{
    (void)self;
    *bus_off = false;
    return UDS_STATUS_OK;
}

static const can_transport_ops_t k_ops = { .transmit = m_tx, .receive = m_rx, .get_status = m_status };
static can_transport_t g_can = { .ops = &k_ops, .platform = NULL, .ready = true };

/* ---- server: real 0x10/0x22/0x2A/0x3E, a deferring 0x31 ----------------- */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;
static uds_exec_t         g_ex;
static isotp_ctx_t        g_tp;
static can_producer_t     g_p;
static uint32_t           g_job_left;

static uds_step_result_t job_step(uds_msg_buf_t *final_resp, uds_nrc_t *nrc)
{
    (void)nrc;
    if (g_job_left > 0U) {
        g_job_left--;
        return UDS_STEP_IN_PROGRESS;
    }
    final_resp->data[0] = 0x71U; final_resp->data[1] = 0x01U;
    final_resp->data[2] = 0xF0U; final_resp->data[3] = 0x00U;
    final_resp->length  = 4U;
    return UDS_STEP_DONE;
}

static void job_abort(void) { }

static uds_status_t h_defer_0x31(uds_server_ctx_t *ctx, const uds_msg_buf_t *req,
                                 uds_msg_buf_t *resp)
{
    static const uds_pending_job_t job = {
        .sid = 0x31U, .step_max_ms = 10U, .step = job_step, .abort = job_abort,
    };
    (void)req;
    return uds_server_defer(ctx, &job, resp);
}

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l; (void)b; (void)n; *o = 0U; return UDS_STATUS_OK;
}
static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl, const uint8_t *k, uint8_t kl)
{
    (void)l; (void)s; (void)sl; (void)k; (void)kl; return false;
}
static uds_status_t read_pdid(uint8_t *buf, uint16_t len, uint16_t *out)
{
    (void)len; buf[0] = 0x5AU; *out = 1U; return UDS_STATUS_OK;
}

static const uds_service_entry_t k_table[] = {
    { 0x10U, uds_service_0x10_handler, true  },
    { 0x22U, uds_service_0x22_handler, true  },
    { 0x2AU, uds_service_0x2A_handler, false },
    { 0x3EU, uds_service_0x3E_handler, true  },
    { 0x31U, h_defer_0x31,             true  },
};
static const uds_access_entry_t k_acl[] = {
    { 0x10U, UDS_ACL_SESSION_ALL,         0U, false },
    { 0x22U, UDS_ACL_SESSION_ALL,         0U, false },
    { 0x2AU, UDS_ACL_SESSION_NON_DEFAULT, 0U, false },
    { 0x3EU, UDS_ACL_SESSION_ALL,         0U, false },
    { 0x31U, UDS_ACL_SESSION_ALL,         0U, false },
};

static void setup(void)
{
    static const uds_security_cfg_t sc = {
        .max_attempts = 3U, .lockout_ms = 100U, .key_validate_cb = t_key, .seed_generate_cb = t_seed,
    };
    static const uds_server_cfg_t svc = {
        .p2_server_max_ms = 50U, .p2_star_server_max_ms = 5000U,
        .session_ctx = &g_sess, .security_ctx = &g_sec,
        .service_table = k_table, .service_table_count = 5U,
        .access_table = k_acl, .access_table_count = 5U,
    };
    static const did_entry_t pdid = {
        .did_id = 0xF2ABU, .access_flags = (uint8_t)DID_ACCESS_READ,
        .min_session = (uint8_t)UDS_SESSION_EXTENDED, .data_length = 1U,
        .read_cb = read_pdid, .description = "test pDID",
    };
    static const uds_exec_producer_cfg_t prod = { .src = SRC_CAN, .periodic_tx = true };
    const uds_exec_cfg_t ec = {
        .srv = &g_srv, .now_ms = f_now, .enter = f_enter, .exit = f_exit,
        .producers = &prod, .producer_count = 1U,
    };
    const isotp_cfg_t tc = {
        .rx_can_id = CAN_RX_ID, .tx_can_id = CAN_TX_ID, .functional_rx_can_id = CAN_FUNC_ID,
        .periodic_tx_can_id = CAN_PER_ID, .block_size = 0U, .stmin_ms = 0U, .can = &g_can,
    };

    (void)uds_session_init(&g_sess, 5000U);
    (void)uds_security_init(&g_sec, &sc);
    (void)uds_server_init(&g_srv, &svc);
    (void)uds_periodic_init();
    uds_periodic_set_tx_can_id(CAN_PER_ID);
    (void)did_database_init();
    (void)did_database_register(&pdid);
    (void)isotp_init(&g_tp, &tc);
    (void)uds_exec_init(&g_ex, &ec);
    can_producer_init(&g_p, &g_ex, SRC_CAN, &g_tp, &g_can);
}

static bool tx_is(uint32_t i, uint32_t id, const uint8_t *d, uint8_t n)
{
    return (i < s_tx_n) && (s_tx[i].id == id) && (memcmp(s_tx[i].data, d, n) == 0);
}

/* 1 + 2: a second request while the first is posted; then its answer. */
static void test_busy_while_posted(void)
{
    static const uint8_t sf_10_03[]  = { 0x02U, 0x10U, 0x03U };
    static const uint8_t sf_3e_00[]  = { 0x02U, 0x3EU, 0x00U };
    static const uint8_t sf_3e_80[]  = { 0x02U, 0x3EU, 0x80U };
    static const uint8_t ff_22[]     = { 0x10U, 0x09U, 0x22U, 0xF1U, 0x90U, 0xF1U, 0x91U, 0xF1U };
    static const uint8_t cf_22[]     = { 0x21U, 0x92U, 0xF1U, 0x93U };
    static const uint8_t nrc_3e_21[] = { 0x03U, 0x7FU, 0x3EU, 0x21U };
    static const uint8_t nrc_22_21[] = { 0x03U, 0x7FU, 0x22U, 0x21U };
    static const uint8_t pos_10_03[] = { 0x06U, 0x50U, 0x03U };
    uint8_t  req_before[2];
    uint32_t n;

    bus_send(CAN_RX_ID, sf_10_03, 3U);
    can_producer_step(&g_p);
    CHECK(g_p.in_flight && (g_p.stats.posted == 1U), "10 03 posted");
    (void)memcpy(req_before, g_p.req.data, 2U);
    (void)memset(g_p.resp.data, 0xEEU, 8U);   /* the posted response: must stay as is */

    /* The executor has not run. A physical 3E 00, then a functional 3E 80,
     * then a whole multi-frame 22 (FF + CF queued together, EDS#558). */
    bus_send(CAN_RX_ID, sf_3e_00, 3U);
    can_producer_step(&g_p);
    CHECK(tx_is(0U, CAN_TX_ID, nrc_3e_21, 4U), "7F 3E 21 from the producer");
    bus_send(CAN_FUNC_ID, sf_3e_80, 3U);
    can_producer_step(&g_p);
    CHECK(s_tx_n == 1U, "functional 3E 80 dropped: nothing sent");
    bus_send(CAN_RX_ID, ff_22, 8U);
    can_producer_step(&g_p);                   /* FF: the ECU answers Flow Control */
    n = s_tx_n;
    CHECK((n == 2U) && ((s_tx[1].data[0] & 0xF0U) == 0x30U), "FC for the FF");
    bus_send(CAN_RX_ID, cf_22, 4U);
    bus_send(CAN_RX_ID, sf_3e_00, 3U);         /* queued behind the CF */
    can_producer_step(&g_p);
    CHECK(s_rxq_n == 0U, "one step drained both queued frames");
    CHECK(tx_is(2U, CAN_TX_ID, nrc_22_21, 4U), "the multi-frame 22 answered 7F 22 21");
    CHECK(tx_is(3U, CAN_TX_ID, nrc_3e_21, 4U), "and the 3E after it 7F 3E 21");
    CHECK(memcmp(g_p.req.data, req_before, 2U) == 0, "posted request untouched");
    CHECK((g_p.resp.data[0] == 0xEEU) && (g_p.resp.data[7] == 0xEEU), "posted response untouched");
    CHECK(g_srv.request_count == 0U, "none reached the server");
    CHECK((g_p.stats.local_busy == 3U) && (g_p.stats.dropped == 1U), "counted: 3 busy, 1 dropped");

    /* Contract 1: the executor runs it; the producer sends and releases. */
    (void)uds_exec_run_once(&g_ex);
    can_producer_step(&g_p);
    CHECK(tx_is(4U, CAN_TX_ID, pos_10_03, 3U), "50 03 sent");
    CHECK(!g_p.in_flight, "slot released");
    bus_send(CAN_RX_ID, sf_3e_00, 3U);
    can_producer_step(&g_p);
    CHECK(g_p.in_flight && (g_p.stats.posted == 2U), "the next request posts");
    (void)uds_exec_run_once(&g_ex);
    can_producer_step(&g_p);
}

/* 3: a job's frames on the channel, a periodic message as UUDT. */
static void test_job_and_periodic_routing(void)
{
    static const uint8_t sf_31[]    = { 0x04U, 0x31U, 0x01U, 0xF0U, 0x00U };
    static const uint8_t sf_2a[]    = { 0x03U, 0x2AU, 0x03U, 0xABU };
    static const uint8_t rcrrp[]    = { 0x03U, 0x7FU, 0x31U, 0x78U };
    static const uint8_t final_71[] = { 0x04U, 0x71U, 0x01U, 0xF0U, 0x00U };
    static const uint8_t uudt[]     = { 0xABU, 0x5AU };
    uint32_t i;
    uint32_t base = s_tx_n;
    bool     final_seen = false;
    bool     uudt_seen  = false;

    g_job_left = 2U;
    bus_send(CAN_RX_ID, sf_31, 5U);
    can_producer_step(&g_p);
    for (i = 0U; i < 20U; i++) {
        g_now++;
        (void)uds_exec_run_once(&g_ex);
        can_producer_step(&g_p);
    }
    CHECK(tx_is(base, CAN_TX_ID, rcrrp, 4U), "7F 31 78 first");
    for (i = base + 1U; i < s_tx_n; i++) {
        final_seen = final_seen || tx_is(i, CAN_TX_ID, final_71, 5U);
    }
    CHECK(final_seen, "final 71 01 F0 00 on the ISO-TP channel");

    base = s_tx_n;
    bus_send(CAN_RX_ID, sf_2a, 4U);            /* in 10 03 from test 1 */
    for (i = 0U; i < 40U; i++) {
        g_now++;
        (void)uds_exec_run_once(&g_ex);
        can_producer_step(&g_p);
    }
    for (i = base; i < s_tx_n; i++) {
        uudt_seen = uudt_seen || tx_is(i, CAN_PER_ID, uudt, 2U);
    }
    CHECK(uudt_seen, "periodic AB 5A as a UUDT frame on its own ID");
}

int main(void)
{
    setup();
    test_busy_while_posted();
    test_job_and_periodic_routing();
    printf("tsan_can_producer: %u frames sent, posted=%u answered=%u busy=%u dropped=%u pushed=%u %s\n",
           (unsigned)s_tx_n, (unsigned)g_p.stats.posted, (unsigned)g_p.stats.answered,
           (unsigned)g_p.stats.local_busy, (unsigned)g_p.stats.dropped, (unsigned)g_p.stats.pushed,
           g_fail ? "FAILED" : "ok");
    return g_fail;
}
