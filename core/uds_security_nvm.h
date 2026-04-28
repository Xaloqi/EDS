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
 * WIRE FORMAT (NVM_KEY_SEC_ATTEMPT_CTR + NVM_KEY_SEC_LOCKOUT_MS):
 *   Two separate NVM records:
 *     NVM_KEY_SEC_ATTEMPT_CTR : uint8_t  — current failed-attempt count
 *     NVM_KEY_SEC_LOCKOUT_MS  : uint32_t — lockout timer residual (ms)
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

/**
 * @brief Load security attempt counter and lockout state from NVM.
 *
 * Implements the uds_security_cfg_t::nvm_load_cb contract.
 * Reads NVM_KEY_SEC_ATTEMPT_CTR and NVM_KEY_SEC_LOCKOUT_MS.
 *
 * @param[out] out_attempts   Restored failed-attempt count.
 * @param[out] out_lockout_ms Restored lockout timer residual in ms.
 *
 * @return UDS_STATUS_OK if both records loaded.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if no persisted data (first boot).
 * @return UDS_STATUS_ERR_PLATFORM if NVM read failed.
 */
uds_status_t uds_security_nvm_load(
    uint8_t  *out_attempts,
    uint32_t *out_lockout_ms);

/**
 * @brief Save security attempt counter and lockout state to NVM.
 *
 * Implements the uds_security_cfg_t::nvm_save_cb contract.
 * Writes NVM_KEY_SEC_ATTEMPT_CTR and NVM_KEY_SEC_LOCKOUT_MS.
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
 * @brief Clear all persisted security NVM records.
 *
 * Called during factory reset (SID 0x14 group 0xFFFFFF or OEM-defined
 * factory NRC sequence). Deletes NVM_KEY_SEC_ATTEMPT_CTR and
 * NVM_KEY_SEC_LOCKOUT_MS.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if NVM delete failed.
 */
uds_status_t uds_security_nvm_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SECURITY_NVM_H */
