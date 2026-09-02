// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_security_nvm.h
 *
 * PURPOSE: NVM adapter for UDS security attempt-counter persistence.
 *
 *          Provides the nvm_load_cb and nvm_save_cb function pointers
 *          for wiring into uds_security_cfg_t. These functions use the
 *          platform NVM store (nvm_store.h) to persist the failed-attempt
 *          counter and lockout-timer residual across ECU resets.
 *
 * USAGE:
 *   uds_security_cfg_t sec_cfg = {
 *       .max_attempts     = 3,
 *       .lockout_ms       = 10000,
 *       .key_validate_cb  = my_key_validate,
 *       .seed_generate_cb = my_seed_gen,
 *       .nvm_load_cb      = uds_security_nvm_load,   // ← add this
 *       .nvm_save_cb      = uds_security_nvm_save,   // ← add this
 *   };
 *
 * WIRE FORMAT (NVM_KEY_SEC_STATE) — v1, integrity-checked (EDS#211):
 *   [magic:2][version:1][attempts:1][lockout_ms:4][crc32:4]  (12 bytes total)
 *   magic      = UDS_SECURITY_NVM_MAGIC_0, UDS_SECURITY_NVM_MAGIC_1 ('S','C').
 *                Identifies a v1-or-later record.
 *   version    = UDS_SECURITY_NVM_FORMAT_VERSION. Bump on any layout change.
 *   attempts   = current failed-attempt count (uint8_t).
 *   lockout_ms = lockout timer residual in ms, big-endian (uint32_t).
 *   crc32      = CRC-32 (same polynomial/algorithm as the transfer-service
 *                helper in core/uds_transfer_ctx.c, and the same one
 *                config/dtc_mirror.c uses for NVM_KEY_DTC_MIRROR) computed
 *                over [version..lockout_ms] (i.e. everything except the
 *                magic and the CRC field itself), big-endian.
 *
 *   Both fields are written together in a single nvm_store_write() call —
 *   this is the actual fix for EDS#211: the counter and lockout residual
 *   used to be two independent NVM_KEY_SEC_ATTEMPT_CTR /
 *   NVM_KEY_SEC_LOCKOUT_MS keys written by two independent
 *   nvm_store_write() calls, with an unprotected window between them — a
 *   power loss after the first write and before the second could leave
 *   attempts=0 (just written) paired with a stale or absent lockout_ms,
 *   silently dropping an engaged lockout on reboot. One record, one write,
 *   removes that window: either the whole record lands, or the previous
 *   one (still valid, still CRC-checked) is what's read back next boot —
 *   there is no partial-write state this format can produce that
 *   uds_security_nvm_load() will treat as valid.
 *
 *   A record whose magic, version, or CRC doesn't check out is reported as
 *   UDS_STATUS_ERR_NVM_DATA_CORRUPT (distinct from the expected first-boot
 *   UDS_STATUS_ERR_DID_NOT_FOUND) — this is a genuine NVM fault as far as
 *   uds_security_init() is concerned, so cfg.nvm_load_fail_closed's
 *   existing fail-open/fail-closed posture (EDS#196) applies exactly as it
 *   already does for a platform read error. See docs/SECURITY_NOTICE.md's
 *   "Failed Attempt Lockout Policy" section.
 *
 * SAFETY  : ASIL-B candidate. Prevents security lockout bypass via power-cycle.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_SECURITY_NVM_H
#define UDS_SECURITY_NVM_H

#include "uds_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Wire format constants (NVM_KEY_SEC_STATE) — see the file header above.
 * -------------------------------------------------------------------------- */

/** Magic bytes identifying a v1-or-later NVM_KEY_SEC_STATE record. */
#define UDS_SECURITY_NVM_MAGIC_0        ((uint8_t)0x53U) /* 'S' */
#define UDS_SECURITY_NVM_MAGIC_1        ((uint8_t)0x43U) /* 'C' */

/** Record format version. Bump on any layout change. */
#define UDS_SECURITY_NVM_FORMAT_VERSION ((uint8_t)1U)

/** Byte offsets within the record. */
#define UDS_SECURITY_NVM_OFF_MAGIC0     (0U)
#define UDS_SECURITY_NVM_OFF_MAGIC1     (1U)
#define UDS_SECURITY_NVM_OFF_VERSION    (2U)
#define UDS_SECURITY_NVM_OFF_ATTEMPTS   (3U)
#define UDS_SECURITY_NVM_OFF_LOCKOUT_MS (4U)
#define UDS_SECURITY_NVM_OFF_CRC        (8U)

/** Total record size: 2 (magic) + 1 (version) + 1 (attempts) + 4 (lockout_ms) + 4 (crc32). */
#define UDS_SECURITY_NVM_RECORD_BYTES   (12U)

/**
 * Number of bytes the CRC-32 covers: [version..lockout_ms], i.e.
 * everything except the 2 magic bytes and the CRC field itself
 * (1 version + 1 attempts + 4 lockout_ms = 6). A literal size_t constant
 * rather than computed as
 * (size_t)(UDS_SECURITY_NVM_OFF_CRC - UDS_SECURITY_NVM_OFF_VERSION) —
 * that subtraction's result has essential type unsigned int (the two
 * offset macros are bare unsigned-int literals), and casting a composite
 * expression to a different/wider essential type is exactly what MISRA
 * Rule 10.8 forbids. Matches config/dtc_mirror.c's own style of literal
 * byte-count constants rather than offset arithmetic.
 */
#define UDS_SECURITY_NVM_CRC_PAYLOAD_BYTES ((size_t)6U)

/**
 * @brief Load security attempt counter and lockout state from NVM.
 *
 * Implements the uds_security_cfg_t::nvm_load_cb contract.
 * Reads and validates the single NVM_KEY_SEC_STATE record (magic, version,
 * CRC-32) — see the file header's WIRE FORMAT section.
 *
 * @param[out] out_attempts   Restored failed-attempt count.
 * @param[out] out_lockout_ms Restored lockout timer residual in ms.
 *
 * @return UDS_STATUS_OK if the record was present and validated.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if no persisted data (first boot).
 * @return UDS_STATUS_ERR_NVM_DATA_CORRUPT if a record is present but its
 *         magic, version, or CRC-32 don't check out — a genuine fault,
 *         not a first-boot condition; see uds_security_cfg_t.nvm_load_fail_closed.
 * @return UDS_STATUS_ERR_PLATFORM if the underlying NVM read failed.
 */
uds_status_t uds_security_nvm_load(
    uint8_t  *out_attempts,
    uint32_t *out_lockout_ms);

/**
 * @brief Save security attempt counter and lockout state to NVM.
 *
 * Implements the uds_security_cfg_t::nvm_save_cb contract.
 * Serializes both fields into one NVM_KEY_SEC_STATE record (magic +
 * version + attempts + lockout_ms + CRC-32) and writes it with a single
 * nvm_store_write() call — see the file header's WIRE FORMAT section for
 * why this must be one write, not two.
 *
 * @param[in] attempts    Current failed-attempt count.
 * @param[in] lockout_ms  Current lockout timer residual (0 if not locked).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed.
 */
uds_status_t uds_security_nvm_save(
    uint8_t  attempts,
    uint32_t lockout_ms);

/**
 * @brief Clear the persisted security NVM record.
 *
 * Called during factory reset (SID 0x14 group 0xFFFFFF or OEM-defined
 * factory NRC sequence). Deletes NVM_KEY_SEC_STATE.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if NVM delete failed.
 */
uds_status_t uds_security_nvm_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SECURITY_NVM_H */
