// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_mcuboot_image_policy.h
 *
 * PURPOSE: Zephyr/MCUboot reference image policy — hardware-dependent half
 *          of issue #232 Phase 2, and the fix for issue #277.
 *
 * Registers a uds_image_policy_t (platform/uds_image_policy.h) that:
 *
 *   - Streams a SHA-256 digest over the image header + body (NOT the TLV
 *     trailer) as 0x36 TransferData blocks arrive, using the primitives in
 *     platform/uds_sha256.c.
 *   - At 0x37 RequestTransferExit, reads the image's own TLV trailer back
 *     from the MCUboot secondary slot (platform/uds_mcuboot_image.c parses
 *     it) and compares its SHA256 TLV against the streamed digest.
 *   - On a match, arms the swap: commit_cb calls
 *     boot_request_upgrade(BOOT_UPGRADE_TEST) — TEST, not PERMANENT, because
 *     main.c already implements the other half of MCUboot's revert safety
 *     net: boot_write_img_confirmed() marks the image permanent only after
 *     it has actually booted and run. A new image that never confirms is
 *     reverted automatically on the next reset.
 *
 * SCOPE — WHAT THIS POLICY DELIBERATELY DOES NOT DO (see #277 discussion):
 *   It does not verify the image's RSA-2048-PSS signature. This board's
 *   MCUboot is built with CONFIG_BOOT_SIGNATURE_TYPE_RSA=y and already
 *   verifies that signature under its own embedded public key before it
 *   will boot a swapped image — that is the actual, already-correct trust
 *   anchor for this configuration. Re-verifying the signature a second time
 *   at the application layer would duplicate that check for no additional
 *   guarantee a single-tenant ECU does not already have, at the cost of a
 *   new RSA-PSS verify primitive in a security-relevant path. What this
 *   policy adds on top of MCUboot's own check is earlier, cheaper failure:
 *   a corrupted or truncated transfer is rejected at 0x37 with a clear NRC
 *   instead of only being discovered — and silently reverted — on next
 *   boot.
 *
 * INTEGRATION:
 *   Call zephyr_mcuboot_image_policy_init() from the application's main()
 *   after uds_generated_init() (which registers the flash ops table this
 *   policy reads flash through) and before the diagnostics thread starts.
 *
 * SAFETY  : Security-relevant. ASIL-B candidate.
 *           This module has not undergone formal ASIL assessment.
 *           OEM must validate before vehicle deployment.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef ZEPHYR_MCUBOOT_IMAGE_POLICY_H
#define ZEPHYR_MCUBOOT_IMAGE_POLICY_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the Zephyr/MCUboot reference image policy.
 *
 * Must be called before any 0x34 RequestDownload can be processed, and
 * after the flash ops table for the MCUboot secondary slot has been
 * registered (uds_generated_init() -> zephyr_flash_ops_init()), since
 * finalise_cb reads the image trailer back from that same slot.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_INVALID_PARAM if uds_image_policy_register()
 *         rejects the table (should not happen; indicates a defect in this
 *         file, not a runtime condition).
 */
uds_status_t zephyr_mcuboot_image_policy_init(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_MCUBOOT_IMAGE_POLICY_H */
