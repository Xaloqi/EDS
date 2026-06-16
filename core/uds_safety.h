// File: core/uds_safety.h
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_safety.h
 *
 * PURPOSE: Safety interface for UDS/DID/DTC operations.
 *          Provides bounds checking, session gating, access control
 *          validation, and deterministic fail-safe entry points suitable
 *          for ISO 26262 ASIL-B software component design.
 *
 *          This header partitions safety-critical validation logic from
 *          non-safety application and tooling code. All safety-relevant
 *          UDS access paths MUST route through the functions declared here
 *          before invoking the DID database or service handlers.
 *
 * SAFETY CLASSIFICATION:
 *   ASIL : ASIL-B candidate (per ISO 26262-6:2018 §9)
 *   SIL  : Not applicable (functional safety scope only)
 *   FMEA : Input validation failures → NRC response, no unsafe state
 *
 * TRACEABILITY:
 *   REQ-SAFE-001 : All DID accesses shall be bounds-checked before execution.
 *   REQ-SAFE-002 : Session state shall be validated prior to service dispatch.
 *   REQ-SAFE-003 : Security level shall be checked before DID write operations.
 *   REQ-SAFE-004 : NULL pointer dereferences shall be prevented at all entry points.
 *   REQ-SAFE-005 : All initialisation shall be deterministic and sequenced.
 *   REQ-SAFE-006 : Diagnostic data_length fields shall match DID database records.
 *   REQ-SAFE-007 : Stack-local buffers shall never exceed compile-time bounds.
 *
 * USAGE:
 *   Include this header in any module that performs UDS session/DID/DTC
 *   validation. The implementation is in core/uds_safety.c.
 *
 *   Typical call sequence (e.g. inside service_0x22.c):
 *     uds_safety_check_null_ptr(req, "req");
 *     uds_safety_validate_session(session_ctx, UDS_SESSION_DEFAULT);
 *     uds_safety_validate_did_access(entry, active_session, active_level,
 *                                    UDS_SAFETY_ACCESS_READ);
 *     uds_safety_check_did_data_length(entry, resp_buf, buf_len);
 *
 * CODING STANDARD: MISRA C:2012 alignment intended.
 * VERSION        : 1.7.0
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef UDS_SAFETY_H
#define UDS_SAFETY_H

#include "uds_types.h"
#include "uds_session.h"
#include "uds_security.h"
#include "did_database.h"
#include "dtc_database.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Compile-time safety configuration
 *
 * These macros gate optional safety check categories.  All are ON by default.
 * Disable selectively in non-safety builds (e.g. unit test hosts) via
 * compile flags: -DUDS_SAFETY_DISABLE_SESSION_CHECKS=1
 *
 * WARNING: Disabling checks in production firmware is a safety deviation and
 *          requires formal justification per ISO 26262-8 §7.
 * ============================================================================= */

/**
 * @defgroup uds_safety_compile_cfg Compile-time safety feature flags
 * @{
 */

/** Enable session-state validation before service dispatch. (REQ-SAFE-002) */
#ifndef UDS_SAFETY_ENABLE_SESSION_CHECKS
#define UDS_SAFETY_ENABLE_SESSION_CHECKS    (1U)
#endif

/** Enable security-level validation before DID write operations. (REQ-SAFE-003) */
#ifndef UDS_SAFETY_ENABLE_SECURITY_CHECKS
#define UDS_SAFETY_ENABLE_SECURITY_CHECKS   (1U)
#endif

/** Enable DID data-length bounds checking. (REQ-SAFE-006) */
#ifndef UDS_SAFETY_ENABLE_BOUNDS_CHECKS
#define UDS_SAFETY_ENABLE_BOUNDS_CHECKS     (1U)
#endif

/** Enable NULL pointer guard checks. (REQ-SAFE-004) */
#ifndef UDS_SAFETY_ENABLE_NULL_CHECKS
#define UDS_SAFETY_ENABLE_NULL_CHECKS       (1U)
#endif

/** @} */

/* =============================================================================
 * Safety check result codes
 *
 * A subset of uds_status_t, restricted to codes emitted by safety checks.
 * Callers may cast to uds_status_t directly — values are identical.
 * ============================================================================= */

/**
 * @brief Result codes returned exclusively by safety check functions.
 *
 * All values correspond to matching entries in uds_status_t.
 * Using a dedicated alias improves traceability in code review.
 */
typedef uds_status_t uds_safety_result_t;

/* =============================================================================
 * Access type selector
 * ============================================================================= */

/**
 * @brief Selects whether a DID access is a read or write operation.
 *
 * Used by uds_safety_validate_did_access() to choose the correct session
 * and security level constraints from the DID descriptor.
 */
typedef enum uds_safety_access_type {
    UDS_SAFETY_ACCESS_READ  = 0U, /**< ReadDataByIdentifier (SID 0x22). */
    UDS_SAFETY_ACCESS_WRITE = 1U  /**< WriteDataByIdentifier (SID 0x2E). */
} uds_safety_access_type_t;

/* =============================================================================
 * Safety module context
 *
 * Tracks invocation counts and the last-seen safety violation code for
 * post-mortem diagnostics.  Statically allocated in uds_safety.c.
 * ============================================================================= */

/**
 * @brief Safety module runtime counters and state.
 *
 * All fields are read-only to external modules; use the accessor API below.
 * SAFETY: Must not be reset in normal operation — counters support failure
 *         analysis and are persisted across session transitions.
 */
typedef struct uds_safety_ctx {
    bool     initialized;               /**< Module initialisation guard.           */
    uint32_t null_check_violations;     /**< Count of NULL pointer check failures.  */
    uint32_t session_check_violations;  /**< Count of session gate violations.      */
    uint32_t security_check_violations; /**< Count of security level violations.    */
    uint32_t bounds_check_violations;   /**< Count of data-length bounds failures.  */
    uint32_t total_checks_performed;    /**< Total number of checks executed.       */
    uds_status_t last_violation_code;   /**< Status code of most recent violation.  */
    /**
     * [HIGH-2 FIX] Count of platform-layer safety events recorded via
     * uds_safety_record_platform_violation().  Incremented when a hardware
     * subsystem that is part of the security path degrades at runtime:
     *   - TRNG callback registered but returning non-OK (entropy source fault)
     * Saturating counter — never wraps.  Readable via uds_safety_get_ctx().
     * A non-zero value in field firmware indicates hardware degradation that
     * must be investigated; it is intentionally separate from
     * security_check_violations (which counts bad key attempts).
     * TRACEABILITY: SEC-TRNG-FAULT-01 / HIGH-2
     */
    uint32_t platform_violations;       /**< Count of platform-layer safety events. */
} uds_safety_ctx_t;

/* =============================================================================
 * Public API — Module lifecycle
 * ============================================================================= */

/**
 * @brief Initialize the UDS safety module.
 *
 * Resets all violation counters and sets the module to operational state.
 * Must be called once during system startup, before any safety checks.
 *
 * TRACEABILITY: REQ-SAFE-005
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 *
 * @note SAFETY: Must be called from initialisation context, not from ISR.
 */
uds_status_t uds_safety_init(void);

/**
 * @brief Return a const pointer to the safety module runtime context.
 *
 * Allows external modules (e.g. DTC logger, test framework) to read
 * violation counters without direct access to the static context.
 *
 * @return Pointer to the safety context (never NULL after uds_safety_init()).
 * @return NULL if uds_safety_init() has not been called.
 */
const uds_safety_ctx_t *uds_safety_get_ctx(void);

/**
 * @brief [HIGH-2 FIX] Record a platform-layer safety violation.
 *
 * Increments platform_violations (saturating) and sets last_violation_code.
 * Called by hardware-adjacent modules (e.g. uds_security_algo.c) when a
 * registered hardware callback fails at runtime — specifically when the
 * TRNG callback (s_rng_cb) returns non-OK during seed generation.
 *
 * Rationale for a dedicated counter (not reusing security_check_violations):
 * A TRNG hardware fault is a platform event, not a protocol violation.  Field
 * diagnostics that read security_check_violations to count brute-force unlock
 * attempts must not be contaminated by unrelated hardware fault events.
 *
 * Safe to call before uds_safety_init() — silently ignored if not initialised,
 * so early-startup hardware faults are not lost.
 *
 * @param[in] code  Status code that caused the platform fault.
 *                  Pass UDS_STATUS_ERR_PLATFORM for TRNG failures.
 *
 * TRACEABILITY: SEC-TRNG-FAULT-01 / HIGH-2
 */
void uds_safety_record_platform_violation(uds_status_t code);

/**
 * @brief Reset violation counters (for test harnesses only).
 *
 * SAFETY: Must NOT be called in production firmware. Resetting counters
 *         invalidates failure history required for safety analysis.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if module not initialized.
 */
uds_status_t uds_safety_reset_counters(void);

/* =============================================================================
 * Public API — NULL pointer checks
 *
 * TRACEABILITY: REQ-SAFE-004
 * ============================================================================= */

/**
 * @brief Assert that a pointer is non-NULL.
 *
 * Increments null_check_violations if ptr is NULL.
 * Records UDS_STATUS_ERR_NULL_PTR as last_violation_code.
 *
 * @param[in] ptr       Pointer to validate.
 * @param[in] ptr_name  Symbolic name for logging (pass __func__ or field name).
 *
 * @return UDS_STATUS_OK if ptr is non-NULL.
 * @return UDS_STATUS_ERR_NULL_PTR if ptr is NULL.
 *
 * @note Compile-gate: no-op if UDS_SAFETY_ENABLE_NULL_CHECKS == 0.
 */
uds_safety_result_t uds_safety_check_null_ptr(
    const void *ptr,
    const char *ptr_name
);

/* =============================================================================
 * Public API — Session validation
 *
 * TRACEABILITY: REQ-SAFE-002
 * ============================================================================= */

/**
 * @brief Validate that the active session satisfies the minimum requirement.
 *
 * Session ordinal mapping (ISO 14229-1 §9):
 *   Default      (0x01) ordinal 1
 *   Programming  (0x02) ordinal 2
 *   Extended     (0x03) ordinal 3
 *   SafetySystem (0x04) ordinal 4
 *
 * A service requiring Extended session (ordinal 3) will reject Default
 * (ordinal 1) and Programming (ordinal 2) but accept SafetySystem (ordinal 4).
 *
 * @param[in] session_ctx   Active session context (must be initialised).
 * @param[in] required      Minimum session type required to proceed.
 *
 * @return UDS_STATUS_OK if the active session meets the requirement.
 * @return UDS_STATUS_ERR_NULL_PTR if session_ctx is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if session_ctx not initialized.
 * @return UDS_STATUS_ERR_SESSION_INVALID if active session is below required.
 *
 * @note Compile-gate: always returns UDS_STATUS_OK if
 *       UDS_SAFETY_ENABLE_SESSION_CHECKS == 0.
 */
uds_safety_result_t uds_safety_validate_session(
    const uds_session_ctx_t *session_ctx,
    uds_session_type_t       required
);

/**
 * @brief Check whether the active session permits a specific service.
 *
 * Maps service IDs to their minimum required session per ISO 14229-1.
 * Supported service IDs: 0x10, 0x11, 0x22, 0x27, 0x2E, 0x3E.
 *
 * @param[in] session_ctx   Active session context.
 * @param[in] service_id    UDS service identifier byte.
 *
 * @return UDS_STATUS_OK if the service is permitted in the active session.
 * @return UDS_STATUS_ERR_NULL_PTR if session_ctx is NULL.
 * @return UDS_STATUS_ERR_SESSION_INVALID if session does not permit service.
 * @return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED if service_id is unknown.
 */
uds_safety_result_t uds_safety_check_service_in_session(
    const uds_session_ctx_t *session_ctx,
    uint8_t                  service_id
);

/* =============================================================================
 * Public API — DID access control
 *
 * TRACEABILITY: REQ-SAFE-001, REQ-SAFE-003
 * ============================================================================= */

/**
 * @brief Validate that a DID access is permitted in the current context.
 *
 * Checks:
 *   1. entry is non-NULL (REQ-SAFE-004).
 *   2. access_type flag is set on the DID entry (read/write capability).
 *   3. Active session meets the DID's min_session requirement (REQ-SAFE-002).
 *   4. If access_type == WRITE: active security level meets write_access_level
 *      from the DID descriptor (REQ-SAFE-003).
 *   5. If access_type == READ and read_access_level > 0: security check (REQ-SAFE-003).
 *
 * @param[in] entry          DID database entry (from did_database_find()).
 * @param[in] session_ctx    Active session context.
 * @param[in] security_ctx   Active security context.
 * @param[in] access_type    UDS_SAFETY_ACCESS_READ or UDS_SAFETY_ACCESS_WRITE.
 *
 * @return UDS_STATUS_OK if all checks pass.
 * @return UDS_STATUS_ERR_NULL_PTR if any required pointer is NULL.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if DID does not support access_type.
 * @return UDS_STATUS_ERR_SESSION_INVALID if session requirement not met.
 * @return UDS_STATUS_ERR_SEC_NOT_UNLOCKED if security level requirement not met.
 */
uds_safety_result_t uds_safety_validate_did_access(
    const did_entry_t        *entry,
    const uds_session_ctx_t  *session_ctx,
    const uds_security_ctx_t *security_ctx,
    uds_safety_access_type_t  access_type
);

/**
 * @brief Look up a DID and validate it exists in the database.
 *
 * Centralises the "find and verify non-NULL" pattern that service handlers
 * would otherwise each implement inline.
 *
 * @param[in]  did_id     16-bit DID identifier.
 * @param[out] out_entry  Set to the found DID entry on success.
 *
 * @return UDS_STATUS_OK if DID was found.
 * @return UDS_STATUS_ERR_NULL_PTR if out_entry is NULL.
 * @return UDS_STATUS_ERR_DID_NOT_FOUND if DID is not in the database.
 */
uds_safety_result_t uds_safety_find_did(
    uint16_t          did_id,
    const did_entry_t **out_entry
);

/* =============================================================================
 * Public API — Data length / buffer bounds checking
 *
 * TRACEABILITY: REQ-SAFE-006, REQ-SAFE-007
 * ============================================================================= */

/**
 * @brief Validate that a buffer is large enough to hold DID data.
 *
 * Compares entry->data_length (from the DID descriptor, ultimately from
 * diagnostics_config.yaml) against the provided buf_len. This prevents
 * buffer overflows in DID read callbacks that copy into caller-provided
 * buffers.
 *
 * @param[in] entry    DID descriptor whose data_length field is the source.
 * @param[in] buf_len  Actual buffer size available to the caller.
 *
 * @return UDS_STATUS_OK if buf_len >= entry->data_length.
 * @return UDS_STATUS_ERR_NULL_PTR if entry is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < entry->data_length.
 *
 * @note Compile-gate: always returns UDS_STATUS_OK if
 *       UDS_SAFETY_ENABLE_BOUNDS_CHECKS == 0.
 */
uds_safety_result_t uds_safety_check_did_data_length(
    const did_entry_t *entry,
    uint16_t           buf_len
);

/**
 * @brief Validate that a request PDU meets the minimum required length.
 *
 * Used at the entry point of every service handler to reject truncated
 * requests before any pointer arithmetic on the PDU buffer.
 *
 * TRACEABILITY: REQ-SAFE-007
 *
 * @param[in] req         Request message buffer.
 * @param[in] min_length  Minimum acceptable PDU length in bytes.
 *
 * @return UDS_STATUS_OK if req->length >= min_length.
 * @return UDS_STATUS_ERR_NULL_PTR if req is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if req->length < min_length.
 */
uds_safety_result_t uds_safety_check_request_length(
    const uds_msg_buf_t *req,
    uint16_t             min_length
);

/**
 * @brief Validate that a write data length matches the DID's expected length.
 *
 * ISO 14229-1 §12.4.2: The write data record length must equal the DID's
 * configured data_length. A mismatch must result in NRC 0x13.
 *
 * @param[in] entry       DID descriptor.
 * @param[in] write_len   Number of bytes received in the write request.
 *
 * @return UDS_STATUS_OK if write_len == entry->data_length.
 * @return UDS_STATUS_ERR_NULL_PTR if entry is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if write_len != entry->data_length.
 */
uds_safety_result_t uds_safety_check_write_data_length(
    const did_entry_t *entry,
    uint16_t           write_len
);

/* =============================================================================
 * Public API — DTC safety checks
 * ============================================================================= */

/**
 * @brief Validate a DTC status byte against the supported mask.
 *
 * ISO 14229-1 §11.3.2: The ECU shall only process status byte bits that
 * are supported in the ECU's DTC status availability mask. Unsupported bits
 * being set in a request constitutes an out-of-range condition.
 *
 * @param[in] status_byte  DTC status byte from request or database.
 * @param[in] support_mask Bitmask of status bits supported by this ECU.
 *
 * @return UDS_STATUS_OK if (status_byte & ~support_mask) == 0.
 * @return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE if unsupported bits are set.
 */
uds_safety_result_t uds_safety_validate_dtc_status_mask(
    uint8_t status_byte,
    uint8_t support_mask
);

/* =============================================================================
 * Public API — Integrity checks (boot-time / test invocation)
 * ============================================================================= */

/**
 * @brief Run compile-time-equivalent runtime sanity checks on the DID database.
 *
 * Verifies:
 *   - DID count does not exceed UDS_MAX_DID_COUNT.
 *   - No DID has a data_length of 0 or > DID_MAX_DATA_LEN.
 *   - All DID entries with access_flags == 0 are flagged as unsafe.
 *   - No duplicate DID IDs in the live database.
 *
 * Intended to be called once at boot (after did_database_init()) as a
 * defensive integrity check.  Results are recorded in the safety context.
 *
 * TRACEABILITY: REQ-SAFE-001
 *
 * @return UDS_STATUS_OK if all DID entries are consistent.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if safety module or DID DB not initialized.
 * @return UDS_STATUS_ERR_INVALID_PARAM if any DID entry fails validation.
 */
uds_safety_result_t uds_safety_verify_did_database(void);

/**
 * @brief Run a startup self-test of the safety module.
 *
 * Exercises each check category with known-good and known-bad inputs to
 * confirm the safety logic is functioning correctly.  All temporary state
 * changes are reverted before the function returns.
 *
 * TRACEABILITY: ISO 26262-6 §9.4.3 (software component pre-start checks)
 *
 * @return UDS_STATUS_OK if all self-tests pass.
 * @return UDS_STATUS_ERR_GENERIC if any self-test fails (logic defect — halt).
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if safety module not initialized.
 *
 * @note SAFETY: Halt or enter safe state if this function returns any error.
 *       A failing self-test indicates a compiler / build / memory corruption
 *       issue that makes diagnostics operation unsafe.
 */
uds_safety_result_t uds_safety_self_test(void);

/* =============================================================================
 * Utility macros
 * ============================================================================= */

/**
 * @brief Early-return macro: check a safety result and propagate on failure.
 *
 * Reduces boilerplate in service handlers. Example usage:
 *
 *   UDS_SAFETY_RETURN_IF_ERR(uds_safety_check_null_ptr(req, "req"));
 *   UDS_SAFETY_RETURN_IF_ERR(uds_safety_validate_session(sess, UDS_SESSION_DEFAULT));
 *
 * @param[in] expr  An expression returning uds_safety_result_t.
 */
#define UDS_SAFETY_RETURN_IF_ERR(expr)              \
    do {                                            \
        uds_safety_result_t _sr = (expr);           \
        if (_sr != UDS_STATUS_OK) {                 \
            return _sr;                             \
        }                                           \
    } while (0)

/**
 * @brief Compile-time assertion for safety-critical size constraints.
 *
 * Generates a compile error if the condition is false.
 * Use for static checks that must never be silently bypassed.
 *
 * Example:
 *   UDS_SAFETY_STATIC_ASSERT(UDS_MAX_DID_COUNT <= 255U,
 *                            did_count_must_fit_in_uint8);
 */
#define UDS_SAFETY_STATIC_ASSERT(cond, msg) \
    typedef char uds_safety_static_assert_##msg[(cond) ? 1 : -1]

/* Enforce that the DID count fits in a uint8_t (used in loop counters). */
UDS_SAFETY_STATIC_ASSERT(
    UDS_MAX_DID_COUNT <= 255U,
    did_count_fits_in_uint8
);

/* Enforce that the DTC count fits in a uint8_t. */
UDS_SAFETY_STATIC_ASSERT(
    UDS_MAX_DTC_COUNT <= 255U,
    dtc_count_fits_in_uint8
);

#ifdef __cplusplus
}
#endif

#endif /* UDS_SAFETY_H */
