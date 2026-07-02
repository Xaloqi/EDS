// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_service_0x37.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x37.c
 *                    SID 0x37 — RequestTransferExit
 *
 * Coverage:
 *   TC-0x37-001  NULL ctx                              → ERR_NULL_PTR
 *   TC-0x37-002  Request too short (0 bytes)           → ERR_INVALID_PARAM
 *   TC-0x37-003  No active transfer (IDLE state)       → ERR_SEC_SEED_UNAVAILABLE (NRC 0x24)
 *   TC-0x37-004  bytes_remaining != 0 (incomplete)     → ERR_REQUEST_OUT_OF_RANGE (NRC 0x31)
 *   TC-0x37-005  Transfer context reset to IDLE after NRC 0x31 (REQ-DL-003)
 *   TC-0x37-006  Malformed CRC record length (2 bytes) → ERR_INVALID_PARAM (NRC 0x13)
 *   TC-0x37-007  Valid exit with no CRC record         → UDS_STATUS_OK
 *   TC-0x37-008  Positive response format: [0x77]
 *   TC-0x37-009  Transfer context reset to IDLE on success (REQ-DL-003)
 *   TC-0x37-010  Valid exit with matching CRC-32 record → UDS_STATUS_OK
 *   TC-0x37-011  CRC mismatch → ERR_PLATFORM (NRC 0x72), transfer reset
 *   TC-0x37-012  Flash verify callback failure → ERR_PLATFORM, transfer reset
 *   TC-0x37-013  Remaining write buffer flushed before verify
 *   TC-0x37-014  Flash ops NULL after active transfer → ERR_CONDITIONS_NOT_MET, reset
 *   TC-0x37-015  CRC record exactly 5 bytes total (SID + 4) accepted
 *   TC-0x37-016  CRC record wrong length (3 bytes, not 5) → ERR_INVALID_PARAM
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

static bool s_write_fail  = false;
static bool s_verify_fail = false;
static uint32_t s_write_call_count  = 0U;
static uint32_t s_verify_call_count = 0U;
static uint32_t s_verify_crc_received = 0U;

/* ==========================================================================
 * Mock flash callbacks
 * ========================================================================== */

static uds_status_t mock_erase(uint32_t address, uint32_t size_bytes)
{
    (void)address; (void)size_bytes;
    return UDS_STATUS_OK;
}

static uds_status_t mock_write(uint32_t address,
                                const uint8_t *data,
                                uint32_t length)
{
    (void)address; (void)data; (void)length;
    s_write_call_count++;
    if (s_write_fail) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    return UDS_STATUS_OK;
}

static uds_status_t mock_verify(uint32_t address,
                                 uint32_t size_bytes,
                                 uint32_t expected_crc)
{
    (void)address; (void)size_bytes;
    s_verify_call_count++;
    s_verify_crc_received = expected_crc;
    if (s_verify_fail) {
        return UDS_STATUS_ERR_GENERIC;
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

static const uds_flash_ops_t k_mock_ops = {
    .erase_cb         = mock_erase,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
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
 * Helper: prime transfer context as if all blocks have been received
 * (bytes_remaining == 0, no partial write pending)
 * ========================================================================== */

static void prime_completed_transfer(void)
{
    uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();
    uds_transfer_ctx_reset(tctx);

    tctx->state                   = UDS_TRANSFER_ACTIVE;
    tctx->target_address          = MOCK_FLASH_BASE;
    tctx->total_size_bytes        = (uint32_t)MOCK_BLOCK_LEN;
    tctx->bytes_remaining         = (uint32_t)0U;   /* all bytes received */
    tctx->next_write_address      = MOCK_FLASH_BASE + (uint32_t)MOCK_BLOCK_LEN;
    tctx->next_expected_block_seq = (uint8_t)0x02U;
    tctx->crc_accumulator         = (uint32_t)0xFFFFFFFFUL; /* no real data accumulated */
    tctx->write_buf_fill          = (uint16_t)0U;    /* nothing in partial buffer */
    tctx->write_buf_capacity      = (uint16_t)MOCK_BLOCK_LEN;
}

/* Helper: prime transfer with a partial write buffer pending
 * (simulates last block smaller than write_buf_capacity). */
static void prime_completed_transfer_with_partial_buf(uint16_t partial_bytes)
{
    prime_completed_transfer();
    uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();
    /* Pretend partial_bytes bytes are sitting in write_buf waiting to be flushed. */
    tctx->write_buf_fill = partial_bytes;
    memset(tctx->write_buf, 0xA5U, (size_t)partial_bytes);
}

/* Helper: build a plain 0x37 request (no CRC record). */
static void build_req_no_crc(void)
{
    s_req.data[0] = 0x37U;
    s_req.length  = 1U;
}

/* Helper: build a 0x37 request with a 4-byte CRC-32 record. */
static void build_req_with_crc(uint32_t crc_value)
{
    s_req.data[0] = 0x37U;
    s_req.data[1] = (uint8_t)((crc_value >> 24U) & 0xFFU);
    s_req.data[2] = (uint8_t)((crc_value >> 16U) & 0xFFU);
    s_req.data[3] = (uint8_t)((crc_value >>  8U) & 0xFFU);
    s_req.data[4] = (uint8_t)( crc_value          & 0xFFU);
    s_req.length  = 5U;
}

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

    s_write_fail          = false;
    s_verify_fail         = false;
    s_write_call_count    = 0U;
    s_verify_call_count   = 0U;
    s_verify_crc_received = 0U;

    (void)uds_safety_init();

    s_sess.initialized    = true;
    s_sess.active_session = UDS_SESSION_PROGRAMMING;
    s_sec.initialized     = true;
    s_sec.active_level    = 1U;
    s_srv.cfg.session_ctx  = &s_sess;
    s_srv.cfg.security_ctx = &s_sec;

    (void)uds_flash_ops_register(&k_mock_ops);
    uds_transfer_ctx_reset(uds_transfer_ctx_get());
}

void tearDown(void) {}

/* ==========================================================================
 * Test suite
 * ========================================================================== */

ZTEST_SUITE(svc_0x37, NULL, NULL, NULL, NULL, NULL);

/* TC-0x37-001 */
ZTEST(svc_0x37, test_null_ctx)
{
    prime_completed_transfer();
    build_req_no_crc();
    zassert_equal(UDS_STATUS_ERR_NULL_PTR,
                  uds_service_0x37_handler(NULL, &s_req, &s_resp), "");
}

/* TC-0x37-002  Request with zero bytes → ERR_INVALID_PARAM */
ZTEST(svc_0x37, test_request_zero_bytes)
{
    prime_completed_transfer();
    s_req.length = 0U;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x37-003  No active transfer (IDLE) → NRC 0x24 requestSequenceError */
ZTEST(svc_0x37, test_no_active_transfer)
{
    /* Transfer context is IDLE from setUp. */
    build_req_no_crc();
    zassert_equal(UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x37-004  bytes_remaining != 0 → NRC 0x31 requestOutOfRange */
ZTEST(svc_0x37, test_bytes_remaining_nonzero_rejected)
{
    /* Incomplete transfer: still 0x10 bytes to go. */
    uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();
    prime_completed_transfer();
    tctx->bytes_remaining = (uint32_t)0x10U;

    build_req_no_crc();
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x37-005  Transfer context reset to IDLE after NRC 0x31 (REQ-DL-003) */
ZTEST(svc_0x37, test_transfer_reset_on_incomplete_rejection)
{
    uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();
    prime_completed_transfer();
    tctx->bytes_remaining = (uint32_t)0x10U;

    build_req_no_crc();
    (void)uds_service_0x37_handler(&s_srv, &s_req, &s_resp);

    zassert_equal(UDS_TRANSFER_IDLE, tctx->state,
                  "REQ-DL-003: context must be IDLE after rejection");
}

/* TC-0x37-006  Malformed CRC record — 2 bytes payload (total 3) → ERR_INVALID_PARAM */
ZTEST(svc_0x37, test_malformed_crc_record_wrong_length)
{
    prime_completed_transfer();

    /* 3 bytes total: [SID, crc_hi, crc_lo] — not valid (must be 1 or 5). */
    s_req.data[0] = 0x37U;
    s_req.data[1] = 0x12U;
    s_req.data[2] = 0x34U;
    s_req.length  = 3U;

    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x37-007  Valid exit with no CRC record → UDS_STATUS_OK */
ZTEST(svc_0x37, test_valid_exit_no_crc)
{
    prime_completed_transfer();
    build_req_no_crc();
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x37-008  Positive response format: [0x77], length == 1 */
ZTEST(svc_0x37, test_positive_response_format)
{
    prime_completed_transfer();
    build_req_no_crc();
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(1U,    s_resp.length,  "response must be 1 byte");
    zassert_equal(0x77U, s_resp.data[0], "RSID must be 0x77");
}

/* TC-0x37-009  Transfer context reset to IDLE on success (REQ-DL-003) */
ZTEST(svc_0x37, test_transfer_reset_on_success)
{
    prime_completed_transfer();
    build_req_no_crc();
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                  "REQ-DL-003: context must be IDLE after successful exit");
}

/* TC-0x37-010  Valid exit with matching CRC-32 → UDS_STATUS_OK
 *
 * Strategy: we know the CRC-32 of an empty payload (no 0x36 calls) is
 * the finalisation of the initial accumulator 0xFFFFFFFF.
 * uds_transfer_crc32_finalise(0xFFFFFFFF) = 0xFFFFFFFF ^ 0xFFFFFFFF = 0x00000000.
 * Supply that as the CRC record — the handler must accept it.
 */
ZTEST(svc_0x37, test_valid_exit_with_matching_crc)
{
    prime_completed_transfer();
    /* Accumulator is 0xFFFFFFFF (no data transferred in this test).
     * Finalised CRC = 0xFFFFFFFF XOR 0xFFFFFFFF = 0x00000000. */
    uint32_t expected_crc = uds_transfer_crc32_finalise(
        uds_transfer_ctx_get()->crc_accumulator);

    build_req_with_crc(expected_crc);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x37-011  CRC mismatch → ERR_PLATFORM (NRC 0x72), transfer reset */
ZTEST(svc_0x37, test_crc_mismatch_rejected)
{
    prime_completed_transfer();
    /* Supply a CRC that will not match (0xDEADBEEF). */
    build_req_with_crc(0xDEADBEEFUL);
    zassert_equal(UDS_STATUS_ERR_PLATFORM,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                  "transfer must be IDLE after CRC mismatch");
}

/* TC-0x37-012  Flash verify callback failure → ERR_PLATFORM, transfer reset */
ZTEST(svc_0x37, test_verify_callback_failure)
{
    prime_completed_transfer();
    s_verify_fail = true;
    build_req_no_crc();
    zassert_equal(UDS_STATUS_ERR_PLATFORM,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                  "transfer must be IDLE after verify failure");
}

/* TC-0x37-013  Remaining write buffer flushed before verify */
ZTEST(svc_0x37, test_partial_buffer_flushed_before_verify)
{
    /* Prime a transfer where 8 bytes remain in the partial write buffer. */
    prime_completed_transfer_with_partial_buf(8U);
    build_req_no_crc();
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
    /* The flush must have been invoked once (for the 8-byte partial buffer). */
    zassert_equal((uint32_t)1U, s_write_call_count,
                  "write_cb must be called once to flush partial buffer");
    /* write_buf_fill must be zero after successful flush. */
    zassert_equal((uint16_t)0U, uds_transfer_ctx_get()->write_buf_fill,
                  "write_buf_fill must be 0 after flush");
}

/* TC-0x37-014  Flash ops NULL after active transfer → ERR_CONDITIONS_NOT_MET, reset */
ZTEST(svc_0x37, test_flash_ops_null_resets_transfer)
{
    prime_completed_transfer();
    (void)uds_flash_ops_register(NULL);

    build_req_no_crc();
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                  "transfer must be IDLE after NULL flash ops detection");
}

/* TC-0x37-015  CRC record exactly 5 bytes total → accepted */
ZTEST(svc_0x37, test_crc_record_exactly_5_bytes_accepted)
{
    prime_completed_transfer();
    /* Provide the correct CRC for the accumulated data. */
    uint32_t crc = uds_transfer_crc32_finalise(
        uds_transfer_ctx_get()->crc_accumulator);
    build_req_with_crc(crc);
    /* Must be exactly 5 bytes — verify our helper produces that. */
    zassert_equal(5U, s_req.length, "CRC request must be exactly 5 bytes");
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x37-016  CRC record wrong length (4 bytes total, 3-byte payload) → ERR_INVALID_PARAM */
ZTEST(svc_0x37, test_crc_record_4_bytes_total_rejected)
{
    prime_completed_transfer();

    /* 4 bytes: [SID, 0x12, 0x34, 0x56] — invalid (must be 1 or 5). */
    s_req.data[0] = 0x37U;
    s_req.data[1] = 0x12U;
    s_req.data[2] = 0x34U;
    s_req.data[3] = 0x56U;
    s_req.length  = 4U;

    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(svc_0x37__test_null_ctx);
    RUN_TEST(svc_0x37__test_request_zero_bytes);
    RUN_TEST(svc_0x37__test_no_active_transfer);
    RUN_TEST(svc_0x37__test_bytes_remaining_nonzero_rejected);
    RUN_TEST(svc_0x37__test_transfer_reset_on_incomplete_rejection);
    RUN_TEST(svc_0x37__test_malformed_crc_record_wrong_length);
    RUN_TEST(svc_0x37__test_valid_exit_no_crc);
    RUN_TEST(svc_0x37__test_positive_response_format);
    RUN_TEST(svc_0x37__test_transfer_reset_on_success);
    RUN_TEST(svc_0x37__test_valid_exit_with_matching_crc);
    RUN_TEST(svc_0x37__test_crc_mismatch_rejected);
    RUN_TEST(svc_0x37__test_verify_callback_failure);
    RUN_TEST(svc_0x37__test_partial_buffer_flushed_before_verify);
    RUN_TEST(svc_0x37__test_flash_ops_null_resets_transfer);
    RUN_TEST(svc_0x37__test_crc_record_exactly_5_bytes_accepted);
    RUN_TEST(svc_0x37__test_crc_record_4_bytes_total_rejected);
}
