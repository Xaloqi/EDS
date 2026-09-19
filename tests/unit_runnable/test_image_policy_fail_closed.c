// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_image_policy_fail_closed.c
 *
 * MODULE UNDER TEST: core/uds_services/service_0x34.c and service_0x37.c
 *                    (PRODUCTION-configuration image policy gate).
 *
 * [#232 Phase 1]
 *
 * PURPOSE:
 *   Prove the PRODUCTION (fail-closed) half of the issue #232 DFU image
 *   policy gate: a production build that reaches RequestDownload — or
 *   RequestTransferExit — with no uds_image_policy_t registered must REFUSE
 *   with NRC 0x22 rather than erase flash, accept an image, or answer
 *   [0x77].  Before #232 both of those requests succeeded, which is exactly
 *   the finding the external Tier-1 review filed.
 *
 *   These two refusals only exist when EDS_BUILD_IS_PRODUCTION != 0, which
 *   cannot be reached from the default dev-configuration build used by
 *   every other test module (see build_tests.sh).  This file is therefore
 *   compiled as its own, separate test binary with
 *   -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0, exactly as
 *   tests/unit_runnable/test_trng_fail_closed.c is.
 *
 *   The mirror DEVELOPMENT-configuration behaviour — both requests still
 *   succeeding without a policy, so that every non-SafeBoot example and
 *   harness test keeps working — is proven in
 *   tests/unit_runnable/test_image_policy.c.
 *
 * TEST CASES:
 *   TC-IPFC-001  Production + no policy: 0x34 -> ERR_IMAGE_POLICY_ABSENT
 *                (NRC 0x22), erase_cb never called, no transfer opened,
 *                platform_violations +1 with last_violation_code ==
 *                ERR_IMAGE_POLICY_ABSENT.
 *   TC-IPFC-002  Production + policy registered: 0x34 accepted — the gate
 *                refuses the missing policy, not production builds.
 *   TC-IPFC-003  Production + no policy at transfer exit (defence in depth,
 *                REQ-IMGPOL-002): 0x37 -> ERR_IMAGE_POLICY_ABSENT (NRC
 *                0x22), NO [0x77] emitted, transfer context reset,
 *                platform_violations +1.
 *   TC-IPFC-004  Production + policy de-registered mid-transfer: the 0x34
 *                gate passed, but 0x37 still refuses — this is the exact
 *                window the defence-in-depth check exists to close.
 *   TC-IPFC-005  Production + policy registered: full 0x34 -> 0x36 -> 0x37
 *                round trip still completes with [0x77].  Fail-closed must
 *                not break the good production path.
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
#include "uds_security_algo.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"
#include "uds_image_policy.h"

/* =============================================================================
 * [#232] Compile-time proof this TU is actually built in the production
 * configuration.
 *
 * Without this guard, a broken build-flag pipeline (build_tests.sh losing
 * the -DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 flag for this module) would
 * silently compile the file in DEVELOPMENT mode, where every refusal below
 * simply does not exist — and the suite would report failures that look
 * like product defects, or worse, be "fixed" by relaxing the assertions.
 * Fail at compile time instead, naming the cause.  Same mechanism, and same
 * reasoning, as tests/unit_runnable/test_trng_fail_closed.c.
 * ============================================================================= */
#if !EDS_BUILD_IS_PRODUCTION
#  error "[#232] test_image_policy_fail_closed.c must be built with " \
         "-DCONFIG_DIAG_PLACEHOLDER_KEYS_ONLY=0 so that " \
         "EDS_BUILD_IS_PRODUCTION is 1 and the fail-closed DFU gates are " \
         "compiled in -- see the extra_flags_for_test() entry in build_tests.sh."
#endif

/* ==========================================================================
 * Mock flash
 * ========================================================================== */

#define MOCK_FLASH_BASE   (0x08020000UL)
#define MOCK_FLASH_SIZE   (0x2000UL)
#define MOCK_BLOCK_LEN    (256U)
#define IMAGE_SIZE        (128U)

static uint32_t s_erase_call_count = 0U;

static uds_status_t mock_erase(uint32_t address, uint32_t size_bytes)
{
    (void)address; (void)size_bytes;
    s_erase_call_count++;
    return UDS_STATUS_OK;
}

static uds_status_t mock_write(uint32_t address, const uint8_t *data, uint32_t length)
{
    (void)address; (void)data; (void)length;
    return UDS_STATUS_OK;
}

static uds_status_t mock_verify(uint32_t address, uint32_t size_bytes, uint32_t expected_crc)
{
    (void)address; (void)size_bytes; (void)expected_crc;
    return UDS_STATUS_OK;
}

static const uds_flash_region_t k_mock_region[1U] = {
    {
        .base_address = MOCK_FLASH_BASE,
        .size_bytes   = (uint32_t)MOCK_FLASH_SIZE,
        .writable     = true,
        .readable     = true,
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
 * Mock image policy — always accepts; this file is about the gate, not the
 * verdict logic (that is covered in test_image_policy.c).
 * ========================================================================== */

static uint32_t s_finalise_calls = 0U;

static uds_status_t accept_begin(const uds_image_transfer_info_t *info)
{
    (void)info;
    return UDS_STATUS_OK;
}

static uds_status_t accept_update(uint32_t image_offset, const uint8_t *data, uint32_t length)
{
    (void)image_offset; (void)data; (void)length;
    return UDS_STATUS_OK;
}

static uds_status_t accept_finalise(const uds_image_evidence_t *evidence,
                                     uds_image_verdict_t        *out_verdict)
{
    (void)evidence;
    s_finalise_calls++;
    *out_verdict = UDS_IMAGE_ACCEPT;
    return UDS_STATUS_OK;
}

static const uds_image_policy_t k_accept_policy = {
    .begin_cb     = accept_begin,
    .update_cb    = accept_update,
    .finalise_cb  = accept_finalise,
    .commit_cb    = NULL,
    .abort_cb     = NULL,
    .policy_flags = 0U,
};

/* ==========================================================================
 * Test state
 * ========================================================================== */

static uds_session_ctx_t  s_sess;
static uds_security_ctx_t s_sec;
static uds_server_ctx_t   s_srv;
static uds_msg_buf_t      s_req;
static uds_msg_buf_t      s_resp;

static void build_0x34_req(uint32_t mem_address, uint32_t mem_size)
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

static void build_0x36_req(uint8_t block_seq, uint16_t payload_len)
{
    uint16_t i;

    s_req.data[0] = 0x36U;
    s_req.data[1] = block_seq;
    for (i = 0U; i < payload_len; i++) {
        s_req.data[2U + i] = (uint8_t)(0xC0U + (uint8_t)i);
    }
    s_req.length = (uint16_t)(2U + payload_len);
}

static void build_0x37_with_crc(uint32_t crc_value)
{
    s_req.data[0] = 0x37U;
    s_req.data[1] = (uint8_t)((crc_value >> 24U) & 0xFFU);
    s_req.data[2] = (uint8_t)((crc_value >> 16U) & 0xFFU);
    s_req.data[3] = (uint8_t)((crc_value >>  8U) & 0xFFU);
    s_req.data[4] = (uint8_t)( crc_value          & 0xFFU);
    s_req.length  = 5U;
}

static uint32_t current_crc(void)
{
    return uds_transfer_crc32_finalise(uds_transfer_ctx_get()->crc_accumulator);
}

static uint32_t platform_violations(void)
{
    const uds_safety_ctx_t *sctx = uds_safety_get_ctx();
    return (sctx != NULL) ? sctx->platform_violations : 0xFFFFFFFFUL;
}

void setUp(void)
{
    (void)memset(&s_sess, 0, sizeof(s_sess));
    (void)memset(&s_sec,  0, sizeof(s_sec));
    (void)memset(&s_srv,  0, sizeof(s_srv));
    (void)memset(&s_req,  0, sizeof(s_req));
    (void)memset(&s_resp, 0, sizeof(s_resp));

    s_erase_call_count = 0U;
    s_finalise_calls   = 0U;

    (void)uds_safety_init();
    (void)uds_safety_reset_counters();

    s_sess.initialized     = true;
    s_sess.active_session  = UDS_SESSION_PROGRAMMING;
    s_sec.initialized      = true;
    s_sec.active_level     = 1U;
    s_srv.cfg.session_ctx  = &s_sess;
    s_srv.cfg.security_ctx = &s_sec;

    (void)uds_flash_ops_register(&k_mock_ops);
    uds_image_policy_clear();
    uds_transfer_ctx_reset(uds_transfer_ctx_get());
}

void tearDown(void)
{
    uds_image_policy_clear();
}

ZTEST_SUITE(image_policy_fail_closed, NULL, NULL, NULL, NULL, NULL);

/* --------------------------------------------------------------------------
 * TC-IPFC-001: production + no policy -> 0x34 refused.
 * -------------------------------------------------------------------------- */
ZTEST(image_policy_fail_closed, tc001_0x34_no_policy_refuses)
{
    const uds_safety_ctx_t *sctx;

    zassert_is_null(uds_image_policy_get(), "precondition: no policy registered");
    zassert_equal(0U, platform_violations(), "counters must start clean");

    build_0x34_req(MOCK_FLASH_BASE, (uint32_t)IMAGE_SIZE);

    zassert_equal(UDS_STATUS_ERR_IMAGE_POLICY_ABSENT,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp),
                  "production build: RequestDownload with no image policy must be "
                  "refused with NRC 0x22, not accepted");

    zassert_equal(0U, s_erase_call_count,
                  "a refused RequestDownload must not erase the target region");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                  "no transfer may be opened by a refused RequestDownload");

    sctx = uds_safety_get_ctx();
    zassert_not_null(sctx, "");
    zassert_equal(1U, sctx->platform_violations,
                  "exactly one platform violation must be recorded");
    zassert_equal(UDS_STATUS_ERR_IMAGE_POLICY_ABSENT, sctx->last_violation_code,
                  "the violation must carry the distinguishing image-policy code");
}

/* --------------------------------------------------------------------------
 * TC-IPFC-002: production + policy registered -> 0x34 accepted.
 * -------------------------------------------------------------------------- */
ZTEST(image_policy_fail_closed, tc002_0x34_with_policy_accepted)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_accept_policy), "");

    build_0x34_req(MOCK_FLASH_BASE, (uint32_t)IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp),
                  "the gate must refuse a missing policy, not production builds");
    zassert_equal(1U, s_erase_call_count, "");
    zassert_equal(UDS_TRANSFER_ACTIVE, uds_transfer_ctx_get()->state, "");
    zassert_equal(0U, platform_violations(), "an armed policy records no violation");
}

/* --------------------------------------------------------------------------
 * TC-IPFC-003: production + no policy at transfer exit -> 0x37 refused
 * (REQ-IMGPOL-002, defence in depth).
 * -------------------------------------------------------------------------- */
ZTEST(image_policy_fail_closed, tc003_0x37_no_policy_refuses)
{
    uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();

    /* Hand-prime a completed transfer: this reproduces the pre-#232 state in
     * which a one-byte 0x37 was answered [0x77] with no image evidence. */
    uds_transfer_ctx_reset(tctx);
    tctx->state                   = UDS_TRANSFER_ACTIVE;
    tctx->direction               = UDS_TRANSFER_DIR_DOWNLOAD;
    tctx->target_address          = MOCK_FLASH_BASE;
    tctx->total_size_bytes        = (uint32_t)IMAGE_SIZE;
    tctx->bytes_remaining         = 0U;
    tctx->next_write_address      = MOCK_FLASH_BASE + (uint32_t)IMAGE_SIZE;
    tctx->next_expected_block_seq = 0x02U;
    tctx->crc_accumulator         = 0xFFFFFFFFUL;
    tctx->write_buf_fill          = 0U;
    tctx->write_buf_capacity      = (uint16_t)MOCK_BLOCK_LEN;

    s_req.data[0] = 0x37U;
    s_req.length  = 1U;

    zassert_equal(UDS_STATUS_ERR_IMAGE_POLICY_ABSENT,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "production build: transfer exit with no image policy must be "
                  "refused with NRC 0x22, not answered [0x77]");

    zassert_not_equal(0x77U, s_resp.data[0],
                      "no positive response may be built");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                  "the transfer context must be reset (REQ-DL-003)");
    zassert_equal(1U, platform_violations(), "");
    zassert_equal(UDS_STATUS_ERR_IMAGE_POLICY_ABSENT,
                  uds_safety_get_ctx()->last_violation_code, "");
}

/* --------------------------------------------------------------------------
 * TC-IPFC-004: the window the defence-in-depth check exists to close — a
 * transfer that started while a policy was armed and reaches exit after it
 * was taken away.
 * -------------------------------------------------------------------------- */
ZTEST(image_policy_fail_closed, tc004_policy_lost_mid_transfer_still_refuses)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_accept_policy), "");

    build_0x34_req(MOCK_FLASH_BASE, (uint32_t)IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp),
                  "the 0x34 gate passes while the policy is armed");

    build_0x36_req(0x01U, (uint16_t)IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    /* The policy goes away between TransferData and TransferExit. */
    uds_image_policy_clear();

    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_ERR_IMAGE_POLICY_ABSENT,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "the 0x34 gate alone is not enough; transfer exit must re-check");
    zassert_not_equal(0x77U, s_resp.data[0], "");
    zassert_equal(0U, s_finalise_calls, "no policy means no finalise_cb ran");
}

/* --------------------------------------------------------------------------
 * TC-IPFC-005: fail-closed must not break the good production path.
 * -------------------------------------------------------------------------- */
ZTEST(image_policy_fail_closed, tc005_full_round_trip_with_policy)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_accept_policy), "");

    build_0x34_req(MOCK_FLASH_BASE, (uint32_t)IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");

    build_0x36_req(0x01U, (uint16_t)IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "a production build with a policy must still complete a DFU");

    zassert_equal(1U, s_finalise_calls, "");
    zassert_equal(0x77U, s_resp.data[0], "");
    zassert_equal(1U, s_resp.length, "");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state, "");
    zassert_equal(0U, platform_violations(), "the good path records no violation");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(image_policy_fail_closed__tc001_0x34_no_policy_refuses);
    RUN_TEST(image_policy_fail_closed__tc002_0x34_with_policy_accepted);
    RUN_TEST(image_policy_fail_closed__tc003_0x37_no_policy_refuses);
    RUN_TEST(image_policy_fail_closed__tc004_policy_lost_mid_transfer_still_refuses);
    RUN_TEST(image_policy_fail_closed__tc005_full_round_trip_with_policy);
}
