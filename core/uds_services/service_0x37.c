/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x37.c
 *
 * PURPOSE: SID 0x37 — RequestTransferExit service handler.
 *
 * ISO 14229-1 §14.4: RequestTransferExit terminates an active firmware
 * download initiated by RequestDownload (0x34).  It flushes any remaining
 * bytes from the write accumulation buffer, optionally validates a CRC-32
 * supplied in the transferRequestParameterRecord, and invokes flash_verify_cb
 * to confirm image integrity.
 *
 * REQUEST FORMAT (ISO 14229-1 §14.4.2):
 *   [0x37 {, transferRequestParameterRecord...}]
 *
 *   The request may carry an optional 4-byte CRC-32 record:
 *     [0x37, CRC_byte3, CRC_byte2, CRC_byte1, CRC_byte0]   (big-endian)
 *   If the record is absent (length == 1), no CRC check is performed.
 *   If present and 4 bytes, it is compared against the accumulated CRC.
 *
 *   [#232/#279] Both of those sentences are now conditional on the
 *   registered image policy — see the parse and CRC blocks below.  With NO
 *   policy registered they hold exactly as written, bit for bit.
 *
 * POSITIVE RESPONSE (ISO 14229-1 §14.4.3):
 *   [0x77]
 *   (No additional data fields required for this implementation.)
 *
 * NRC BEHAVIOUR:
 *   NRC 0x13 (incorrectMessageLengthOrInvalidFormat) — bad CRC record length;
 *                                                       or a mandatory record
 *                                                       (CRC / policy param
 *                                                       record) was not
 *                                                       supplied (#232/#279)
 *   NRC 0x24 (requestSequenceError)                  — no active transfer
 *   NRC 0x72 (generalProgrammingFailure)              — flash write or verify
 *                                                       fail; image policy
 *                                                       rejected the image on
 *                                                       format/digest/signature
 *                                                       grounds; commit_cb failed
 *   NRC 0x31 (requestOutOfRange)                      — bytes_remaining != 0, or
 *                                                       image policy rejected on
 *                                                       anti-rollback grounds
 *   NRC 0x22 (conditionsNotCorrect)                   — flash ops de-registered;
 *                                                       image policy raised an
 *                                                       integrator-specific
 *                                                       refusal; or (production
 *                                                       build) no image policy is
 *                                                       registered at all
 *
 * INCOMPLETE TRANSFER DETECTION (REQ-DL-002):
 *   If bytes_remaining > 0 when 0x37 is received, the tester sent fewer
 *   bytes than declared in the RequestDownload memorySize.  The transfer
 *   is aborted with NRC 0x31 (requestOutOfRange).
 *
 * SAFETY:
 *   REQ-DL-002: bytes_remaining must be 0 before accepting exit.
 *   REQ-DL-003: Transfer context reset to IDLE after successful or failed exit.
 *   REQ-FLASH-003: CRC verified before positive response is sent.
 *   REQ-IMGPOL-002: On a production build, reaching transfer exit with no
 *                  image policy registered refuses rather than answering
 *                  [0x77] — defence in depth behind the 0x34 gate (#232).
 *   REQ-IMGPOL-003: A non-ACCEPT verdict aborts the transfer; no positive
 *                  response is emitted.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_safety.h"
#include "uds_security_algo.h"
#include "uds_transfer_ctx.h"
#include "uds_flash_ops.h"
#include "uds_image_policy.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/** Minimum valid request: [SID] only = 1 byte. */
#define SVC_0x37_MIN_REQ_LEN          (1U)

/** Size of the optional CRC-32 transferRequestParameterRecord (4 bytes). */
#define SVC_0x37_CRC_RECORD_LEN       (4U)

/** Offset of first CRC byte within the request (after SID). */
#define SVC_0x37_CRC_OFFSET           (1U)

/** Offset of the transferRequestParameterRecord within the request (after SID). */
#define SVC_0x37_PARAM_REC_OFFSET     (1U)

/* --------------------------------------------------------------------------
 * Static helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief [#232] Map a non-ACCEPT image policy verdict to a uds_status_t.
 *
 * Every arm reuses a status code that already exists and already maps to
 * the required NRC in srv_status_to_nrc() (core/uds_server.c); no verdict
 * needed a status code invented for it alone.
 *
 *   REJECT_FORMAT / REJECT_DIGEST / REJECT_SIGNATURE -> NRC 0x72
 *       ERR_TRANSFER_ABORTED, not ERR_PLATFORM: the flash driver did not
 *       fail, the image did.  Both map to 0x72, so the tester cannot tell
 *       the two apart — which is intended — while the distinction survives
 *       internally for post-mortem analysis.  The three verdicts collapse
 *       onto one NRC on purpose: an unauthenticated tester does not get an
 *       oracle telling it which check it failed.
 *   REJECT_ROLLBACK                                   -> NRC 0x31
 *       ERR_REQUEST_OUT_OF_RANGE: the version the tester offered is out of
 *       the acceptable range, which is literally what 0x31 means.
 *   REJECT_POLICY                                     -> NRC 0x22
 *       ERR_IMAGE_POLICY_REJECTED.
 *
 * @param[in] verdict  Verdict returned by finalise_cb (must not be ACCEPT).
 *
 * @return The uds_status_t whose NRC mapping matches the verdict.
 */
static uds_status_t s_verdict_to_status(uds_image_verdict_t verdict)
{
    uds_status_t status;

    switch (verdict) {
        case UDS_IMAGE_REJECT_ROLLBACK:
            status = UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;   /* NRC 0x31 */
            break;

        case UDS_IMAGE_REJECT_POLICY:
            status = UDS_STATUS_ERR_IMAGE_POLICY_REJECTED;  /* NRC 0x22 */
            break;

        case UDS_IMAGE_REJECT_FORMAT:
        case UDS_IMAGE_REJECT_DIGEST:
        case UDS_IMAGE_REJECT_SIGNATURE:
        case UDS_IMAGE_ACCEPT:
        default:
            /* ACCEPT never reaches here (the caller checks first); an
             * out-of-range verdict value from a misbehaving policy must
             * fail closed, so the default arm is a rejection too. */
            status = UDS_STATUS_ERR_TRANSFER_ABORTED;       /* NRC 0x72 */
            break;
    }

    return status;
}

/* --------------------------------------------------------------------------
 * SID 0x37 handler
 * -------------------------------------------------------------------------- */

/**
 * @brief SID 0x37 — RequestTransferExit handler.
 *
 * Flushes the remaining write buffer, validates the optional CRC record,
 * invokes flash_verify_cb, resets the transfer state machine, and returns
 * a positive response.
 */
uds_status_t uds_service_0x37_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t              status;
    uds_transfer_ctx_t       *tctx;
    const uds_flash_ops_t    *ops;
    const uds_image_policy_t *policy;
    bool                      param_record_required = false;
    bool                      crc_supplied  = false;
    uint32_t                  supplied_crc  = (uint32_t)0U;
    uint32_t                  computed_crc;
    const uint8_t            *param_record     = NULL;
    uint16_t                  param_record_len = (uint16_t)0U;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum length: just [SID]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x37_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    /* REQ-DL-SEQ: A transfer must be active. */
    tctx = uds_transfer_ctx_get();
    if (tctx->state != UDS_TRANSFER_ACTIVE) {
        /* NRC 0x24 requestSequenceError. */
        return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
    }

    ops = uds_flash_ops_get();
    if (ops == NULL) {
        uds_image_policy_notify_abort();   /* [#232] drop partial state */
        uds_transfer_ctx_reset(tctx);
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    policy = uds_image_policy_get();
    if (policy != NULL) {
        param_record_required =
            ((policy->policy_flags & (uint8_t)UDS_IMAGE_POLICY_REQUIRE_PARAM_RECORD) != (uint8_t)0U);
    }

    /* ----------------------------------------------------------------------
     * Parse the transferRequestParameterRecord.
     *
     * TWO MUTUALLY EXCLUSIVE INTERPRETATIONS — the PDU carries exactly one
     * record, so it has exactly one owner:
     *
     *   (a) No policy, or a policy that does not set REQUIRE_PARAM_RECORD:
     *       the record is a CRC-32 and NOTHING ELSE.  Length must be 0 or
     *       4 bytes.  This arm is bit-for-bit the pre-#232 behaviour and
     *       must stay that way — every example and harness test that does
     *       not use SafeBoot goes through it.
     *
     *   (b) A policy that sets REQUIRE_PARAM_RECORD: the record belongs to
     *       the policy (signed manifest, version blob, detached signature),
     *       may be ANY length, and is NEVER reinterpreted as a CRC — not
     *       even when it happens to be 4 bytes long, because a silent
     *       change of meaning based on a length coincidence is exactly the
     *       kind of ambiguity a Tier-1 review flags.  Since the flag says
     *       the record is REQUIRED, a bare [0x37] is refused here with NRC
     *       0x13, mirroring the malformed-length refusal in arm (a): both
     *       are "the tester's request was the wrong shape", and like that
     *       one this leaves the transfer context ACTIVE so the tester can
     *       simply resend a well-formed 0x37.
     * -------------------------------------------------------------------- */
    if (param_record_required) {
        if (req->length <= (uint16_t)SVC_0x37_MIN_REQ_LEN) {
            /* Bare [0x37] but the policy requires a parameter record. */
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
        param_record     = &req->data[SVC_0x37_PARAM_REC_OFFSET];
        param_record_len = (uint16_t)(req->length - (uint16_t)SVC_0x37_PARAM_REC_OFFSET);
    } else {
        if (req->length == (uint16_t)(SVC_0x37_MIN_REQ_LEN + SVC_0x37_CRC_RECORD_LEN)) {
            /* Exactly 5 bytes: [SID, CRC3, CRC2, CRC1, CRC0]. */
            supplied_crc = ((uint32_t)req->data[SVC_0x37_CRC_OFFSET    ] << (uint32_t)24U) |
                           ((uint32_t)req->data[SVC_0x37_CRC_OFFSET + 1U] << (uint32_t)16U) |
                           ((uint32_t)req->data[SVC_0x37_CRC_OFFSET + 2U] << (uint32_t)8U)  |
                            (uint32_t)req->data[SVC_0x37_CRC_OFFSET + 3U];
            crc_supplied = true;

            param_record     = &req->data[SVC_0x37_PARAM_REC_OFFSET];
            param_record_len = (uint16_t)SVC_0x37_CRC_RECORD_LEN;
        } else if (req->length != (uint16_t)SVC_0x37_MIN_REQ_LEN) {
            /* Any other length is malformed. */
            return UDS_STATUS_ERR_INVALID_PARAM;
        } else {
            /* length == 1 — no record.  param_record stays NULL, len 0. */
        }
    }

    /* --- REQ-DL-002: All declared bytes must have been received --- */
    if (tctx->bytes_remaining != (uint32_t)0U) {
        /* Tester sent fewer bytes than declared in RequestDownload. */
        uds_image_policy_notify_abort();   /* [#232] drop partial state */
        uds_transfer_ctx_reset(tctx);
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    /* --- Flush any remaining bytes in the write accumulation buffer --- */
    if (tctx->write_buf_fill > (uint16_t)0U) {
        status = ops->write_cb(
            tctx->next_write_address,
            tctx->write_buf,
            (uint32_t)tctx->write_buf_fill);

        if (status != UDS_STATUS_OK) {
            uds_image_policy_notify_abort();   /* [#232] drop partial state */
            uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
            return UDS_STATUS_ERR_PLATFORM;
        }

        tctx->next_write_address += (uint32_t)tctx->write_buf_fill;
        tctx->write_buf_fill      = (uint16_t)0U;
    }

    /* Finalise once and reuse.  uds_transfer_crc32_finalise() is pure, so
     * this is behaviourally identical to the two separate calls it
     * replaces; it exists because the value is now needed in three places
     * (the CRC comparison, verify_cb, and the policy evidence record). */
    computed_crc = uds_transfer_crc32_finalise(tctx->crc_accumulator);

    /* --- CRC validation --- */
    if (crc_supplied) {
        if (computed_crc != supplied_crc) {
            uds_image_policy_notify_abort();   /* [#232] drop partial state */
            uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
            /* NRC 0x72 generalProgrammingFailure — CRC mismatch. */
            return UDS_STATUS_ERR_PLATFORM;
        }
    } else if (tctx->crc_check_requested) {
        /* ------------------------------------------------------------------
         * [#279] crc_check_requested is finally consulted.
         *
         * Set at 0x34 when an image policy is registered and that policy
         * has not claimed the parameter record for itself — see the long
         * rationale at the assignment site in service_0x34.c.  A
         * policy-armed ECU does not accept a transfer exit that carries no
         * integrity evidence at all.
         *
         * NRC 0x13, and the transfer context is deliberately NOT reset:
         * this is the same class of fault as the malformed-record-length
         * refusal above — the tester's request was the wrong shape, not the
         * image — so the tester may simply resend a well-formed
         * [0x37, CRC32] and complete the transfer.  Nothing has been
         * accepted, and on a production build the 0x34 gate has already
         * established that a policy exists, so leaving the transfer open
         * costs no authority.
         * ------------------------------------------------------------------ */
        return UDS_STATUS_ERR_INVALID_PARAM;
    } else {
        /* No CRC supplied and none required — pre-#232 behaviour. */
    }

    /* --- REQ-FLASH-003: Platform verify callback --- */
    status = ops->verify_cb(
        tctx->target_address,
        tctx->total_size_bytes,
        computed_crc);

    if (status != UDS_STATUS_OK) {
        uds_image_policy_notify_abort();   /* [#232] drop partial state */
        uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
        /* NRC 0x72 generalProgrammingFailure — platform verify failed. */
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* ----------------------------------------------------------------------
     * [#232 Phase 1] Image policy finalise + commit — REQ-IMGPOL-003.
     *
     * Runs after the flush, the tester CRC comparison and the flash
     * readback, so the policy is handed evidence about an image that is
     * fully written and self-consistent, and is the LAST word before a
     * positive response can be built.
     * -------------------------------------------------------------------- */
    if (policy != NULL) {
        uds_image_evidence_t evidence;
        /* Fail-closed initial value: a finalise_cb that returns OK without
         * writing *out_verdict must not be read as an acceptance. */
        uds_image_verdict_t  verdict = UDS_IMAGE_REJECT_POLICY;

        evidence.target_address   = tctx->target_address;
        evidence.total_size_bytes = tctx->total_size_bytes;
        evidence.crc32            = computed_crc;
        evidence.param_record     = param_record;
        evidence.param_record_len = param_record_len;

        status = policy->finalise_cb(&evidence, &verdict);
        if (status != UDS_STATUS_OK) {
            /* The policy could not reach a verdict at all (its own
             * dependency failed, say).  Treat as a rejection, not as an
             * acceptance — NRC 0x72. */
            verdict = UDS_IMAGE_REJECT_FORMAT;
        }

        if (verdict != UDS_IMAGE_ACCEPT) {
            uds_status_t reject_status = s_verdict_to_status(verdict);

            uds_safety_record_platform_violation(reject_status);
            uds_image_policy_notify_abort();
            uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
            return reject_status;
        }

        /* Accepted.  Arm activation if the policy supplies a commit hook.
         * Phase 1 ships no implementation that does; the wiring exists so
         * Phase 2 (boot_request_upgrade()) has nothing to redesign. */
        if (policy->commit_cb != NULL) {
            status = policy->commit_cb();
            if (status != UDS_STATUS_OK) {
                uds_safety_record_platform_violation(UDS_STATUS_ERR_TRANSFER_ABORTED);
                uds_image_policy_notify_abort();
                uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
                /* NRC 0x72 — image verified but could not be armed; no
                 * positive response. */
                return UDS_STATUS_ERR_TRANSFER_ABORTED;
            }
        }
    }

    /* ----------------------------------------------------------------------
     * [#232 Phase 1] Defence in depth — REQ-IMGPOL-002.
     *
     * The 0x34 gate already refuses to start an unverified download on a
     * production build.  This re-check closes the only window that gate
     * leaves: a transfer that began while a policy was registered and
     * reaches exit after it was taken away.  Reaching here with no policy
     * on a production build means nothing verified this image, so no
     * [0x77] may be emitted.
     *
     * Development / CI builds fall straight through, which is what keeps
     * every existing example, harness test and unit test that downloads
     * without a policy behaving exactly as before.
     * -------------------------------------------------------------------- */
#if EDS_BUILD_IS_PRODUCTION
    if (policy == NULL) {
        uds_safety_record_platform_violation(UDS_STATUS_ERR_IMAGE_POLICY_ABSENT);
        uds_transfer_ctx_reset(tctx); /* REQ-DL-003 */
        /* NRC 0x22 — conditionsNotCorrect (no image policy registered). */
        return UDS_STATUS_ERR_IMAGE_POLICY_ABSENT;
    }
#endif /* EDS_BUILD_IS_PRODUCTION */

    /* --- REQ-DL-003: Reset transfer context --- */
    uds_transfer_ctx_reset(tctx);

    /* --- Build positive response: [0x77] --- */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_REQUEST_TRANSFER_EXIT, resp);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    resp->length = (uint16_t)1U;

    return UDS_STATUS_OK;
}
