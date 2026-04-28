// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_service_0x85.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x85.c
 *                    SID 0x85 — ControlDTCSetting
 *
 * PURPOSE:
 *   Verify DTCSetting on/off transitions, session gating, response format,
 *   suppress-bit stripping, optional control record handling, and the
 *   automatic restore-on-default-session path.
 *
 * TEST CASES:
 *   TC-0x85-001  DTCSettingOn  (0x01) in EXTENDED session → 0xC5 0x01, len=2
 *   TC-0x85-002  DTCSettingOff (0x02) in EXTENDED session → 0xC5 0x02
 *   TC-0x85-003  dtcSettingType echoed correctly in response byte 1
 *   TC-0x85-004  Response is exactly 2 bytes
 *   TC-0x85-005  DEFAULT session → ERR_SERVICE_NOT_SUPPORTED_IN_SESSION (NRC 0x7E)
 *   TC-0x85-006  PROGRAMMING session is accepted
 *   TC-0x85-007  Unsupported setting type 0x00 → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12)
 *   TC-0x85-008  Unsupported setting type 0x03 → ERR_SUBFUNCTION_NOT_SUP
 *   TC-0x85-009  Request length 1 (SID only) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x85-010  Optional control record bytes accepted (length > 2 is OK)
 *   TC-0x85-011  suppressPosRspMsgIndicationBit stripped (0x82 → 0x02)
 *   TC-0x85-012  DTC setting state changes: dtc_setting_is_on() reflects mode
 *   TC-0x85-013  After Off then On, dtc_setting_is_on() returns true again
 *   TC-0x85-014  restore_defaults() restores DTC setting to ON
 *   TC-0x85-015  uds_comm_control_dtc_setting_is_on() = true before any request
 *   TC-0x85-016  NULL req → ERR_NULL_PTR
 *   TC-0x85-017  NULL resp → ERR_NULL_PTR
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "services.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_types.h"
#include "uds_comm_control.h"

/* Test-only reset (defined in uds_comm_control.c, not in public header) */
extern void uds_comm_control_test_reset(void);

/* =========================================================================
 * Security stubs
 * ========================================================================= */

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0U; i < n && i < 4U; i++) { b[i] = (uint8_t)(0x85U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl,
                  const uint8_t *k, uint8_t kl)
{
    (void)l; (void)s; (void)sl; (void)k; (void)kl;
    return true;
}

/* =========================================================================
 * Shared context
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

static void setup(void)
{
    uds_comm_control_test_reset();

    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));

    uds_session_init(&g_sess, 5000U);
    uds_session_transition(&g_sess, UDS_SESSION_EXTENDED); /* 0x85 needs non-default */

    static const uds_security_cfg_t sc = {
        .max_attempts     = 3U,
        .lockout_ms       = 100U,
        .key_validate_cb  = t_key,
        .seed_generate_cb = t_seed,
    };
    uds_security_init(&g_sec, &sc);

    static const uds_server_cfg_t svc = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_uds_service_table,
        .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
    };
    uds_server_init(&g_srv, &svc);

    /* Initialize comm_control with no platform callbacks */
    static const uds_comm_control_cfg_t cc_cfg = {
        .comm_cb = NULL,
        .dtc_cb  = NULL,
    };
    uds_comm_control_init(&cc_cfg);
}

/** Build a ControlDTCSetting request. */
static uds_msg_buf_t make_req(uint8_t setting_type)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x85U;
    r.data[1] = setting_type;
    r.length  = 2U;
    return r;
}

/* =========================================================================
 * Test cases
 * ========================================================================= */

ZTEST_SUITE(test_service_0x85, NULL, NULL, NULL, NULL, NULL);

/**
 * TC-0x85-001: DTCSettingOn (0x01) in EXTENDED session → positive response 0xC5 0x01.
 */
ZTEST(test_service_0x85, tc001_dtc_setting_on)
{
    setup();

    uds_msg_buf_t req  = make_req(0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "DTCSettingOn must return OK");
    zassert_equal(resp.data[0], 0xC5U, "response SID must be 0xC5");
    zassert_equal(resp.data[1], 0x01U, "echo setting type must be 0x01");
}

/**
 * TC-0x85-002: DTCSettingOff (0x02) in EXTENDED session → 0xC5 0x02.
 */
ZTEST(test_service_0x85, tc002_dtc_setting_off)
{
    setup();

    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "DTCSettingOff must return OK");
    zassert_equal(resp.data[0], 0xC5U, "response SID must be 0xC5");
    zassert_equal(resp.data[1], 0x02U, "echo setting type must be 0x02");
}

/**
 * TC-0x85-003: dtcSettingType is echoed correctly in response byte 1.
 */
ZTEST(test_service_0x85, tc003_setting_type_echoed)
{
    setup();

    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};
    uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[1], 0x02U,
        "echoed dtcSettingType must match request");
}

/**
 * TC-0x85-004: Response is exactly 2 bytes.
 */
ZTEST(test_service_0x85, tc004_response_two_bytes)
{
    setup();

    uds_msg_buf_t req  = make_req(0x01U);
    uds_msg_buf_t resp = {0};
    uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 2U,
        "ControlDTCSetting response must be exactly 2 bytes");
}

/**
 * TC-0x85-005: In DEFAULT session → ERR_SERVICE_NOT_SUPPORTED_IN_SESSION (NRC 0x7E).
 */
ZTEST(test_service_0x85, tc005_default_session_rejected)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_DEFAULT);

    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION,
        "DEFAULT session must return NRC 0x7E");
}

/**
 * TC-0x85-006: PROGRAMMING session is accepted.
 */
ZTEST(test_service_0x85, tc006_programming_session_accepted)
{
    setup();
    uds_session_transition(&g_sess, UDS_SESSION_PROGRAMMING);

    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
        "PROGRAMMING session must be accepted for 0x85");
}

/**
 * TC-0x85-007: Setting type 0x00 is undefined → ERR_SUBFUNCTION_NOT_SUP (NRC 0x12).
 */
ZTEST(test_service_0x85, tc007_unsupported_type_00)
{
    setup();

    uds_msg_buf_t req  = make_req(0x00U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
        "setting type 0x00 must return NRC 0x12");
}

/**
 * TC-0x85-008: Setting type 0x03 is undefined → ERR_SUBFUNCTION_NOT_SUP.
 */
ZTEST(test_service_0x85, tc008_unsupported_type_03)
{
    setup();

    uds_msg_buf_t req  = make_req(0x03U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
        "setting type 0x03 must return NRC 0x12");
}

/**
 * TC-0x85-009: Request length 1 (SID only, no settingType) → ERR_INVALID_PARAM (NRC 0x13).
 */
ZTEST(test_service_0x85, tc009_length_one_rejected)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x85U;
    req.length  = 1U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "length-1 request must return NRC 0x13");
}

/**
 * TC-0x85-010: Request with optional control record bytes (length > 2) is accepted.
 * ISO 14229-1 §15.3.2: extra bytes after dtcSettingType are the
 * dtcSettingControlOptionRecord; this implementation ignores them.
 */
ZTEST(test_service_0x85, tc010_optional_control_record_accepted)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x85U;
    req.data[1] = 0x02U; /* DTCSettingOff */
    req.data[2] = 0xABU; /* optional record byte 1 */
    req.data[3] = 0xCDU; /* optional record byte 2 */
    req.length  = 4U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
        "optional control record bytes must not cause rejection");
}

/**
 * TC-0x85-011: suppressPosRspMsgIndicationBit stripped.
 * 0x82 = (0x80 | 0x02) → handler treats as DTCSettingOff.
 */
ZTEST(test_service_0x85, tc011_suppress_bit_stripped)
{
    setup();

    uds_msg_buf_t req  = make_req(0x82U); /* 0x80 | 0x02 */
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
        "suppress bit must be stripped; DTCSettingOff must succeed");
    zassert_equal(resp.data[1], 0x02U,
        "echoed settingType must be 0x02 (suppress bit stripped)");
}

/**
 * TC-0x85-012: After DTCSettingOff request, dtc_setting_is_on() returns false.
 */
ZTEST(test_service_0x85, tc012_dtc_setting_off_state)
{
    setup();

    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};
    uds_service_0x85_handler(&g_srv, &req, &resp);

    zassert_false(uds_comm_control_dtc_setting_is_on(),
        "DTC setting must be OFF after DTCSettingOff request");
}

/**
 * TC-0x85-013: After Off then On, dtc_setting_is_on() returns true again.
 */
ZTEST(test_service_0x85, tc013_dtc_setting_on_restores_state)
{
    setup();

    /* Turn off */
    uds_msg_buf_t off_req  = make_req(0x02U);
    uds_msg_buf_t off_resp = {0};
    uds_service_0x85_handler(&g_srv, &off_req, &off_resp);
    zassert_false(uds_comm_control_dtc_setting_is_on(), "precondition: must be OFF");

    /* Turn on */
    uds_msg_buf_t on_req  = make_req(0x01U);
    uds_msg_buf_t on_resp = {0};
    uds_service_0x85_handler(&g_srv, &on_req, &on_resp);

    zassert_true(uds_comm_control_dtc_setting_is_on(),
        "DTC setting must be ON after DTCSettingOn request");
}

/**
 * TC-0x85-014: After restore_defaults(), DTC setting returns to ON regardless
 * of what was set by the tester. Models Default session transition.
 */
ZTEST(test_service_0x85, tc014_restore_defaults_reenables_dtc_setting)
{
    setup();

    /* Disable DTC setting */
    uds_msg_buf_t req  = make_req(0x02U);
    uds_msg_buf_t resp = {0};
    uds_service_0x85_handler(&g_srv, &req, &resp);
    zassert_false(uds_comm_control_dtc_setting_is_on(), "precondition: must be OFF");

    /* Simulate Default session transition */
    uds_comm_control_restore_defaults();

    zassert_true(uds_comm_control_dtc_setting_is_on(),
        "DTC setting must be restored to ON on Default session transition");
}

/**
 * TC-0x85-015: Before any request, DTC setting is ON by default.
 */
ZTEST(test_service_0x85, tc015_default_state_is_on)
{
    setup();

    zassert_true(uds_comm_control_dtc_setting_is_on(),
        "DTC setting must be ON by default (no request issued yet)");
}

/**
 * TC-0x85-016: NULL req → ERR_NULL_PTR.
 */
ZTEST(test_service_0x85, tc016_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x85_handler(&g_srv, NULL, &resp);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
        "NULL req must return ERR_NULL_PTR");
}

/**
 * TC-0x85-017: NULL resp → ERR_NULL_PTR.
 */
ZTEST(test_service_0x85, tc017_null_resp)
{
    setup();
    uds_msg_buf_t req = make_req(0x01U);
    uds_status_t rc = uds_service_0x85_handler(&g_srv, &req, NULL);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
        "NULL resp must return ERR_NULL_PTR");
}

/* =========================================================================
 * run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x85__tc001_dtc_setting_on(void);
extern void test_service_0x85__tc002_dtc_setting_off(void);
extern void test_service_0x85__tc003_setting_type_echoed(void);
extern void test_service_0x85__tc004_response_two_bytes(void);
extern void test_service_0x85__tc005_default_session_rejected(void);
extern void test_service_0x85__tc006_programming_session_accepted(void);
extern void test_service_0x85__tc007_unsupported_type_00(void);
extern void test_service_0x85__tc008_unsupported_type_03(void);
extern void test_service_0x85__tc009_length_one_rejected(void);
extern void test_service_0x85__tc010_optional_control_record_accepted(void);
extern void test_service_0x85__tc011_suppress_bit_stripped(void);
extern void test_service_0x85__tc012_dtc_setting_off_state(void);
extern void test_service_0x85__tc013_dtc_setting_on_restores_state(void);
extern void test_service_0x85__tc014_restore_defaults_reenables_dtc_setting(void);
extern void test_service_0x85__tc015_default_state_is_on(void);
extern void test_service_0x85__tc016_null_req(void);
extern void test_service_0x85__tc017_null_resp(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x85__tc001_dtc_setting_on);
    RUN_TEST(test_service_0x85__tc002_dtc_setting_off);
    RUN_TEST(test_service_0x85__tc003_setting_type_echoed);
    RUN_TEST(test_service_0x85__tc004_response_two_bytes);
    RUN_TEST(test_service_0x85__tc005_default_session_rejected);
    RUN_TEST(test_service_0x85__tc006_programming_session_accepted);
    RUN_TEST(test_service_0x85__tc007_unsupported_type_00);
    RUN_TEST(test_service_0x85__tc008_unsupported_type_03);
    RUN_TEST(test_service_0x85__tc009_length_one_rejected);
    RUN_TEST(test_service_0x85__tc010_optional_control_record_accepted);
    RUN_TEST(test_service_0x85__tc011_suppress_bit_stripped);
    RUN_TEST(test_service_0x85__tc012_dtc_setting_off_state);
    RUN_TEST(test_service_0x85__tc013_dtc_setting_on_restores_state);
    RUN_TEST(test_service_0x85__tc014_restore_defaults_reenables_dtc_setting);
    RUN_TEST(test_service_0x85__tc015_default_state_is_on);
    RUN_TEST(test_service_0x85__tc016_null_req);
    RUN_TEST(test_service_0x85__tc017_null_resp);
}
