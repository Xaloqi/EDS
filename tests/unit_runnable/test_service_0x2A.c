// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_service_0x2A.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x2A.c
 *                    SID 0x2A — ReadDataByPeriodicIdentifier handler
 *
 * PURPOSE:
 *   Verify all branches of the 0x2A handler: transmissionMode validation,
 *   DID lookup, access-flag checking, subscription registration, stop-sending,
 *   and positive-response encoding.
 *
 *   Two test DIDs are registered:
 *     0xF2AB — readable (DID_ACCESS_READ)  — periodic identifier 0xAB
 *     0xF2CD — write-only (DID_ACCESS_WRITE) — used to test "no read flag" path
 *
 * TEST CASES:
 *   TC-0x2A-001  NULL ctx  → ERR_NULL_PTR
 *   TC-0x2A-002  NULL req  → ERR_NULL_PTR (from uds_service_validate_length)
 *   TC-0x2A-003  Request too short (len 2, mode != stopSending) → ERR_INVALID_PARAM
 *   TC-0x2A-004  transmissionMode 0x00 (below SLOW) → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31, EDS#466)
 *   TC-0x2A-005  transmissionMode 0x05 (above STOP)  → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31, EDS#466)
 *   TC-0x2A-006  Subscribe 0xAB with SLOW (0x01)   → OK; response SID = 0x6A
 *   TC-0x2A-007  Subscribe 0xAB with MEDIUM (0x02) → OK
 *   TC-0x2A-008  Subscribe 0xAB with FAST (0x03)   → OK
 *   TC-0x2A-009  stopSending (0x04) for 0xAB → OK (unsubscribes; no error)
 *   TC-0x2A-010  Subscribe DID not in database (0xF2FF) → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x2A-011  Subscribe write-only DID 0xF2CD (no DID_ACCESS_READ) → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x2A-012  Positive response: data[0] = 0x6A, length = 1
 *
 * FRAMEWORK: Zephyr Ztest (compiled as host Unity via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_periodic.h"
#include "uds_types.h"
#include "did_database.h"

/* =========================================================================
 * Stub callbacks for UDS security seed/key (required by uds_security_init)
 * ========================================================================= */

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0U; i < n && i < 4U; i++) { b[i] = (uint8_t)(0x10U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl,
                  const uint8_t *k, uint8_t kl)
{
    (void)l;
    if (sl != kl) { return false; }
    for (uint8_t i = 0U; i < sl; i++) {
        if (k[i] != (uint8_t)(s[i] ^ 0xAAU)) { return false; }
    }
    return true;
}

/* =========================================================================
 * Test DID read callbacks
 * ========================================================================= */

static uds_status_t t_read_f2ab(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    (void)buf_len;
    buf[0]   = 0x01U;
    *out_len = (uint16_t)1U;
    return UDS_STATUS_OK;
}

/* =========================================================================
 * Stack context (initialised once per binary)
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;
static bool               g_stack_ready = false;

static void setup(void)
{
    if (!g_stack_ready) {
        memset(&g_sess, 0, sizeof(g_sess));
        memset(&g_sec,  0, sizeof(g_sec));
        memset(&g_srv,  0, sizeof(g_srv));

        uds_session_init(&g_sess, 5000U);

        static const uds_security_cfg_t sc = {
            .max_attempts     = 3U,
            .lockout_ms       = 100U,
            .key_validate_cb  = t_key,
            .seed_generate_cb = t_seed,
        };
        uds_security_init(&g_sec, &sc);

        /* Initialise DID database and register test DIDs */
        (void)did_database_init();

        static const did_entry_t s_did_readable = {
            .did_id             = (uint16_t)0xF2ABU,
            .access_flags       = (uint8_t)DID_ACCESS_READ,
            .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
            .read_access_level  = (uint8_t)0U,
            .write_access_level = (uint8_t)0U,
            .data_length        = (uint16_t)1U,
            .read_cb            = t_read_f2ab,
            .write_cb           = NULL,
            .description        = "Test readable periodic DID",
        };
        (void)did_database_register(&s_did_readable);

        static const did_entry_t s_did_writeonly = {
            .did_id             = (uint16_t)0xF2CDU,
            .access_flags       = (uint8_t)DID_ACCESS_WRITE,
            .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
            .read_access_level  = (uint8_t)0U,
            .write_access_level = (uint8_t)0U,
            .data_length        = (uint16_t)1U,
            .read_cb            = NULL,
            .write_cb           = NULL,
            .description        = "Test write-only DID",
        };
        (void)did_database_register(&s_did_writeonly);

        /* [EDS#466] [SEC-PDID-ACCESS-01] More pDIDs:
         *   0xF200 readable (pDID 0x00), 0xF201 secured (level 1),
         *   0xF202 SAFETY_SYSTEM only (not in EXTENDED), 0xF210-0xF218 readable (capacity). */
        {
            static const struct { uint16_t id; uint8_t sess; uint8_t lvl; } k[] = {
                { 0xF200U, (uint8_t)UDS_SESSION_EXTENDED, 0U }, { 0xF201U, (uint8_t)UDS_SESSION_EXTENDED, 1U },
                { 0xF202U, (uint8_t)UDS_SESSION_SAFETY_SYSTEM, 0U },
                { 0xF210U, (uint8_t)UDS_SESSION_EXTENDED, 0U }, { 0xF211U, (uint8_t)UDS_SESSION_EXTENDED, 0U }, { 0xF212U, (uint8_t)UDS_SESSION_EXTENDED, 0U },
                { 0xF213U, (uint8_t)UDS_SESSION_EXTENDED, 0U }, { 0xF214U, (uint8_t)UDS_SESSION_EXTENDED, 0U }, { 0xF215U, (uint8_t)UDS_SESSION_EXTENDED, 0U },
                { 0xF216U, (uint8_t)UDS_SESSION_EXTENDED, 0U }, { 0xF217U, (uint8_t)UDS_SESSION_EXTENDED, 0U }, { 0xF218U, (uint8_t)UDS_SESSION_EXTENDED, 0U },
            };
            for (uint8_t i = 0U; i < (uint8_t)(sizeof(k) / sizeof(k[0])); i++) {
                did_entry_t e;
                memset(&e, 0, sizeof(e));
                e.did_id            = k[i].id;
                e.access_flags      = (uint8_t)DID_ACCESS_READ;
                e.min_session       = k[i].sess;
                e.read_access_level = k[i].lvl;
                e.data_length       = 1U;
                e.read_cb           = t_read_f2ab;
                e.description       = "test pDID";
                (void)did_database_register(&e);
            }
        }

        static const uds_server_cfg_t svc = {
            .p2_server_max_ms      = 25U,
            .p2_star_server_max_ms = 5000U,
            .session_ctx           = &g_sess,
            .security_ctx          = &g_sec,
            .service_table         = g_uds_service_table,
            .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
        };
        uds_server_init(&g_srv, &svc);
        g_stack_ready = true;
    }

    /* Reset session to EXTENDED before each test (0x2A is non-default) */
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    g_sec.active_level = 0U;
    (void)uds_periodic_init();
    uds_periodic_set_tx_can_id((uint32_t)0x5E8U);   /* [EDS#546] no default: set per test */
    uds_periodic_set_access_ctx(NULL, NULL);
}

/** Build a 0x2A request: [0x2A, mode, id1 {, id2 ...}] */
static uds_msg_buf_t make_req(uint8_t mode, uint8_t periodic_id)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = (uint8_t)0x2AU;
    r.data[1] = mode;
    r.data[2] = periodic_id;
    r.length  = (uint16_t)3U;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x2A, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------- */

/**
 * TC-0x2A-001: NULL ctx → ERR_NULL_PTR.
 */
ZTEST(test_service_0x2A, tc001_null_ctx)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(NULL, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL ctx must return ERR_NULL_PTR");
}

/**
 * TC-0x2A-002: NULL req → ERR_NULL_PTR (from validate_length).
 */
ZTEST(test_service_0x2A, tc002_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, NULL, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR, "NULL req must return ERR_NULL_PTR");
}

/**
 * TC-0x2A-003: Request too short (length 2, missing periodic ID) → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x2A, tc003_too_short)
{
    setup();
    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = (uint8_t)0x2AU;
    req.data[1] = UDS_PERIODIC_MODE_SLOW;
    req.length  = (uint16_t)2U;
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
                  "request shorter than 3 bytes must be rejected");
}

/**
 * TC-0x2A-004: transmissionMode 0x00 (below SLOW) → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31, EDS#466).
 */
ZTEST(test_service_0x2A, tc004_invalid_mode_zero)
{
    setup();
    uds_msg_buf_t req  = make_req((uint8_t)0x00U, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "mode 0x00 must return ERR_REQUEST_OUT_OF_RANGE (NRC 0x31, EDS#466)");
}

/**
 * TC-0x2A-005: transmissionMode 0x05 (above STOP=0x04) → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31, EDS#466).
 */
ZTEST(test_service_0x2A, tc005_invalid_mode_high)
{
    setup();
    uds_msg_buf_t req  = make_req((uint8_t)0x05U, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "mode 0x05 must return ERR_REQUEST_OUT_OF_RANGE (NRC 0x31, EDS#466)");
}

/**
 * TC-0x2A-006: Subscribe 0xAB with SLOW (0x01) → OK.
 */
ZTEST(test_service_0x2A, tc006_subscribe_slow)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "SLOW subscribe for 0xAB must succeed");
}

/**
 * TC-0x2A-007: Subscribe 0xAB with MEDIUM (0x02) → OK.
 */
ZTEST(test_service_0x2A, tc007_subscribe_medium)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_MEDIUM, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "MEDIUM subscribe for 0xAB must succeed");
}

/**
 * TC-0x2A-008: Subscribe 0xAB with FAST (0x03) → OK.
 */
ZTEST(test_service_0x2A, tc008_subscribe_fast)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_FAST, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "FAST subscribe for 0xAB must succeed");
}

/**
 * TC-0x2A-009: stopSending (0x04) for 0xAB → OK.
 * Also verifies stopSending is a no-op when the ID is not subscribed.
 */
ZTEST(test_service_0x2A, tc009_stop_sending)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_STOP, (uint8_t)0xABU);
    uds_msg_buf_t resp = {0};

    /* Stop on an ID that was never subscribed must still return OK. */
    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "stopSending must return OK");
}

/**
 * TC-0x2A-010: Subscribe a DID not in the database (0xF2FF) → ERR_REQUEST_OUT_OF_RANGE.
 * Periodic identifier 0xFF → DID 0xF2FF, which is not registered.
 */
ZTEST(test_service_0x2A, tc010_unknown_did)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xFFU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "DID not in database must return ERR_REQUEST_OUT_OF_RANGE");
}

/**
 * TC-0x2A-011: Subscribe a write-only DID (0xF2CD, no DID_ACCESS_READ flag) →
 *              ERR_REQUEST_OUT_OF_RANGE.
 */
ZTEST(test_service_0x2A, tc011_write_only_did)
{
    setup();
    /* 0xCD → DID 0xF2CD, registered as write-only */
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xCDU);
    uds_msg_buf_t resp = {0};

    uds_status_t rc = uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  "write-only DID must return ERR_REQUEST_OUT_OF_RANGE");
}

/**
 * TC-0x2A-012: Positive response format: resp.data[0] = 0x6A, resp.length = 1.
 */
ZTEST(test_service_0x2A, tc012_positive_response_format)
{
    setup();
    uds_msg_buf_t req  = make_req(UDS_PERIODIC_MODE_SLOW, (uint8_t)0xABU);
    uds_msg_buf_t resp;
    memset(&resp, 0, sizeof(resp));

    (void)uds_service_0x2A_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], (uint8_t)0x6AU,
                  "positive response SID must be 0x6A");
    zassert_equal(resp.length, (uint16_t)1U,
                  "positive response for 0x2A must be exactly 1 byte");
}

/* ---- [EDS#466] [SEC-PDID-ACCESS-01] ISO 14229-1:2026 §10.5, Figure 24 -- */

static uds_msg_buf_t make_req_n(uint8_t mode, const uint8_t *ids, uint16_t n)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = (uint8_t)0x2AU;
    r.data[1] = mode;
    for (uint16_t i = 0U; i < n; i++) { r.data[2U + i] = ids[i]; }
    r.length = (uint16_t)(2U + n);
    return r;
}

static uds_status_t send(uint8_t mode, const uint8_t *ids, uint16_t n)
{
    uds_msg_buf_t req  = make_req_n(mode, ids, n);
    uds_msg_buf_t resp = {0};
    return uds_service_0x2A_handler(&g_srv, &req, &resp);
}

/** TC-0x2A-013: stopSending with no pDID stops all (Table 218 note). */
ZTEST(test_service_0x2A, tc013_stop_all_without_pdid)
{
    setup();
    static const uint8_t ids[] = { 0xABU, 0x10U };
    zassert_equal(send(UDS_PERIODIC_MODE_FAST, ids, 2U), UDS_STATUS_OK, "subscribe");
    zassert_equal(send(UDS_PERIODIC_MODE_STOP, NULL, 0U), UDS_STATUS_OK,
                  "2A 04 with no pDID must be accepted (EDS#466)");
    zassert_false(uds_periodic_is_subscribed(0xABU), "0xAB stopped");
    zassert_false(uds_periodic_is_subscribed(0x10U), "0x10 stopped");
}

/** TC-0x2A-014: an unsupported pDID is skipped; the supported one is
 *  scheduled (Figure 24). */
ZTEST(test_service_0x2A, tc014_unsupported_pdid_skipped)
{
    setup();
    static const uint8_t ids[] = { 0xFFU, 0xCDU, 0x02U, 0xABU };  /* unknown, write-only, not in EXTENDED, ok */
    zassert_equal(send(UDS_PERIODIC_MODE_SLOW, ids, 4U), UDS_STATUS_OK,
                  "supported pDID must be scheduled (EDS#466)");
    zassert_true(uds_periodic_is_subscribed(0xABU), "0xAB scheduled");
    zassert_false(uds_periodic_is_subscribed(0x02U), "pDID not available in EXTENDED skipped");
}

/** TC-0x2A-015: scheduler can't take all → NRC 0x31 and nothing scheduled
 *  (§10.5.1). */
ZTEST(test_service_0x2A, tc015_scheduler_full_schedules_none)
{
    setup();
    static const uint8_t ids[] = { 0x10U, 0x11U, 0x12U, 0x13U, 0x14U,
                                   0x15U, 0x16U, 0x17U, 0x18U };      /* 9 > 8 */
    zassert_equal(send(UDS_PERIODIC_MODE_SLOW, ids, 9U),
                  UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE, "over capacity → 0x31");
    zassert_equal(uds_periodic_free_slots(), (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS,
                  "none of the request may be scheduled (EDS#466)");
}

/** TC-0x2A-020: Figure 24 checks the length before the transmissionMode, so a
 *  two-byte request with a mode other than stopSending is NRC 0x13 whether or
 *  not the mode is valid (`2A 00`, `2A 01`, `2A 05`); stopSending stays valid [EDS#546]. */
ZTEST(test_service_0x2A, tc020_length_checked_before_mode)
{
    setup();
    static const uint8_t modes[] = { 0x00U, 0x01U, 0x05U, 0xFFU };
    uint8_t i;
    for (i = 0U; i < (uint8_t)sizeof(modes); i++) {
        uds_msg_buf_t req  = {0};
        uds_msg_buf_t resp = {0};
        req.data[0] = (uint8_t)0x2AU;
        req.data[1] = modes[i];
        req.length  = (uint16_t)2U;
        zassert_equal(uds_service_0x2A_handler(&g_srv, &req, &resp),
                      UDS_STATUS_ERR_INVALID_PARAM, "2A %02X must be 0x13", modes[i]);
    }
}

/** TC-0x2A-021: no periodic CAN ID configured → NRC 0x22 and nothing scheduled;
 *  stopSending still works [EDS#546, ISO 14229-3 Table 3: no implicit default]. */
ZTEST(test_service_0x2A, tc021_no_periodic_can_id_is_0x22)
{
    setup();
    static const uint8_t ids[] = { 0xABU };
    uds_periodic_set_tx_can_id((uint32_t)0U);
    zassert_equal(send(UDS_PERIODIC_MODE_SLOW, ids, 1U),
                  UDS_STATUS_ERR_CONDITIONS_NOT_MET, "no periodic CAN ID → 0x22");
    zassert_false(uds_periodic_is_subscribed(0xABU), "nothing scheduled");
    zassert_equal(send(UDS_PERIODIC_MODE_STOP, ids, 1U), UDS_STATUS_OK, "stop is fine");
    /* an unsupported pDID is still 0x31: the 0x22 check comes after the pDID checks */
    static const uint8_t bad[] = { 0x77U };
    zassert_equal(send(UDS_PERIODIC_MODE_SLOW, bad, 1U),
                  UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE, "unsupported pDID stays 0x31");
    uds_periodic_set_tx_can_id((uint32_t)0x5E8U);
}

/** TC-0x2A-016: a repeated pDID is scheduled once (§10.5.1). */
ZTEST(test_service_0x2A, tc016_repeated_pdid_once)
{
    setup();
    static const uint8_t ids[] = { 0xABU, 0xABU, 0xABU };
    zassert_equal(send(UDS_PERIODIC_MODE_FAST, ids, 3U), UDS_STATUS_OK, "accepted");
    zassert_equal(uds_periodic_free_slots(),
                  (uint8_t)(UDS_PERIODIC_MAX_SUBSCRIPTIONS - 1U), "one slot used");
}

/** TC-0x2A-017: [SEC-PDID-ACCESS-01] a secured pDID while locked → NRC 0x33
 *  and nothing scheduled. */
ZTEST(test_service_0x2A, tc017_secured_pdid_denied)
{
    setup();
    static const uint8_t ids[] = { 0xABU, 0x01U };
    zassert_equal(send(UDS_PERIODIC_MODE_FAST, ids, 2U),
                  UDS_STATUS_ERR_SEC_NOT_UNLOCKED, "secured pDID must be 0x33");
    zassert_false(uds_periodic_is_subscribed(0xABU), "nothing scheduled");
    zassert_false(uds_periodic_is_subscribed(0x01U), "secured pDID not scheduled");
}

/** TC-0x2A-018: [SEC-PDID-ACCESS-01] a secured pDID subscribed while unlocked
 *  stops streaming once security re-locks. */
ZTEST(test_service_0x2A, tc018_relock_stops_secured_stream)
{
    setup();
    static uds_msg_buf_t frame;
    static const uint8_t ids[] = { 0x01U };
    g_sec.active_level = 1U;
    zassert_equal(send(UDS_PERIODIC_MODE_FAST, ids, 1U), UDS_STATUS_OK, "unlocked: accepted");

    for (uint16_t t = 0U; t < (uint16_t)UDS_PERIODIC_FAST_MS; t++) { (void)uds_periodic_tick_1ms(); }
    zassert_equal(uds_periodic_pop_due(&frame), UDS_STATUS_OK, "streams while unlocked");

    (void)uds_security_reset(&g_sec);                  /* re-lock, as on DSC */
    for (uint16_t t = 0U; t < (uint16_t)UDS_PERIODIC_FAST_MS; t++) { (void)uds_periodic_tick_1ms(); }
    zassert_equal(uds_periodic_pop_due(&frame), UDS_STATUS_ERR_NOT_FOUND,
                  "must not stream after re-lock (SEC-PDID-ACCESS-01)");
    zassert_false(uds_periodic_is_subscribed(0x01U), "subscription dropped");
}

/** TC-0x2A-019: pDID 0x00 (DID 0xF200) is scheduled and streams. */
ZTEST(test_service_0x2A, tc019_pdid_zero_streams)
{
    setup();
    static uds_msg_buf_t frame;
    static const uint8_t ids[] = { 0x00U };
    zassert_equal(send(UDS_PERIODIC_MODE_FAST, ids, 1U), UDS_STATUS_OK, "accepted");
    (void)uds_periodic_tick_1ms();     /* first message at the next tick [EDS#546] */
    zassert_equal(uds_periodic_pop_due(&frame), UDS_STATUS_OK, "pDID 0x00 must stream");
    zassert_equal(frame.data[0], 0x00U, "pDID is byte 1 of the periodic message");
}

/* =========================================================================
 * AUTO-GENERATED: run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x2A__tc001_null_ctx(void);
extern void test_service_0x2A__tc002_null_req(void);
extern void test_service_0x2A__tc003_too_short(void);
extern void test_service_0x2A__tc004_invalid_mode_zero(void);
extern void test_service_0x2A__tc005_invalid_mode_high(void);
extern void test_service_0x2A__tc006_subscribe_slow(void);
extern void test_service_0x2A__tc007_subscribe_medium(void);
extern void test_service_0x2A__tc008_subscribe_fast(void);
extern void test_service_0x2A__tc009_stop_sending(void);
extern void test_service_0x2A__tc010_unknown_did(void);
extern void test_service_0x2A__tc011_write_only_did(void);
extern void test_service_0x2A__tc012_positive_response_format(void);
extern void test_service_0x2A__tc013_stop_all_without_pdid(void);
extern void test_service_0x2A__tc014_unsupported_pdid_skipped(void);
extern void test_service_0x2A__tc015_scheduler_full_schedules_none(void);
extern void test_service_0x2A__tc016_repeated_pdid_once(void);
extern void test_service_0x2A__tc017_secured_pdid_denied(void);
extern void test_service_0x2A__tc018_relock_stops_secured_stream(void);
extern void test_service_0x2A__tc019_pdid_zero_streams(void);
extern void test_service_0x2A__tc020_length_checked_before_mode(void);
extern void test_service_0x2A__tc021_no_periodic_can_id_is_0x22(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x2A__tc001_null_ctx);
    RUN_TEST(test_service_0x2A__tc002_null_req);
    RUN_TEST(test_service_0x2A__tc003_too_short);
    RUN_TEST(test_service_0x2A__tc004_invalid_mode_zero);
    RUN_TEST(test_service_0x2A__tc005_invalid_mode_high);
    RUN_TEST(test_service_0x2A__tc006_subscribe_slow);
    RUN_TEST(test_service_0x2A__tc007_subscribe_medium);
    RUN_TEST(test_service_0x2A__tc008_subscribe_fast);
    RUN_TEST(test_service_0x2A__tc009_stop_sending);
    RUN_TEST(test_service_0x2A__tc010_unknown_did);
    RUN_TEST(test_service_0x2A__tc011_write_only_did);
    RUN_TEST(test_service_0x2A__tc012_positive_response_format);
    RUN_TEST(test_service_0x2A__tc013_stop_all_without_pdid);
    RUN_TEST(test_service_0x2A__tc014_unsupported_pdid_skipped);
    RUN_TEST(test_service_0x2A__tc015_scheduler_full_schedules_none);
    RUN_TEST(test_service_0x2A__tc016_repeated_pdid_once);
    RUN_TEST(test_service_0x2A__tc017_secured_pdid_denied);
    RUN_TEST(test_service_0x2A__tc018_relock_stops_secured_stream);
    RUN_TEST(test_service_0x2A__tc019_pdid_zero_streams);
    RUN_TEST(test_service_0x2A__tc020_length_checked_before_mode);
    RUN_TEST(test_service_0x2A__tc021_no_periodic_can_id_is_0x22);
}
