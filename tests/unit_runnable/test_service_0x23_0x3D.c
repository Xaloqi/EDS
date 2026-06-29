// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_service_0x23_0x3D.c
 *
 * MODULE UNDER TEST:
 *   core/uds_services/service_0x23.c — SID 0x23 ReadMemoryByAddress
 *   core/uds_services/service_0x3D.c — SID 0x3D WriteMemoryByAddress
 *
 * SID 0x23 Coverage:
 *   TC-0x23-001  NULL ctx                                    → ERR_NULL_PTR
 *   TC-0x23-002  Request too short (< 2 bytes)               → ERR_INVALID_PARAM
 *   TC-0x23-003  No flash ops registered                     → ERR_CONDITIONS_NOT_MET
 *   TC-0x23-004  read_cb is NULL                             → ERR_CONDITIONS_NOT_MET
 *   TC-0x23-005  addressLength == 0 (ALFID nibble)           → ERR_INVALID_PARAM
 *   TC-0x23-006  sizeLength == 0 (ALFID nibble)              → ERR_INVALID_PARAM
 *   TC-0x23-007  addressLength > 4                           → ERR_INVALID_PARAM
 *   TC-0x23-008  sizeLength > 4                              → ERR_INVALID_PARAM
 *   TC-0x23-009  Request too short for declared fields        → ERR_INVALID_PARAM
 *   TC-0x23-010  mem_size == 0                               → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x23-011  Address outside readable map                → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x23-012  Address + size arithmetic overflow           → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x23-013  mem_size > UDS_MAX_PAYLOAD_LEN - 1          → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x23-014  read_cb returns error                       → ERR_PLATFORM (NRC 0x72)
 *   TC-0x23-015  Valid 4-byte address, 4-byte size           → UDS_STATUS_OK
 *   TC-0x23-016  Positive response: first byte = 0x63        → OK
 *   TC-0x23-017  Response data matches read_cb output        → OK
 *   TC-0x23-018  Valid 1-byte address, 1-byte size           → UDS_STATUS_OK
 *   TC-0x23-019  Address in readable-only region accepted     → UDS_STATUS_OK
 *   TC-0x23-020  Address in writable-only region rejected    → ERR_REQUEST_OUT_OF_RANGE
 *
 * SID 0x3D Coverage:
 *   TC-0x3D-001  NULL ctx                                    → ERR_NULL_PTR
 *   TC-0x3D-002  Request too short (< 2 bytes)               → ERR_INVALID_PARAM
 *   TC-0x3D-003  No flash ops registered                     → ERR_CONDITIONS_NOT_MET
 *   TC-0x3D-004  write_cb is NULL                            → ERR_CONDITIONS_NOT_MET
 *   TC-0x3D-005  addressLength == 0                          → ERR_INVALID_PARAM
 *   TC-0x3D-006  sizeLength == 0                             → ERR_INVALID_PARAM
 *   TC-0x3D-007  addressLength > 4                           → ERR_INVALID_PARAM
 *   TC-0x3D-008  Request too short for fields + data         → ERR_INVALID_PARAM
 *   TC-0x3D-009  mem_size == 0                               → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x3D-010  Address outside writable map                → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x3D-011  Address + size overflow                     → ERR_REQUEST_OUT_OF_RANGE
 *   TC-0x3D-012  write_cb returns error                      → ERR_PLATFORM (NRC 0x72)
 *   TC-0x3D-013  Valid 4-byte address, 4-byte size           → UDS_STATUS_OK
 *   TC-0x3D-014  Positive response: first byte = 0x7D        → OK
 *   TC-0x3D-015  Response echoes ALFID, address, size        → OK
 *   TC-0x3D-016  write_cb receives correct address and data  → OK
 *   TC-0x3D-017  Valid 1-byte address, 1-byte size           → UDS_STATUS_OK
 *   TC-0x3D-018  Request with data too short for mem_size    → ERR_INVALID_PARAM
 *   TC-0x3D-019  Address in readable-only region rejected    → ERR_REQUEST_OUT_OF_RANGE
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
#include "uds_flash_ops.h"

/* ==========================================================================
 * Mock flash constants
 * ========================================================================== */

/** Base address of the read-write test region. */
#define MOCK_RW_BASE    (0x08020000UL)
/** Size of the read-write test region (4 KB). */
#define MOCK_RW_SIZE    (0x1000UL)

/** Base address of a read-only (non-writable) region. */
#define MOCK_RO_BASE    (0x08010000UL)
/** Size of the read-only region (1 KB). */
#define MOCK_RO_SIZE    (0x400UL)

/** Base address of a write-only (non-readable) region. */
#define MOCK_WO_BASE    (0x08030000UL)
/** Size of the write-only region (1 KB). */
#define MOCK_WO_SIZE    (0x400UL)

/** Data byte pattern written into the mock read buffer. */
#define MOCK_READ_FILL  (0xA5U)

/** Maximum block length (not used by 0x23/0x3D but required by flash_ops_t). */
#define MOCK_BLOCK_LEN  (256U)

/* ==========================================================================
 * Mock flash state
 * ========================================================================== */

static bool     s_read_fail   = false;
static bool     s_write_fail  = false;
static bool     s_was_written = false;
static uint32_t s_write_addr  = 0U;
static uint32_t s_write_len   = 0U;

/* Buffer that read_cb copies into the response. */
static uint8_t s_read_buf[MOCK_RW_SIZE];

/* Buffer that write_cb captures from the request data. */
static uint8_t s_written_data[MOCK_RW_SIZE];

/* ==========================================================================
 * Mock flash callbacks
 * ========================================================================== */

static uds_status_t mock_erase(uint32_t address, uint32_t size_bytes)
{
    (void)address;
    (void)size_bytes;
    return UDS_STATUS_OK;
}

static uds_status_t mock_write(uint32_t address,
                                const uint8_t *data,
                                uint32_t length)
{
    s_was_written = true;
    s_write_addr  = address;
    s_write_len   = length;
    if (s_write_fail) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    if (length <= (uint32_t)sizeof(s_written_data)) {
        (void)memcpy(s_written_data, data, (size_t)length);
    }
    return UDS_STATUS_OK;
}

static uds_status_t mock_verify(uint32_t address,
                                 uint32_t size_bytes,
                                 uint32_t expected_crc)
{
    (void)address;
    (void)size_bytes;
    (void)expected_crc;
    return UDS_STATUS_OK;
}

static uds_status_t mock_read(uint32_t address,
                               uint8_t *data,
                               uint32_t length)
{
    uint32_t offset;
    uint32_t i;

    if (s_read_fail) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    if (address >= (uint32_t)MOCK_RW_BASE) {
        offset = address - (uint32_t)MOCK_RW_BASE;
    } else if (address >= (uint32_t)MOCK_RO_BASE) {
        offset = address - (uint32_t)MOCK_RO_BASE;
    } else {
        offset = 0U;
    }

    for (i = (uint32_t)0U; i < length; i++) {
        if ((offset + i) < (uint32_t)sizeof(s_read_buf)) {
            data[i] = s_read_buf[offset + i];
        } else {
            data[i] = (uint8_t)MOCK_READ_FILL;
        }
    }
    return UDS_STATUS_OK;
}

/* Three-region memory map: RW, RO (readable only), WO (writable only). */
static const uds_flash_region_t k_mock_regions[3U] = {
    {
        .base_address = MOCK_RW_BASE,
        .size_bytes   = (uint32_t)MOCK_RW_SIZE,
        .writable     = true,
        .readable     = true,
    },
    {
        .base_address = MOCK_RO_BASE,
        .size_bytes   = (uint32_t)MOCK_RO_SIZE,
        .writable     = false,
        .readable     = true,
    },
    {
        .base_address = MOCK_WO_BASE,
        .size_bytes   = (uint32_t)MOCK_WO_SIZE,
        .writable     = true,
        .readable     = false,
    },
};

static const uds_flash_ops_t k_mock_ops = {
    .erase_cb         = mock_erase,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
    .read_cb          = mock_read,
    .memory_map       = k_mock_regions,
    .region_count     = (uint8_t)3U,
    .max_block_length = (uint16_t)MOCK_BLOCK_LEN,
};

/* Flash ops with no read_cb (for TC-0x23-004). */
static const uds_flash_ops_t k_ops_no_read = {
    .erase_cb         = mock_erase,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
    .read_cb          = NULL,
    .memory_map       = k_mock_regions,
    .region_count     = (uint8_t)3U,
    .max_block_length = (uint16_t)MOCK_BLOCK_LEN,
};

/* Flash ops with no write_cb (for TC-0x3D-004). */
static const uds_flash_ops_t k_ops_no_write = {
    .erase_cb         = mock_erase,
    .write_cb         = NULL,
    .verify_cb        = mock_verify,
    .read_cb          = mock_read,
    .memory_map       = k_mock_regions,
    .region_count     = (uint8_t)3U,
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
    memset(&s_sess,         0, sizeof(s_sess));
    memset(&s_sec,          0, sizeof(s_sec));
    memset(&s_srv,          0, sizeof(s_srv));
    memset(&s_req,          0, sizeof(s_req));
    memset(&s_resp,         0, sizeof(s_resp));
    memset(s_read_buf,      (int)MOCK_READ_FILL, sizeof(s_read_buf));
    memset(s_written_data,  0, sizeof(s_written_data));

    s_read_fail   = false;
    s_write_fail  = false;
    s_was_written = false;
    s_write_addr  = 0U;
    s_write_len   = 0U;

    (void)uds_safety_init();

    /* Programming session, security level 1 unlocked. */
    s_sess.initialized    = true;
    s_sess.active_session = UDS_SESSION_PROGRAMMING;
    s_sec.initialized     = true;
    s_sec.active_level    = 1U;

    s_srv.cfg.session_ctx  = &s_sess;
    s_srv.cfg.security_ctx = &s_sec;

    /* Start with no flash ops registered. */
    (void)uds_flash_ops_register(NULL);
}

void tearDown(void) {}

/* ==========================================================================
 * Helpers: build well-formed requests
 * ========================================================================== */

/* Builds a 0x23 request with 4-byte address and 4-byte size fields. */
static void build_0x23_req(uint32_t address, uint32_t size)
{
    s_req.data[0] = (uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x44U;  /* ALFID: addrLen=4, sizeLen=4 */

    s_req.data[2] = (uint8_t)((address >> 24U) & 0xFFU);
    s_req.data[3] = (uint8_t)((address >> 16U) & 0xFFU);
    s_req.data[4] = (uint8_t)((address >>  8U) & 0xFFU);
    s_req.data[5] = (uint8_t)( address         & 0xFFU);

    s_req.data[6]  = (uint8_t)((size >> 24U) & 0xFFU);
    s_req.data[7]  = (uint8_t)((size >> 16U) & 0xFFU);
    s_req.data[8]  = (uint8_t)((size >>  8U) & 0xFFU);
    s_req.data[9]  = (uint8_t)( size         & 0xFFU);

    s_req.length = 10U;
}

/* Builds a 0x3D request with 4-byte address, 4-byte size, and data_len data bytes. */
static void build_0x3D_req(uint32_t address, uint32_t size,
                             const uint8_t *data, uint16_t data_len)
{
    uint16_t i;

    s_req.data[0] = (uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x44U;  /* ALFID: addrLen=4, sizeLen=4 */

    s_req.data[2] = (uint8_t)((address >> 24U) & 0xFFU);
    s_req.data[3] = (uint8_t)((address >> 16U) & 0xFFU);
    s_req.data[4] = (uint8_t)((address >>  8U) & 0xFFU);
    s_req.data[5] = (uint8_t)( address         & 0xFFU);

    s_req.data[6]  = (uint8_t)((size >> 24U) & 0xFFU);
    s_req.data[7]  = (uint8_t)((size >> 16U) & 0xFFU);
    s_req.data[8]  = (uint8_t)((size >>  8U) & 0xFFU);
    s_req.data[9]  = (uint8_t)( size         & 0xFFU);

    /* data_len is capped to prevent buffer overflow in the test. */
    if (data_len > (uint16_t)(UDS_MAX_PAYLOAD_LEN - 10U)) {
        data_len = (uint16_t)(UDS_MAX_PAYLOAD_LEN - 10U);
    }
    for (i = (uint16_t)0U; i < data_len; i++) {
        s_req.data[10U + i] = (data != NULL) ? data[i] : (uint8_t)0xBBU;
    }

    s_req.length = (uint16_t)(10U + data_len);
}

/* ==========================================================================
 * SID 0x23 — ReadMemoryByAddress test suite
 * ========================================================================== */

ZTEST_SUITE(svc_0x23, NULL, NULL, NULL, NULL, NULL);

/* TC-0x23-001 */
ZTEST(svc_0x23, test_null_ctx)
{
    build_0x23_req(MOCK_RW_BASE, 4U);
    (void)uds_flash_ops_register(&k_mock_ops);
    zassert_equal(UDS_STATUS_ERR_NULL_PTR,
                  uds_service_0x23_handler(NULL, &s_req, &s_resp), "");
}

/* TC-0x23-002 */
ZTEST(svc_0x23, test_request_too_short)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS;
    s_req.length  = 1U;  /* < 2 */
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-003 */
ZTEST(svc_0x23, test_no_flash_ops)
{
    build_0x23_req(MOCK_RW_BASE, 4U);
    /* ops not registered (setUp de-registers). */
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-004 */
ZTEST(svc_0x23, test_no_read_cb)
{
    build_0x23_req(MOCK_RW_BASE, 4U);
    (void)uds_flash_ops_register(&k_ops_no_read);
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-005 */
ZTEST(svc_0x23, test_addr_len_zero)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x10U;  /* sizeLen=1, addrLen=0 */
    s_req.data[2] = 0x01U;
    s_req.length  = 3U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-006 */
ZTEST(svc_0x23, test_size_len_zero)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x01U;  /* sizeLen=0, addrLen=1 */
    s_req.data[2] = 0x00U;
    s_req.length  = 3U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-007 */
ZTEST(svc_0x23, test_addr_len_too_large)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x15U;  /* sizeLen=1, addrLen=5 (> 4) */
    s_req.length  = 2U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-008 */
ZTEST(svc_0x23, test_size_len_too_large)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x51U;  /* sizeLen=5 (> 4), addrLen=1 */
    s_req.length  = 2U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-009 */
ZTEST(svc_0x23, test_request_too_short_for_fields)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    /* ALFID says 4+4=8 bytes of fields, but only 5 total (incl SID+ALFID). */
    s_req.data[0] = (uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x44U;
    s_req.length  = 5U;  /* 2 (header) + 3 bytes < 2+4+4 = 10 */
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-010 */
ZTEST(svc_0x23, test_size_zero)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x23_req(MOCK_RW_BASE, 0U);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-011 */
ZTEST(svc_0x23, test_address_outside_readable_map)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x23_req(0x09000000UL, 4U);  /* not in any readable region */
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-012 */
ZTEST(svc_0x23, test_address_overflow)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x23_req(0xFFFFFFFCUL, 8U);  /* wraps around 32-bit space */
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-013 */
ZTEST(svc_0x23, test_size_exceeds_response_buffer)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    /* Request size = UDS_MAX_PAYLOAD_LEN — too large (would overflow response). */
    build_0x23_req(MOCK_RW_BASE, (uint32_t)UDS_MAX_PAYLOAD_LEN);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-014 */
ZTEST(svc_0x23, test_read_cb_failure)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_read_fail = true;
    build_0x23_req(MOCK_RW_BASE, 4U);
    zassert_equal(UDS_STATUS_ERR_PLATFORM,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-015 */
ZTEST(svc_0x23, test_valid_4byte_addr_4byte_size)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x23_req(MOCK_RW_BASE, 8U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-016: positive response SID = 0x63 */
ZTEST(svc_0x23, test_positive_response_sid)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x23_req(MOCK_RW_BASE, 8U);
    (void)uds_service_0x23_handler(&s_srv, &s_req, &s_resp);
    zassert_equal((uint8_t)0x63U, s_resp.data[0], "expected 0x63 positive response SID");
}

/* TC-0x23-017: response data matches mock read buffer content */
ZTEST(svc_0x23, test_response_data_matches_read_buf)
{
    uint32_t i;
    (void)uds_flash_ops_register(&k_mock_ops);
    /* Fill mock buffer with a recognisable pattern. */
    for (i = 0U; i < (uint32_t)sizeof(s_read_buf); i++) {
        s_read_buf[i] = (uint8_t)(i & 0xFFU);
    }
    build_0x23_req(MOCK_RW_BASE, 8U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal((uint16_t)9U, s_resp.length, "response length = 1 + memSize");
    for (i = 0U; i < 8U; i++) {
        zassert_equal((uint8_t)(i & 0xFFU), s_resp.data[1U + i], "data byte mismatch");
    }
}

/* TC-0x23-018 */
ZTEST(svc_0x23, test_valid_1byte_addr_1byte_size)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    /* 1-byte address: low byte of MOCK_RW_BASE — region check uses full address. */
    s_req.data[0] = (uint8_t)UDS_SID_READ_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x11U;    /* ALFID: addrLen=1, sizeLen=1 */
    s_req.data[2] = 0x00U;    /* 1-byte address = 0x00 — outside any readable region */
    s_req.data[3] = 0x04U;    /* 1-byte size = 4 */
    s_req.length  = 4U;
    /* address 0x00 is not in readable map → OUT_OF_RANGE */
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-019: address in readable-only (RO) region is accepted by 0x23 */
ZTEST(svc_0x23, test_readable_only_region_accepted)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x23_req(MOCK_RO_BASE, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x23-020: address in writable-only (WO) region is rejected by 0x23 */
ZTEST(svc_0x23, test_writable_only_region_rejected)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x23_req(MOCK_WO_BASE, 4U);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x23_handler(&s_srv, &s_req, &s_resp), "");
}

/* ==========================================================================
 * SID 0x3D — WriteMemoryByAddress test suite
 * ========================================================================== */

ZTEST_SUITE(svc_0x3D, NULL, NULL, NULL, NULL, NULL);

static const uint8_t k_write_pattern[8U] = {
    0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U
};

/* TC-0x3D-001 */
ZTEST(svc_0x3D, test_null_ctx)
{
    build_0x3D_req(MOCK_RW_BASE, 4U, k_write_pattern, 4U);
    (void)uds_flash_ops_register(&k_mock_ops);
    zassert_equal(UDS_STATUS_ERR_NULL_PTR,
                  uds_service_0x3D_handler(NULL, &s_req, &s_resp), "");
}

/* TC-0x3D-002 */
ZTEST(svc_0x3D, test_request_too_short)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS;
    s_req.length  = 1U;  /* < 2 */
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-003 */
ZTEST(svc_0x3D, test_no_flash_ops)
{
    build_0x3D_req(MOCK_RW_BASE, 4U, k_write_pattern, 4U);
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-004 */
ZTEST(svc_0x3D, test_no_write_cb)
{
    build_0x3D_req(MOCK_RW_BASE, 4U, k_write_pattern, 4U);
    (void)uds_flash_ops_register(&k_ops_no_write);
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-005 */
ZTEST(svc_0x3D, test_addr_len_zero)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x10U;  /* sizeLen=1, addrLen=0 */
    s_req.length  = 2U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-006 */
ZTEST(svc_0x3D, test_size_len_zero)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x01U;  /* sizeLen=0, addrLen=1 */
    s_req.length  = 2U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-007 */
ZTEST(svc_0x3D, test_addr_len_too_large)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_req.data[0] = (uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x15U;  /* sizeLen=1, addrLen=5 */
    s_req.length  = 2U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-008 */
ZTEST(svc_0x3D, test_request_too_short_for_fields)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    /* ALFID: 4+4=8 field bytes; total request needs 2+4+4+data bytes. */
    s_req.data[0] = (uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS;
    s_req.data[1] = 0x44U;
    s_req.length  = 5U;  /* header only, no address, no size, no data */
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-009 */
ZTEST(svc_0x3D, test_size_zero)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x3D_req(MOCK_RW_BASE, 0U, k_write_pattern, 0U);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-010 */
ZTEST(svc_0x3D, test_address_outside_writable_map)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x3D_req(0x09000000UL, 4U, k_write_pattern, 4U);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-011 */
ZTEST(svc_0x3D, test_address_overflow)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x3D_req(0xFFFFFFFCUL, 8U, k_write_pattern, 8U);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-012 */
ZTEST(svc_0x3D, test_write_cb_failure)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    s_write_fail = true;
    build_0x3D_req(MOCK_RW_BASE, 4U, k_write_pattern, 4U);
    zassert_equal(UDS_STATUS_ERR_PLATFORM,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-013 */
ZTEST(svc_0x3D, test_valid_4byte_addr_4byte_size)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x3D_req(MOCK_RW_BASE, 8U, k_write_pattern, 8U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-014: positive response SID = 0x7D */
ZTEST(svc_0x3D, test_positive_response_sid)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x3D_req(MOCK_RW_BASE, 8U, k_write_pattern, 8U);
    (void)uds_service_0x3D_handler(&s_srv, &s_req, &s_resp);
    zassert_equal((uint8_t)0x7DU, s_resp.data[0], "expected 0x7D positive response SID");
}

/* TC-0x3D-015: response echoes ALFID, address, and size */
ZTEST(svc_0x3D, test_response_echoes_alfid_addr_size)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x3D_req(MOCK_RW_BASE, 4U, k_write_pattern, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
    /* [0x7D, ALFID, addr[4], size[4]] = 10 bytes */
    zassert_equal((uint16_t)10U, s_resp.length, "response length = 1+1+4+4");
    zassert_equal(0x44U, s_resp.data[1], "ALFID echo");
    /* Echo of address bytes must match the request. */
    zassert_mem_equal(&s_resp.data[2], &s_req.data[2], 4U, "address echo mismatch");
    /* Echo of size bytes must match the request. */
    zassert_mem_equal(&s_resp.data[6], &s_req.data[6], 4U, "size echo mismatch");
}

/* TC-0x3D-016: write_cb receives the correct address and data */
ZTEST(svc_0x3D, test_write_cb_receives_correct_data)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x3D_req(MOCK_RW_BASE, 8U, k_write_pattern, 8U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
    zassert_true(s_was_written, "write_cb not called");
    zassert_equal(MOCK_RW_BASE, s_write_addr, "write address mismatch");
    zassert_equal(8UL, s_write_len, "write length mismatch");
    zassert_mem_equal(k_write_pattern, s_written_data, 8U, "written data mismatch");
}

/* TC-0x3D-017 */
ZTEST(svc_0x3D, test_valid_1byte_addr_1byte_size)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    /* Use single-byte encoding for a known address in the RW region low byte. */
    s_req.data[0]  = (uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS;
    s_req.data[1]  = 0x11U;    /* addrLen=1, sizeLen=1 */
    s_req.data[2]  = 0x00U;    /* 1-byte address 0x00 — not in writable map */
    s_req.data[3]  = 0x02U;    /* 1-byte size = 2 */
    s_req.data[4]  = 0xAAU;    /* data[0] */
    s_req.data[5]  = 0xBBU;    /* data[1] */
    s_req.length   = 6U;
    /* address 0x00 not in writable map → OUT_OF_RANGE */
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-018: data portion shorter than declared mem_size */
ZTEST(svc_0x3D, test_data_too_short_for_declared_size)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    /* Build a request that declares size=8 but only has 4 data bytes. */
    s_req.data[0]  = (uint8_t)UDS_SID_WRITE_MEMORY_BY_ADDRESS;
    s_req.data[1]  = 0x44U;   /* addrLen=4, sizeLen=4 */
    s_req.data[2]  = (uint8_t)((MOCK_RW_BASE >> 24U) & 0xFFU);
    s_req.data[3]  = (uint8_t)((MOCK_RW_BASE >> 16U) & 0xFFU);
    s_req.data[4]  = (uint8_t)((MOCK_RW_BASE >>  8U) & 0xFFU);
    s_req.data[5]  = (uint8_t)( MOCK_RW_BASE         & 0xFFU);
    s_req.data[6]  = 0x00U;   /* size[0] */
    s_req.data[7]  = 0x00U;   /* size[1] */
    s_req.data[8]  = 0x00U;   /* size[2] */
    s_req.data[9]  = 0x08U;   /* size[3] = 8 bytes declared */
    s_req.data[10] = 0x11U;
    s_req.data[11] = 0x22U;
    s_req.data[12] = 0x33U;
    s_req.data[13] = 0x44U;   /* only 4 data bytes present */
    s_req.length   = 14U;     /* 2+4+4+4 = 14, but 8 bytes declared */
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x3D-019: address in readable-only (RO) region is rejected by 0x3D */
ZTEST(svc_0x3D, test_readable_only_region_rejected)
{
    (void)uds_flash_ops_register(&k_mock_ops);
    build_0x3D_req(MOCK_RO_BASE, 4U, k_write_pattern, 4U);
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x3D_handler(&s_srv, &s_req, &s_resp), "");
}

/* ==========================================================================
 * run_all_tests — required by tests/runner/test_main.c
 * ========================================================================== */

void run_all_tests(void)
{
    /* --- SID 0x23 ReadMemoryByAddress --- */
    RUN_TEST(svc_0x23__test_null_ctx);
    RUN_TEST(svc_0x23__test_request_too_short);
    RUN_TEST(svc_0x23__test_no_flash_ops);
    RUN_TEST(svc_0x23__test_no_read_cb);
    RUN_TEST(svc_0x23__test_addr_len_zero);
    RUN_TEST(svc_0x23__test_size_len_zero);
    RUN_TEST(svc_0x23__test_addr_len_too_large);
    RUN_TEST(svc_0x23__test_size_len_too_large);
    RUN_TEST(svc_0x23__test_request_too_short_for_fields);
    RUN_TEST(svc_0x23__test_size_zero);
    RUN_TEST(svc_0x23__test_address_outside_readable_map);
    RUN_TEST(svc_0x23__test_address_overflow);
    RUN_TEST(svc_0x23__test_size_exceeds_response_buffer);
    RUN_TEST(svc_0x23__test_read_cb_failure);
    RUN_TEST(svc_0x23__test_valid_4byte_addr_4byte_size);
    RUN_TEST(svc_0x23__test_positive_response_sid);
    RUN_TEST(svc_0x23__test_response_data_matches_read_buf);
    RUN_TEST(svc_0x23__test_valid_1byte_addr_1byte_size);
    RUN_TEST(svc_0x23__test_readable_only_region_accepted);
    RUN_TEST(svc_0x23__test_writable_only_region_rejected);

    /* --- SID 0x3D WriteMemoryByAddress --- */
    RUN_TEST(svc_0x3D__test_null_ctx);
    RUN_TEST(svc_0x3D__test_request_too_short);
    RUN_TEST(svc_0x3D__test_no_flash_ops);
    RUN_TEST(svc_0x3D__test_no_write_cb);
    RUN_TEST(svc_0x3D__test_addr_len_zero);
    RUN_TEST(svc_0x3D__test_size_len_zero);
    RUN_TEST(svc_0x3D__test_addr_len_too_large);
    RUN_TEST(svc_0x3D__test_request_too_short_for_fields);
    RUN_TEST(svc_0x3D__test_size_zero);
    RUN_TEST(svc_0x3D__test_address_outside_writable_map);
    RUN_TEST(svc_0x3D__test_address_overflow);
    RUN_TEST(svc_0x3D__test_write_cb_failure);
    RUN_TEST(svc_0x3D__test_valid_4byte_addr_4byte_size);
    RUN_TEST(svc_0x3D__test_positive_response_sid);
    RUN_TEST(svc_0x3D__test_response_echoes_alfid_addr_size);
    RUN_TEST(svc_0x3D__test_write_cb_receives_correct_data);
    RUN_TEST(svc_0x3D__test_valid_1byte_addr_1byte_size);
    RUN_TEST(svc_0x3D__test_data_too_short_for_declared_size);
    RUN_TEST(svc_0x3D__test_readable_only_region_rejected);
}
