// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_service_0x36.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x36.c
 *                    SID 0x36 — TransferData
 *
 * Coverage:
 *   TC-0x36-001  NULL ctx                            → ERR_NULL_PTR
 *   TC-0x36-002  Request too short (< 3 bytes)       → ERR_INVALID_PARAM
 *   TC-0x36-003  No active transfer (IDLE state)     → ERR_SERVICE_NOT_SUPPORTED_IN_SESSION (NRC 0x24)
 *   TC-0x36-004  Block sequence counter 0x00         → ERR_TP_UNEXPECTED_PDU (NRC 0x73)
 *   TC-0x36-005  Block counter mismatch (wrong value) → ERR_TP_UNEXPECTED_PDU (NRC 0x73)
 *   TC-0x36-006  First block (seq=0x01) accepted      → UDS_STATUS_OK
 *   TC-0x36-007  Positive response format [0x76, blockSeq]
 *   TC-0x36-008  bytes_remaining decremented per payload byte
 *   TC-0x36-009  Block counter advances after acceptance
 *   TC-0x36-010  Block counter wrap: 0xFF → 0x01 (REQ-DL-001)
 *   TC-0x36-011  Counter never 0x00 after wrap
 *   TC-0x36-012  Write callback invoked when buffer is full
 *   TC-0x36-013  Write failure → ERR_PLATFORM, transfer reset to IDLE
 *   TC-0x36-014  CRC accumulator updated over payload bytes
 *   TC-0x36-015  No flash ops after 0x34 → ERR_CONDITIONS_NOT_MET
 *   TC-0x36-016  Payload capped at bytes_remaining (oversized last block)
 *   TC-0x36-017  Second block accepted with correct incremented counter
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
#define MOCK_BLOCK_LEN    (256U)     /* payload bytes per block */

/* ==========================================================================
 * Mock flash state
 * ========================================================================== */

static bool     s_write_fail       = false;
static uint32_t s_write_call_count = 0U;
static uint32_t s_last_write_addr  = 0U;
static uint32_t s_last_write_len   = 0U;

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
    (void)data;
    s_write_call_count++;
    s_last_write_addr = address;
    s_last_write_len  = length;
    if (s_write_fail) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    return UDS_STATUS_OK;
}

static uds_status_t mock_verify(uint32_t address,
                                 uint32_t size_bytes,
                                 uint32_t expected_crc)
{
    (void)address; (void)size_bytes; (void)expected_crc;
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
 * Helper: prime the transfer context as if a successful 0x34 was processed
 * ========================================================================== */

static void prime_active_transfer(uint32_t total_size)
{
    uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();
    uds_transfer_ctx_reset(tctx);

    tctx->state                   = UDS_TRANSFER_ACTIVE;
    tctx->target_address          = MOCK_FLASH_BASE;
    tctx->total_size_bytes        = total_size;
    tctx->bytes_remaining         = total_size;
    tctx->next_write_address      = MOCK_FLASH_BASE;
    tctx->next_expected_block_seq = (uint8_t)0x01U;
    tctx->crc_accumulator         = (uint32_t)0xFFFFFFFFUL;
    tctx->write_buf_fill          = (uint16_t)0U;
    tctx->write_buf_capacity      = (uint16_t)MOCK_BLOCK_LEN;
}

/* Helper: build a 0x36 request with blockSeq and n payload bytes. */
static void build_req(uint8_t block_seq, uint8_t payload_val, uint16_t payload_len)
{
    uint16_t i;
    s_req.data[0] = 0x36U;
    s_req.data[1] = block_seq;
    for (i = (uint16_t)0U; i < payload_len && (i + 2U) < (uint16_t)sizeof(s_req.data); i++) {
        s_req.data[i + 2U] = payload_val;
    }
    s_req.length = (uint16_t)(2U + payload_len);
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

    s_write_fail       = false;
    s_write_call_count = 0U;
    s_last_write_addr  = 0U;
    s_last_write_len   = 0U;

    (void)uds_safety_init();

    s_sess.initialized    = true;
    s_sess.active_session = UDS_SESSION_PROGRAMMING;
    s_sec.initialized     = true;
    s_sec.active_level    = 1U;
    s_srv.cfg.session_ctx  = &s_sess;
    s_srv.cfg.security_ctx = &s_sec;

    (void)uds_flash_ops_register(&k_mock_ops);

    /* Start with IDLE state — each test primes the context as needed. */
    uds_transfer_ctx_reset(uds_transfer_ctx_get());
}

void tearDown(void) {}

/* ==========================================================================
 * Test suite
 * ========================================================================== */

ZTEST_SUITE(svc_0x36, NULL, NULL, NULL, NULL, NULL);

/* TC-0x36-001 */
ZTEST(svc_0x36, test_null_ctx)
{
    prime_active_transfer(0x100U);
    build_req(0x01U, 0xAAU, 4U);
    zassert_equal(UDS_STATUS_ERR_NULL_PTR,
                  uds_service_0x36_handler(NULL, &s_req, &s_resp), "");
}

/* TC-0x36-002 */
ZTEST(svc_0x36, test_request_too_short)
{
    prime_active_transfer(0x100U);
    s_req.data[0] = 0x36U;
    s_req.data[1] = 0x01U;
    s_req.length  = 2U;   /* < 3 */
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x36-003  No active transfer (IDLE) → NRC 0x24 requestSequenceError */
ZTEST(svc_0x36, test_no_active_transfer)
{
    /* transfer context is IDLE from setUp. */
    build_req(0x01U, 0xBBU, 4U);
    zassert_equal(UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x36-004  Block sequence counter 0x00 → NRC 0x73 */
ZTEST(svc_0x36, test_block_seq_zero_always_invalid)
{
    prime_active_transfer(0x100U);
    build_req(0x00U, 0xCCU, 4U);
    zassert_equal(UDS_STATUS_ERR_TP_UNEXPECTED_PDU,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x36-005  Block counter mismatch → NRC 0x73 */
ZTEST(svc_0x36, test_block_counter_mismatch)
{
    prime_active_transfer(0x100U);
    /* Expected 0x01, send 0x02 — mismatch. */
    build_req(0x02U, 0xDDU, 4U);
    zassert_equal(UDS_STATUS_ERR_TP_UNEXPECTED_PDU,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x36-006  First block accepted → UDS_STATUS_OK */
ZTEST(svc_0x36, test_first_block_accepted)
{
    prime_active_transfer(0x100U);
    build_req(0x01U, 0xEEU, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x36-007  Positive response format: [0x76, blockSeq] */
ZTEST(svc_0x36, test_positive_response_format)
{
    prime_active_transfer(0x100U);
    build_req(0x01U, 0xFFU, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    zassert_equal(2U,    s_resp.length,  "response must be 2 bytes");
    zassert_equal(0x76U, s_resp.data[0], "RSID must be 0x76");
    zassert_equal(0x01U, s_resp.data[1], "echo block seq 0x01");
}

/* TC-0x36-008  bytes_remaining decremented per payload byte */
ZTEST(svc_0x36, test_bytes_remaining_decremented)
{
    prime_active_transfer(0x100U);
    build_req(0x01U, 0x11U, 8U);   /* 8 payload bytes */
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal((uint32_t)(0x100U - 8U),
                  uds_transfer_ctx_get()->bytes_remaining,
                  "bytes_remaining must decrease by payload_len");
}

/* TC-0x36-009  Block counter advances after acceptance */
ZTEST(svc_0x36, test_block_counter_advances)
{
    prime_active_transfer(0x200U);
    build_req(0x01U, 0x22U, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(0x02U,
                  uds_transfer_ctx_get()->next_expected_block_seq,
                  "counter must advance to 0x02 after first block");
}

/* TC-0x36-010  Block counter wrap: 0xFF → 0x01 (REQ-DL-001) */
ZTEST(svc_0x36, test_block_counter_wrap_ff_to_01)
{
    prime_active_transfer(0x2000U);
    /* Manually set the counter to 0xFF — the next accepted block must wrap. */
    uds_transfer_ctx_get()->next_expected_block_seq = (uint8_t)0xFFU;

    build_req(0xFFU, 0x55U, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    zassert_equal(0x01U,
                  uds_transfer_ctx_get()->next_expected_block_seq,
                  "REQ-DL-001: counter must wrap 0xFF → 0x01");
}

/* TC-0x36-011  Counter never 0x00 after wrap */
ZTEST(svc_0x36, test_counter_never_zero_after_wrap)
{
    prime_active_transfer(0x2000U);
    uds_transfer_ctx_get()->next_expected_block_seq = (uint8_t)0xFFU;

    build_req(0xFFU, 0x66U, 4U);
    (void)uds_service_0x36_handler(&s_srv, &s_req, &s_resp);

    zassert_not_equal(0x00U,
                      uds_transfer_ctx_get()->next_expected_block_seq,
                      "counter 0x00 is always invalid — must never be next expected");
}

/* TC-0x36-012  Write callback invoked when buffer is full */
ZTEST(svc_0x36, test_write_callback_invoked_when_buffer_full)
{
    /* Set up a transfer where write_buf_capacity == payload_len so one
     * block fills the buffer exactly and triggers s_flush_write_buf(). */
    prime_active_transfer((uint32_t)MOCK_BLOCK_LEN * 4U);

    /* Send exactly MOCK_BLOCK_LEN bytes — should trigger one flush. */
    build_req(0x01U, 0x77U, (uint16_t)MOCK_BLOCK_LEN);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal((uint32_t)1U, s_write_call_count, "one flush expected");
    zassert_equal((uint32_t)MOCK_BLOCK_LEN, s_last_write_len,
                  "flush must write MOCK_BLOCK_LEN bytes");
}

/* TC-0x36-013  Write failure → ERR_PLATFORM, transfer reset to IDLE */
ZTEST(svc_0x36, test_write_failure_resets_transfer)
{
    prime_active_transfer((uint32_t)MOCK_BLOCK_LEN * 2U);
    s_write_fail = true;

    /* A full block will trigger a flush which will fail. */
    build_req(0x01U, 0x88U, (uint16_t)MOCK_BLOCK_LEN);
    zassert_equal(UDS_STATUS_ERR_PLATFORM,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(UDS_TRANSFER_IDLE,
                  uds_transfer_ctx_get()->state,
                  "REQ-DL-003: transfer must be IDLE after write failure");
}

/* TC-0x36-014  CRC accumulator updated over payload bytes */
ZTEST(svc_0x36, test_crc_accumulator_updated)
{
    prime_active_transfer(0x100U);
    uint32_t crc_before = uds_transfer_ctx_get()->crc_accumulator;

    build_req(0x01U, 0xA5U, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    /* CRC must have changed from its initial value. */
    zassert_not_equal(crc_before,
                      uds_transfer_ctx_get()->crc_accumulator,
                      "CRC accumulator must be updated after TransferData");
}

/* TC-0x36-015  Flash ops de-registered after 0x34 → ERR_CONDITIONS_NOT_MET */
ZTEST(svc_0x36, test_flash_ops_null_after_active_transfer)
{
    prime_active_transfer(0x100U);
    /* Simulate flash ops de-registered after 0x34. */
    (void)uds_flash_ops_register(NULL);

    build_req(0x01U, 0xBBU, 4U);
    zassert_equal(UDS_STATUS_ERR_CONDITIONS_NOT_MET,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
}

/* TC-0x36-016  Payload capped at bytes_remaining (oversized last block) */
ZTEST(svc_0x36, test_payload_capped_at_bytes_remaining)
{
    /* Set bytes_remaining to just 4 bytes, but send 16. */
    prime_active_transfer(4U);

    build_req(0x01U, 0xCCU, 16U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
    /* bytes_remaining must be exactly 0 after capping to 4. */
    zassert_equal((uint32_t)0U,
                  uds_transfer_ctx_get()->bytes_remaining,
                  "bytes_remaining must be 0 after last block");
}

/* TC-0x36-017  Second block accepted with correctly incremented counter */
ZTEST(svc_0x36, test_second_block_accepted)
{
    prime_active_transfer(0x200U);

    /* First block. */
    build_req(0x01U, 0xA1U, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    /* Second block — counter must be 0x02 now. */
    memset(&s_resp, 0, sizeof(s_resp));
    build_req(0x02U, 0xA2U, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(0x02U, s_resp.data[1], "echo must be 0x02");
    zassert_equal(0x03U,
                  uds_transfer_ctx_get()->next_expected_block_seq,
                  "counter must advance to 0x03 after second block");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(svc_0x36__test_null_ctx);
    RUN_TEST(svc_0x36__test_request_too_short);
    RUN_TEST(svc_0x36__test_no_active_transfer);
    RUN_TEST(svc_0x36__test_block_seq_zero_always_invalid);
    RUN_TEST(svc_0x36__test_block_counter_mismatch);
    RUN_TEST(svc_0x36__test_first_block_accepted);
    RUN_TEST(svc_0x36__test_positive_response_format);
    RUN_TEST(svc_0x36__test_bytes_remaining_decremented);
    RUN_TEST(svc_0x36__test_block_counter_advances);
    RUN_TEST(svc_0x36__test_block_counter_wrap_ff_to_01);
    RUN_TEST(svc_0x36__test_counter_never_zero_after_wrap);
    RUN_TEST(svc_0x36__test_write_callback_invoked_when_buffer_full);
    RUN_TEST(svc_0x36__test_write_failure_resets_transfer);
    RUN_TEST(svc_0x36__test_crc_accumulator_updated);
    RUN_TEST(svc_0x36__test_flash_ops_null_after_active_transfer);
    RUN_TEST(svc_0x36__test_payload_capped_at_bytes_remaining);
    RUN_TEST(svc_0x36__test_second_block_accepted);
}
