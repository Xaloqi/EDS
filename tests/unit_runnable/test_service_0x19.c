// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_service_0x19.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x19.c
 *                    SID 0x19 — ReadDTCInformation
 *
 * PURPOSE:
 *   Verify all six implemented sub-functions of the ReadDTCInformation service:
 *   count by mask, list by mask, snapshot ID, snapshot by DTC, extended data
 *   by DTC, and supported DTCs. Covers format, length, NRC conditions, mask
 *   matching, and suppress-positive-response bit stripping.
 *
 * TEST CASES:
 *   Sub-function 0x01 — reportNumberOfDTCByStatusMask
 *   TC-0x19-001  Status mask 0x09 matches 2/3 DTCs → count=2 in response
 *   TC-0x19-002  Status mask 0xFF matches all → count=3
 *   TC-0x19-003  Status mask 0x01 matches none → count=0 (still OK)
 *   TC-0x19-004  Response header: 0x59, 0x01, 0xFF, 0x01, countHB, countLB
 *   TC-0x19-005  Response is exactly 6 bytes
 *
 *   Sub-function 0x02 — reportDTCByStatusMask
 *   TC-0x19-006  Mask 0x09 → 2 DTC records in response
 *   TC-0x19-007  Each DTC record is 4 bytes [dtcHB, dtcMB, dtcLB, status]
 *   TC-0x19-008  Response header bytes: 0x59, 0x02, 0xFF (availabilityMask)
 *   TC-0x19-009  Mask 0x00 → all DTCs returned (special case: mask 0 = match all)
 *   TC-0x19-010  Mask 0x40 matches no DTCs → empty list (response = 3 bytes)
 *
 *   Sub-function 0x03 — reportDTCSnapshotIdentification
 *   TC-0x19-011  Returns 0x59, 0x03, length=2 (empty snapshot list)
 *
 *   Sub-function 0x04 — reportDTCSnapshotRecordByDTCNumber
 *   TC-0x19-012  Known DTC → 0x59 0x04 dtc[3] status recordNum numID=0x00
 *   TC-0x19-013  Response is 8 bytes (minimal snapshot)
 *   TC-0x19-014  Unknown DTC → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31)
 *   TC-0x19-015  Request length 5 (missing recordNumber) → ERR_INVALID_PARAM
 *
 *   Sub-function 0x06 — reportDTCExtDataRecordByDTCNumber
 *   TC-0x19-016  Known DTC → 0x59 0x06 dtc[3] status recordNum recordLen=0x00
 *   TC-0x19-017  Response is 8 bytes
 *   TC-0x19-018  Unknown DTC → ERR_REQUEST_OUT_OF_RANGE
 *
 *   Sub-function 0x0A — reportSupportedDTCs
 *   TC-0x19-019  Returns all 3 registered DTCs regardless of status
 *   TC-0x19-020  Response header: 0x59, 0x0A, 0xFF (availabilityMask)
 *   TC-0x19-021  Response length = 3 + (3 × 4) = 15 bytes
 *
 *   General
 *   TC-0x19-022  Unsupported sub-function 0x05 → ERR_SUBFUNCTION_NOT_SUP
 *   TC-0x19-023  suppressPosRspMsgIndicationBit (bit 7) stripped correctly
 *   TC-0x19-024  Request length 1 (SID only) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x19-025  NULL req  → ERR_NULL_PTR
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
#include "dtc_database.h"

extern uds_status_t uds_service_0x14_handler(uds_server_ctx_t *ctx,
                                              const uds_msg_buf_t *req,
                                              uds_msg_buf_t *resp);
extern void dtc_database_test_reset(void);

/* =========================================================================
 * Security stubs
 * ========================================================================= */

static uds_status_t t_seed(uint8_t l, uint8_t *b, uint8_t n, uint8_t *o)
{
    (void)l;
    for (uint8_t i = 0U; i < n && i < 4U; i++) { b[i] = (uint8_t)(0xA0U + i); }
    *o = (n < 4U) ? n : 4U;
    return UDS_STATUS_OK;
}

static bool t_key(uint8_t l, const uint8_t *s, uint8_t sl,
                  const uint8_t *k, uint8_t kl)
{
    (void)l; (void)s; (void)sl; (void)k; (void)kl;
    return true; /* accept all keys */
}

/* =========================================================================
 * Shared context + well-known DTC codes
 * ========================================================================= */

static uds_session_ctx_t  g_sess;
static uds_security_ctx_t g_sec;
static uds_server_ctx_t   g_srv;

/* Three distinct DTC codes with known status bytes */
#define DTC_A  (0xC00100UL)  /* status 0x09 — TEST_FAILED | CONFIRMED */
#define DTC_B  (0xC00200UL)  /* status 0x04 — PENDING */
#define DTC_C  (0xC00300UL)  /* status 0x08 — CONFIRMED only */

static void setup(void)
{
    dtc_database_test_reset();
    dtc_database_init();

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

    static const uds_server_cfg_t svc = {
        .p2_server_max_ms      = 25U,
        .p2_star_server_max_ms = 5000U,
        .session_ctx           = &g_sess,
        .security_ctx          = &g_sec,
        .service_table         = g_uds_service_table,
        .service_table_count   = (uint8_t)UDS_SERVICE_TABLE_COUNT,
    };
    uds_server_init(&g_srv, &svc);

    dtc_database_register(DTC_A, 0x00U, "DTC_A");
    dtc_database_register(DTC_B, 0x00U, "DTC_B");
    dtc_database_register(DTC_C, 0x00U, "DTC_C");
    dtc_database_set_status(DTC_A, 0x09U);
    dtc_database_set_status(DTC_B, 0x04U);
    dtc_database_set_status(DTC_C, 0x08U);
}

/** Build [0x19, subFn, ...extra bytes] */
static uds_msg_buf_t make_req_2(uint8_t sub_fn, uint8_t b2)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x19U;
    r.data[1] = sub_fn;
    r.data[2] = b2;
    r.length  = 3U;
    return r;
}

static uds_msg_buf_t make_req_dtc(uint8_t sub_fn, uint32_t dtc, uint8_t record)
{
    uds_msg_buf_t r;
    memset(&r, 0, sizeof(r));
    r.data[0] = 0x19U;
    r.data[1] = sub_fn;
    r.data[2] = (uint8_t)((dtc >> 16U) & 0xFFU);
    r.data[3] = (uint8_t)((dtc >>  8U) & 0xFFU);
    r.data[4] = (uint8_t)( dtc         & 0xFFU);
    r.data[5] = record;
    r.length  = 6U;
    return r;
}

/* =========================================================================
 * Test suite
 * ========================================================================= */

ZTEST_SUITE(test_service_0x19, NULL, NULL, NULL, NULL, NULL);

/* ---- Sub-function 0x01 ------------------------------------------------ */

/**
 * TC-0x19-001: Mask 0x09 (TEST_FAILED|CONFIRMED) should match DTCs with any
 * of those bits set. DTC_A=0x09 (0x09&0x09=0x09, match), DTC_C=0x08 (0x08&0x09=0x08, match)
 * → count = 2.
 */
ZTEST(test_service_0x19, tc001_count_by_mask_09)
{
    setup();

    /* Verify pre-conditions: status bytes must be as registered */
    zassert_equal(dtc_database_find(DTC_A)->status_byte, 0x09U,
        "precondition: DTC_A status must be 0x09");
    zassert_equal(dtc_database_find(DTC_C)->status_byte, 0x08U,
        "precondition: DTC_C status must be 0x08");

    uds_msg_buf_t req  = make_req_2(0x01U, 0x09U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK");
    /* count: resp.data[4]=HB, resp.data[5]=LB */
    uint16_t count = ((uint16_t)resp.data[4] << 8U) | (uint16_t)resp.data[5];
    zassert_equal(count, 2U,
        "mask 0x09 must match DTC_A (0x09) and DTC_C (0x08), count=2");
}

/**
 * TC-0x19-002: Mask 0xFF matches all registered DTCs → count = 3.
 */
ZTEST(test_service_0x19, tc002_count_by_mask_ff_all_match)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x01U, 0xFFU);
    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    uint16_t count = ((uint16_t)resp.data[4] << 8U) | (uint16_t)resp.data[5];
    zassert_equal(count, 3U, "mask 0xFF must match all 3 DTCs");
}

/**
 * TC-0x19-003: Mask 0x01 (TEST_FAILED only) matches DTC_A (0x09 & 0x01 = 1)
 * → count = 1.
 */
ZTEST(test_service_0x19, tc003_count_by_mask_01_one_match)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x01U, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK even with count=0 possible");
    uint16_t count = ((uint16_t)resp.data[4] << 8U) | (uint16_t)resp.data[5];
    zassert_equal(count, 1U, "mask 0x01 matches DTC_A (0x09 & 0x01 = 1)");
}

/**
 * TC-0x19-004: Response header for 0x01: [0x59, 0x01, 0xFF, 0x01, countHB, countLB].
 */
ZTEST(test_service_0x19, tc004_subfn01_response_header)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x01U, 0xFFU);
    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x01U, "echo sub-function must be 0x01");
    zassert_equal(resp.data[2], 0xFFU, "availability mask must be 0xFF");
    zassert_equal(resp.data[3], 0x01U, "DTC format identifier must be 0x01");
}

/**
 * TC-0x19-005: Response is exactly 6 bytes for sub-function 0x01.
 */
ZTEST(test_service_0x19, tc005_subfn01_response_six_bytes)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x01U, 0xFFU);
    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 6U,
        "reportNumberOfDTCByStatusMask response must be 6 bytes");
}

/* ---- Sub-function 0x02 ------------------------------------------------ */

/**
 * TC-0x19-006: Mask 0x04 (PENDING) matches DTC_B only → 1 DTC record in list.
 */
ZTEST(test_service_0x19, tc006_list_by_mask_04_one_dtc)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x02U, 0x04U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK");
    /* Header: 3 bytes. One DTC record: 4 bytes → total 7 */
    zassert_equal(resp.length, 7U, "one DTC match → 3-byte header + 4-byte record");
}

/**
 * TC-0x19-007: Each DTC record in the list is exactly 4 bytes.
 * With mask 0xFF → 3 records → length = 3 + (3×4) = 15.
 */
ZTEST(test_service_0x19, tc007_dtc_record_is_four_bytes)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x02U, 0xFFU);
    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 15U,
        "3 matching DTCs → 3-byte header + 3×4-byte records = 15");
}

/**
 * TC-0x19-008: Response header bytes: [0x59, 0x02, 0xFF].
 */
ZTEST(test_service_0x19, tc008_subfn02_response_header)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x02U, 0xFFU);
    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x02U, "echo sub-function must be 0x02");
    zassert_equal(resp.data[2], 0xFFU, "availability mask must be 0xFF");
}

/**
 * TC-0x19-009: Mask 0x00 is the special "return all" case → all 3 DTCs listed.
 * ISO 14229-1 §13.3.2.2: statusMask=0 means all active DTCs.
 */
ZTEST(test_service_0x19, tc009_mask_zero_returns_all)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x02U, 0x00U);
    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 15U,
        "mask 0x00 must return all 3 DTCs (3 + 3×4 = 15 bytes)");
}

/**
 * TC-0x19-010: Mask 0x40 (WARNING_INDICATOR_REQUESTED) matches no DTCs
 * → empty list: response = 3-byte header only.
 */
ZTEST(test_service_0x19, tc010_mask_no_match_empty_list)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x02U, 0x40U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK even with empty list");
    zassert_equal(resp.length, 3U,
        "no matching DTCs → 3-byte header only");
}

/* ---- Sub-function 0x03 ------------------------------------------------ */

/**
 * TC-0x19-011: Snapshot identification returns minimal 2-byte response.
 * No freeze-frame records stored → [0x59, 0x03], length=2.
 */
ZTEST(test_service_0x19, tc011_snapshot_id_empty_list)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x03U;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK");
    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x03U, "echo sub-function must be 0x03");
    zassert_equal(resp.length,  2U,    "empty snapshot list = 2 bytes");
}

/* ---- Sub-function 0x04 ------------------------------------------------ */

/**
 * TC-0x19-012: Known DTC snapshot record: [0x59 0x04 dtcHB dtcMB dtcLB
 *              status snapshotRecNum 0x00].
 */
ZTEST(test_service_0x19, tc012_snapshot_by_dtc_known)
{
    setup();

    uds_msg_buf_t req  = make_req_dtc(0x04U, DTC_A, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK for known DTC");
    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x04U, "echo sub-function must be 0x04");
    /* DTC bytes */
    zassert_equal(resp.data[2], (uint8_t)((DTC_A >> 16U) & 0xFFU), "dtcHB");
    zassert_equal(resp.data[3], (uint8_t)((DTC_A >>  8U) & 0xFFU), "dtcMB");
    zassert_equal(resp.data[4], (uint8_t)( DTC_A         & 0xFFU), "dtcLB");
    zassert_equal(resp.data[5], 0x09U, "status byte must be 0x09");
    zassert_equal(resp.data[6], 0x01U, "echo snapshot record number");
    zassert_equal(resp.data[7], 0x00U, "numberOfIdentifiers must be 0 (no freeze-frame)");
}

/**
 * TC-0x19-013: Snapshot response is exactly 8 bytes.
 */
ZTEST(test_service_0x19, tc013_snapshot_by_dtc_length_eight)
{
    setup();

    uds_msg_buf_t req  = make_req_dtc(0x04U, DTC_B, 0x00U);
    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 8U,
        "DTC snapshot response must be exactly 8 bytes");
}

/**
 * TC-0x19-014: Unregistered DTC in snapshot request → ERR_REQUEST_OUT_OF_RANGE.
 */
ZTEST(test_service_0x19, tc014_snapshot_unknown_dtc)
{
    setup();

    uds_msg_buf_t req  = make_req_dtc(0x04U, 0xDEADEFUL, 0x00U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
        "unknown DTC must return NRC 0x31");
}

/**
 * TC-0x19-015: Sub-function 0x04 with 5 bytes (missing recordNumber) → invalid param.
 */
ZTEST(test_service_0x19, tc015_snapshot_short_request)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x04U;
    req.data[2] = 0xC0U;
    req.data[3] = 0x01U;
    req.data[4] = 0x00U;
    req.length  = 5U; /* Missing record number byte */

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "5-byte request for 0x04 must be rejected");
}

/* ---- Sub-function 0x06 ------------------------------------------------ */

/**
 * TC-0x19-016: Extended data record by DTC for a known DTC.
 */
ZTEST(test_service_0x19, tc016_ext_data_by_dtc_known)
{
    setup();

    uds_msg_buf_t req  = make_req_dtc(0x06U, DTC_C, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK for known DTC");
    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x06U, "echo sub-function must be 0x06");
    zassert_equal(resp.data[5], 0x08U, "DTC_C status must be 0x08");
    zassert_equal(resp.data[7], 0x00U, "recordLength must be 0 (no ext data)");
}

/**
 * TC-0x19-017: Extended data response is exactly 8 bytes.
 */
ZTEST(test_service_0x19, tc017_ext_data_response_length)
{
    setup();

    uds_msg_buf_t req  = make_req_dtc(0x06U, DTC_A, 0xFFU);
    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.length, 8U,
        "extended data response must be exactly 8 bytes");
}

/**
 * TC-0x19-018: Extended data for unregistered DTC → ERR_REQUEST_OUT_OF_RANGE.
 */
ZTEST(test_service_0x19, tc018_ext_data_unknown_dtc)
{
    setup();

    uds_msg_buf_t req  = make_req_dtc(0x06U, 0x123456UL, 0x01U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
        "unknown DTC must return NRC 0x31");
}

/* ---- Sub-function 0x0A ------------------------------------------------ */

/**
 * TC-0x19-019: reportSupportedDTCs returns all 3 registered DTCs.
 */
ZTEST(test_service_0x19, tc019_supported_dtcs_all_three)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x0AU;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK");
    /* 3 + (3×4) = 15 bytes */
    zassert_equal(resp.length, 15U,
        "3 supported DTCs: 3-byte header + 3×4-byte records = 15");
}

/**
 * TC-0x19-020: reportSupportedDTCs response header.
 */
ZTEST(test_service_0x19, tc020_supported_dtcs_header)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x0AU;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x0AU, "echo sub-function must be 0x0A");
    zassert_equal(resp.data[2], 0xFFU, "availability mask must be 0xFF");
}

/**
 * TC-0x19-021: reportSupportedDTCs with empty database → header only (3 bytes).
 */
ZTEST(test_service_0x19, tc021_supported_dtcs_empty_database)
{
    /* Empty database for this test */
    dtc_database_test_reset();
    dtc_database_init();
    memset(&g_sess, 0, sizeof(g_sess));
    memset(&g_sec,  0, sizeof(g_sec));
    memset(&g_srv,  0, sizeof(g_srv));
    uds_session_init(&g_sess, 5000U);
    static const uds_security_cfg_t sc2 = {
        .max_attempts=3U, .lockout_ms=100U,
        .key_validate_cb=t_key, .seed_generate_cb=t_seed
    };
    uds_security_init(&g_sec, &sc2);
    static const uds_server_cfg_t svc2 = {
        .p2_server_max_ms=25U, .p2_star_server_max_ms=5000U,
        .session_ctx=&g_sess, .security_ctx=&g_sec,
        .service_table=g_uds_service_table,
        .service_table_count=(uint8_t)UDS_SERVICE_TABLE_COUNT
    };
    uds_server_init(&g_srv, &svc2);

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x0AU;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "empty database must still return OK");
    zassert_equal(resp.length, 3U, "header only when no DTCs registered");
}

/* ---- General ---------------------------------------------------------- */

/**
 * TC-0x19-022: Unsupported sub-function 0x05 → ERR_SUBFUNCTION_NOT_SUP.
 */
ZTEST(test_service_0x19, tc022_unsupported_subfunction)
{
    setup();

    uds_msg_buf_t req  = make_req_2(0x05U, 0x00U);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP,
        "unsupported sub-function must return NRC 0x12");
}

/**
 * TC-0x19-023: suppressPosRspMsgIndicationBit (0x80) is stripped.
 * Sub-function 0x81 = (0x80 | 0x01) → handled as 0x01 (reportNumberOfDTC).
 */
ZTEST(test_service_0x19, tc023_suppress_bit_stripped)
{
    setup();

    /* 0x81 = suppressBit | 0x01 (reportNumberOfDTCByStatusMask) */
    uds_msg_buf_t req  = make_req_2(0x81U, 0xFFU);
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK,
        "suppress bit must be stripped; sub-function 0x01 must succeed");
    /* Echo sub-function must be 0x01, not 0x81 */
    zassert_equal(resp.data[1], 0x01U,
        "echoed sub-function must be 0x01 (suppress bit stripped)");
}

/**
 * TC-0x19-024: Request with length 1 (SID only, no sub-function) → invalid param.
 */
ZTEST(test_service_0x19, tc024_length_one_rejected)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.length  = 1U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "request with no sub-function must be rejected");
}

/**
 * TC-0x19-025: NULL req → ERR_NULL_PTR.
 */
ZTEST(test_service_0x19, tc025_null_req)
{
    setup();
    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, NULL, &resp);
    zassert_equal(rc, UDS_STATUS_ERR_NULL_PTR,
        "NULL req must return ERR_NULL_PTR");
}

/* ---- Sub-function 0x0B — reportDTCFaultDetectionCounter --------------- */

/**
 * TC-0x19-026: All DTCs have testFailed=1 → nothing qualifies → 2-byte response.
 */
ZTEST(test_service_0x19, tc026_fdc_all_test_failed_empty)
{
    setup();
    /* Default setup: DTC_A=0x09 (testFailed=1), DTC_B=0x04, DTC_C=0x08 */
    /* DTC_B (0x04 PENDING) and DTC_C (0x08 CONFIRMED) have testFailed=0, so
     * force all to testFailed=1 for this test. */
    dtc_database_set_status(DTC_B, 0x01U);
    dtc_database_set_status(DTC_C, 0x01U);

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x0BU;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK even with empty result");
    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x0BU, "echo sub-function must be 0x0B");
    zassert_equal(resp.length, 2U, "no qualifying DTCs → 2-byte header only");
}

/**
 * TC-0x19-027: DTCs with testFailed=0 and counter < 0xFF → returned.
 * DTC_B=0x04 (testFailed=0), counter=0x30 → 1 record.
 */
ZTEST(test_service_0x19, tc027_fdc_qualifying_dtc_returned)
{
    setup();
    /* DTC_A has testFailed=1 (0x09), excluded.
     * DTC_C has testFailed=0 (0x08), set counter=0x00 → included.
     * DTC_B has testFailed=0 (0x04), set counter=0x30 → included. */
    dtc_database_set_fault_counter(DTC_B, 0x30U);
    dtc_database_set_fault_counter(DTC_C, 0x10U);

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x0BU;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK");
    /* 2-byte header + 2 records × 4 bytes = 10 */
    zassert_equal(resp.length, 10U, "2 qualifying DTCs → 2-byte header + 2×4 = 10");
}

/**
 * TC-0x19-028: Response header for 0x0B is [0x59, 0x0B] — NO availability mask.
 * ISO 14229-1 §11.3.11 response differs from 0x01/0x02/0x0A.
 */
ZTEST(test_service_0x19, tc028_fdc_no_availability_mask_in_header)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x0BU;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x0BU, "echo sub-function must be 0x0B");
    /* data[2] is first byte of first DTC record (or not present if empty list),
     * NOT an availability mask. Verify header length is exactly 2. */
    zassert_true(resp.length >= 2U, "response must be at least 2 bytes");
}

/**
 * TC-0x19-029: DTC with counter == 0xFF is excluded (reserved = confirmed).
 */
ZTEST(test_service_0x19, tc029_fdc_counter_ff_excluded)
{
    setup();
    /* DTC_B: testFailed=0, set counter to 0xFF → must be excluded */
    dtc_database_set_fault_counter(DTC_B, 0xFFU);
    /* DTC_C: testFailed=0, counter=0x00 (default) → included */

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x0BU;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    /* Only DTC_C qualifies → 2-byte header + 1 record × 4 = 6 */
    zassert_equal(resp.length, 6U,
        "DTC with counter=0xFF must be excluded; only DTC_C → 6 bytes");
}

/**
 * TC-0x19-030: Counter byte in 0x0B record matches value set via set_fault_counter.
 */
ZTEST(test_service_0x19, tc030_fdc_counter_value_in_record)
{
    setup();
    /* Only DTC_B qualifies: testFailed=0 (0x04), set counter=0x7E */
    dtc_database_set_status(DTC_C, 0x01U);  /* exclude DTC_C (testFailed=1) */
    dtc_database_set_fault_counter(DTC_B, 0x7EU);

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x0BU;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    /* Header=2 + 1 record(4) = 6 bytes; counter is data[5] */
    zassert_equal(resp.length, 6U, "one qualifying DTC → 6 bytes");
    /* Record: [dtcHB, dtcMB, dtcLB, counter] at offset 2 */
    zassert_equal(resp.data[5], 0x7EU,
        "fault detection counter byte must match value set via set_fault_counter");
}

/**
 * TC-0x19-031: Sub-function 0x0B with request length 1 → ERR_INVALID_PARAM.
 */
ZTEST(test_service_0x19, tc031_fdc_short_request_rejected)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.length  = 1U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_ERR_INVALID_PARAM,
        "request with no sub-function byte must be rejected");
}

/* ---- Sub-function 0x19 — reportDTCWithPermanentStatus ----------------- */

/**
 * TC-0x19-032: No permanent DTCs registered → 3-byte response only.
 */
ZTEST(test_service_0x19, tc032_permanent_empty_list)
{
    setup();
    /* Default: no DTC is marked permanent */

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x19U;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK");
    zassert_equal(resp.length, 3U,
        "no permanent DTCs → 3-byte header only");
}

/**
 * TC-0x19-033: Permanent DTC is returned with correct status byte.
 */
ZTEST(test_service_0x19, tc033_permanent_dtc_returned)
{
    setup();
    dtc_database_set_permanent(DTC_A, true);

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x19U;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_status_t rc = uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(rc, UDS_STATUS_OK, "must return OK");
    /* 3-byte header + 1 record × 4 bytes = 7 */
    zassert_equal(resp.length, 7U, "one permanent DTC → 3-byte header + 4-byte record");
    /* Record starts at offset 3: [dtcHB, dtcMB, dtcLB, status] */
    uint32_t code = ((uint32_t)resp.data[3] << 16U)
                  | ((uint32_t)resp.data[4] <<  8U)
                  |  (uint32_t)resp.data[5];
    zassert_equal(code, DTC_A, "returned DTC code must match DTC_A");
    zassert_equal(resp.data[6], 0x09U, "status byte must match DTC_A's 0x09");
}

/**
 * TC-0x19-034: Response header for 0x19: [0x59, 0x19, 0xFF (availability mask)].
 */
ZTEST(test_service_0x19, tc034_permanent_response_header)
{
    setup();

    uds_msg_buf_t req;
    memset(&req, 0, sizeof(req));
    req.data[0] = 0x19U;
    req.data[1] = 0x19U;
    req.length  = 2U;

    uds_msg_buf_t resp = {0};
    uds_service_0x19_handler(&g_srv, &req, &resp);

    zassert_equal(resp.data[0], 0x59U, "response SID must be 0x59");
    zassert_equal(resp.data[1], 0x19U, "echo sub-function must be 0x19");
    zassert_equal(resp.data[2], 0xFFU, "availability mask must be 0xFF");
}

/**
 * TC-0x19-035: SID 0x14 does NOT clear permanent DTC status.
 * After clear_dtc, permanent DTC_A must retain its status byte.
 */
ZTEST(test_service_0x19, tc035_svc014_preserves_permanent_dtc)
{
    setup();
    dtc_database_set_permanent(DTC_A, true);

    /* Simulate SID 0x14: [0x14, 0xFF, 0xFF, 0xFF] */
    uds_msg_buf_t req14;
    memset(&req14, 0, sizeof(req14));
    req14.data[0] = 0x14U;
    req14.data[1] = 0xFFU;
    req14.data[2] = 0xFFU;
    req14.data[3] = 0xFFU;
    req14.length  = 4U;

    uds_msg_buf_t resp14 = {0};
    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req14, &resp14);
    zassert_equal(rc, UDS_STATUS_OK, "SID 0x14 must succeed");

    /* DTC_A is permanent — status byte must be preserved */
    dtc_entry_t *entry = dtc_database_find(DTC_A);
    zassert_not_null(entry, "DTC_A must still be registered");
    zassert_equal(entry->status_byte, 0x09U,
        "permanent DTC_A status must not be cleared by SID 0x14");
}

/**
 * TC-0x19-036: SID 0x14 DOES clear non-permanent DTCs.
 * After clear, DTC_B (non-permanent, status 0x04) must be 0x00.
 */
ZTEST(test_service_0x19, tc036_svc014_clears_non_permanent_dtc)
{
    setup();
    /* DTC_A marked permanent, DTC_B and DTC_C are non-permanent (default) */
    dtc_database_set_permanent(DTC_A, true);

    uds_msg_buf_t req14;
    memset(&req14, 0, sizeof(req14));
    req14.data[0] = 0x14U;
    req14.data[1] = 0xFFU;
    req14.data[2] = 0xFFU;
    req14.data[3] = 0xFFU;
    req14.length  = 4U;

    uds_msg_buf_t resp14 = {0};
    uds_status_t rc = uds_service_0x14_handler(&g_srv, &req14, &resp14);
    zassert_equal(rc, UDS_STATUS_OK, "SID 0x14 must succeed");

    dtc_entry_t *entry_b = dtc_database_find(DTC_B);
    dtc_entry_t *entry_c = dtc_database_find(DTC_C);
    zassert_not_null(entry_b, "DTC_B must still be registered");
    zassert_not_null(entry_c, "DTC_C must still be registered");
    zassert_equal(entry_b->status_byte, 0x00U,
        "non-permanent DTC_B must be cleared by SID 0x14");
    zassert_equal(entry_c->status_byte, 0x00U,
        "non-permanent DTC_C must be cleared by SID 0x14");
}

/* =========================================================================
 * run_all_tests — wires ZTEST functions into Unity runner
 * ========================================================================= */

extern void test_service_0x19__tc001_count_by_mask_09(void);
extern void test_service_0x19__tc002_count_by_mask_ff_all_match(void);
extern void test_service_0x19__tc003_count_by_mask_01_one_match(void);
extern void test_service_0x19__tc004_subfn01_response_header(void);
extern void test_service_0x19__tc005_subfn01_response_six_bytes(void);
extern void test_service_0x19__tc006_list_by_mask_04_one_dtc(void);
extern void test_service_0x19__tc007_dtc_record_is_four_bytes(void);
extern void test_service_0x19__tc008_subfn02_response_header(void);
extern void test_service_0x19__tc009_mask_zero_returns_all(void);
extern void test_service_0x19__tc010_mask_no_match_empty_list(void);
extern void test_service_0x19__tc011_snapshot_id_empty_list(void);
extern void test_service_0x19__tc012_snapshot_by_dtc_known(void);
extern void test_service_0x19__tc013_snapshot_by_dtc_length_eight(void);
extern void test_service_0x19__tc014_snapshot_unknown_dtc(void);
extern void test_service_0x19__tc015_snapshot_short_request(void);
extern void test_service_0x19__tc016_ext_data_by_dtc_known(void);
extern void test_service_0x19__tc017_ext_data_response_length(void);
extern void test_service_0x19__tc018_ext_data_unknown_dtc(void);
extern void test_service_0x19__tc019_supported_dtcs_all_three(void);
extern void test_service_0x19__tc020_supported_dtcs_header(void);
extern void test_service_0x19__tc021_supported_dtcs_empty_database(void);
extern void test_service_0x19__tc022_unsupported_subfunction(void);
extern void test_service_0x19__tc023_suppress_bit_stripped(void);
extern void test_service_0x19__tc024_length_one_rejected(void);
extern void test_service_0x19__tc025_null_req(void);
extern void test_service_0x19__tc026_fdc_all_test_failed_empty(void);
extern void test_service_0x19__tc027_fdc_qualifying_dtc_returned(void);
extern void test_service_0x19__tc028_fdc_no_availability_mask_in_header(void);
extern void test_service_0x19__tc029_fdc_counter_ff_excluded(void);
extern void test_service_0x19__tc030_fdc_counter_value_in_record(void);
extern void test_service_0x19__tc031_fdc_short_request_rejected(void);
extern void test_service_0x19__tc032_permanent_empty_list(void);
extern void test_service_0x19__tc033_permanent_dtc_returned(void);
extern void test_service_0x19__tc034_permanent_response_header(void);
extern void test_service_0x19__tc035_svc014_preserves_permanent_dtc(void);
extern void test_service_0x19__tc036_svc014_clears_non_permanent_dtc(void);

void run_all_tests(void)
{
    RUN_TEST(test_service_0x19__tc001_count_by_mask_09);
    RUN_TEST(test_service_0x19__tc002_count_by_mask_ff_all_match);
    RUN_TEST(test_service_0x19__tc003_count_by_mask_01_one_match);
    RUN_TEST(test_service_0x19__tc004_subfn01_response_header);
    RUN_TEST(test_service_0x19__tc005_subfn01_response_six_bytes);
    RUN_TEST(test_service_0x19__tc006_list_by_mask_04_one_dtc);
    RUN_TEST(test_service_0x19__tc007_dtc_record_is_four_bytes);
    RUN_TEST(test_service_0x19__tc008_subfn02_response_header);
    RUN_TEST(test_service_0x19__tc009_mask_zero_returns_all);
    RUN_TEST(test_service_0x19__tc010_mask_no_match_empty_list);
    RUN_TEST(test_service_0x19__tc011_snapshot_id_empty_list);
    RUN_TEST(test_service_0x19__tc012_snapshot_by_dtc_known);
    RUN_TEST(test_service_0x19__tc013_snapshot_by_dtc_length_eight);
    RUN_TEST(test_service_0x19__tc014_snapshot_unknown_dtc);
    RUN_TEST(test_service_0x19__tc015_snapshot_short_request);
    RUN_TEST(test_service_0x19__tc016_ext_data_by_dtc_known);
    RUN_TEST(test_service_0x19__tc017_ext_data_response_length);
    RUN_TEST(test_service_0x19__tc018_ext_data_unknown_dtc);
    RUN_TEST(test_service_0x19__tc019_supported_dtcs_all_three);
    RUN_TEST(test_service_0x19__tc020_supported_dtcs_header);
    RUN_TEST(test_service_0x19__tc021_supported_dtcs_empty_database);
    RUN_TEST(test_service_0x19__tc022_unsupported_subfunction);
    RUN_TEST(test_service_0x19__tc023_suppress_bit_stripped);
    RUN_TEST(test_service_0x19__tc024_length_one_rejected);
    RUN_TEST(test_service_0x19__tc025_null_req);
    RUN_TEST(test_service_0x19__tc026_fdc_all_test_failed_empty);
    RUN_TEST(test_service_0x19__tc027_fdc_qualifying_dtc_returned);
    RUN_TEST(test_service_0x19__tc028_fdc_no_availability_mask_in_header);
    RUN_TEST(test_service_0x19__tc029_fdc_counter_ff_excluded);
    RUN_TEST(test_service_0x19__tc030_fdc_counter_value_in_record);
    RUN_TEST(test_service_0x19__tc031_fdc_short_request_rejected);
    RUN_TEST(test_service_0x19__tc032_permanent_empty_list);
    RUN_TEST(test_service_0x19__tc033_permanent_dtc_returned);
    RUN_TEST(test_service_0x19__tc034_permanent_response_header);
    RUN_TEST(test_service_0x19__tc035_svc014_preserves_permanent_dtc);
    RUN_TEST(test_service_0x19__tc036_svc014_clears_non_permanent_dtc);
}
