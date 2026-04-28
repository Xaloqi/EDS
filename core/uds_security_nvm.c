// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_security_nvm.c
 *
 * PURPOSE: NVM adapter for UDS security attempt-counter persistence.
 *
 * SAFETY  : ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_security_nvm.h"
#include "nvm_store.h"
#include "uds_types.h"

#include <string.h>

uds_status_t uds_security_nvm_load(
    uint8_t  *out_attempts,
    uint32_t *out_lockout_ms)
{
    uds_status_t rc_attempts;
    uds_status_t rc_lockout;
    uint8_t      attempts   = (uint8_t)0U;
    uint32_t     lockout_ms = 0U;

    if ((out_attempts == NULL) || (out_lockout_ms == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /*
     * Read attempt counter. ERR_DID_NOT_FOUND = first boot, use 0.
     * Any other error: propagate as PLATFORM error.
     */
    rc_attempts = nvm_store_read(
        (uint16_t)NVM_KEY_SEC_ATTEMPT_CTR,
        &attempts, sizeof(attempts), NULL);

    if ((rc_attempts != UDS_STATUS_OK) &&
        (rc_attempts != UDS_STATUS_ERR_DID_NOT_FOUND)) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    /*
     * Read lockout timer residual. ERR_DID_NOT_FOUND = first boot, use 0.
     */
    rc_lockout = nvm_store_read(
        (uint16_t)NVM_KEY_SEC_LOCKOUT_MS,
        &lockout_ms, sizeof(lockout_ms), NULL);

    if ((rc_lockout != UDS_STATUS_OK) &&
        (rc_lockout != UDS_STATUS_ERR_DID_NOT_FOUND)) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    *out_attempts   = attempts;
    *out_lockout_ms = lockout_ms;

    /*
     * Return ERR_DID_NOT_FOUND only if BOTH records are absent (true first boot).
     * If at least one record exists, return OK with whatever was recovered.
     */
    if ((rc_attempts == UDS_STATUS_ERR_DID_NOT_FOUND) &&
        (rc_lockout  == UDS_STATUS_ERR_DID_NOT_FOUND)) {
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_security_nvm_save(
    uint8_t  attempts,
    uint32_t lockout_ms)
{
    uds_status_t rc;

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* Write attempt counter (1 byte). */
    rc = nvm_store_write(
        (uint16_t)NVM_KEY_SEC_ATTEMPT_CTR,
        &attempts, sizeof(attempts));

    if (rc != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* Write lockout timer residual (4 bytes). */
    rc = nvm_store_write(
        (uint16_t)NVM_KEY_SEC_LOCKOUT_MS,
        &lockout_ms, sizeof(lockout_ms));

    if (rc != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_security_nvm_clear(void)
{
    uds_status_t rc;

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    rc = nvm_store_delete((uint16_t)NVM_KEY_SEC_ATTEMPT_CTR);
    if (rc != UDS_STATUS_OK) {
        return rc;
    }

    rc = nvm_store_delete((uint16_t)NVM_KEY_SEC_LOCKOUT_MS);
    return rc;
}
