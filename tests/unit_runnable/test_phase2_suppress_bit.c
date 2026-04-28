// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_phase2_suppress_bit.c
 *
 * PURPOSE: Phase-2 regression tests — suppressPosRspMsgIndicationBit handling
 *          for SID 0x10, 0x11, 0x3E and P2/P2* encoding from server config.
 *
 * TEST CASES:
 *   TC-SUPP-001  0x10 bit 7 set        → resp.length == 0
 *   TC-SUPP-002  0x10 bit 7 clear      → resp.length == 6
 *   TC-SUPP-003  0x10 0x83 session     → transitions to EXTENDED (0x03)
 *   TC-SUPP-004  0x11 bit 7 set        → resp.length == 0
 *   TC-SUPP-005  0x11 bit 7 clear      → resp.length == 2
 *   TC-SUPP-006  0x11 hardReset        → pending_reset_type == 0x01
 *   TC-SUPP-007  0x3E bit 7 set        → resp.length == 0
 *   TC-SUPP-008  0x3E bit 7 clear      → resp.length == 2
 *   TC-SUPP-009  0x3E 0x80 still resets S3 timer
 *   TC-SUPP-010  0x10 P2 from config   → bytes encode cfg.p2_server_max_ms=50
 *   TC-SUPP-011  0x10 P2* from config  → bytes encode cfg.p2_star/10=200
 *   TC-SUPP-012  DEFAULT transition resets security
 *
 * FRAMEWORK: Zephyr Ztest (host shim)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stdbool.h>
#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_types.h"

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0; i < n && i < 4U; i++) { b[i] = (uint8_t)(0xA0U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl,
                  const uint8_t *k, uint8_t kl)
{
    (void)l;
    if (sl != kl) return false;
    for (uint8_t i = 0; i < sl; i++) {
        if (k[i] != (uint8_t)(s[i] ^ 0xAAU)) return false;
    }
    return true;
}

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

static void setup(void)
{
    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));
    uds_session_init(&g_sess, 5000U);
    static const uds_security_cfg_t sc = {
        .max_attempts = 3U, .lockout_ms = 100U,
        .key_validate_cb = t_key, .seed_generate_cb = t_seed,
    };
    uds_security_init(&g_sec, &sc);
    static const uds_server_cfg_t svc = {
        .p2_server_max_ms = 50U, .p2_star_server_max_ms = 2000U,
        .session_ctx = &g_sess, .security_ctx = &g_sec,
        .service_table = g_uds_service_table,
        .service_table_count = (uint8_t)UDS_SERVICE_TABLE_COUNT,
    };
    uds_server_init(&g_srv, &svc);
}

static uds_msg_buf_t req2(uint8_t sid, uint8_t sub)
{
    uds_msg_buf_t r; memset(&r, 0, sizeof(r));
    r.data[0] = sid; r.data[1] = sub; r.length = 2U; return r;
}

ZTEST_SUITE(test_phase2_suppress, NULL, NULL, NULL, NULL, NULL);

ZTEST(test_phase2_suppress, tc001_0x10_suppress_set)
{
    setup();
    uds_msg_buf_t req = req2(0x10U, (uint8_t)(0x03U | 0x80U));
    uds_msg_buf_t rsp = {0};
    zassert_equal(UDS_STATUS_OK, uds_service_0x10_handler(&g_srv, &req, &rsp), "");
    zassert_equal((uint16_t)0U, rsp.length, "Suppress bit must zero response length");
}

ZTEST(test_phase2_suppress, tc002_0x10_suppress_clear)
{
    setup();
    uds_msg_buf_t req = req2(0x10U, 0x03U);
    uds_msg_buf_t rsp = {0};
    zassert_equal(UDS_STATUS_OK, uds_service_0x10_handler(&g_srv, &req, &rsp), "");
    zassert_equal((uint16_t)6U, rsp.length, "Normal 0x10 response = 6 bytes");
}

ZTEST(test_phase2_suppress, tc003_0x10_session_masked)
{
    setup();
    uds_msg_buf_t req = req2(0x10U, 0x83U); /* 0x80|0x03 = EXTENDED + suppress */
    uds_msg_buf_t rsp = {0};
    uds_service_0x10_handler(&g_srv, &req, &rsp);
    uds_session_type_t s;
    uds_session_get_active(&g_sess, &s);
    zassert_equal(UDS_SESSION_EXTENDED, s, "Session must be EXTENDED despite suppress bit");
}

ZTEST(test_phase2_suppress, tc004_0x11_suppress_set)
{
    setup();
    uds_msg_buf_t req = req2(0x11U, (uint8_t)(0x01U | 0x80U));
    uds_msg_buf_t rsp = {0};
    zassert_equal(UDS_STATUS_OK, uds_service_0x11_handler(&g_srv, &req, &rsp), "");
    zassert_equal((uint16_t)0U, rsp.length, "Suppress bit must zero 0x11 response");
}

ZTEST(test_phase2_suppress, tc005_0x11_suppress_clear)
{
    setup();
    uds_msg_buf_t req = req2(0x11U, 0x01U);
    uds_msg_buf_t rsp = {0};
    zassert_equal(UDS_STATUS_OK, uds_service_0x11_handler(&g_srv, &req, &rsp), "");
    zassert_equal((uint16_t)2U, rsp.length, "Normal 0x11 response = 2 bytes");
}

ZTEST(test_phase2_suppress, tc006_0x11_pending_reset)
{
    setup();
    g_srv.pending_reset_type = 0U;
    uds_msg_buf_t req = req2(0x11U, 0x01U);
    uds_msg_buf_t rsp = {0};
    uds_service_0x11_handler(&g_srv, &req, &rsp);
    zassert_equal(0x01U, g_srv.pending_reset_type, "pending_reset_type must be hardReset");
}

ZTEST(test_phase2_suppress, tc007_0x3E_suppress_set)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    uds_msg_buf_t req = req2(0x3EU, 0x80U);
    uds_msg_buf_t rsp = {0};
    zassert_equal(UDS_STATUS_OK, uds_service_0x3E_handler(&g_srv, &req, &rsp), "");
    zassert_equal((uint16_t)0U, rsp.length, "Suppress bit must zero 0x3E response");
}

ZTEST(test_phase2_suppress, tc008_0x3E_suppress_clear)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    uds_msg_buf_t req = req2(0x3EU, 0x00U);
    uds_msg_buf_t rsp = {0};
    zassert_equal(UDS_STATUS_OK, uds_service_0x3E_handler(&g_srv, &req, &rsp), "");
    zassert_equal((uint16_t)2U, rsp.length, "Normal 0x3E response = 2 bytes");
}

ZTEST(test_phase2_suppress, tc009_0x3E_suppress_resets_s3)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    g_sess.s3_timer_ms = 100U;
    uds_msg_buf_t req = req2(0x3EU, 0x80U);
    uds_msg_buf_t rsp = {0};
    uds_service_0x3E_handler(&g_srv, &req, &rsp);
    zassert_equal(g_sess.s3_timeout_cfg_ms, g_sess.s3_timer_ms,
                  "S3 timer reset even with suppress bit");
}

ZTEST(test_phase2_suppress, tc010_p2_from_config)
{
    setup();
    uds_msg_buf_t req = req2(0x10U, 0x03U);
    uds_msg_buf_t rsp = {0};
    uds_service_0x10_handler(&g_srv, &req, &rsp);
    zassert_equal(0x00U, rsp.data[2], "P2 hi for 50ms");
    zassert_equal(0x32U, rsp.data[3], "P2 lo for 50ms");
}

ZTEST(test_phase2_suppress, tc011_p2star_from_config)
{
    setup();
    uds_msg_buf_t req = req2(0x10U, 0x03U);
    uds_msg_buf_t rsp = {0};
    uds_service_0x10_handler(&g_srv, &req, &rsp);
    /* 2000ms / 10 = 200 = 0x00C8 */
    zassert_equal(0x00U, rsp.data[4], "P2* hi for 2000ms");
    zassert_equal(0xC8U, rsp.data[5], "P2* lo for 2000ms");
}

ZTEST(test_phase2_suppress, tc012_default_resets_security)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED);
    g_sec.active_level = 0x01U;
    uds_msg_buf_t req = req2(0x10U, 0x01U);
    uds_msg_buf_t rsp = {0};
    uds_service_0x10_handler(&g_srv, &req, &rsp);
    bool unlocked = true;
    uds_security_is_unlocked(&g_sec, 0x01U, &unlocked);
    zassert_equal(false, unlocked, "Security must be locked after DEFAULT transition");
}

/* Runner */
extern void test_phase2_suppress__tc001_0x10_suppress_set(void);
extern void test_phase2_suppress__tc002_0x10_suppress_clear(void);
extern void test_phase2_suppress__tc003_0x10_session_masked(void);
extern void test_phase2_suppress__tc004_0x11_suppress_set(void);
extern void test_phase2_suppress__tc005_0x11_suppress_clear(void);
extern void test_phase2_suppress__tc006_0x11_pending_reset(void);
extern void test_phase2_suppress__tc007_0x3E_suppress_set(void);
extern void test_phase2_suppress__tc008_0x3E_suppress_clear(void);
extern void test_phase2_suppress__tc009_0x3E_suppress_resets_s3(void);
extern void test_phase2_suppress__tc010_p2_from_config(void);
extern void test_phase2_suppress__tc011_p2star_from_config(void);
extern void test_phase2_suppress__tc012_default_resets_security(void);

void run_all_tests(void)
{
    RUN_TEST(test_phase2_suppress__tc001_0x10_suppress_set);
    RUN_TEST(test_phase2_suppress__tc002_0x10_suppress_clear);
    RUN_TEST(test_phase2_suppress__tc003_0x10_session_masked);
    RUN_TEST(test_phase2_suppress__tc004_0x11_suppress_set);
    RUN_TEST(test_phase2_suppress__tc005_0x11_suppress_clear);
    RUN_TEST(test_phase2_suppress__tc006_0x11_pending_reset);
    RUN_TEST(test_phase2_suppress__tc007_0x3E_suppress_set);
    RUN_TEST(test_phase2_suppress__tc008_0x3E_suppress_clear);
    RUN_TEST(test_phase2_suppress__tc009_0x3E_suppress_resets_s3);
    RUN_TEST(test_phase2_suppress__tc010_p2_from_config);
    RUN_TEST(test_phase2_suppress__tc011_p2star_from_config);
    RUN_TEST(test_phase2_suppress__tc012_default_resets_security);
}
