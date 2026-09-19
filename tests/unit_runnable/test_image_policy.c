// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — Unit Tests
 * FILE: tests/unit_runnable/test_image_policy.c
 *
 * MODULE UNDER TEST: platform/uds_image_policy.c (registration singleton) and
 *                    the policy call sites in core/uds_services/service_0x34.c,
 *                    service_0x36.c and service_0x37.c.
 *
 * [#232 Phase 1] [#279]
 *
 * PURPOSE:
 *   Prove the DEVELOPMENT/CI half of the issue #232 fail-closed DFU image
 *   policy gate, plus every behaviour of the gate that is build-mode
 *   independent.  The PRODUCTION half — the two fail-closed refusals that
 *   only exist when EDS_BUILD_IS_PRODUCTION != 0 — cannot be reached from
 *   this configuration and lives in its own separately-compiled binary,
 *   tests/unit_runnable/test_image_policy_fail_closed.c.
 *
 * TEST CASES:
 *   Registration (platform/uds_image_policy.c)
 *     TC-IMGPOL-001  register(NULL) -> ERR_NULL_PTR, and does NOT
 *                    de-register an already-registered policy.
 *     TC-IMGPOL-002  NULL begin_cb / update_cb / finalise_cb each rejected
 *                    with ERR_INVALID_PARAM, and none of them is installed.
 *     TC-IMGPOL-003  NULL commit_cb + NULL abort_cb accepted (documented
 *                    optional hooks); get() returns the registered table.
 *     TC-IMGPOL-004  clear() returns to the unregistered state.
 *
 *   0x34 RequestDownload
 *     TC-IMGPOL-010  Dev/CI, no policy: download still accepted (pre-#232
 *                    behaviour preserved), and the absence is recorded as a
 *                    platform safety violation with the distinguishing code
 *                    UDS_STATUS_ERR_IMAGE_POLICY_ABSENT.
 *     TC-IMGPOL-011  Policy registered: begin_cb receives the validated
 *                    address, size and dataFormatIdentifier, and runs
 *                    BEFORE erase_cb.
 *     TC-IMGPOL-012  begin_cb refusal -> ERR_UPLOAD_DOWNLOAD_NOT_ACCEPTED
 *                    (NRC 0x70) and the flash region is NOT erased.
 *     TC-IMGPOL-013  [#279] crc_check_requested binding: true for a plain
 *                    policy, false for a REQUIRE_PARAM_RECORD policy, false
 *                    with no policy registered.
 *
 *   0x36 TransferData
 *     TC-IMGPOL-020  update_cb sees contiguous, non-overlapping, correctly
 *                    offset ranges covering [0, size) exactly once across a
 *                    4-block transfer with an uneven final block.
 *     TC-IMGPOL-021  update_cb failure aborts: ERR_TRANSFER_ABORTED (NRC
 *                    0x72), transfer context back to IDLE, abort_cb fired.
 *
 *   0x37 RequestTransferExit
 *     TC-IMGPOL-030  finalise_cb receives correct evidence; ACCEPT proceeds
 *                    to [0x77].
 *     TC-IMGPOL-031  Verdict -> NRC mapping, all five rejections, each
 *                    resetting the transfer and recording a violation.
 *     TC-IMGPOL-032  finalise_cb returning non-OK (no verdict reached) is
 *                    treated as a rejection -> NRC 0x72.
 *     TC-IMGPOL-033  finalise_cb returning OK without writing *out_verdict
 *                    fails closed (the fail-closed initial value).
 *     TC-IMGPOL-034  commit_cb fires exactly once on ACCEPT, after
 *                    finalise_cb; failure -> NRC 0x72 and no [0x77].
 *     TC-IMGPOL-035  ACCEPT with commit_cb == NULL still returns [0x77].
 *     TC-IMGPOL-036  Dev/CI, no policy: bare [0x37] still returns [0x77]
 *                    (pre-#232 behaviour preserved).
 *
 *   #279 / parameter-record ownership
 *     TC-IMGPOL-040  Plain policy + bare [0x37] -> ERR_INVALID_PARAM (NRC
 *                    0x13); the transfer stays ACTIVE and a resent
 *                    [0x37, CRC32] completes it.
 *     TC-IMGPOL-041  No policy + bare [0x37] -> accepted (today's exact
 *                    optional-CRC behaviour, unchanged).
 *     TC-IMGPOL-042  REQUIRE_PARAM_RECORD: a 10-byte record is accepted and
 *                    handed to finalise_cb verbatim.
 *     TC-IMGPOL-043  REQUIRE_PARAM_RECORD: a bare [0x37] is refused (NRC
 *                    0x13) because the record is required.
 *     TC-IMGPOL-044  REQUIRE_PARAM_RECORD: a 4-byte record is NOT
 *                    reinterpreted as a CRC — it reaches finalise_cb as the
 *                    parameter record even though its bytes do not match
 *                    the accumulated CRC-32.
 *     TC-IMGPOL-045  No policy: the strict length rule is bit-for-bit
 *                    unchanged — 1 and 5 accepted, 2/3/4/6/10 refused.
 *
 *   Abort notification
 *     TC-IMGPOL-050  abort_cb fires on the 0x37 incomplete-transfer abort
 *                    and on a 0x34 that pre-empts an in-progress download.
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
#include "uds_image_policy.h"

/* =============================================================================
 * Compile-time proof this TU is built in the DEVELOPMENT configuration.
 *
 * Every "pre-#232 behaviour preserved" assertion below would also pass in a
 * production build for the wrong reason (the request would be refused before
 * reaching the behaviour under test), so a build-flag pipeline that lost the
 * dev configuration must fail loudly rather than silently.
 * ============================================================================= */
#include "uds_security_algo.h"
#if EDS_BUILD_IS_PRODUCTION
#  error "[#232] test_image_policy.c must be built in the DEVELOPMENT " \
         "configuration (EDS_BUILD_IS_PRODUCTION == 0). The production " \
         "fail-closed gates are proven by test_image_policy_fail_closed.c."
#endif

/* ==========================================================================
 * Mock flash constants
 * ========================================================================== */

#define MOCK_FLASH_BASE   (0x08020000UL)
#define MOCK_FLASH_SIZE   (0x2000UL)
#define MOCK_BLOCK_LEN    (256U)

/** Image size used by the multi-block transfer tests: 3 full 256-byte
 *  blocks plus a 100-byte remainder, so the final range is uneven. */
#define MULTI_IMAGE_SIZE  (868U)

/* ==========================================================================
 * Mock flash state
 * ========================================================================== */

static bool     s_erase_fail       = false;
static uint32_t s_erase_call_count = 0U;
static uint32_t s_write_call_count = 0U;
static bool     s_verify_fail      = false;

static uds_status_t mock_erase(uint32_t address, uint32_t size_bytes)
{
    (void)address; (void)size_bytes;
    s_erase_call_count++;
    if (s_erase_fail) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    return UDS_STATUS_OK;
}

static uds_status_t mock_write(uint32_t address, const uint8_t *data, uint32_t length)
{
    (void)address; (void)data; (void)length;
    s_write_call_count++;
    return UDS_STATUS_OK;
}

static uds_status_t mock_verify(uint32_t address, uint32_t size_bytes, uint32_t expected_crc)
{
    (void)address; (void)size_bytes; (void)expected_crc;
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

/* [#232 review fix] read_cb + an ops table that supplies it, so this file
 * can also drive SID 0x35 RequestUpload — needed to prove the image policy
 * gate stays out of the upload path, which is what the finding actually
 * was. */
static uds_status_t mock_read(uint32_t address, uint8_t *data, uint32_t length)
{
    uint32_t i;

    (void)address;
    for (i = 0U; i < length; i++) {
        data[i] = (uint8_t)0xA5U;
    }
    return UDS_STATUS_OK;
}

static const uds_flash_ops_t k_mock_ops_with_read = {
    .erase_cb         = mock_erase,
    .write_cb         = mock_write,
    .verify_cb        = mock_verify,
    .read_cb          = mock_read,
    .memory_map       = k_mock_region,
    .region_count     = (uint8_t)1U,
    .max_block_length = (uint16_t)MOCK_BLOCK_LEN,
};

/* ==========================================================================
 * Mock image policy state
 * ========================================================================== */

/** Maximum update_cb invocations recorded by the spy. */
#define MAX_UPDATE_RANGES  (16U)

static uint32_t s_begin_calls;
static uint32_t s_update_calls;
static uint32_t s_finalise_calls;
static uint32_t s_commit_calls;
static uint32_t s_abort_calls;

static uds_image_transfer_info_t s_begin_info;
static uds_status_t              s_begin_rc;

/* Recorded update_cb ranges. */
static uint32_t s_update_offset[MAX_UPDATE_RANGES];
static uint32_t s_update_length[MAX_UPDATE_RANGES];
static uint8_t  s_update_first_byte[MAX_UPDATE_RANGES];
/** update_cb call index (1-based) that must fail, or 0 for "never fail". */
static uint32_t s_update_fail_on_call;

static uds_image_evidence_t s_finalise_evidence;
static uint8_t              s_finalise_record_copy[64U];
static uds_status_t         s_finalise_rc;
static uds_image_verdict_t  s_finalise_verdict;
/** When true, finalise_cb returns OK but never writes *out_verdict. */
static bool                 s_finalise_skip_verdict;
/** Value of s_commit_calls observed by finalise_cb, to prove ordering. */
static uint32_t             s_commit_calls_seen_by_finalise;

static uds_status_t         s_commit_rc;

static uds_status_t spy_begin(const uds_image_transfer_info_t *info)
{
    s_begin_calls++;
    s_begin_info = *info;
    return s_begin_rc;
}

static uds_status_t spy_update(uint32_t image_offset, const uint8_t *data, uint32_t length)
{
    if (s_update_calls < (uint32_t)MAX_UPDATE_RANGES) {
        s_update_offset[s_update_calls]     = image_offset;
        s_update_length[s_update_calls]     = length;
        s_update_first_byte[s_update_calls] = (length > 0U) ? data[0] : 0xFFU;
    }
    s_update_calls++;

    if ((s_update_fail_on_call != 0U) && (s_update_calls == s_update_fail_on_call)) {
        return UDS_STATUS_ERR_GENERIC;
    }
    return UDS_STATUS_OK;
}

static uds_status_t spy_finalise(const uds_image_evidence_t *evidence,
                                  uds_image_verdict_t        *out_verdict)
{
    s_finalise_calls++;
    s_finalise_evidence             = *evidence;
    s_commit_calls_seen_by_finalise = s_commit_calls;

    (void)memset(s_finalise_record_copy, 0, sizeof(s_finalise_record_copy));
    if ((evidence->param_record != NULL) &&
        (evidence->param_record_len <= (uint16_t)sizeof(s_finalise_record_copy))) {
        (void)memcpy(s_finalise_record_copy,
                     evidence->param_record,
                     (size_t)evidence->param_record_len);
    }

    if (!s_finalise_skip_verdict) {
        *out_verdict = s_finalise_verdict;
    }
    return s_finalise_rc;
}

static uds_status_t spy_commit(void)
{
    s_commit_calls++;
    return s_commit_rc;
}

static void spy_abort(void)
{
    s_abort_calls++;
}

/** Full policy: every hook populated, no policy_flags set. */
static const uds_image_policy_t k_policy_full = {
    .begin_cb     = spy_begin,
    .update_cb    = spy_update,
    .finalise_cb  = spy_finalise,
    .commit_cb    = spy_commit,
    .abort_cb     = spy_abort,
    .policy_flags = 0U,
};

/** Minimal policy: only the three mandatory hooks. */
static const uds_image_policy_t k_policy_minimal = {
    .begin_cb     = spy_begin,
    .update_cb    = spy_update,
    .finalise_cb  = spy_finalise,
    .commit_cb    = NULL,
    .abort_cb     = NULL,
    .policy_flags = 0U,
};

/** Policy that owns the 0x37 transferRequestParameterRecord. */
static const uds_image_policy_t k_policy_param_record = {
    .begin_cb     = spy_begin,
    .update_cb    = spy_update,
    .finalise_cb  = spy_finalise,
    .commit_cb    = spy_commit,
    .abort_cb     = spy_abort,
    .policy_flags = (uint8_t)UDS_IMAGE_POLICY_REQUIRE_PARAM_RECORD,
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
 * Request builders
 * ========================================================================== */

static void build_0x34_req(uint32_t mem_address, uint32_t mem_size)
{
    s_req.data[0] = 0x34U;
    s_req.data[1] = 0x00U;                       /* dataFormatIdentifier */
    s_req.data[2] = 0x44U;                       /* ALFID: 4 addr, 4 size */

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

/**
 * @brief [#232 review fix] Build a well-formed [0x35] RequestUpload —
 * byte-identical to build_0x34_req() apart from the SID, matching
 * tests/unit_runnable/test_service_0x35.c's own request shape.
 */
static void build_0x35_req(uint32_t mem_address, uint32_t mem_size)
{
    build_0x34_req(mem_address, mem_size);
    s_req.data[0] = 0x35U;
}

/**
 * @brief Build [0x36, blockSeq, payload...] where payload[i] = first_byte + i.
 */
static void build_0x36_req(uint8_t block_seq, uint8_t first_byte, uint16_t payload_len)
{
    uint16_t i;

    s_req.data[0] = 0x36U;
    s_req.data[1] = block_seq;
    for (i = 0U; i < payload_len; i++) {
        s_req.data[2U + i] = (uint8_t)(first_byte + (uint8_t)i);
    }
    s_req.length = (uint16_t)(2U + payload_len);
}

static void build_0x37_bare(void)
{
    s_req.data[0] = 0x37U;
    s_req.length  = 1U;
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

static void build_0x37_with_record(const uint8_t *record, uint16_t record_len)
{
    s_req.data[0] = 0x37U;
    (void)memcpy(&s_req.data[1], record, (size_t)record_len);
    s_req.length  = (uint16_t)(1U + record_len);
}

/* ==========================================================================
 * Helpers
 * ========================================================================== */

/** Prime a transfer context that has received every declared byte.
 *
 *  The CRC accumulator is fed a fixed non-empty pattern so that
 *  current_crc() is not 0x00000000 — a zero CRC would let an all-zero
 *  parameter record accidentally match it, which would silently weaken
 *  TC-IMGPOL-044. */
static void prime_completed_transfer(uint32_t size_bytes)
{
    static const uint8_t k_seed_bytes[8] = {
        0x5AU, 0xA5U, 0x01U, 0x02U, 0xFEU, 0xEDU, 0x7CU, 0x13U
    };
    uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();

    uds_transfer_ctx_reset(tctx);
    tctx->state                   = UDS_TRANSFER_ACTIVE;
    tctx->direction               = UDS_TRANSFER_DIR_DOWNLOAD;
    tctx->target_address          = MOCK_FLASH_BASE;
    tctx->total_size_bytes        = size_bytes;
    tctx->bytes_remaining         = 0U;
    tctx->next_write_address      = MOCK_FLASH_BASE + size_bytes;
    tctx->next_expected_block_seq = 0x02U;
    tctx->crc_accumulator         = uds_transfer_crc32_update(
                                        0xFFFFFFFFUL,
                                        k_seed_bytes,
                                        (uint32_t)sizeof(k_seed_bytes));
    tctx->write_buf_fill          = 0U;
    tctx->write_buf_capacity      = (uint16_t)MOCK_BLOCK_LEN;
}

/** Finalised CRC-32 of whatever the transfer context has accumulated. */
static uint32_t current_crc(void)
{
    return uds_transfer_crc32_finalise(uds_transfer_ctx_get()->crc_accumulator);
}

static uint32_t platform_violations(void)
{
    const uds_safety_ctx_t *sctx = uds_safety_get_ctx();
    return (sctx != NULL) ? sctx->platform_violations : 0xFFFFFFFFUL;
}

/**
 * @brief Drive a complete MULTI_IMAGE_SIZE download through 0x34 + four 0x36
 *        blocks, leaving the transfer ready for 0x37.
 *
 * Block payload sizes: 256, 256, 256, 100 (MULTI_IMAGE_SIZE == 868).
 */
static void run_multiblock_download(void)
{
    static const uint16_t k_block_len[4] = { 256U, 256U, 256U, 100U };
    uint8_t  seq;
    uint32_t sent = 0U;
    uint32_t i;

    build_0x34_req(MOCK_FLASH_BASE, (uint32_t)MULTI_IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp),
                  "0x34 must be accepted");

    for (i = 0U; i < 4U; i++) {
        seq = (uint8_t)(i + 1U);
        build_0x36_req(seq, (uint8_t)(0x10U * (i + 1U)), k_block_len[i]);
        zassert_equal(UDS_STATUS_OK,
                      uds_service_0x36_handler(&s_srv, &s_req, &s_resp),
                      "0x36 block must be accepted");
        sent += (uint32_t)k_block_len[i];
    }

    zassert_equal((uint32_t)MULTI_IMAGE_SIZE, sent, "helper must send the whole image");
    zassert_equal(0U, uds_transfer_ctx_get()->bytes_remaining,
                  "whole image must have been transferred");
}

/* ==========================================================================
 * setUp / tearDown
 * ========================================================================== */

void setUp(void)
{
    (void)memset(&s_sess, 0, sizeof(s_sess));
    (void)memset(&s_sec,  0, sizeof(s_sec));
    (void)memset(&s_srv,  0, sizeof(s_srv));
    (void)memset(&s_req,  0, sizeof(s_req));
    (void)memset(&s_resp, 0, sizeof(s_resp));

    s_erase_fail       = false;
    s_erase_call_count = 0U;
    s_write_call_count = 0U;
    s_verify_fail      = false;

    s_begin_calls    = 0U;
    s_update_calls   = 0U;
    s_finalise_calls = 0U;
    s_commit_calls   = 0U;
    s_abort_calls    = 0U;

    (void)memset(&s_begin_info, 0, sizeof(s_begin_info));
    s_begin_rc = UDS_STATUS_OK;

    (void)memset(s_update_offset,     0, sizeof(s_update_offset));
    (void)memset(s_update_length,     0, sizeof(s_update_length));
    (void)memset(s_update_first_byte, 0, sizeof(s_update_first_byte));
    s_update_fail_on_call = 0U;

    (void)memset(&s_finalise_evidence, 0, sizeof(s_finalise_evidence));
    (void)memset(s_finalise_record_copy, 0, sizeof(s_finalise_record_copy));
    s_finalise_rc                   = UDS_STATUS_OK;
    s_finalise_verdict              = UDS_IMAGE_ACCEPT;
    s_finalise_skip_verdict         = false;
    s_commit_calls_seen_by_finalise = 0xFFFFFFFFUL;

    s_commit_rc = UDS_STATUS_OK;

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

/* ==========================================================================
 * Test suite
 * ========================================================================== */

ZTEST_SUITE(image_policy, NULL, NULL, NULL, NULL, NULL);

/* --------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------- */

/* TC-IMGPOL-001 */
ZTEST(image_policy, test_register_null_rejected)
{
    zassert_equal(UDS_STATUS_ERR_NULL_PTR,
                  uds_image_policy_register(NULL),
                  "register(NULL) must be refused, not treated as de-registration");
    zassert_is_null(uds_image_policy_get(), "no policy may be installed by a failed call");

    /* REQ-IMGPOL-004: a NULL must not silently disarm a live registration. */
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    zassert_equal(UDS_STATUS_ERR_NULL_PTR, uds_image_policy_register(NULL), "");
    zassert_true(uds_image_policy_get() == &k_policy_full,
                 "a refused register(NULL) must leave the existing policy armed");
}

/* TC-IMGPOL-002 */
ZTEST(image_policy, test_register_missing_required_cb_rejected)
{
    uds_image_policy_t bad;

    bad = k_policy_full;
    bad.begin_cb = NULL;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM, uds_image_policy_register(&bad),
                  "begin_cb is mandatory");
    zassert_is_null(uds_image_policy_get(), "");

    bad = k_policy_full;
    bad.update_cb = NULL;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM, uds_image_policy_register(&bad),
                  "update_cb is mandatory");
    zassert_is_null(uds_image_policy_get(), "");

    bad = k_policy_full;
    bad.finalise_cb = NULL;
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM, uds_image_policy_register(&bad),
                  "finalise_cb is mandatory");
    zassert_is_null(uds_image_policy_get(), "");
}

/* TC-IMGPOL-003 */
ZTEST(image_policy, test_register_optional_cbs_may_be_null)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_minimal),
                  "commit_cb and abort_cb are documented as optional");
    zassert_true(uds_image_policy_get() == &k_policy_minimal, "");
}

/* TC-IMGPOL-004 */
ZTEST(image_policy, test_clear_returns_to_unregistered)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    zassert_not_null(uds_image_policy_get(), "");
    uds_image_policy_clear();
    zassert_is_null(uds_image_policy_get(), "");
}

/* --------------------------------------------------------------------------
 * 0x34 RequestDownload
 * -------------------------------------------------------------------------- */

/* TC-IMGPOL-010 */
ZTEST(image_policy, test_0x34_dev_build_permits_missing_policy)
{
    const uds_safety_ctx_t *sctx;

    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp),
                  "dev/CI build must still accept a download with no policy");
    zassert_equal(1U, s_erase_call_count, "erase must still have run");
    zassert_equal(UDS_TRANSFER_ACTIVE, uds_transfer_ctx_get()->state, "");

    sctx = uds_safety_get_ctx();
    zassert_not_null(sctx, "");
    zassert_equal(1U, sctx->platform_violations,
                  "the missing policy must still be recorded in dev/CI");
    zassert_equal(UDS_STATUS_ERR_IMAGE_POLICY_ABSENT, sctx->last_violation_code,
                  "the recorded code must distinguish 'no image policy' from "
                  "every other conditions-not-correct case");
}

/* TC-IMGPOL-011 */
ZTEST(image_policy, test_0x34_begin_cb_receives_transfer_info)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");

    build_0x34_req(MOCK_FLASH_BASE + 0x400UL, 0x123U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");

    zassert_equal(1U, s_begin_calls, "begin_cb must be called exactly once");
    zassert_equal(MOCK_FLASH_BASE + 0x400UL, s_begin_info.target_address, "");
    zassert_equal(0x123U, s_begin_info.total_size_bytes, "");
    zassert_equal(0x00U, s_begin_info.data_format_id,
                  "dataFormatIdentifier must be echoed to the policy");
    zassert_equal(1U, s_erase_call_count, "erase must run after begin_cb accepted");
    zassert_equal(0U, platform_violations(),
                  "an armed policy must record no violation");
}

/* TC-IMGPOL-012 */
ZTEST(image_policy, test_0x34_begin_cb_refusal_maps_to_nrc_0x70)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    s_begin_rc = UDS_STATUS_ERR_GENERIC;

    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_ERR_UPLOAD_DOWNLOAD_NOT_ACCEPTED,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp),
                  "begin_cb refusal must map to NRC 0x70 uploadDownloadNotAccepted");
    zassert_equal(1U, s_begin_calls, "");
    zassert_equal(0U, s_erase_call_count,
                  "a refusal must leave the target region un-erased");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                  "no transfer may be opened by a refused 0x34");
}

/* TC-IMGPOL-013  [#279] */
ZTEST(image_policy, test_0x34_crc_check_requested_binding)
{
    const uds_transfer_ctx_t *tctx = uds_transfer_ctx_get();

    /* (a) No policy -> the field stays false (pre-#232 behaviour). */
    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    zassert_false(tctx->crc_check_requested,
                  "no policy must not make the tester CRC mandatory");

    /* (b) Plain policy -> the tester CRC becomes mandatory. */
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    zassert_true(tctx->crc_check_requested,
                  "a registered policy must make the tester CRC mandatory (#279)");

    /* (c) REQUIRE_PARAM_RECORD policy -> the record belongs to the policy,
     *     so the generic CRC check stands down. */
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_param_record), "");
    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    zassert_false(tctx->crc_check_requested,
                  "a policy that owns the parameter record must not also demand a CRC");
}

/* --------------------------------------------------------------------------
 * 0x36 TransferData
 * -------------------------------------------------------------------------- */

/* TC-IMGPOL-020 */
ZTEST(image_policy, test_0x36_update_cb_ranges_are_contiguous_and_offset_correctly)
{
    uint32_t i;
    uint32_t expected_offset;
    static const uint32_t k_expected_len[4]   = { 256U, 256U, 256U, 100U };
    static const uint8_t  k_expected_first[4] = { 0x10U, 0x20U, 0x30U, 0x40U };

    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");

    run_multiblock_download();

    zassert_equal(4U, s_update_calls,
                  "update_cb must be called once per accepted 0x36 block");

    expected_offset = 0U;
    for (i = 0U; i < 4U; i++) {
        zassert_equal(expected_offset, s_update_offset[i],
                      "range must start at the offset the chunk STARTS at, "
                      "not the one it ends at");
        zassert_equal(k_expected_len[i], s_update_length[i], "");
        zassert_equal(k_expected_first[i], s_update_first_byte[i],
                      "the bytes handed to update_cb must be this block's payload");
        expected_offset += k_expected_len[i];
    }

    zassert_equal((uint32_t)MULTI_IMAGE_SIZE, expected_offset,
                  "the ranges together must cover [0, total_size_bytes) exactly once");
}

/* TC-IMGPOL-021 */
ZTEST(image_policy, test_0x36_update_cb_failure_aborts_transfer)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    s_update_fail_on_call = 2U;   /* fail on the SECOND block */

    build_0x34_req(MOCK_FLASH_BASE, (uint32_t)MULTI_IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");

    build_0x36_req(0x01U, 0x10U, 256U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    build_0x36_req(0x02U, 0x20U, 256U);
    zassert_equal(UDS_STATUS_ERR_TRANSFER_ABORTED,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp),
                  "an update_cb refusal must abort with NRC 0x72");

    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                  "the transfer context must be reset (REQ-DL-003)");
    zassert_equal(1U, s_abort_calls,
                  "abort_cb must fire so the policy can drop partial state");
}

/* --------------------------------------------------------------------------
 * 0x37 RequestTransferExit
 * -------------------------------------------------------------------------- */

/* TC-IMGPOL-030 */
ZTEST(image_policy, test_0x37_finalise_receives_evidence_and_accept_proceeds)
{
    uint32_t crc;

    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    run_multiblock_download();

    crc = current_crc();
    build_0x37_with_crc(crc);

    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "ACCEPT must complete the transfer");

    zassert_equal(1U, s_finalise_calls, "finalise_cb must be called exactly once");
    zassert_equal(MOCK_FLASH_BASE, s_finalise_evidence.target_address, "");
    zassert_equal((uint32_t)MULTI_IMAGE_SIZE, s_finalise_evidence.total_size_bytes, "");
    zassert_equal(crc, s_finalise_evidence.crc32,
                  "evidence must carry the finalised streamed CRC-32");
    zassert_equal(4U, s_finalise_evidence.param_record_len,
                  "the 4-byte CRC record is still visible as the parameter record");

    zassert_equal(0x77U, s_resp.data[0], "positive response must be [0x77]");
    zassert_equal(1U, s_resp.length, "");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state, "");
    zassert_equal(0U, s_abort_calls, "no abort on the happy path");
}

/* TC-IMGPOL-031 */
ZTEST(image_policy, test_0x37_verdict_to_nrc_mapping)
{
    static const uds_image_verdict_t k_verdict[5] = {
        UDS_IMAGE_REJECT_FORMAT,
        UDS_IMAGE_REJECT_DIGEST,
        UDS_IMAGE_REJECT_SIGNATURE,
        UDS_IMAGE_REJECT_ROLLBACK,
        UDS_IMAGE_REJECT_POLICY,
    };
    static const uds_status_t k_expected[5] = {
        UDS_STATUS_ERR_TRANSFER_ABORTED,          /* NRC 0x72 */
        UDS_STATUS_ERR_TRANSFER_ABORTED,          /* NRC 0x72 */
        UDS_STATUS_ERR_TRANSFER_ABORTED,          /* NRC 0x72 */
        UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,      /* NRC 0x31 */
        UDS_STATUS_ERR_IMAGE_POLICY_REJECTED,     /* NRC 0x22 */
    };
    uint32_t i;

    for (i = 0U; i < 5U; i++) {
        setUp();
        zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");

        prime_completed_transfer(0x100U);
        uds_transfer_ctx_get()->crc_check_requested = true;
        s_finalise_verdict = k_verdict[i];

        build_0x37_with_crc(current_crc());
        zassert_equal(k_expected[i],
                      uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                      "verdict must map to the documented NRC");

        zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state,
                      "a rejection must reset the transfer context");
        zassert_equal(1U, s_abort_calls, "a rejection must notify abort_cb");
        zassert_equal(0U, s_commit_calls, "commit_cb must not run on a rejection");
        zassert_equal(1U, platform_violations(),
                      "a rejection must increment the safety violation counter");
        zassert_equal(k_expected[i], uds_safety_get_ctx()->last_violation_code, "");
        zassert_not_equal(0x77U, s_resp.data[0],
                          "no positive response may be built for a rejection");
    }
}

/* TC-IMGPOL-032 */
ZTEST(image_policy, test_0x37_finalise_non_ok_return_is_a_rejection)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    prime_completed_transfer(0x100U);
    s_finalise_rc      = UDS_STATUS_ERR_PLATFORM;
    s_finalise_verdict = UDS_IMAGE_ACCEPT;   /* must be ignored */

    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_ERR_TRANSFER_ABORTED,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "a finalise_cb that reached no verdict must fail closed (NRC 0x72)");
    zassert_equal(0U, s_commit_calls, "");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state, "");
}

/* TC-IMGPOL-033 */
ZTEST(image_policy, test_0x37_finalise_leaving_verdict_unwritten_fails_closed)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    prime_completed_transfer(0x100U);
    s_finalise_skip_verdict = true;   /* returns OK, never writes *out_verdict */

    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_ERR_IMAGE_POLICY_REJECTED,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "an unwritten verdict must default to a rejection, never ACCEPT");
    zassert_equal(0U, s_commit_calls, "");
}

/* TC-IMGPOL-034 */
ZTEST(image_policy, test_0x37_commit_cb_wiring)
{
    /* (a) commit_cb fires exactly once, and only after finalise_cb. */
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    prime_completed_transfer(0x100U);
    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_OK, uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(1U, s_commit_calls, "commit_cb must fire exactly once on ACCEPT");
    zassert_equal(0U, s_commit_calls_seen_by_finalise,
                  "commit_cb must run AFTER finalise_cb, never before");
    zassert_equal(0x77U, s_resp.data[0], "");

    /* (b) commit_cb failure -> NRC 0x72, no positive response. */
    setUp();
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    prime_completed_transfer(0x100U);
    s_commit_rc = UDS_STATUS_ERR_PLATFORM;
    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_ERR_TRANSFER_ABORTED,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "a verified image that cannot be armed must not be reported as done");
    zassert_equal(1U, s_commit_calls, "");
    zassert_equal(1U, s_abort_calls, "");
    zassert_not_equal(0x77U, s_resp.data[0], "");
    zassert_equal(UDS_TRANSFER_IDLE, uds_transfer_ctx_get()->state, "");
}

/* TC-IMGPOL-035 */
ZTEST(image_policy, test_0x37_accept_without_commit_cb_still_completes)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_minimal), "");
    prime_completed_transfer(0x100U);
    uds_transfer_ctx_get()->crc_check_requested = true;

    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "a policy with no commit_cb must still complete on ACCEPT");
    zassert_equal(0x77U, s_resp.data[0], "");
}

/* TC-IMGPOL-036 */
ZTEST(image_policy, test_0x37_dev_build_no_policy_unchanged)
{
    prime_completed_transfer(0x100U);
    build_0x37_bare();

    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "dev/CI with no policy must still answer [0x77] to a bare 0x37");
    zassert_equal(0x77U, s_resp.data[0], "");
    zassert_equal(1U, s_resp.length, "");
    zassert_equal(0U, s_finalise_calls, "no policy means no finalise_cb");
}

/* --------------------------------------------------------------------------
 * #279 / parameter-record ownership
 * -------------------------------------------------------------------------- */

/* TC-IMGPOL-040 */
ZTEST(image_policy, test_0x37_policy_makes_crc_mandatory)
{
    uint32_t crc;

    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    run_multiblock_download();
    zassert_true(uds_transfer_ctx_get()->crc_check_requested, "");

    crc = current_crc();

    build_0x37_bare();
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "#279: a policy-armed ECU must refuse a 0x37 that supplies no CRC");
    zassert_equal(0U, s_finalise_calls,
                  "finalise_cb must not run for a malformed request");
    zassert_equal(UDS_TRANSFER_ACTIVE, uds_transfer_ctx_get()->state,
                  "a wrong-shaped request leaves the transfer open for a retry");

    /* The tester resends a well-formed 0x37 and the transfer completes. */
    build_0x37_with_crc(crc);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(0x77U, s_resp.data[0], "");
}

/* TC-IMGPOL-041 */
ZTEST(image_policy, test_0x37_no_policy_crc_stays_optional)
{
    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    uds_transfer_ctx_get()->bytes_remaining = 0U;

    build_0x37_bare();
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "with no policy the tester CRC must stay optional, exactly as before");
}

/* TC-IMGPOL-042 */
ZTEST(image_policy, test_0x37_param_record_relaxation)
{
    static const uint8_t k_record[10] = {
        0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U
    };

    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_param_record), "");
    prime_completed_transfer(0x100U);

    build_0x37_with_record(k_record, 10U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "REQUIRE_PARAM_RECORD must relax the strict 0/4-byte length rule");

    zassert_equal(1U, s_finalise_calls, "");
    zassert_equal(10U, s_finalise_evidence.param_record_len, "");
    zassert_equal(0, memcmp(s_finalise_record_copy, k_record, sizeof(k_record)),
                  "the record must reach finalise_cb verbatim");
    zassert_equal(0x77U, s_resp.data[0], "");
}

/* TC-IMGPOL-043 */
ZTEST(image_policy, test_0x37_param_record_required_refuses_bare_request)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_param_record), "");
    prime_completed_transfer(0x100U);

    build_0x37_bare();
    zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "REQUIRE_PARAM_RECORD means the record is required");
    zassert_equal(0U, s_finalise_calls, "");
}

/* TC-IMGPOL-044 */
ZTEST(image_policy, test_0x37_param_record_4_bytes_is_not_a_crc)
{
    static const uint8_t k_record[4] = { 0x00U, 0x00U, 0x00U, 0x00U };
    uint32_t crc;

    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_param_record), "");
    prime_completed_transfer(0x100U);

    crc = current_crc();
    zassert_not_equal(0U, crc,
                      "the test needs a CRC that a zero record cannot match");

    build_0x37_with_record(k_record, 4U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "a 4-byte record must NOT be reinterpreted as a CRC when the "
                  "policy owns the parameter record");
    zassert_equal(4U, s_finalise_evidence.param_record_len, "");
    zassert_equal(0, memcmp(s_finalise_record_copy, k_record, sizeof(k_record)), "");
}

/* TC-IMGPOL-045 */
ZTEST(image_policy, test_0x37_no_policy_strict_lengths_unchanged)
{
    static const uint8_t k_pad[16] = { 0U };
    static const uint16_t k_bad_len[5] = { 1U, 2U, 3U, 5U, 9U }; /* record lengths */
    uint32_t i;

    /* Accepted: bare [0x37] (record length 0). */
    prime_completed_transfer(0x100U);
    build_0x37_bare();
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");

    /* Accepted: exactly 4 record bytes carrying the matching CRC. */
    prime_completed_transfer(0x100U);
    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp), "");

    /* Refused: every other record length, exactly as before #232. */
    for (i = 0U; i < 5U; i++) {
        prime_completed_transfer(0x100U);
        build_0x37_with_record(k_pad, k_bad_len[i]);
        zassert_equal(UDS_STATUS_ERR_INVALID_PARAM,
                      uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                      "with no policy registered only record lengths 0 and 4 are legal");
    }
}

/* --------------------------------------------------------------------------
 * Abort notification
 * -------------------------------------------------------------------------- */

/* TC-IMGPOL-050 */
ZTEST(image_policy, test_abort_cb_fires_on_abort_paths)
{
    /* (a) 0x37 on an incomplete transfer. */
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    build_0x34_req(MOCK_FLASH_BASE, (uint32_t)MULTI_IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    build_0x36_req(0x01U, 0x10U, 256U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    build_0x37_with_crc(current_crc());
    zassert_equal(UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "incomplete transfer must still be NRC 0x31");
    zassert_equal(1U, s_abort_calls, "abort_cb must fire when the transfer is torn down");

    /* (b) a 0x34 that pre-empts an in-progress download. */
    setUp();
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    build_0x34_req(MOCK_FLASH_BASE, (uint32_t)MULTI_IMAGE_SIZE);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    build_0x36_req(0x01U, 0x10U, 256U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(1U, s_abort_calls,
                  "the pre-empted transfer's partial state must be dropped");
    zassert_equal(2U, s_begin_calls, "");
}

/* --------------------------------------------------------------------------
 * [#232 review fixes] — findings from the code-review pass on the Phase 1
 * implementation, before it was ever pushed as a PR. Each of these three
 * was confirmed to FAIL against the pre-fix code before the corresponding
 * fix landed (see the PR description for the exact mutation and failure).
 * -------------------------------------------------------------------------- */

/* TC-IMGPOL-060 [review finding 1] */
ZTEST(image_policy, test_0x35_upload_never_reaches_image_policy)
{
    (void)uds_flash_ops_register(&k_mock_ops_with_read);

    /* REQUIRE_PARAM_RECORD is the sharpest version of this finding: if the
     * gate wrongly applied to uploads, finalise_cb would run with bogus
     * "evidence" built from an upload's read-out bytes, and the relaxed
     * param-record length parsing would apply to a service that has no
     * parameter record shape of its own to relax. */
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_param_record), "");

    build_0x35_req(MOCK_FLASH_BASE, 0x40U);
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x35_handler(&s_srv, &s_req, &s_resp),
                  "a normal upload must not be touched by the DFU image policy at all");
    zassert_equal((uint8_t)0x75U, s_resp.data[0],
                  "positive response to 0x35 is SID+0x40, not [0x77]");

    /* Drain the upload via 0x36 — upload's own request shape is
     * [0x36, blockSeq], no tester payload (service_0x36.c dispatches to
     * s_handle_upload_block() before ever reaching the image-policy hook,
     * so this leg is not part of what the review finding was about, but
     * REQ-DL-002's "transfer must be complete" check at 0x37 still applies
     * to either direction and the request must actually finish). 0x40
     * bytes fits in a single block (MOCK_BLOCK_LEN is far larger). */
    s_req.data[0] = 0x36U;
    s_req.data[1] = 0x01U;
    s_req.length  = 2U;
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(0U, uds_transfer_ctx_get()->bytes_remaining,
                  "the whole 64-byte upload must have been read in one block");

    /* The transfer exit for that upload — plain bare [0x37], the shape a
     * real upload-capable tester actually sends. Before the fix this was
     * misrouted through the download-only finalise/commit gate. */
    build_0x37_bare();
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "upload transfer exit must not be gated by the image policy");
    zassert_equal((uint8_t)0x77U, s_resp.data[0], "");
    zassert_equal(0U, s_begin_calls,   "begin_cb is a 0x34-only hook, never reached from 0x35");
    zassert_equal(0U, s_finalise_calls,
                  "finalise_cb must never run against upload (read-out) data");
    zassert_equal(0U, s_commit_calls,
                  "commit_cb must never fire from an upload — nothing was verified");
    zassert_equal(0U, s_abort_calls, "a clean upload exit is not an abort");
}

/* TC-IMGPOL-061 [review finding 2] */
ZTEST(image_policy, test_0x34_erase_failure_after_begin_accepted_notifies_abort)
{
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    s_erase_fail = true;

    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_ERR_PLATFORM,
                  uds_service_0x34_handler(&s_srv, &s_req, &s_resp),
                  "erase failure must still surface as NRC 0x70 (unchanged)");
    zassert_equal(1U, s_begin_calls, "begin_cb ran and accepted before erase was attempted");
    zassert_equal(1U, s_abort_calls,
                  "begin_cb's state must be dropped when erase fails afterwards — "
                  "otherwise it leaks until process restart");

    /* The ECU must still be usable afterwards: a following 0x34 opens
     * cleanly rather than being wedged by the previous failure. */
    s_erase_fail = false;
    build_0x34_req(MOCK_FLASH_BASE, 0x100U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    zassert_equal(2U, s_begin_calls, "");
}

/* TC-IMGPOL-062 [review finding 3] */
ZTEST(image_policy, test_0x37_crc_check_requested_recomputed_live_not_cached)
{
    uint8_t record[1] = { 0xAAU };

    /* Policy A (k_policy_full: no REQUIRE_PARAM_RECORD) is registered when
     * the download STARTS — this is what sets tctx->crc_check_requested,
     * the field #279 wired. */
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_full), "");
    build_0x34_req(MOCK_FLASH_BASE, 0x40U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x34_handler(&s_srv, &s_req, &s_resp), "");
    zassert_true(uds_transfer_ctx_get()->crc_check_requested,
                 "sanity: policy A's registration must set the #279 field");

    build_0x36_req(0x01U, 0x10U, 0x40U);
    zassert_equal(UDS_STATUS_OK, uds_service_0x36_handler(&s_srv, &s_req, &s_resp), "");

    /* Policy B replaces A before 0x37 — nothing in uds_image_policy_register()
     * forbids this (REQ-IMGPOL-004 only concerns NULL). B claims the
     * parameter record for itself, so a bare CRC-shaped 0x37 is no longer
     * the right request shape; a record is. */
    zassert_equal(UDS_STATUS_OK, uds_image_policy_register(&k_policy_param_record), "");

    build_0x37_with_record(record, (uint16_t)sizeof(record));
    zassert_equal(UDS_STATUS_OK,
                  uds_service_0x37_handler(&s_srv, &s_req, &s_resp),
                  "the CURRENTLY registered policy (B) does not require a CRC — "
                  "the stale field from A's registration at 0x34 must not override it");
    zassert_equal((uint8_t)0x77U, s_resp.data[0], "");
    zassert_equal(1U, s_finalise_calls, "policy B's finalise_cb must be the one that ran");
}

/* ==========================================================================
 * run_all_tests
 * ========================================================================== */

void run_all_tests(void)
{
    RUN_TEST(image_policy__test_register_null_rejected);
    RUN_TEST(image_policy__test_register_missing_required_cb_rejected);
    RUN_TEST(image_policy__test_register_optional_cbs_may_be_null);
    RUN_TEST(image_policy__test_clear_returns_to_unregistered);

    RUN_TEST(image_policy__test_0x34_dev_build_permits_missing_policy);
    RUN_TEST(image_policy__test_0x34_begin_cb_receives_transfer_info);
    RUN_TEST(image_policy__test_0x34_begin_cb_refusal_maps_to_nrc_0x70);
    RUN_TEST(image_policy__test_0x34_crc_check_requested_binding);

    RUN_TEST(image_policy__test_0x36_update_cb_ranges_are_contiguous_and_offset_correctly);
    RUN_TEST(image_policy__test_0x36_update_cb_failure_aborts_transfer);

    RUN_TEST(image_policy__test_0x37_finalise_receives_evidence_and_accept_proceeds);
    RUN_TEST(image_policy__test_0x37_verdict_to_nrc_mapping);
    RUN_TEST(image_policy__test_0x37_finalise_non_ok_return_is_a_rejection);
    RUN_TEST(image_policy__test_0x37_finalise_leaving_verdict_unwritten_fails_closed);
    RUN_TEST(image_policy__test_0x37_commit_cb_wiring);
    RUN_TEST(image_policy__test_0x37_accept_without_commit_cb_still_completes);
    RUN_TEST(image_policy__test_0x37_dev_build_no_policy_unchanged);

    RUN_TEST(image_policy__test_0x37_policy_makes_crc_mandatory);
    RUN_TEST(image_policy__test_0x37_no_policy_crc_stays_optional);
    RUN_TEST(image_policy__test_0x37_param_record_relaxation);
    RUN_TEST(image_policy__test_0x37_param_record_required_refuses_bare_request);
    RUN_TEST(image_policy__test_0x37_param_record_4_bytes_is_not_a_crc);
    RUN_TEST(image_policy__test_0x37_no_policy_strict_lengths_unchanged);

    RUN_TEST(image_policy__test_abort_cb_fires_on_abort_paths);

    RUN_TEST(image_policy__test_0x35_upload_never_reaches_image_policy);
    RUN_TEST(image_policy__test_0x34_erase_failure_after_begin_accepted_notifies_abort);
    RUN_TEST(image_policy__test_0x37_crc_check_requested_recomputed_live_not_cached);
}
