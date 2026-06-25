/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_access_table.h
 *
 * PURPOSE: Data-driven access rights table for the UDS server dispatcher.
 *
 * PHASE-5 ADDITIONS:
 *   [P5-ACL-01] Replaces the hardcoded switch in srv_check_access_rights()
 *               with a table that maps (service_id, session_type) → required
 *               security level.
 *   [P5-ACL-02] Provides a default table covering all 10 registered services,
 *               matching ISO 14229-1 recommended restrictions.
 *   [P5-ACL-03] Allows OEMs to supply a custom table via uds_server_cfg_t
 *               without modifying any stack source file.
 *
 * TABLE STRUCTURE:
 *
 *   Each entry describes ONE (service_id, session) combination:
 *
 *     {
 *       .service_id      = UDS_SID_WRITE_DATA_BY_ID,
 *       .session_mask    = UDS_ACL_SESSION_EXTENDED | UDS_ACL_SESSION_PROGRAMMING,
 *       .required_sec    = UDS_SEC_LEVEL_1_SEED,   -- must have unlocked level 1
 *       .require_unlocked = true
 *     }
 *
 *   session_mask is a bitfield — one bit per session type. Multiple sessions
 *   can share the same access rule in a single entry.
 *
 *   A service with no matching entry is ALLOWED (the default is permissive so
 *   that service-level guards in individual handlers remain authoritative).
 *   The table only ADDS restrictions on top of the handler's own checks.
 *
 * LOOKUP ALGORITHM:
 *   1. Walk the table from entry 0 to entry (count - 1).
 *   2. If entry.service_id matches AND (1 << active_session) & entry.session_mask != 0:
 *        → enforce entry.required_sec + entry.require_unlocked
 *   3. If no matching entry: access granted.
 *   4. First match wins — put more specific rules before general ones.
 *
 * DEFAULT TABLE (uds_access_table_get_default() / UDS_ACCESS_TABLE_DEFAULT):
 *
 *   SID  0x10 DiagnosticSessionControl — all sessions, no security
 *   SID  0x11 ECUReset                 — all sessions, no security
 *   SID  0x14 ClearDiagnosticInfo      — Extended+Programming, no security
 *   SID  0x19 ReadDTCInformation       — all sessions, no security
 *   SID  0x22 ReadDataByIdentifier     — all sessions, no security
 *   SID  0x27 SecurityAccess           — Extended+Programming, no security
 *                                         (security guard is inside the handler)
 *   SID  0x28 CommunicationControl     — Extended+Programming, no security
 *   SID  0x2E WriteDataByIdentifier    — Extended+Programming, Level 1 required
 *   SID  0x3E TesterPresent            — all sessions, no security
 *   SID  0x85 ControlDTCSetting        — Extended+Programming, no security
 *
 * OEM CUSTOMISATION:
 *   Declare a custom uds_access_entry_t[] array and pass it via the
 *   access_table / access_table_count fields in uds_server_cfg_t.
 *   NULL leaves the default table active.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef UDS_ACCESS_TABLE_H
#define UDS_ACCESS_TABLE_H

#include "uds_types.h"
#include "uds_security.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Session bit-mask constants
 *
 * One bit per session type. Use OR to combine multiple sessions:
 *   UDS_ACL_SESSION_EXTENDED | UDS_ACL_SESSION_PROGRAMMING
 * -------------------------------------------------------------------------- */

/**
 * @brief Bit representing UDS_SESSION_DEFAULT (0x01) in a session_mask.
 * @note Value: 1 << (0x01 - 1) = bit 0.
 */
#define UDS_ACL_SESSION_DEFAULT      (0x01U)   /* bit 0 */

/**
 * @brief Bit representing UDS_SESSION_PROGRAMMING (0x02) in a session_mask.
 * @note Value: 1 << (0x02 - 1) = bit 1.
 */
#define UDS_ACL_SESSION_PROGRAMMING  (0x02U)   /* bit 1 */

/**
 * @brief Bit representing UDS_SESSION_EXTENDED (0x03) in a session_mask.
 * @note Value: 1 << (0x03 - 1) = bit 2.
 */
#define UDS_ACL_SESSION_EXTENDED     (0x04U)   /* bit 2 */

/**
 * @brief Bit representing UDS_SESSION_SAFETY_SYSTEM (0x04) in a session_mask.
 * @note Value: 1 << (0x04 - 1) = bit 3.
 */
#define UDS_ACL_SESSION_SAFETY       (0x08U)   /* bit 3 */

/**
 * @brief Convenience mask matching all known session types.
 */
#define UDS_ACL_SESSION_ALL          (UDS_ACL_SESSION_DEFAULT     | \
                                      UDS_ACL_SESSION_PROGRAMMING | \
                                      UDS_ACL_SESSION_EXTENDED    | \
                                      UDS_ACL_SESSION_SAFETY)

/**
 * @brief Convenience mask for non-default sessions (Programming + Extended + Safety).
 */
#define UDS_ACL_SESSION_NON_DEFAULT  (UDS_ACL_SESSION_PROGRAMMING | \
                                      UDS_ACL_SESSION_EXTENDED    | \
                                      UDS_ACL_SESSION_SAFETY)

/* --------------------------------------------------------------------------
 * Access table entry
 * -------------------------------------------------------------------------- */

/**
 * @brief One row in the access rights table.
 *
 * Describes the constraints that apply when service_id is requested
 * from any session whose bit is set in session_mask.
 */
typedef struct uds_access_entry {
    /**
     * @brief UDS service identifier this rule applies to.
     * Use the UDS_SID_* constants from uds_types.h.
     */
    uint8_t service_id;

    /**
     * @brief Bitfield of sessions where this rule is active.
     * Combine UDS_ACL_SESSION_* constants with bitwise OR.
     * Zero means "never match" — useful for temporarily disabling a rule.
     */
    uint8_t session_mask;

    /**
     * @brief Security level that must be unlocked for the service to proceed.
     *
     * Interpreted only when require_unlocked is true.
     * Use UDS_SEC_LEVEL_1_SEED (0x01) for Level 1,
     *     UDS_SEC_LEVEL_2_SEED (0x03) for Level 2.
     * (Odd values match the ISO 14229-1 seed sub-function convention.)
     */
    uint8_t required_sec_level;

    /**
     * @brief If true, the required_sec_level must be unlocked before the
     * service is dispatched. If false, no security check is performed by
     * the dispatcher for this entry (individual handlers may still check).
     */
    bool require_unlocked;

} uds_access_entry_t;

/* --------------------------------------------------------------------------
 * Default access table size
 * -------------------------------------------------------------------------- */

/**
 * @brief Number of entries in the built-in default access rights table.
 */
#define UDS_ACCESS_TABLE_DEFAULT_COUNT  (15U)

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Return a pointer to the built-in default access rights table.
 *
 * The default table enforces the ISO 14229-1 recommended session gating
 * for all 10 registered services. OEMs who need stricter or looser rules
 * should define their own table and pass it via uds_server_cfg_t instead.
 *
 * @return Pointer to the first entry of the default table (never NULL).
 *         Table has UDS_ACCESS_TABLE_DEFAULT_COUNT entries.
 */
const uds_access_entry_t *uds_access_table_get_default(void);

/**
 * @brief Look up access constraints for a given (service_id, session) pair.
 *
 * Scans the provided table for the first entry whose service_id matches
 * and whose session_mask includes the active session.
 *
 * @param[in]  table         Pointer to the access table array.
 * @param[in]  count         Number of entries in table[].
 * @param[in]  service_id    SID of the request being dispatched.
 * @param[in]  active_session Current session type (UDS_SESSION_*).
 * @param[out] out_entry     Set to a pointer to the matched entry, or NULL
 *                           if no matching entry was found (access granted).
 *
 * @return UDS_STATUS_OK on success (check *out_entry for match result).
 * @return UDS_STATUS_ERR_NULL_PTR if table or out_entry is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if count is zero.
 *
 * @note out_entry is set to NULL when no matching entry exists — caller
 *       must treat a NULL out_entry as "no restriction, access granted".
 */
uds_status_t uds_access_table_lookup(
    const uds_access_entry_t *table,
    uint8_t                   count,
    uint8_t                   service_id,
    uds_session_type_t        active_session,
    const uds_access_entry_t **out_entry);

/**
 * @brief Enforce access constraints from a table lookup result.
 *
 * Given the output of uds_access_table_lookup(), query the security context
 * to determine whether the required level is unlocked.
 *
 * @param[in] entry        Matched entry from uds_access_table_lookup(),
 *                         or NULL (in which case UDS_STATUS_OK is returned).
 * @param[in] security_ctx Initialized security context to query.
 *
 * @return UDS_STATUS_OK if access is allowed (no entry, or security unlocked).
 * @return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION if session_mask
 *         implies this session is disallowed (entry exists but session was not
 *         expected to reach here — defensive check).
 * @return UDS_STATUS_ERR_SEC_NOT_UNLOCKED if require_unlocked is true and
 *         the required security level is not active.
 * @return UDS_STATUS_ERR_NULL_PTR if security_ctx is NULL and require_unlocked.
 */
uds_status_t uds_access_table_enforce(
    const uds_access_entry_t *entry,
    uds_security_ctx_t       *security_ctx);

#ifdef __cplusplus
}
#endif

#endif /* UDS_ACCESS_TABLE_H */
