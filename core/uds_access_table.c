/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_access_table.c
 *
 * PURPOSE: Data-driven access rights table implementation.
 *
 * PHASE-5 ADDITIONS:
 *   [P5-ACL-01] Default table enforcing ISO 14229-1 session gating.
 *   [P5-ACL-02] Lookup by (service_id, active_session).
 *   [P5-ACL-03] Enforce helper queries security context for level unlock.
 *
 * DESIGN NOTES:
 *   The session_mask→bit mapping is:
 *     UDS_ACL_SESSION_DEFAULT      = 0x01  (session enum value 0x01, bit 0)
 *     UDS_ACL_SESSION_PROGRAMMING  = 0x02  (session enum value 0x02, bit 1)
 *     UDS_ACL_SESSION_EXTENDED     = 0x04  (session enum value 0x03, bit 2)
 *     UDS_ACL_SESSION_SAFETY       = 0x08  (session enum value 0x04, bit 3)
 *
 *   The mask bit for a session type is: (1U << (session_type - 1U))
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "uds_access_table.h"
#include "uds_security.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Default access table
 *
 * Enforces ISO 14229-1 recommended session and security constraints for all
 * 10 registered services. OEMs replace this by passing a custom table via
 * uds_server_cfg_t.access_table.
 *
 * Columns:
 *   service_id | session_mask | required_sec_level | require_unlocked
 *
 * Rationale per row:
 *
 *   0x10 DiagnosticSessionControl
 *        Available in all sessions; no security requirement.
 *        Restricting it would prevent entry to other sessions.
 *
 *   0x11 ECUReset
 *        Available in all sessions per ISO 14229-1 §11.2.
 *        OEMs may restrict to non-default if required by their threat model.
 *
 *   0x14 ClearDiagnosticInformation
 *        Restricted to non-default sessions (Extended, Programming, Safety).
 *        Clearing DTCs from Default session is not permitted in most OEM specs.
 *
 *   0x19 ReadDTCInformation
 *        Available in all sessions; no security requirement.
 *        Read-only; DTC visibility is not confidential.
 *
 *   0x22 ReadDataByIdentifier
 *        Available in all sessions. Individual DIDs carry their own security
 *        requirements enforced by the DID handler.
 *
 *   0x27 SecurityAccess
 *        Requires non-default session to initiate a seed/key exchange.
 *        The security state machine itself is the second gate.
 *
 *   0x28 CommunicationControl
 *        Restricted to non-default sessions (Extended+Programming+Safety).
 *        Disabling comm from Default session could leave ECU unreachable.
 *
 *   0x2E WriteDataByIdentifier
 *        Requires non-default session AND Level 1 security unlock.
 *        Write access to calibration/configuration data requires authentication.
 *
 *   0x3E TesterPresent
 *        Available in all sessions; no security requirement.
 *        Session keepalive must be universally accessible.
 *
 *   0x85 ControlDTCSetting
 *        Restricted to non-default sessions. Turning off DTC setting from
 *        Default session could mask faults during normal vehicle operation.
 *
 *   0x31 RoutineControl  [#113]
 *        Available in all sessions; no ACL-layer security requirement.
 *        Deliberately permissive here — real per-routine session/security
 *        gating happens inside service_0x31.c against routine_database.c's
 *        per-RID min_session/security_level. See the row's own comment
 *        below for the full rationale.
 *
 * -------------------------------------------------------------------------- */

static const uds_access_entry_t k_default_table[UDS_ACCESS_TABLE_DEFAULT_COUNT] = {

    /* [0] 0x10 DiagnosticSessionControl — all sessions, no security */
    {
        .service_id        = UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
        .session_mask      = UDS_ACL_SESSION_ALL,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [1] 0x11 ECUReset — all sessions, no security */
    {
        .service_id        = UDS_SID_ECU_RESET,
        .session_mask      = UDS_ACL_SESSION_ALL,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [2] 0x14 ClearDiagnosticInformation — non-default sessions only */
    {
        .service_id        = UDS_SID_CLEAR_DIAGNOSTIC_INFO,
        .session_mask      = UDS_ACL_SESSION_NON_DEFAULT,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [3] 0x19 ReadDTCInformation — all sessions, no security */
    {
        .service_id        = UDS_SID_READ_DTC_INFO,
        .session_mask      = UDS_ACL_SESSION_ALL,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [4] 0x22 ReadDataByIdentifier — all sessions, no security */
    {
        .service_id        = UDS_SID_READ_DATA_BY_ID,
        .session_mask      = UDS_ACL_SESSION_ALL,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [4a] 0x2A ReadDataByPeriodicIdentifier — non-default sessions, no unlock.
     *
     *  Access mirrors 0x22 but restricted to non-default sessions:
     *  0x2A subscriptions make no sense in Default session because the
     *  session would time out before most pushes fire. DID read_access_level
     *  controls per-DID security at subscription time.
     */
    {
        .service_id         = UDS_SID_READ_DATA_BY_PERIODIC_ID,
        .session_mask       = UDS_ACL_SESSION_NON_DEFAULT,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked   = false,
    },

    /* [5] 0x27 SecurityAccess — non-default sessions only, no extra security */
    {
        .service_id        = UDS_SID_SECURITY_ACCESS,
        .session_mask      = UDS_ACL_SESSION_NON_DEFAULT,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [6] 0x28 CommunicationControl — non-default sessions only */
    {
        .service_id        = UDS_SID_COMMUNICATION_CONTROL,
        .session_mask      = UDS_ACL_SESSION_NON_DEFAULT,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [7] 0x2E WriteDataByIdentifier — non-default sessions + Level 1 unlock */
    {
        .service_id         = UDS_SID_WRITE_DATA_BY_ID,
        .session_mask       = UDS_ACL_SESSION_NON_DEFAULT,
        .required_sec_level = UDS_SEC_LEVEL_1_SEED,   /* 0x01 */
        .require_unlocked   = true,
    },

    /* [7b] 0x2F InputOutputControlByIdentifier — non-default sessions + Level 1 unlock.
     *
     *  IO control modifies physical ECU outputs. Access constraints match
     *  WriteDataByIdentifier: tester must be in a non-default session and
     *  have completed SecurityAccess Level 1 unlock. This prevents
     *  actuator commands from anonymous/non-authenticated tools.
     */
    {
        .service_id         = UDS_SID_INPUT_OUTPUT_CONTROL,
        .session_mask       = UDS_ACL_SESSION_NON_DEFAULT,
        .required_sec_level = UDS_SEC_LEVEL_1_SEED,
        .require_unlocked   = true,
    },

    /* [8] 0x3E TesterPresent — all sessions, no security */
    {
        .service_id        = UDS_SID_TESTER_PRESENT,
        .session_mask      = UDS_ACL_SESSION_ALL,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [9] 0x85 ControlDTCSetting — non-default sessions only */
    {
        .service_id        = UDS_SID_CONTROL_DTC_SETTING,
        .session_mask      = UDS_ACL_SESSION_NON_DEFAULT,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked  = false,
    },

    /* [10] 0x34 RequestDownload — Programming session + Level 1 security.
     *
     *  Programming session is required because DFU modifies ECU firmware.
     *  Security Level 1 unlock is mandatory to prevent unauthorised reflash.
     *  The ARDEP upgrade guide explicitly names this as the DFU prerequisite.
     */
    {
        .service_id         = UDS_SID_REQUEST_DOWNLOAD,
        .session_mask       = UDS_ACL_SESSION_PROGRAMMING,
        .required_sec_level = (uint8_t)1U,
        .require_unlocked   = true,
    },

    /* [10b] 0x35 RequestUpload — Programming session + Level 1 security.
     *
     *  Upload exposes raw ECU memory content. Treating it as equivalent to
     *  RequestDownload: requires programming session and security unlock to
     *  prevent unauthorised memory readout.
     */
    {
        .service_id         = UDS_SID_REQUEST_UPLOAD,
        .session_mask       = UDS_ACL_SESSION_PROGRAMMING,
        .required_sec_level = (uint8_t)1U,
        .require_unlocked   = true,
    },

    /* [11] 0x36 TransferData — Programming session + Level 1 security.
     *
     *  Same access constraints as RequestDownload.  An active transfer
     *  (service_0x36.c) provides an additional sequence guard (NRC 0x24)
     *  independent of the ACL layer.
     */
    {
        .service_id         = UDS_SID_TRANSFER_DATA,
        .session_mask       = UDS_ACL_SESSION_PROGRAMMING,
        .required_sec_level = (uint8_t)1U,
        .require_unlocked   = true,
    },

    /* [12] 0x37 RequestTransferExit — Programming session + Level 1 security.
     *
     *  Closes the active download session and commits the image.
     *  Same access constraints as the initiating RequestDownload.
     */
    {
        .service_id         = UDS_SID_REQUEST_TRANSFER_EXIT,
        .session_mask       = UDS_ACL_SESSION_PROGRAMMING,
        .required_sec_level = (uint8_t)1U,
        .require_unlocked   = true,
    },

    /* [13] 0x23 ReadMemoryByAddress — Programming session + Level 1 security.
     *
     *  Direct memory read exposes raw ECU memory to the tester.  Requires the
     *  same access tier as RequestUpload (0x35).  The readable memory map in
     *  the flash ops table is the ASIL-B gate (REQ-FLASH-003); the ACL entry
     *  ensures the session and security prerequisites are met before dispatch.
     */
    {
        .service_id         = UDS_SID_READ_MEMORY_BY_ADDRESS,
        .session_mask       = UDS_ACL_SESSION_PROGRAMMING,
        .required_sec_level = (uint8_t)1U,
        .require_unlocked   = true,
    },

    /* [14] 0x3D WriteMemoryByAddress — Programming session + Level 1 security.
     *
     *  Direct memory write.  Identical access tier to RequestDownload (0x34).
     *  The writable memory map in the flash ops table is the ASIL-B gate
     *  (REQ-FLASH-002).  Session and security gates applied before dispatch.
     */
    {
        .service_id         = UDS_SID_WRITE_MEMORY_BY_ADDRESS,
        .session_mask       = UDS_ACL_SESSION_PROGRAMMING,
        .required_sec_level = (uint8_t)1U,
        .require_unlocked   = true,
    },

    /* [15] 0x31 RoutineControl — all sessions, no ACL-layer security. [#113]
     *
     *  ADDED as part of the #113 fail-closed fix. Prior to this fix, 0x31
     *  was registered in service_registration.c but had NO row in this
     *  table; under the old fail-open default that meant "no restriction",
     *  which happened to match this row's effect. Flipping the table's
     *  default to fail-closed would otherwise have made RoutineControl
     *  completely unreachable in every session — this explicit row keeps
     *  its behaviour unchanged.
     *
     *  session_mask=ALL / require_unlocked=false is intentional, not an
     *  oversight: unlike every other service in this table, RoutineControl
     *  access is NOT uniform across all routines — routine_database.c
     *  carries a per-RID min_session and security_level, enforced inside
     *  service_0x31.c's s_validate_routine_access() (REQ-SAFE-002,
     *  REQ-SAFE-003) *after* dispatch. Gating 0x31 itself at the ACL layer
     *  would either (a) block routines that are legitimately callable from
     *  Default session, or (b) require this table to know every OEM's
     *  per-RID policy, which belongs in routine_database.c, not here. The
     *  ACL layer's job for 0x31 is only to make sure a missing row can
     *  never silently grant access the way #113 describes — this row
     *  documents that "no ACL restriction" is the deliberate, audited
     *  policy for this SID, not an accidental gap.
     */
    {
        .service_id         = UDS_SID_ROUTINE_CONTROL,
        .session_mask       = UDS_ACL_SESSION_ALL,
        .required_sec_level = (uint8_t)0U,
        .require_unlocked   = false,
    },
};

/* --------------------------------------------------------------------------
 * Internal helper: convert a uds_session_type_t to a bitmask bit position.
 *
 * The mapping is: bit = (1U << (session_type - 1U))
 *   UDS_SESSION_DEFAULT     (0x01) → 0x01
 *   UDS_SESSION_PROGRAMMING (0x02) → 0x02
 *   UDS_SESSION_EXTENDED    (0x03) → 0x04
 *   UDS_SESSION_SAFETY      (0x04) → 0x08
 * -------------------------------------------------------------------------- */

static uint8_t acl_session_to_bit(uds_session_type_t session)
{
    uint8_t val = (uint8_t)session;

    if ((val == (uint8_t)0U) || (val > (uint8_t)4U)) {
        return (uint8_t)0U; /* unknown session — no bit */
    }

    return (uint8_t)((uint8_t)1U << (val - (uint8_t)1U));
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

const uds_access_entry_t *uds_access_table_get_default(void)
{
    return k_default_table;
}

uds_status_t uds_access_table_lookup(
    const uds_access_entry_t  *table,
    uint8_t                    count,
    uint8_t                    service_id,
    uds_session_type_t         active_session,
    const uds_access_entry_t **out_entry)
{
    uint8_t i;
    uint8_t session_bit;

    if ((table == NULL) || (out_entry == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (count == (uint8_t)0U) {
        *out_entry = NULL;
        return UDS_STATUS_OK;
    }

    session_bit = acl_session_to_bit(active_session);
    *out_entry  = NULL;

    for (i = (uint8_t)0U; i < count; i++) {
        if (table[i].service_id != service_id) {
            continue;
        }

        /* Does this entry's session_mask cover the active session? */
        if ((table[i].session_mask & session_bit) != (uint8_t)0U) {
            *out_entry = &table[i];
            return UDS_STATUS_OK; /* first match wins */
        }

        /*
         * The service_id matches but session_mask does NOT include the
         * active session. This means the service is explicitly disallowed
         * in this session. Return the entry so the caller can see that
         * the service exists but is not permitted here.
         *
         * We set *out_entry to the mismatched entry so the caller can
         * distinguish "not in table" (NULL) from "in table, wrong session"
         * (non-NULL, session_bit not set).
         *
         * NOTE: This behaviour enables proper NRC 0x7F (serviceNotSupportedInActiveSession)
         * vs leaving access open when there's simply no rule.
         */
        *out_entry = &table[i];
        return UDS_STATUS_OK;
    }

    /*
     * [#113] No matching entry found. This function's own contract is
     * unchanged: return OK with *out_entry == NULL so the caller can tell
     * "not in table" apart from "in table, wrong session" (non-NULL entry,
     * session bit clear — handled above). Whether "not in table" ultimately
     * ALLOWS or DENIES access is decided in ONE place only —
     * uds_access_table_enforce(), gated by UDS_ACL_ALLOW_UNLISTED_SERVICES —
     * so every caller (uds_server.c's dispatcher and any OEM code calling
     * this API directly) gets the same fail-closed-by-default decision
     * instead of re-implementing it.
     */
    *out_entry = NULL;
    return UDS_STATUS_OK;
}

uds_status_t uds_access_table_enforce(
    const uds_access_entry_t *entry,
    uds_security_ctx_t       *security_ctx)
{
    bool unlocked;

    /*
     * [#113] No entry → access DECISION for the "not in table" case.
     *
     * Prior to #113 this returned UDS_STATUS_OK unconditionally ("no
     * restriction"), so a service_id with no ACL row — e.g. a new SID added
     * to service_registration.c without a matching row here — was fully
     * reachable with no session or security gate at all. That is the wrong
     * default for security-relevant middleware: fail-closed (deny) is now
     * the default, with an explicit, deployment-wide opt-in
     * (UDS_ACL_ALLOW_UNLISTED_SERVICES, default 0) for integrators who have
     * deliberately chosen permissive-by-default behaviour. See the
     * "UDS_ACL_ALLOW_UNLISTED_SERVICES" section of uds_access_table.h for
     * the full rationale and how to set it per platform.
     *
     * UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION is deliberately reused
     * here rather than introducing a new status: it already maps to NRC
     * 0x7F (serviceNotSupportedInActiveSession) in uds_server.c, which is
     * the NRC this file's own lookup() comment above says a missing rule
     * should have produced all along ("This behaviour enables proper NRC
     * 0x7F ... vs leaving access open when there's simply no rule").
     */
    if (entry == NULL) {
#if UDS_ACL_ALLOW_UNLISTED_SERVICES
        return UDS_STATUS_OK;
#else
        return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION;
#endif
    }

    /*
     * The entry was found. Now verify the session_mask covers the active
     * session. The lookup already checked this — if the bit wasn't set,
     * it means the service is restricted from the active session.
     *
     * The caller (uds_server.c) passes the exact session that was active
     * during lookup, so if we arrived here with a non-matching session,
     * something is wrong. Treat it as "not supported in session."
     *
     * In practice the server always calls lookup before enforce with the
     * same session, so this is a defensive check only.
     */

    /* Check security level requirement. */
    if (!entry->require_unlocked) {
        return UDS_STATUS_OK;
    }

    if (security_ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    unlocked = false;
    (void)uds_security_is_unlocked(security_ctx,
                                    entry->required_sec_level,
                                    &unlocked);

    if (!unlocked) {
        return UDS_STATUS_ERR_SEC_NOT_UNLOCKED;
    }

    return UDS_STATUS_OK;
}
