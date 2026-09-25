/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x34.c
 *
 * PURPOSE: SID 0x34 — RequestDownload service handler.
 *
 * ISO 14229-1 §14.2: RequestDownload initiates a data transfer from the
 * external test tool (tester) to the ECU.  It is the mandatory first step
 * in a firmware update (DFU) sequence:
 *
 *   1. Erase flash  — SID 0x31 routine 0xFF10 (EraseApplicationFlash)
 *   2. Open session — SID 0x34 RequestDownload         ← THIS FILE
 *   3. Send data    — SID 0x36 TransferData (repeated)
 *   4. Commit       — SID 0x37 RequestTransferExit
 *   5. Verify       — SID 0x31 routine 0xFF11 (VerifyApplicationImage)
 *
 * REQUEST FORMAT (ISO 14229-1 §14.2.2):
 *   [0x34, dataFormatIdentifier, addressAndLengthFormatIdentifier,
 *    memoryAddress[0..M], memorySize[0..N]]
 *
 *   dataFormatIdentifier (1 byte):
 *     Bits [7:4] = compressionMethod  (0x0 = uncompressed)
 *     Bits [3:0] = encryptingMethod   (0x0 = unencrypted)
 *     This implementation only accepts 0x00.
 *
 *   addressAndLengthFormatIdentifier (1 byte):
 *     Bits [7:4] = memorySizeLength  (number of bytes encoding memorySize)
 *     Bits [3:0] = memoryAddressLength (number of bytes encoding memoryAddress)
 *     Both fields must be 1–4.
 *
 *   memoryAddress[0..M]:  big-endian, M = memoryAddressLength bytes.
 *   memorySize[0..N]:     big-endian, N = memorySizeLength bytes.
 *
 * POSITIVE RESPONSE (ISO 14229-1 §14.2.3):
 *   [0x74, lengthFormatIdentifier, maxNumberOfBlockLength[0..K]]
 *
 *   lengthFormatIdentifier:
 *     Bits [7:4] = number of bytes encoding maxNumberOfBlockLength (always 2)
 *     Bits [3:0] = reserved (0x0)
 *   maxNumberOfBlockLength: 16-bit value including the 1-byte block counter.
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — request too short
 *   NRC 0x22 (conditionsNotCorrect)                  — no flash ops registered,
 *                                                       or (production build)
 *                                                       no image policy
 *                                                       registered — see #232
 *   NRC 0x31 (requestOutOfRange)                     — address/size invalid
 *   NRC 0x70 (uploadDownloadNotAccepted)              — flash erase failed, or
 *                                                       image policy begin_cb
 *                                                       refused the transfer
 *   NRC 0x7F (serviceNotSupportedInActiveSession)     — wrong session
 *
 * SESSION REQUIREMENT:
 *   Programming session (UDS_SESSION_PROGRAMMING) required.
 *   The default ACL table enforces this.
 *
 * SAFETY:
 *   REQ-DL-001: Block counter initialised to 0x01 on successful 0x34.
 *   REQ-DL-002: bytes_remaining set to total_size_bytes from request.
 *   REQ-FLASH-002: address + size validated against flash memory map.
 *   REQ-IMGPOL-001: On a production build an image verification policy must
 *                  be registered before a RequestDownload is accepted
 *                  (issue #232).  See platform/uds_image_policy.h.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "services.h"
#include "service_transfer_common.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_safety.h"
#include "uds_security_algo.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"
#include "uds_image_policy.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * [#312] Chunked-erase pending state
 *
 * When ops->erase_step_cb is present, the handler below does not erase the
 * target region synchronously — a single-call whole-region erase can block
 * for seconds on hardware with a large per-sector worst-case erase time
 * (STM32H7: up to 4000 ms/sector), which starves a short ASIL-B poll-loop
 * watchdog window and starves the UDS P2/P2*max timing contract in the same
 * stroke. See platform/uds_flash_ops.h's erase_step_cb doc and issue #312
 * for the full evidence trail.
 *
 * Instead the handler answers immediately with NRC 0x78
 * (requestCorrectlyReceived-ResponsePending) and records the erase job
 * here. uds_service_0x34_pending_tick() — called once per poll-loop
 * iteration by the application (see examples/safeboot_ecu/src/main.c) —
 * drives it forward one bounded increment per call, repeating 0x78 between
 * increments, until the whole region is erased, at which point it performs
 * exactly the transfer-state-machine setup and [0x74] response the
 * synchronous path below builds directly.
 *
 * Single static instance: only one erase can be pending at a time, exactly
 * like uds_transfer_ctx_t's single-transfer design that already governs
 * 0x34/0x36/0x37.
 * -------------------------------------------------------------------------- */
typedef struct {
    bool     active;                 /**< An erase job is in progress.        */
    uint32_t address;                /**< Erase cursor — advances per chunk.  */
    uint32_t remaining;               /**< Bytes left to erase.                */
    uint32_t target_address;         /**< Original requested address.         */
    uint32_t total_size_bytes;       /**< Original requested size.            */
    uint16_t write_buf_capacity;     /**< Precomputed for the eventual [0x74]
                                       *   response and transfer_ctx setup.    */
    bool     crc_check_requested;    /**< Precomputed — see #279 note below.  */
} svc_0x34_erase_pending_t;

static svc_0x34_erase_pending_t s_erase_pending;

/**
 * @brief Drop any in-progress chunked erase without completing it.
 *
 * Called wherever an active erase job must not be allowed to finish (a new
 * 0x34 pre-empting it, session loss). Mirrors uds_image_policy_notify_abort()
 * — safe to call when nothing is pending.
 */
static void s_erase_pending_abort(void)
{
    (void)memset(&s_erase_pending, 0, sizeof(s_erase_pending));
}

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum request length: [SID, dataFmt, addrLenFmt] = 3 bytes.
 *  Actual minimum depends on addrLenFmt fields — validated dynamically. */
#define SVC_0x34_MIN_REQ_LEN          (3U)

/** Offset of dataFormatIdentifier in the request PDU. */
#define SVC_0x34_DATA_FMT_OFFSET      (1U)

/** Offset of addressAndLengthFormatIdentifier in the request PDU. */
#define SVC_0x34_ALFID_OFFSET         (2U)

/** Offset of the first memoryAddress byte in the request PDU. */
#define SVC_0x34_MEM_ADDR_OFFSET      (3U)

/** Only uncompressed / unencrypted data is accepted. */
#define SVC_0x34_ACCEPTED_DATA_FMT    (0x00U)

/** Number of bytes used to encode maxNumberOfBlockLength in the response
 *  (always 2 bytes: one uint16_t). Stored in bits [7:4] of
 *  lengthFormatIdentifier. */
#define SVC_0x34_MXBL_BYTE_COUNT      (2U)

/* --------------------------------------------------------------------------
 * SID 0x34 handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x34 — RequestDownload handler.
 *
 * Validates the request, erases the target flash region, initialises the
 * transfer state machine, and returns maxNumberOfBlockLength.
 */
uds_status_t uds_service_0x34_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t           status;
    uint8_t                data_fmt;
    uint8_t                alfid;
    uint8_t                addr_len;
    uint8_t                size_len;
    uint16_t               fields_end;
    uint32_t               mem_address = (uint32_t)0U;
    uint32_t               mem_size    = (uint32_t)0U;
    const uds_flash_ops_t    *ops;
    uds_transfer_ctx_t       *tctx;
    const uds_image_policy_t *policy;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum length: [SID, dataFmt, addrLenFmt]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x34_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* REQ-FLASH-001: Flash ops must be registered. */
    ops = uds_flash_ops_get();
    if (ops == NULL) {
        /* NRC 0x22 — conditions not correct (flash ops not registered). */
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* --- Parse dataFormatIdentifier --- */
    data_fmt = req->data[SVC_0x34_DATA_FMT_OFFSET];
    if (data_fmt != (uint8_t)SVC_0x34_ACCEPTED_DATA_FMT) {
        /* Compression or encryption requested — not supported. */
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* --- Parse addressAndLengthFormatIdentifier --- */
    alfid  = req->data[SVC_0x34_ALFID_OFFSET];
    status = uds_transfer_parse_alfid(alfid, &addr_len, &size_len);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* 0x34 carries no request-side data payload beyond the address/size
     * fields (data itself arrives later via 0x36), so the request must be
     * exactly fields_end bytes -- not just "at least". A `<` check
     * silently accepted trailing garbage bytes (EDS#233).
     * fields_end = SVC_0x34_MEM_ADDR_OFFSET + addr_len + size_len. */
    fields_end = (uint16_t)((uint16_t)SVC_0x34_MEM_ADDR_OFFSET +
                             (uint16_t)addr_len +
                             (uint16_t)size_len);

    if (req->length != fields_end) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memoryAddress (big-endian, addr_len bytes) --- */
    status = uds_transfer_parse_be(
        &req->data[SVC_0x34_MEM_ADDR_OFFSET],
        addr_len,
        &mem_address);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* --- Parse memorySize (big-endian, size_len bytes) --- */
    status = uds_transfer_parse_be(
        &req->data[(uint16_t)SVC_0x34_MEM_ADDR_OFFSET + (uint16_t)addr_len],
        size_len,
        &mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* REQ-FLASH-002: Validate address range against registered memory map. */
    status = uds_transfer_validate_memory_range(ops, mem_address, mem_size);
    if (status != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* A 0x34 arriving while a download is already in progress pre-empts it
     * (ISO 14229-1 §14.3.2 permits this; the context reset below carries it
     * out).  Tell the policy to drop that transfer's partial state BEFORE
     * begin_cb opens a new one, so a policy never sees a begin nested
     * inside an unterminated transfer.  No-op when no policy is registered
     * or the policy supplies no abort_cb. */
    {
        const uds_transfer_ctx_t *prev_tctx = uds_transfer_ctx_get();

        /* [#312] A chunked erase started by an EARLIER 0x34 leaves tctx
         * untouched until it completes (see uds_service_0x34_pending_tick()),
         * so prev_tctx->state alone would miss it here — s_erase_pending is
         * the only record of that in-flight policy state until this check
         * was added. */
        if (((prev_tctx->state == UDS_TRANSFER_ACTIVE) &&
             (prev_tctx->direction == UDS_TRANSFER_DIR_DOWNLOAD)) ||
            s_erase_pending.active) {
            uds_image_policy_notify_abort();
            s_erase_pending_abort();
        }
    }

    /* ----------------------------------------------------------------------
     * [#232 Phase 1] DFU image policy gate — REQ-IMGPOL-001.
     *
     * Runs after range validation and BEFORE erase_cb, so that a refusal
     * here leaves the target region intact rather than erased and empty.
     *
     * PRODUCTION (EDS_BUILD_IS_PRODUCTION != 0): no policy registered means
     * this build would accept an image nothing ever verifies.  Refuse with
     * NRC 0x22 and record a platform safety violation.  Build-mode
     * derivation is the shared EDS_BUILD_IS_PRODUCTION primitive from
     * core/uds_security_algo.h [SEC-BUILD-MODE-01 / issue #84] — the same
     * one the CRIT-4 key gate and the generated Step 7.1 init guard use, so
     * the three cannot drift apart.
     *
     * DEVELOPMENT / CI: permitted, because every example and host test that
     * does not use SafeBoot downloads without a policy and must keep
     * working.  The violation is still recorded, with its own distinguishing
     * status code (UDS_STATUS_ERR_IMAGE_POLICY_ABSENT), so a dev build that
     * silently lost its policy is still visible in uds_safety_get_ctx().
     *
     * NOTE ON "warning": core/ has no logging facility of any kind — no
     * LOG_WRN, no printk, nothing.  uds_safety_record_platform_violation()
     * IS this stack's warning channel for a degraded-but-continuing
     * condition (it is what the TRNG fail-closed gate uses), so the record
     * below is the warning.  It is recorded on every such 0x34 rather than
     * once per boot: the counter is saturating and exists precisely to
     * count occurrences, and one-shotting it would discard the information
     * it was added to carry.
     * -------------------------------------------------------------------- */

    policy = uds_image_policy_get();

    if (policy == NULL) {
        uds_safety_record_platform_violation(UDS_STATUS_ERR_IMAGE_POLICY_ABSENT);
#if EDS_BUILD_IS_PRODUCTION
        /* NRC 0x22 — conditionsNotCorrect (no image policy registered). */
        return UDS_STATUS_ERR_IMAGE_POLICY_ABSENT;
#endif /* EDS_BUILD_IS_PRODUCTION */
    } else {
        uds_image_transfer_info_t info;

        info.target_address   = mem_address;
        info.total_size_bytes = mem_size;
        info.data_format_id   = data_fmt;

        status = policy->begin_cb(&info);
        if (status != UDS_STATUS_OK) {
            /* NRC 0x70 — uploadDownloadNotAccepted (policy refused). */
            return UDS_STATUS_ERR_UPLOAD_DOWNLOAD_NOT_ACCEPTED;
        }
    }

    /* ----------------------------------------------------------------------
     * [#312] Chunked erase path — preferred whenever the platform supports
     * it. See the svc_0x34_erase_pending_t comment above and
     * platform/uds_flash_ops.h's erase_step_cb doc for why: a single
     * whole-region erase_cb() call can block long enough to starve both the
     * watchdog and the UDS P2/P2*max timing contract on this hardware.
     *
     * crc_check_requested and write_buf_capacity are computed HERE, before
     * the erase (chunked or not) runs, in both paths — this mirrors the
     * original synchronous code's own ordering (policy and ops were already
     * fetched before the erase call), so pre-computing them into
     * s_erase_pending changes nothing about what they observe.
     * -------------------------------------------------------------------- */
    if (ops->erase_step_cb != NULL) {
        s_erase_pending.active           = true;
        s_erase_pending.address          = mem_address;
        s_erase_pending.remaining        = mem_size;
        s_erase_pending.target_address   = mem_address;
        s_erase_pending.total_size_bytes = mem_size;

        {
            uint16_t raw_cap = ops->max_block_length;
            uds_transfer_ctx_t *sizing_tctx = uds_transfer_ctx_get();

            if (raw_cap > (uint16_t)sizeof(sizing_tctx->write_buf)) {
                raw_cap = (uint16_t)sizeof(sizing_tctx->write_buf);
            }
            s_erase_pending.write_buf_capacity = raw_cap;
        }

        if (policy != NULL) {
            s_erase_pending.crc_check_requested =
                ((policy->policy_flags & (uint8_t)UDS_IMAGE_POLICY_REQUIRE_PARAM_RECORD) == (uint8_t)0U);
        } else {
            s_erase_pending.crc_check_requested = false;
        }

        /* NRC 0x78 requestCorrectlyReceived-ResponsePending — answered as a
         * positive return (UDS_STATUS_OK) with an NRC-shaped payload, so
         * uds_server_process_request forwards it verbatim instead of
         * re-wrapping it through srv_status_to_nrc(). The real [0x74] (or a
         * failure NRC) follows asynchronously from
         * uds_service_0x34_pending_tick() once the erase completes. */
        return uds_server_build_negative_response(
            (uint8_t)UDS_SID_REQUEST_DOWNLOAD,
            UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING,
            resp);
    }

    /* --- Erase the target flash region (synchronous fallback — unchanged
     * behaviour for any platform without erase_step_cb) --- */
    status = ops->erase_cb(mem_address, mem_size);
    if (status != UDS_STATUS_OK) {
        /* [#232 review fix] begin_cb (above) may already have started
         * per-transfer policy state — a digest context, a reserved slot —
         * on the expectation that either finalise_cb or abort_cb will
         * eventually be called to close it out. Without this, an erase
         * failure here left that state permanently orphaned: tctx itself
         * was never marked ACTIVE, so the next 0x34's pre-emption check
         * never saw an active download to abort, and no abort_cb call was
         * reachable from any path until the next process restart. */
        uds_image_policy_notify_abort();
        /* NRC 0x70 — uploadDownloadNotAccepted (erase failure). */
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* --- Initialise transfer state machine --- */
    tctx = uds_transfer_ctx_get();
    uds_transfer_ctx_reset(tctx);   /* abort any in-progress transfer */

    tctx->state                   = UDS_TRANSFER_ACTIVE;
    tctx->direction               = UDS_TRANSFER_DIR_DOWNLOAD;
    tctx->target_address          = mem_address;
    tctx->total_size_bytes        = mem_size;
    tctx->bytes_remaining         = mem_size;
    tctx->next_write_address      = mem_address;
    tctx->next_expected_block_seq = (uint8_t)0x01U; /* REQ-DL-001 */
    tctx->crc_accumulator         = (uint32_t)0xFFFFFFFFUL; /* CRC init */
    tctx->write_buf_fill          = (uint16_t)0U;

    /* ----------------------------------------------------------------------
     * [#279] crc_check_requested — finally given a producer.
     *
     * The field was declared and documented ("True if 0x37 must validate
     * CRC") but never written or read anywhere in the repo, so
     * service_0x37.c treated the tester CRC as unconditionally optional.
     * #232 gives it the natural producer: the image policy registration.
     *
     * BINDING CHOSEN (the architecture review left this open, so the
     * reasoning belongs here):
     *
     *   crc_check_requested = (a policy is registered)
     *                         AND NOT (policy requires the param record)
     *
     * Half one — "a policy is registered" — is the point of #279: a
     * registered policy means this ECU treats DFU as a verified operation,
     * so the one integrity check the transport layer already owns stops
     * being optional.  A tester that supplies no CRC to a policy-armed ECU
     * is refused rather than quietly accepted.
     *
     * Half two — the REQUIRE_PARAM_RECORD carve-out — is forced by the fact
     * that a 0x37 carries exactly ONE transferRequestParameterRecord.  If
     * the policy has claimed that record (signed manifest, version blob,
     * detached signature), a mandatory 4-byte CRC cannot also occupy it;
     * demanding both would make any such policy impossible to satisfy.  In
     * that configuration the policy is strictly the more authoritative
     * check anyway — finalise_cb receives the same streamed CRC-32 in
     * evidence->crc32 plus the full record, and can bind them however its
     * scheme requires.  So the generic check stands down in favour of the
     * specific one; it does not simply disappear.
     *
     * The alternative binding — "any registered policy makes the CRC
     * mandatory, full stop" — was rejected for exactly that reason: it
     * reads simpler but makes UDS_IMAGE_POLICY_REQUIRE_PARAM_RECORD
     * unusable, which is the same class of defect as #279 itself (a
     * documented feature the code cannot honour).
     * -------------------------------------------------------------------- */
    if (policy != NULL) {
        tctx->crc_check_requested =
            ((policy->policy_flags & (uint8_t)UDS_IMAGE_POLICY_REQUIRE_PARAM_RECORD) == (uint8_t)0U);
    } else {
        tctx->crc_check_requested = false;
    }

    /* Capacity is min(ops->max_block_length, sizeof(write_buf)).
     * Subtract 1 for the block counter byte carried in the 0x36 request. */
    {
        uint16_t raw_cap = ops->max_block_length;
        if (raw_cap > (uint16_t)sizeof(tctx->write_buf)) {
            raw_cap = (uint16_t)sizeof(tctx->write_buf);
        }
        tctx->write_buf_capacity = raw_cap;
    }

    /* --- Build positive response --- */
    /* [0x74, lengthFormatIdentifier, maxNumberOfBlockLength_Hi, maxNumberOfBlockLength_Lo] */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_REQUEST_DOWNLOAD, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* lengthFormatIdentifier: bits [7:4] = number of maxBlockLen bytes = 2,
     * bits [3:0] = 0 (reserved). */
    resp->data[1U] = (uint8_t)((uint8_t)SVC_0x34_MXBL_BYTE_COUNT << (uint8_t)4U);

    /* maxNumberOfBlockLength includes the 1-byte block counter field. */
    {
        uint16_t max_block = (uint16_t)(tctx->write_buf_capacity + (uint16_t)1U);
        resp->data[2U] = (uint8_t)((max_block >> (uint16_t)8U) & (uint16_t)0xFFU);
        resp->data[3U] = (uint8_t)( max_block                  & (uint16_t)0xFFU);
    }
    resp->length = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * [#312] Chunked-erase driver
 * -------------------------------------------------------------------------- */

bool uds_service_0x34_pending_tick(uds_msg_buf_t *out_frame)
{
    const uds_flash_ops_t *ops;
    uint32_t                erased = (uint32_t)0U;
    uds_status_t            status;
    uds_transfer_ctx_t     *tctx;

    if (!s_erase_pending.active) {
        return false;
    }
    if (out_frame == NULL) {
        return false;
    }

    ops = uds_flash_ops_get();
    if ((ops == NULL) || (ops->erase_step_cb == NULL)) {
        /* Flash ops de-registered (or its erase_step_cb withdrawn) mid-erase
         * — should not happen in any real integration, fail closed rather
         * than spin forever answering nothing. */
        uds_image_policy_notify_abort();
        s_erase_pending_abort();
        (void)uds_server_build_negative_response(
            (uint8_t)UDS_SID_REQUEST_DOWNLOAD,
            UDS_NRC_CONDITIONS_NOT_CORRECT,
            out_frame);
        return true;
    }

    status = ops->erase_step_cb(s_erase_pending.address,
                                s_erase_pending.remaining,
                                &erased);
    if ((status != UDS_STATUS_OK) || (erased == (uint32_t)0U) ||
        (erased > s_erase_pending.remaining)) {
        uds_image_policy_notify_abort();
        s_erase_pending_abort();
        /* Same NRC the synchronous erase_cb failure path above uses
         * (UDS_STATUS_ERR_PLATFORM -> NRC 0x70 uploadDownloadNotAccepted). */
        (void)uds_server_build_negative_response(
            (uint8_t)UDS_SID_REQUEST_DOWNLOAD,
            UDS_NRC_UPLOAD_DOWNLOAD_NOT_ACCEPTED,
            out_frame);
        return true;
    }

    s_erase_pending.address   += erased;
    s_erase_pending.remaining -= erased;

    if (s_erase_pending.remaining > (uint32_t)0U) {
        /* Still going — repeat NRC 0x78. Each chunk's own worst-case
         * duration (STM32H7: <= 4000 ms/sector) is comfortably under
         * P2*max (5000 ms on the reference configuration), so one 0x78 per
         * completed chunk keeps the tester inside the timing contract
         * without needing a separate keep-alive timer. */
        (void)uds_server_build_negative_response(
            (uint8_t)UDS_SID_REQUEST_DOWNLOAD,
            UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING,
            out_frame);
        return true;
    }

    /* --- Erase complete: finish exactly what the synchronous path does
     * immediately after a successful erase_cb() call ---
     *
     * Read every s_erase_pending field into locals BEFORE
     * s_erase_pending_abort() clears it — the abort has to happen before
     * tctx is touched (uds_transfer_ctx_reset() below must not observe a
     * still-active erase job), so the read order matters here. */
    {
        uint32_t done_target_address   = s_erase_pending.target_address;
        uint32_t done_total_size_bytes = s_erase_pending.total_size_bytes;
        bool     done_crc_requested    = s_erase_pending.crc_check_requested;
        uint16_t done_write_buf_cap    = s_erase_pending.write_buf_capacity;

        s_erase_pending_abort();

        tctx = uds_transfer_ctx_get();
        uds_transfer_ctx_reset(tctx);

        tctx->state                   = UDS_TRANSFER_ACTIVE;
        tctx->direction               = UDS_TRANSFER_DIR_DOWNLOAD;
        tctx->target_address          = done_target_address;
        tctx->total_size_bytes        = done_total_size_bytes;
        tctx->bytes_remaining         = done_total_size_bytes;
        tctx->next_write_address      = done_target_address;
        tctx->next_expected_block_seq = (uint8_t)0x01U; /* REQ-DL-001 */
        tctx->crc_accumulator         = (uint32_t)0xFFFFFFFFUL; /* CRC init */
        tctx->write_buf_fill          = (uint16_t)0U;
        tctx->crc_check_requested     = done_crc_requested;
        tctx->write_buf_capacity      = done_write_buf_cap;
    }

    /* [0x74, lengthFormatIdentifier, maxNumberOfBlockLength_Hi/Lo] */
    (void)uds_service_write_pos_sid((uint8_t)UDS_SID_REQUEST_DOWNLOAD, out_frame);
    out_frame->data[1U] = (uint8_t)((uint8_t)SVC_0x34_MXBL_BYTE_COUNT << (uint8_t)4U);
    {
        uint16_t max_block = (uint16_t)(tctx->write_buf_capacity + (uint16_t)1U);
        out_frame->data[2U] = (uint8_t)((max_block >> (uint16_t)8U) & (uint16_t)0xFFU);
        out_frame->data[3U] = (uint8_t)( max_block                  & (uint16_t)0xFFU);
    }
    out_frame->length = (uint16_t)4U;

    return true;
}
