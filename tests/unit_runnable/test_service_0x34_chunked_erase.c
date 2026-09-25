// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_service_0x34_chunked_erase.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x34.c — [#312] chunked-erase
 *                    path (ops->erase_step_cb + uds_service_0x34_pending_tick())
 *
 * Coverage:
 *   TC-CE-001  erase_step_cb present -> immediate NRC 0x78, NOT [0x74];
 *              transfer_ctx stays IDLE until the erase actually completes.
 *   TC-CE-002  pending_tick() with nothing pending -> returns false,
 *              out_frame untouched.
 *   TC-CE-003  Multi-chunk erase: 0x78 repeated for every incomplete chunk,
 *              final call yields [0x74] with the correct positive-response
 *              bytes AND transfer_ctx correctly initialised (REQ-DL-001/002).
 *   TC-CE-004  erase_step_cb failure mid-erase -> NRC 0x70
 *              (uploadDownloadNotAccepted), pending state dropped, tctx
 *              stays IDLE (transfer never starts).
 *   TC-CE-005  erase_step_cb reporting more bytes erased than were
 *              remaining is treated as a failure (defensive bound check),
 *              not accepted at face value.
 *   TC-CE-006  A new 0x34 arriving while an earlier erase is still pending
 *              aborts it and starts a fresh one for the NEW request's
 *              address/size — proven by observing the fresh request's own
 *              chunk boundaries, not the pre-empted one's.
 *
 * WHY THIS FILE EXISTS: issue #312 (RequestDownload's whole-region
 * erase_cb() call starves a 100 ms watchdog window on hardware with a
 * multi-second per-sector worst-case erase time). This is genuinely new
 * state-machine logic — a first self-review already caught a real bug
 * (reading s_erase_pending's fields AFTER the abort call that zeroes them)
 * before this file existed; per this repo's testing discipline a class of
 * defect like that needs its own regression coverage, not just correctness
 * by inspection.
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

#define MOCK_FLASH_BASE     (0x08020000UL)
#define MOCK_FLASH_SIZE     (0x2000UL)   /* 8 KB */
#define MOCK_BLOCK_LEN      (256U)
#define MOCK_CHUNK_SIZE     (1024U)      /* mock "sector" size for erase_step_cb */

/* ==========================================================================
 * Mock erase_step_cb state
 * ========================================================================== */

static uint32_t s_step_calls          = 0U;
static uint32_t s_fail_on_call        = 0U; /* 0 = never fail */
static uint32_t s_overreport_on_call  = 0U; /* 0 = never overreport */
static uint32_t s_last_step_address   = 0U;

static uds_status_t mock_erase_step(uint32_t   address,
                                     uint32_t   max_size,
                                     uint32_t  *out_erased)
{
    uint32_t chunk;

    s_step_calls++;
    s_last_step_address = address;

    if (s_step_calls == s_fail_on_call) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    chunk = (max_size < (uint32_t)MOCK_CHUNK_SIZE) ? max_size : (uint32_t)MOCK_CHUNK_SIZE;

    if (s_step_calls == s_overreport_on_call) {
        /* Defensive-bound violation: claim more than was remaining. */
        chunk = max_size + (uint32_t)1U;
    }

    *out_erased = chunk;
    return UDS_STATUS_OK;
}

static uint32_t s_whole_erase_calls = 0U;

/* Never actually reached by the chunked path (erase_step_cb takes over),
 * but erase_cb is a MANDATORY field of uds_flash_ops_t — must be non-NULL
 * for registration to succeed. Counts calls rather than asserting directly
 * (this runs from a non-void C callback; the host ztest shim's
 * zassert_true() expands to a bare `return;`, which does not compile in a
 * function returning uds_status_t) — each test checks s_whole_erase_calls
 * itself, from its own void test function. */
static uds_status_t mock_erase_whole_unused(uint32_t address, uint32_t size_bytes)
{
    (void)address;
    (void)size_bytes;
    s_whole_erase_calls++;
    return UDS_STATUS_ERR_PLATFORM;
}

static uds_status_t mock_write(uint32_t address, const uint8_t *data, uint32_t length)
{
    (void)address;
    (void)data;
    (void)length;
    return UDS_STATUS_OK;
}

static uds_status_t mock_verify(uint32_t address, uint32_t size_bytes, uint32_t expected_crc)
{
    (void)address;
    (void)size_bytes;
    (void)expected_crc;
    return UDS_STATUS_OK;
}

static const uds_flash_region_t k_mock_region[1U] = {
    {
        .base_address = MOCK_FLASH_BASE,
        .size_bytes   = (uint32_t)MOCK_FLASH_SIZE,
        .writable     = true,
    }
};

static const uds_flash_ops_t k_mock_ops_chunked = {
    .erase_cb         = mock_erase_whole_unused,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
    .erase_step_cb    = mock_erase_step,
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
static uds_msg_buf_t      s_tick_frame;

/* ==========================================================================
 * setUp / tearDown
 * ========================================================================== */

void setUp(void)
{
    memset(&s_sess,       0, sizeof(s_sess));
    memset(&s_sec,        0, sizeof(s_sec));
    memset(&s_srv,        0, sizeof(s_srv));
    memset(&s_req,        0, sizeof(s_req));
    memset(&s_resp,       0, sizeof(s_resp));
    memset(&s_tick_frame, 0, sizeof(s_tick_frame));

    s_step_calls          = 0U;
    s_fail_on_call        = 0U;
    s_overreport_on_call  = 0U;
    s_last_step_address   = 0U;
    s_whole_erase_calls   = 0U;

    (void)uds_safety_init();

    s_sess.initialized    = true;
    s_sess.active_session = UDS_SESSION_PROGRAMMING;
    s_sec.initialized     = true;
    s_sec.active_level    = 1U;

    s_srv.cfg.session_ctx  = &s_sess;
    s_srv.cfg.security_ctx = &s_sec;

    uds_transfer_ctx_reset(uds_transfer_ctx_get());

    (void)uds_flash_ops_register(NULL);
    /* Drain any pending erase left over from a previous test — a fresh
     * pending_tick() with no ops registered is a safe way to no-op-check,
     * but the real drain is: nothing to do, since setUp always runs after
     * a test that either completed or explicitly aborted its own erase.
     * Re-registering k_mock_ops_chunked below is each test's own choice. */
}

void tearDown(void) {}

/* ==========================================================================
 * Helper
 * ========================================================================== */

static void build_valid_req(uint32_t mem_address, uint32_t mem_size)
{
    s_req.data[0] = 0x34U;
    s_req.data[1] = 0x00U;
    s_req.data[2] = 0x44U;

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

ZTEST_SUITE(svc_0x34_chunked_erase, NULL, NULL, NULL, NULL, NULL);

/* TC-CE-001 */
ZTEST(svc_0x34_chunked_erase, test_immediate_response_pending)
{
    uds_status_t status;
    uds_transfer_ctx_t *tctx;

    (void)uds_flash_ops_register(&k_mock_ops_chunked);
    build_valid_req(MOCK_FLASH_BASE, (uint32_t)3000U); /* 3 chunks of 1024 */

    status = uds_service_0x34_handler(&s_srv, &s_req, &s_resp);

    zassert_equal(UDS_STATUS_OK, status, "handler must return OK (NRC-shaped positive)");
    zassert_equal(3U, s_resp.length, "0x78 response is [0x7F, SID, NRC] = 3 bytes");
    zassert_equal(0x7FU, s_resp.data[0], "");
    zassert_equal(0x34U, s_resp.data[1], "");
    zassert_equal(0x78U, s_resp.data[2], "NRC must be requestCorrectlyReceived-ResponsePending");

    tctx = uds_transfer_ctx_get();
    zassert_not_equal((int)UDS_TRANSFER_ACTIVE, (int)tctx->state,
                      "transfer must NOT be active until the erase actually completes");
    zassert_equal(0U, s_step_calls, "0x34 itself must not call erase_step_cb synchronously");

    /* Drain the pending erase so it doesn't leak into the next test. */
    while (uds_service_0x34_pending_tick(&s_tick_frame)) {
        /* consume */
    }

    zassert_equal(0U, s_whole_erase_calls,
                  "chunked path must never fall back to the whole-region erase_cb");
}

/* TC-CE-002 */
ZTEST(svc_0x34_chunked_erase, test_tick_noop_when_nothing_pending)
{
    bool result;

    memset(&s_tick_frame, 0xAA, sizeof(s_tick_frame)); /* sentinel */
    result = uds_service_0x34_pending_tick(&s_tick_frame);

    zassert_false(result, "");
    zassert_equal(0xAA, s_tick_frame.data[0], "out_frame must be left untouched");
}

/* TC-CE-003 */
ZTEST(svc_0x34_chunked_erase, test_multi_chunk_completes_and_arms_transfer)
{
    uds_status_t status;
    uds_transfer_ctx_t *tctx;
    uint32_t total_size = (uint32_t)3000U; /* 1024 + 1024 + 952 = 3 chunks */
    int chunks_seen_0x78 = 0;
    bool got_final = false;
    int guard;

    (void)uds_flash_ops_register(&k_mock_ops_chunked);
    build_valid_req(MOCK_FLASH_BASE, total_size);

    status = uds_service_0x34_handler(&s_srv, &s_req, &s_resp);
    zassert_equal(UDS_STATUS_OK, status, "");
    zassert_equal(0x78U, s_resp.data[2], "");

    for (guard = 0; guard < 10; guard++) {
        bool has_frame = uds_service_0x34_pending_tick(&s_tick_frame);

        zassert_true(has_frame, "a tick must always produce a frame while erase is pending");

        if (s_tick_frame.data[0] == 0x7FU) {
            zassert_equal(0x78U, s_tick_frame.data[2], "interim frames must be 0x78");
            chunks_seen_0x78++;
            continue;
        }

        zassert_equal(0x74U, s_tick_frame.data[0], "final frame must be the [0x74] positive response");
        got_final = true;
        break;
    }

    zassert_true(got_final, "erase must eventually complete within a bounded number of ticks");
    zassert_equal(3U, s_step_calls, "3000 bytes / 1024-byte chunks = 3 erase_step_cb calls");
    zassert_equal(2, chunks_seen_0x78, "2 interim 0x78s before the 3rd (final) chunk");

    /* [0x74, lengthFormatIdentifier, maxBlockLenHi, maxBlockLenLo] */
    zassert_equal(4U, s_tick_frame.length, "");
    zassert_equal((uint8_t)(2U << 4U), s_tick_frame.data[1], "lengthFormatIdentifier: 2 bytes, reserved=0");
    {
        uint16_t max_block = ((uint16_t)s_tick_frame.data[2] << 8U) | (uint16_t)s_tick_frame.data[3];
        zassert_equal((uint16_t)(MOCK_BLOCK_LEN + 1U), max_block,
                      "maxNumberOfBlockLength includes the block-counter byte");
    }

    tctx = uds_transfer_ctx_get();
    zassert_equal((int)UDS_TRANSFER_ACTIVE, (int)tctx->state, "REQ-DL-002 setup must have run");
    zassert_equal((int)UDS_TRANSFER_DIR_DOWNLOAD, (int)tctx->direction, "");
    zassert_equal(MOCK_FLASH_BASE, tctx->target_address, "");
    zassert_equal(total_size, tctx->total_size_bytes, "");
    zassert_equal(total_size, tctx->bytes_remaining, "REQ-DL-002");
    zassert_equal(MOCK_FLASH_BASE, tctx->next_write_address, "");
    zassert_equal((uint8_t)0x01U, tctx->next_expected_block_seq, "REQ-DL-001");
    zassert_equal((uint32_t)0xFFFFFFFFUL, tctx->crc_accumulator, "");
    zassert_equal(0U, (unsigned)tctx->write_buf_fill, "");

    /* No further ticks should do anything once complete. */
    zassert_false(uds_service_0x34_pending_tick(&s_tick_frame), "");
}

/* TC-CE-004 */
ZTEST(svc_0x34_chunked_erase, test_step_failure_mid_erase)
{
    uds_status_t status;
    uds_transfer_ctx_t *tctx;
    bool has_frame;

    s_fail_on_call = 2U; /* fail on the 2nd chunk */

    (void)uds_flash_ops_register(&k_mock_ops_chunked);
    build_valid_req(MOCK_FLASH_BASE, (uint32_t)3000U);

    status = uds_service_0x34_handler(&s_srv, &s_req, &s_resp);
    zassert_equal(UDS_STATUS_OK, status, "");

    has_frame = uds_service_0x34_pending_tick(&s_tick_frame); /* chunk 1: OK, 0x78 */
    zassert_true(has_frame, "");
    zassert_equal(0x78U, s_tick_frame.data[2], "");

    has_frame = uds_service_0x34_pending_tick(&s_tick_frame); /* chunk 2: fails */
    zassert_true(has_frame, "");
    zassert_equal(0x7FU, s_tick_frame.data[0], "");
    zassert_equal(0x34U, s_tick_frame.data[1], "");
    zassert_equal(0x70U, s_tick_frame.data[2],
                  "NRC 0x70 uploadDownloadNotAccepted — same NRC the synchronous "
                  "erase_cb failure path produces");

    tctx = uds_transfer_ctx_get();
    zassert_not_equal((int)UDS_TRANSFER_ACTIVE, (int)tctx->state,
                      "a failed erase must never arm the transfer");

    zassert_false(uds_service_0x34_pending_tick(&s_tick_frame),
                  "pending state must be dropped after the failure is reported");
}

/* TC-CE-005 */
ZTEST(svc_0x34_chunked_erase, test_overreport_treated_as_failure)
{
    uds_status_t status;
    bool has_frame;

    s_overreport_on_call = 1U; /* 1st call claims more than max_size */

    (void)uds_flash_ops_register(&k_mock_ops_chunked);
    build_valid_req(MOCK_FLASH_BASE, (uint32_t)3000U);

    status = uds_service_0x34_handler(&s_srv, &s_req, &s_resp);
    zassert_equal(UDS_STATUS_OK, status, "");

    has_frame = uds_service_0x34_pending_tick(&s_tick_frame);
    zassert_true(has_frame, "");
    zassert_equal(0x7FU, s_tick_frame.data[0], "");
    zassert_equal(0x70U, s_tick_frame.data[2],
                  "erase_step_cb claiming more bytes erased than were remaining "
                  "must fail closed, not be trusted at face value");
}

/* TC-CE-006 */
ZTEST(svc_0x34_chunked_erase, test_preempted_by_new_request)
{
    uds_status_t status;
    bool has_frame;
    uint32_t second_address = MOCK_FLASH_BASE + (uint32_t)0x1000U;

    (void)uds_flash_ops_register(&k_mock_ops_chunked);

    /* First 0x34 — starts an erase, never let it finish. */
    build_valid_req(MOCK_FLASH_BASE, (uint32_t)3000U);
    status = uds_service_0x34_handler(&s_srv, &s_req, &s_resp);
    zassert_equal(UDS_STATUS_OK, status, "");

    has_frame = uds_service_0x34_pending_tick(&s_tick_frame); /* one chunk in */
    zassert_true(has_frame, "");
    zassert_equal(0x78U, s_tick_frame.data[2], "");
    zassert_equal(1U, s_step_calls, "");

    /* Second 0x34 pre-empts the first — must abort it and start fresh at
     * the NEW address, not silently continue the old one. */
    build_valid_req(second_address, (uint32_t)512U);
    status = uds_service_0x34_handler(&s_srv, &s_req, &s_resp);
    zassert_equal(UDS_STATUS_OK, status, "");
    zassert_equal(0x78U, s_resp.data[2], "");

    has_frame = uds_service_0x34_pending_tick(&s_tick_frame);
    zassert_true(has_frame, "");
    zassert_equal(second_address, s_last_step_address,
                  "the pre-empting request's own address must be what gets erased");
    zassert_equal(0x74U, s_tick_frame.data[0],
                  "512 bytes fits in one chunk — this tick must already be final");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(svc_0x34_chunked_erase__test_immediate_response_pending);
    RUN_TEST(svc_0x34_chunked_erase__test_tick_noop_when_nothing_pending);
    RUN_TEST(svc_0x34_chunked_erase__test_multi_chunk_completes_and_arms_transfer);
    RUN_TEST(svc_0x34_chunked_erase__test_step_failure_mid_erase);
    RUN_TEST(svc_0x34_chunked_erase__test_overreport_treated_as_failure);
    RUN_TEST(svc_0x34_chunked_erase__test_preempted_by_new_request);
}
