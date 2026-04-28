// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_access_rights.h
 *
 * PURPOSE: Data-driven UDS service access rights table.
 *
 * PHASE 5: Security Hardening [P5-ACL]
 *
 * This module replaces the hardcoded switch statement in uds_server.c's
 * srv_check_access_rights() with a data-driven table that OEMs can configure
 * at compile time without touching core stack source files.
 *
 * DESIGN:
 *   Each row in the access rights table describes the minimum session type
 *   and minimum security level required to execute a given SID.
 *
 *   The server calls uds_access_rights_check() before dispatching any
 *   service request. If no entry is found for a SID, the fallback policy
 *   (configurable: ALLOW or DENY) is applied.
 *
 * SESSION REQUIREMENT FIELD:
 *   UDS_ACCESS_SESSION_ANY      — service available in any session
 *   UDS_ACCESS_SESSION_NON_DEFAULT — service requires non-default session
 *   UDS_ACCESS_SESSION_PROGRAMMING — service requires programming session
 *   UDS_ACCESS_SESSION_EXTENDED   — service requires extended session or higher
 *
 * SECURITY LEVEL REQUIREMENT FIELD:
 *   UDS_ACCESS_SEC_NONE         — no security unlock required
 *   UDS_ACCESS_SEC_LEVEL_1      — security level 1 (0x01) must be unlocked
 *   UDS_ACCESS_SEC_LEVEL_2      — security level 2 (0x03) must be unlocked
 *
 * OEM INTEGRATION:
 *   Define your own access rights table as a const array and pass it to
 *   uds_server_cfg_t.access_rights_table / access_rights_count.
 *
 *   Example: require security level 1 for SID 0x2E (WriteDataByIdentifier)
 *
 *     static const uds_access_rights_entry_t my_acl[] = {
 *         { UDS_SID_WRITE_DATA_BY_ID, UDS_ACCESS_SESSION_NON_DEFAULT,
 *           UDS_ACCESS_SEC_LEVEL_1 },
 *         ...
 *     };
 *
 *   The canonical default table is defined in uds_access_rights.c and
 *   exported as g_uds_default_access_rights[].
 *
 * FALLBACK POLICY:
 *   UDS_ACCESS_POLICY_ALLOW — SIDs not in the table are allowed (permissive).
 *   UDS_ACCESS_POLICY_DENY  — SIDs not in the table are denied (strict).
 *
 *   The default table uses ALLOW to maintain backward compatibility with
 *   Phase 1–4 behavior. OEMs SHOULD switch to DENY in production.
 *
 * THREAD SAFETY:
 *   Read-only after initialization. Safe to call from any context.
 *
 * SAFETY  : Access control is safety-relevant. ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_ACCESS_RIGHTS_H
#define UDS_ACCESS_RIGHTS_H

#include "uds_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Session requirement enum
 * -------------------------------------------------------------------------- */

/**
 * @brief Minimum session type required to execute a service.
 */
typedef enum uds_access_session_req {
    /** Service is allowed in any session (including Default). */
    UDS_ACCESS_SESSION_ANY           = 0x00U,
    /** Service requires any non-Default session (Extended or Programming). */
    UDS_ACCESS_SESSION_NON_DEFAULT   = 0x01U,
    /** Service requires Extended diagnostic session or Programming session. */
    UDS_ACCESS_SESSION_EXTENDED      = 0x02U,
    /** Service requires Programming session only. */
    UDS_ACCESS_SESSION_PROGRAMMING   = 0x03U,
} uds_access_session_req_t;

/* --------------------------------------------------------------------------
 * Security level requirement enum
 * -------------------------------------------------------------------------- */

/**
 * @brief Minimum security level that must be unlocked to execute a service.
 */
typedef enum uds_access_sec_req {
    /** No security unlock required. */
    UDS_ACCESS_SEC_NONE    = 0x00U,
    /** Security level 1 (seed/key level 0x01/0x02) must be unlocked. */
    UDS_ACCESS_SEC_LEVEL_1 = 0x01U,
    /** Security level 2 (seed/key level 0x03/0x04) must be unlocked. */
    UDS_ACCESS_SEC_LEVEL_2 = 0x02U,
} uds_access_sec_req_t;

/* --------------------------------------------------------------------------
 * Fallback policy enum
 * -------------------------------------------------------------------------- */

/**
 * @brief Policy applied when a SID has no entry in the access rights table.
 */
typedef enum uds_access_fallback_policy {
    /**
     * @brief Allow services not found in the table.
     *
     * Backward-compatible with hardcoded Phase 1–4 behavior.
     * NOT recommended for production deployments.
     */
    UDS_ACCESS_POLICY_ALLOW = 0x00U,

    /**
     * @brief Deny services not found in the table.
     *
     * Recommended for production. Requires all supported SIDs to have
     * explicit entries in the access rights table.
     */
    UDS_ACCESS_POLICY_DENY  = 0x01U,
} uds_access_fallback_policy_t;

/* --------------------------------------------------------------------------
 * Access rights table entry
 * -------------------------------------------------------------------------- */

/**
 * @brief One row in the UDS access rights table.
 *
 * Describes the access requirements for a single service identifier.
 *
 * MEMORY: const, ROM-resident. No heap allocation.
 */
typedef struct uds_access_rights_entry {
    /** UDS service identifier (e.g. UDS_SID_READ_DATA_BY_ID = 0x22). */
    uint8_t service_id;

    /** Minimum diagnostic session required. */
    uds_access_session_req_t session_req;

    /** Minimum security level required (UDS_ACCESS_SEC_NONE if no unlock needed). */
    uds_access_sec_req_t sec_req;
} uds_access_rights_entry_t;

/* --------------------------------------------------------------------------
 * Default access rights table (canonical for this ECU profile)
 * -------------------------------------------------------------------------- */

/**
 * @brief Number of entries in the default access rights table.
 *
 * Used with g_uds_default_access_rights[].
 */
#define UDS_DEFAULT_ACCESS_RIGHTS_COUNT  (10U)

/**
 * @brief Canonical default access rights table for the reference ECU profile.
 *
 * Defined in uds_access_rights.c. Pass to uds_server_cfg_t to activate.
 *
 * All ten registered SIDs are enumerated explicitly. The fallback policy
 * for this table is ALLOW (backward compatibility). OEMs SHOULD define
 * their own stricter table with DENY fallback.
 */
extern const uds_access_rights_entry_t g_uds_default_access_rights[UDS_DEFAULT_ACCESS_RIGHTS_COUNT];

/* --------------------------------------------------------------------------
 * Access check API
 * -------------------------------------------------------------------------- */

/**
 * @brief Check whether the current session and security state allow a service.
 *
 * Searches the provided table for the requested service_id. If found,
 * validates the current session and security level against the entry's
 * requirements. If not found, applies the fallback_policy.
 *
 * @param[in] table           Access rights table (array of entries).
 * @param[in] count           Number of entries in table.
 * @param[in] fallback_policy Policy when SID has no entry.
 * @param[in] service_id      UDS service identifier from the request.
 * @param[in] active_session  Currently active diagnostic session.
 * @param[in] active_sec_level Currently unlocked security level
 *                             (UDS_SEC_LEVEL_UNLOCKED = 0x00 if none).
 *
 * @return UDS_STATUS_OK if access is permitted.
 * @return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION if session requirement
 *         is not met.
 * @return UDS_STATUS_ERR_SEC_ACCESS_DENIED if security level requirement is not
 *         met.
 * @return UDS_STATUS_ERR_NULL_PTR if table is NULL and count > 0.
 *
 * @note SAFETY: This function must be called before every service dispatch.
 *               Skipping the call leaves the ECU without access control.
 */
uds_status_t uds_access_rights_check(
    const uds_access_rights_entry_t *table,
    uint8_t                          count,
    uds_access_fallback_policy_t     fallback_policy,
    uint8_t                          service_id,
    uds_session_type_t               active_session,
    uint8_t                          active_sec_level
);

#ifdef __cplusplus
}
#endif

#endif /* UDS_ACCESS_RIGHTS_H */
