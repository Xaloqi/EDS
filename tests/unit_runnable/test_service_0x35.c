// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_service_0x35.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x35.c
 *                    SID 0x35 — RequestUpload
 *
 * Coverage:
 *   TC-0x35-001  NULL ctx                              → ERR_NULL_PTR
 *   TC-0x35-002  Request too short (< 3 bytes)         → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x35-003  read_cb not registered (NULL)         → ERR_CONDITIONS_NOT_MET (NRC 0x22)
 *   TC-0x35-004  dataFormatIdentifier != 0x00          → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31)
 *   TC-0x35-005  addressLength == 0                    → ERR_INVALID_PARAM
 *   TC-0x35-006  sizeLength == 0                       → ERR_INVALID_PARAM
 *   TC-0x35-007  addressLength > 4                     → ERR_INVALID_PARAM
 *   TC-0x35-008  sizeLength > 4                        → ERR_INVALID_PARAM
 *   TC-0x35-009  Request too short for declared fields  → ERR_INVALID_PARAM
 *   TC-0x35-010  Address outside registered region     → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x35-011  Size == 0                             → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x35-012  Valid 4+4 request                     → UDS_STATUS_OK, response [0x75, 0x20, Hi, Lo]
 *   TC-0x35-013  Transfer direction set to UPLOAD      → tctx->direction == UDS_TRANSFER_DIR_UPLOAD
 *   TC-0x35-014  Transfer state set to ACTIVE
 *   TC-0x35-015  erase_cb NOT called (upload → read only)
 *   TC-0x35-016  New 0x35 while upload active resets context then opens fresh transfer
 *
 * FRAMEWORK: Zephyr Ztest (via ztest_shim.h for host compilation)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_security.h"
#include "uds_safety.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"

/* ==========================================================================
 * Mock flash constants
 * ========================================================================== */

#define MOCK_FLASH_BASE   (0x08020000UL)
#define MOCK_FLASH_SIZE   (0x2000UL)
#define MOCK_BLOCK_LEN    (256U)

/* ==========================================================================
 * Mock flash state
 * ========================================================================== */

static bool     s_erase_called = false;
static uint32_t s_read_call_count = 0U;

/* ==========================================================================
 * Mock flash callbacks
 * ========================================================================== */

static uds_status_t mock_erase(uint32_t address, uint32_t size_bytes)
{
    (void)address; (void)size_bytes;
    s_erase_called = true;
    return UDS_STATUS_OK;
}

static uds_status_t mock_write(uint32_t address,
                                const uint8_t *data,
                                uint32_t length)
{
    (void)address; (void)data; (void)length;
    return UDS_STATUS_OK;
}

static uds_status_t mock_verify(uint32_t address,
                                 uint32_t size_bytes,
                                 uint32_t expected_crc)
{
    (void)address; (void)size_bytes; (void)expected_crc;
    return UDS_STATUS_OK;
}

static uds_status_t mock_read(uint32_t address,
                               uint8_t *data,
                               uint32_t length)
{
    uint32_t i;
    (void)address;
    s_read_call_count++;
    for (i = (uint32_t)0U; i < length; i++) {
        data[i] = (uint8_t)(i & (uint32_t)0xFFU);
    }
    return UDS_STATUS_OK;
}

static const uds_flash_region_t k_mock_region[1U] = {
    {
        .base_address = MOCK_FLASH_BASE,
        .size_bytes   = (uint32_t)MOCK_FLASH_SIZE,
        .writable     = true,
    }
};

/* ops table with read_cb populated */
static const uds_flash_ops_t k_mock_ops_with_read = {
    .erase_cb         = mock_erase,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
    .read_cb          = mock_read,
    .memory_map       = k_mock_region,
    .region_count     = (uint8_t)1U,
    .max_block_length = (uint16_t)MOCK_BLOCK_LEN,
};

/* ops table without read_cb (download-only) */
static const uds_flash_ops_t k_mock_ops_no_read = {
    .erase_cb         = mock_erase,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
    .read_cb          = NULL,
    .memory_map       = k_mock_region,
    .region_count     = (uint8_t)1U,
    .max_block_length = (uint16_t)MOCK_BLOCK_LEN,
};

/* ==========================================================================
 * Test state
 * ========================================================================== */

static uds_session_ctx_t  s_sess;
static uds_security_ctx_t s_sec;
static uds_server_ctx_t   s_srv;
static uds_msg_buf_t      s_req;
static uds_msg_buf_t      s_resp;

/* ==========================================================================
 * setUp / tearDown
 * ========================================================================== */

void setUp(void)
{
    memset(&s_sess, 0, sizeof(s_sess));
    memset(&s_sec,  0, sizeof(s_sec));
    memset(&s_srv,  0, sizeof(s_srv));
    memset(&s_req,  0, sizeof(s_req));
    memset(&s_resp, 0, sizeof(s_resp));

    s_erase_called    = false;
    s_read_call_count = 0U;

    (void)uds_safety_init();

    s_sess.initialized    = true;
    s_sess.active_session = UDS_SESSION_PROGRAMMING;
    s_sec.initialized     = true;
    s_sec.active_level    = 1U;

    s_srv.cfg.session_ctx  = &s_sess;
    s_srv.cfg.security_ctx = &s_sec;

    uds_transfer_ctx_reset(uds_transfer_ctx_get());
    (void)uds_flash_ops_register(NULL);
}

void tearDown(void) {}

/* ==========================================================================
 * Helper: build a well-formed 0x35 request
 * ========================================================================== */

static void build_valid_req(uint32_t mem_address, uint32_t mem_size)
{
    s_req.data[0] = 0x35U;
    s_req.data[1] = 0x00U;  /* dataFormatIdentifier */
    s_req.data[2] = 0x44U;  /* ALFID: 4 addr bytes, 4 size bytes */

    s_req.data[3] = (uint8_t)((mem_address >> 24U) & 0xFFU);
    s_req.data[4] = (uint8_t)((mem_address >> 16U) & 0xFFU);
    s_req.data[5] = (uint8_t)((mem_address >>  8U) & 0xFFU);
    s_req.data[6] = (uint8_t)( mem_address         & 0xFFU);

    s_req.data[7]  = (uint8_t)((mem_size >> 24U) & 0xFFU);
    s_req.data[8]  = (uint8_t)((mem_size >> 16U) & 0xFFU);
    s_req.data[9]  = (uint8_t)((mem_size >>  8U) & 0xFFU);
    s_req.data[10] = (uint8_t)( mem_size         & 0xFFU);

    s_req.length = 11U;
}

/* ==========================================================================
 * Test suite
 * ========================================================================== */

ZTEST_SUITE(svc_0x35, NULL, NULL, NULL, NULL, NULL);

/* TC-0x35-001 */
ZTEST(svc_0x35, test_null_ctx)
{
    build_valid_req(MOCK_FLASH_BASE, 0x100U);
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    zassert_equal(UDS_STATUS_ERR_NULL_PTR,
                  uds_service_0x35_handler(NULL, &s_req, &s_resp), "");
}

/* TC-0x35-002 */
ZTEST(svc_0x35, test_request_too_short)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    s_req.data[0] = 0x35U;
    s_req.data[1] = 0x00U;
    s_req.length  = 2U;   /* < 3 */
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-003  read_cb not registered → NRC 0x22 */
ZTEST(svc_0x35, test_read_cb_null)
{
    (void)uds_flash_ops_register(&k_mock_ops_no_read);
    build_valid_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-004  dataFormatIdentifier != 0x00 → NRC 0x31 */
ZTEST(svc_0x35, test_nonzero_data_format_identifier)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    build_valid_req(MOCK_FLASH_BASE, 0x100U);
    s_req.data[1] = 0x10U;
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-005  addressLength == 0 → NRC 0x13 */
ZTEST(svc_0x35, test_address_length_zero)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    s_req.data[0] = 0x35U;
    s_req.data[1] = 0x00U;
    s_req.data[2] = 0x10U;  /* sizeLen=1, addrLen=0 */
    s_req.data[3] = 0x01U;
    s_req.length  = 4U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-006  sizeLength == 0 → NRC 0x13 */
ZTEST(svc_0x35, test_size_length_zero)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    s_req.data[0] = 0x35U;
    s_req.data[1] = 0x00U;
    s_req.data[2] = 0x01U;  /* sizeLen=0, addrLen=1 */
    s_req.data[3] = 0x00U;
    s_req.length  = 4U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-007  addressLength > 4 → NRC 0x13 */
ZTEST(svc_0x35, test_address_length_too_large)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    s_req.data[0] = 0x35U;
    s_req.data[1] = 0x00U;
    s_req.data[2] = 0x15U;  /* sizeLen=1, addrLen=5 */
    s_req.length  = 3U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-008  sizeLength > 4 → NRC 0x13 */
ZTEST(svc_0x35, test_size_length_too_large)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    s_req.data[0] = 0x35U;
    s_req.data[1] = 0x00U;
    s_req.data[2] = 0x51U;  /* sizeLen=5, addrLen=1 */
    s_req.length  = 3U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-009  Request too short for declared fields → NRC 0x13 */
ZTEST(svc_0x35, test_request_too_short_for_fields)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    s_req.data[0] = 0x35U;
    s_req.data[1] = 0x00U;
    s_req.data[2] = 0x44U;  /* claims 4 addr + 4 size bytes */
    s_req.data[3] = 0x08U;
    s_req.data[4] = 0x02U;
    s_req.length  = 5U;     /* 5 < 3 + 4 + 4 = 11 */
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-010  Address outside registered region → NRC 0x31 */
ZTEST(svc_0x35, test_address_outside_region)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    build_valid_req(0x20000000UL, 0x100U);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-011  Size == 0 → NRC 0x31 */
ZTEST(svc_0x35, test_size_zero_rejected)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    build_valid_req(MOCK_FLASH_BASE, 0x0U);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x35-012  Valid 4+4 request → UDS_STATUS_OK, response [0x75, 0x20, Hi, Lo] */
ZTEST(svc_0x35, test_valid_request_4byte_fields)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    build_valid_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");

    zassert_equal(4U,    s_resp.length,  "response must be 4 bytes");
    zassert_equal(0x75U, s_resp.data[0], "RSID must be 0x75");
    zassert_equal(0x20U, s_resp.data[1], "LFI must be 0x20");
}

/* TC-0x35-013  Transfer direction set to UPLOAD */
ZTEST(svc_0x35, test_transfer_direction_upload)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    build_valid_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_DIR_UPLOAD,
                  uds_transfer_ctx_get()->direction,
                  "direction must be UPLOAD after 0x35");
}

/* TC-0x35-014  Transfer state set to ACTIVE */
ZTEST(svc_0x35, test_transfer_state_active)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    build_valid_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_ACTIVE,
                  uds_transfer_ctx_get()->state,
                  "transfer must be ACTIVE after 0x35");
}

/* TC-0x35-015  erase_cb NOT called (upload is read-only) */
ZTEST(svc_0x35, test_erase_not_called)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);
    build_valid_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
    zassert_false(s_erase_called, "erase_cb must NOT be called for upload");
}

/* TC-0x35-016  New 0x35 while upload active resets context then opens fresh transfer */
ZTEST(svc_0x35, test_new_upload_aborts_active_transfer)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);

    /* First 0x35 — start an upload. */
    build_valid_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_ACTIVE, uds_transfer_ctx_get()->state, "");

    /* Second 0x35 — must reset and open a fresh upload. */
    memset(&s_resp, 0, sizeof(s_resp));
    build_valid_req(MOCK_FLASH_BASE, 0x200U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_ACTIVE, uds_transfer_ctx_get()->state, "");
    zassert_equal((uint32_t)0x200U,
                  uds_transfer_ctx_get()->bytes_remaining,
                  "bytes_remaining must reflect new upload size");
    zassert_equal(UDS_TRANSFER_DIR_UPLOAD,
                  uds_transfer_ctx_get()->direction,
                  "direction must remain UPLOAD after restart");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(svc_0x35__test_null_ctx);
    RUN_TEST(svc_0x35__test_request_too_short);
    RUN_TEST(svc_0x35__test_read_cb_null);
    RUN_TEST(svc_0x35__test_nonzero_data_format_identifier);
    RUN_TEST(svc_0x35__test_address_length_zero);
    RUN_TEST(svc_0x35__test_size_length_zero);
    RUN_TEST(svc_0x35__test_address_length_too_large);
    RUN_TEST(svc_0x35__test_size_length_too_large);
    RUN_TEST(svc_0x35__test_request_too_short_for_fields);
    RUN_TEST(svc_0x35__test_address_outside_region);
    RUN_TEST(svc_0x35__test_size_zero_rejected);
    RUN_TEST(svc_0x35__test_valid_request_4byte_fields);
    RUN_TEST(svc_0x35__test_transfer_direction_upload);
    RUN_TEST(svc_0x35__test_transfer_state_active);
    RUN_TEST(svc_0x35__test_erase_not_called);
    RUN_TEST(svc_0x35__test_new_upload_aborts_active_transfer);
}
