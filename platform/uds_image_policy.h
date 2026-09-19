/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/uds_image_policy.h
 *
 * PURPOSE: Firmware image verification policy abstraction for the UDS
 *          download services (issue #232, Phase 1).
 *
 * Provides the uds_image_policy_t callback table registered at stack init,
 * mirroring uds_flash_ops_t.  Service 0x34 (RequestDownload), 0x36
 * (TransferData) and 0x37 (RequestTransferExit) call these hooks so that an
 * integrator can bind cryptographic image verification — signature, digest,
 * anti-rollback — into the transfer state machine without any crypto
 * primitive entering core/.
 *
 * WHY THIS EXISTS (issue #232):
 *   Before this interface, 0x34 -> 0x36 -> 0x37 completed with no
 *   cryptographic evidence about the image at any point: a one-byte [0x37]
 *   was accepted and answered [0x77].  The boundary ("EDS is not a
 *   bootloader; authenticity is the integrator's job") was documented but
 *   unenforced, which an external Tier-1 review read as no boundary at all.
 *   This header converts that documented boundary into a gate that FAILS
 *   CLOSED: a production build that reaches RequestDownload with no policy
 *   registered refuses the request (NRC 0x22) instead of accepting an
 *   unverified image.
 *
 * PLATFORM IMPLEMENTATIONS:
 *   None ship in Phase 1.  Phase 2 adds the Zephyr reference policy that
 *   verifies the MCUboot image header, its SHA-256 TLV and its ECDSA-P256
 *   signature under MCUboot's own key, then calls boot_request_upgrade()
 *   from commit_cb.  Phase 3 adds the anti-rollback counter; Phase 4 adds
 *   the FreeRTOS equivalent.  The hook set below is complete for all four
 *   phases so that none of them has to redesign this interface.
 *
 * DESIGN CONSTRAINTS:
 *   - No dynamic memory allocation.
 *   - update_cb runs once per accepted TransferData payload range, inside
 *     the P2server window (50 ms on the reference configuration).  It must
 *     be O(length) and allocation-free.  This is why the image digest is
 *     STREAMED during 0x36 rather than computed over the whole image at
 *     0x37: the server has no NRC 0x78 (responsePending) mechanism to buy
 *     itself time at transfer exit (issue #282).
 *   - The policy sees only the download direction.  0x35 RequestUpload
 *     reads flash out to the tester and never reaches these hooks.
 *
 * SAFETY:
 *   REQ-IMGPOL-001: In a production build (EDS_BUILD_IS_PRODUCTION != 0) a
 *                   policy must be registered before any 0x34 request is
 *                   accepted.  service_0x34.c records a platform safety
 *                   violation and returns NRC 0x22 if none is.
 *   REQ-IMGPOL-002: service_0x37.c re-checks the same condition before
 *                   building the positive response (defence in depth: a
 *                   transfer could in principle have started before a
 *                   policy was de-registered).
 *   REQ-IMGPOL-003: A non-ACCEPT verdict from finalise_cb must abort the
 *                   transfer; no positive response may be emitted.
 *   REQ-IMGPOL-004: The registration entry point must not accept NULL as a
 *                   silent de-registration — see uds_image_policy_register().
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef UDS_IMAGE_POLICY_H
#define UDS_IMAGE_POLICY_H

#include "uds_types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Verdict
 * -------------------------------------------------------------------------- */

/**
 * @brief Verdict returned by finalise_cb.
 *
 * The verdict — not the uds_status_t return value of finalise_cb — decides
 * which NRC the tester sees.  finalise_cb returns UDS_STATUS_OK whenever it
 * reached a decision at all; *out_verdict then carries that decision.
 *
 * NRC mapping applied by service_0x37.c:
 *   UDS_IMAGE_ACCEPT           -> transfer completes, [0x77]
 *   UDS_IMAGE_REJECT_FORMAT    -> NRC 0x72 generalProgrammingFailure
 *   UDS_IMAGE_REJECT_DIGEST    -> NRC 0x72 generalProgrammingFailure
 *   UDS_IMAGE_REJECT_SIGNATURE -> NRC 0x72 generalProgrammingFailure
 *   UDS_IMAGE_REJECT_ROLLBACK  -> NRC 0x31 requestOutOfRange
 *   UDS_IMAGE_REJECT_POLICY    -> NRC 0x22 conditionsNotCorrect
 *
 * The three "the image is bad" verdicts deliberately collapse onto one NRC.
 * Telling an unauthenticated tester which of signature, digest or header
 * framing failed is an oracle it does not need.
 */
typedef enum uds_image_verdict {
    UDS_IMAGE_ACCEPT            = 0,  /* image accepted                       */
    UDS_IMAGE_REJECT_FORMAT     = 1,  /* header/TLV malformed, size mismatch  */
    UDS_IMAGE_REJECT_DIGEST     = 2,  /* streamed digest != image's own TLV   */
    UDS_IMAGE_REJECT_SIGNATURE  = 3,  /* signature invalid under trust anchor */
    UDS_IMAGE_REJECT_ROLLBACK   = 4,  /* version < stored monotonic counter   */
    UDS_IMAGE_REJECT_POLICY     = 5   /* integrator-specific refusal          */
} uds_image_verdict_t;

/* --------------------------------------------------------------------------
 * Hook payload descriptors
 * -------------------------------------------------------------------------- */

/**
 * @brief Immutable description of the transfer, passed to begin_cb.
 *
 * Populated by service_0x34.c from the validated RequestDownload request.
 * Valid only for the duration of the begin_cb call.
 */
typedef struct uds_image_transfer_info {
    uint32_t target_address;        /**< Validated memoryAddress from 0x34.    */
    uint32_t total_size_bytes;      /**< Validated memorySize from 0x34.       */
    uint8_t  data_format_id;        /**< Echo of 0x34 dataFormatIdentifier.    */
} uds_image_transfer_info_t;

/**
 * @brief Evidence available at transfer exit, passed to finalise_cb.
 *
 * Populated by service_0x37.c after the write buffer has been flushed, the
 * tester CRC (if any) has been compared and ops->verify_cb has confirmed
 * the flash readback.  Valid only for the duration of the finalise_cb call;
 * param_record points into the request PDU buffer and must not be retained.
 */
typedef struct uds_image_evidence {
    uint32_t       target_address;   /**< Base address the image was written to. */
    uint32_t       total_size_bytes; /**< Image size declared at 0x34.           */
    uint32_t       crc32;            /**< Finalised accumulator, for continuity. */
    const uint8_t *param_record;     /**< 0x37 transferRequestParameterRecord,
                                       *   NULL if the tester sent a bare [0x37]. */
    uint16_t       param_record_len; /**< 0 if the tester sent a bare [0x37].    */
} uds_image_evidence_t;

/* --------------------------------------------------------------------------
 * Image policy callback table
 * -------------------------------------------------------------------------- */

/**
 * @brief Image verification policy table — registered at stack init.
 *
 * begin_cb, update_cb and finalise_cb are MANDATORY and are validated
 * non-NULL by uds_image_policy_register().  A table that could be
 * registered with a NULL verification hook would be a gate that silently
 * verifies nothing, which is the exact failure mode issue #232 exists to
 * remove.
 *
 * commit_cb and abort_cb are OPTIONAL and may be NULL:
 *   - commit_cb NULL means the integrator arms activation elsewhere (for
 *     example from a 0x31 routine, or from the bootloader itself).
 *   - abort_cb NULL means the policy keeps no partial state that needs
 *     dropping, which is true of any stateless verify-at-exit policy.
 *
 * REQ-IMGPOL-001: Register before any 0x34 request is accepted.
 */
typedef struct uds_image_policy {
    /**
     * @brief Called from 0x34 AFTER range validation, BEFORE erase.  May refuse.
     *
     * Runs before ops->erase_cb, so a refusal here leaves the target region
     * untouched rather than erased-and-empty.
     *
     * @param[in] info  Description of the transfer about to start.
     *
     * @return UDS_STATUS_OK to allow the download to proceed.
     * @return Any other status to refuse — mapped to NRC 0x70
     *         (uploadDownloadNotAccepted).
     */
    uds_status_t (*begin_cb)(const uds_image_transfer_info_t *info);

    /**
     * @brief Called from 0x36 for every accepted payload byte range, in order.
     *
     * Ranges are contiguous, non-overlapping, and cover
     * [0, total_size_bytes) exactly once.  image_offset is the offset
     * within the image at which this chunk STARTS.
     *
     * Must be O(length) and allocation-free: this runs inside P2.
     *
     * @param[in] image_offset  Offset of data[0] within the image.
     * @param[in] data          Payload bytes for this range.
     * @param[in] length        Number of bytes at data.
     *
     * @return UDS_STATUS_OK on success.
     * @return Any other status to abort the transfer — mapped to NRC 0x72.
     */
    uds_status_t (*update_cb)(uint32_t       image_offset,
                              const uint8_t *data,
                              uint32_t       length);

    /**
     * @brief Called from 0x37 after flush + CRC + ops->verify_cb, before the
     *        positive response is built.
     *
     * @param[in]  evidence     Evidence gathered about the completed transfer.
     * @param[out] out_verdict  Set to the policy's verdict.  Only inspected
     *                          when the function returns UDS_STATUS_OK.
     *
     * @return UDS_STATUS_OK if a verdict was reached (see *out_verdict).
     * @return Any other status if the policy could not reach a verdict at
     *         all — treated as a rejection and mapped to NRC 0x72.
     */
    uds_status_t (*finalise_cb)(const uds_image_evidence_t *evidence,
                                uds_image_verdict_t        *out_verdict);

    /**
     * @brief Optional.  Called only after finalise_cb yields UDS_IMAGE_ACCEPT.
     *
     * NULL = the integrator arms activation elsewhere.  Phase 1 ships no
     * implementation that populates this hook; it is wired so that Phase 2
     * (which calls boot_request_upgrade() here) has nothing to redesign.
     *
     * @return UDS_STATUS_OK on success.
     * @return Any other status to fail the transfer exit — mapped to NRC
     *         0x72, and no positive response is emitted.
     */
    uds_status_t (*commit_cb)(void);

    /**
     * @brief Optional.  Called on any abort path so the policy can drop
     *        partial state.
     *
     * Invoked wherever an in-progress DOWNLOAD transfer is torn down
     * without a successful exit — a new 0x34 pre-empting an active
     * transfer, an over-long or unwritable 0x36 block, a short or refused
     * 0x37.  Must not fail and must be safe to call when the policy holds
     * no partial state.
     */
    void (*abort_cb)(void);

    /** Bitmask of UDS_IMAGE_POLICY_* requirements — see below. */
    uint8_t policy_flags;
} uds_image_policy_t;

/* --------------------------------------------------------------------------
 * policy_flags bits
 * -------------------------------------------------------------------------- */

/**
 * @brief The policy verifies a signature.  Declarative in Phase 1.
 *
 * Reserved for the Phase 2 Zephyr/MCUboot reference policy and for
 * integrator self-description; core reads no behaviour from this bit today.
 */
#define UDS_IMAGE_POLICY_REQUIRE_SIGNATURE      (1U << 0)

/**
 * @brief The policy enforces an anti-rollback counter.  Declarative in Phase 1.
 *
 * Reserved for Phase 3; core reads no behaviour from this bit today.
 */
#define UDS_IMAGE_POLICY_REQUIRE_ANTIROLLBACK   (1U << 1)

/**
 * @brief The 0x37 transferRequestParameterRecord belongs to the policy.
 *
 * ACTIVE in Phase 1.  When set:
 *   - service_0x37.c accepts a transferRequestParameterRecord of ANY length
 *     (today's strict "exactly 0 or exactly 4 bytes" rule is relaxed) and
 *     hands it to finalise_cb verbatim in uds_image_evidence_t;
 *   - that record is NEVER reinterpreted as a CRC-32, not even when it
 *     happens to be 4 bytes long — the record has exactly one owner;
 *   - a bare [0x37] with no record at all is refused with NRC 0x13, because
 *     the flag says the record is required.
 *
 * When clear (and with no policy registered at all), the 0x37 record
 * validation is bit-for-bit what it was before issue #232.
 */
#define UDS_IMAGE_POLICY_REQUIRE_PARAM_RECORD   (1U << 2)

/* --------------------------------------------------------------------------
 * Global image policy registration
 *
 * The single global policy pointer is set once at stack init via
 * uds_image_policy_register().  The three download services read it.
 * -------------------------------------------------------------------------- */

/**
 * @brief Register the image verification policy with the download services.
 *
 * Must be called before any 0x34 request can be processed on a production
 * build.  Calling again with a different policy replaces the existing
 * registration.
 *
 * DELIBERATE ASYMMETRY WITH uds_flash_ops_register() (REQ-IMGPOL-004):
 * uds_flash_ops_register(NULL) de-registers the flash ops table.  This
 * function REFUSES NULL instead.  The two are not symmetric on purpose: a
 * NULL flash ops table stops downloads working at all and is loudly
 * self-evident, whereas silently accepting a NULL policy would disarm a
 * security gate on the strength of one uninitialised pointer in an
 * integrator's init path.  De-registration, where it is genuinely needed
 * (host test suites, bring-up), has its own differently-named entry point
 * — uds_image_policy_clear() — so it can never happen by accident.
 *
 * @param[in] policy  Pointer to a statically-allocated uds_image_policy_t.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if policy is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if policy->begin_cb, update_cb or
 *         finalise_cb is NULL.  commit_cb and abort_cb may be NULL.
 */
uds_status_t uds_image_policy_register(const uds_image_policy_t *policy);

/**
 * @brief Retrieve the currently registered image policy.
 *
 * Returns NULL if no policy has been registered.
 *
 * @return Pointer to the registered uds_image_policy_t, or NULL.
 */
const uds_image_policy_t *uds_image_policy_get(void);

/**
 * @brief De-register the image policy (test harnesses and bring-up only).
 *
 * SAFETY: Must NOT be called in production firmware.  On a production build
 *         the stack fails closed afterwards — every subsequent 0x34 and
 *         0x37 is refused with NRC 0x22 — so this cannot open a hole, but
 *         it does take DFU offline.
 *
 * Exists so that uds_image_policy_register() can refuse NULL
 * (REQ-IMGPOL-004) without leaving host test suites unable to return to the
 * unregistered state.  Mirrors uds_safety_reset_counters(), which is
 * test-only for the same kind of reason.
 */
void uds_image_policy_clear(void);

/**
 * @brief Notify the registered policy that an in-progress transfer aborted.
 *
 * Convenience wrapper that performs both NULL checks (no policy registered;
 * policy registered but abort_cb optional and NULL) in one place, so that
 * every abort site in services 0x34/0x36/0x37 is a single call and cannot
 * drift from the others.  No-op when there is nothing to notify.
 */
void uds_image_policy_notify_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* UDS_IMAGE_POLICY_H */
