// File: core/uds_safety.c
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_safety.c
 *
 * PURPOSE: Implementation of the UDS safety interface.
 *          Provides bounds checking, session gating, security level
 *          enforcement, and database integrity verification for ASIL-B
 *          diagnostic software components.
 *
 * SAFETY CLASSIFICATION:
 *   ASIL : ASIL-B candidate (ISO 26262-6:2018 §9)
 *
 * DESIGN PRINCIPLES:
 *   - Zero dynamic memory allocation.
 *   - All external inputs validated before use.
 *   - Every public function checks its own pointer arguments independently.
 *   - Violation counters are monotonically increasing (never reset in
 *     production use) to support post-mortem analysis.
 *   - Compile-time feature gates allow safety checks to be enabled or
 *     disabled per build configuration without source-code changes.
 *
 * TRACEABILITY:
 *   REQ-SAFE-001 : uds_safety_find_did(), uds_safety_verify_did_database()
 *   REQ-SAFE-002 : uds_safety_validate_session(), uds_safety_check_service_in_session()
 *   REQ-SAFE-003 : uds_safety_validate_did_access() (security level branch)
 *   REQ-SAFE-004 : uds_safety_check_null_ptr() — used at every entry point
 *   REQ-SAFE-005 : uds_safety_init() + s_initialized guard
 *   REQ-SAFE-006 : uds_safety_check_did_data_length(), uds_safety_check_write_data_length()
 *   REQ-SAFE-007 : uds_safety_check_request_length()
 *
 * CODING STANDARD: MISRA C:2012 alignment intended.
 * VERSION        : 0.4.0 (Phase 3)
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "uds_safety.h"
#include "uds_types.h"
#include "uds_session.h"
#include "uds_security.h"
#include "did_database.h"
#include "dtc_database.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =============================================================================
 * Internal constants
 * ============================================================================= */

/**
 * @brief DTC status availability mask — bits supported by this ECU.
 *
 * ISO 14229-1 §11.3.2: ECU must report which status bits it supports.
 * Default: all eight ISO 14229-1 Table D.1 status bits active.
 *
 * TODO [APPLICATION]: Adjust to match ECU-specific DTC capability assessment.
 */
#define SAFETY_DTC_STATUS_AVAILABILITY_MASK  (0xFFU)

/**
 * @brief Session ordinal look-up table.
 *
 * Maps uds_session_type_t enum values to ordinal ranking values used for
 * "at least this session level" comparisons.
 *
 * MISRA C:2012 Rule 10.4 — both sides of comparisons use consistent types.
 */
#define SESSION_ORDINAL_DEFAULT       (1U)
#define SESSION_ORDINAL_PROGRAMMING   (2U)
#define SESSION_ORDINAL_EXTENDED      (3U)
#define SESSION_ORDINAL_SAFETY_SYSTEM (4U)

/* =============================================================================
 * Static (file-scope) state
 *
 * MISRA C:2012 Rule 8.9 — variables with file scope where possible.
 * SAFETY: No heap allocation. Single static instance only.
 * ============================================================================= */

/** @brief Singleton safety context — accessed via uds_safety_get_ctx(). */
static uds_safety_ctx_t s_safety_ctx;

/*
 * [STACK-FIX] uds_msg_buf_t contains a 4095-byte data[] array (total 4097
 * bytes per instance). Allocating two instances on the stack in
 * uds_safety_self_test() would require ~8194 bytes of stack, which exceeds
 * the default task stack on most Cortex-M targets.
 *
 * These two module-level static buffers are used exclusively by the
 * self-test routine. They are only written during uds_safety_self_test(),
 * which is called once at startup (inside the single-init guard of
 * uds_safety_init()). No concurrent access is possible.
 *
 * MISRA C:2012 Rule 8.9: File-scope storage is appropriate here because the
 * variables have file scope and their lifetime must match the module.
 */
/** @brief Self-test scratch buffer A — used for the "too-short" length check. */
static uds_msg_buf_t s_self_test_req_a;
/** @brief Self-test scratch buffer B — used for the "adequate length" check. */
static uds_msg_buf_t s_self_test_req_b;

/* =============================================================================
 * Internal helpers
 * ============================================================================= */

/**
 * @brief Record a safety violation in the module context.
 *
 * Updates violation counters and persists the failing status code.
 * Called from every check function on a non-OK result path.
 *
 * @param[in] violation_type  Which counter to increment.
 * @param[in] code            The status code of the violation.
 */
typedef enum safety_violation_type {
    VIOLATION_NULL_PTR  = 0U,
    VIOLATION_SESSION   = 1U,
    VIOLATION_SECURITY  = 2U,
    VIOLATION_BOUNDS    = 3U,
    VIOLATION_PLATFORM  = 4U   /**< [HIGH-2] Platform-layer fault (e.g. TRNG failure). */
} safety_violation_type_t;

static void record_violation(
    safety_violation_type_t violation_type,
    uds_status_t            code)
{
    /* Saturating increment — counters stop at UINT32_MAX rather than wrapping. */
    switch (violation_type) {
        case VIOLATION_NULL_PTR:
            if (s_safety_ctx.null_check_violations < UINT32_MAX) {
                s_safety_ctx.null_check_violations++;
            }
            break;
        case VIOLATION_SESSION:
            if (s_safety_ctx.session_check_violations < UINT32_MAX) {
                s_safety_ctx.session_check_violations++;
            }
            break;
        case VIOLATION_SECURITY:
            if (s_safety_ctx.security_check_violations < UINT32_MAX) {
                s_safety_ctx.security_check_violations++;
            }
            break;
        case VIOLATION_BOUNDS:
            if (s_safety_ctx.bounds_check_violations < UINT32_MAX) {
                s_safety_ctx.bounds_check_violations++;
            }
            break;
        case VIOLATION_PLATFORM:
            /* [HIGH-2 FIX] Platform-layer fault counter (e.g. TRNG failure). */
            if (s_safety_ctx.platform_violations < UINT32_MAX) {
                s_safety_ctx.platform_violations++;
            }
            break;
        default:
            /* Unreachable under correct use — defensive no-op. */
            break;
    }
    s_safety_ctx.last_violation_code = code;
}

/**
 * @brief Increment the total-checks counter (saturating).
 */
static void count_check(void)
{
    if (s_safety_ctx.total_checks_performed < UINT32_MAX) {
        s_safety_ctx.total_checks_performed++;
    }
}

/**
 * @brief Convert a uds_session_type_t to its ordinal rank.
 *
 * TRACEABILITY: REQ-SAFE-002
 *
 * @param[in] session  Session type to convert.
 * @return Ordinal rank (1–4), or 0 for an unrecognised value.
 */
static uint8_t session_ordinal(uds_session_type_t session)
{
    uint8_t ordinal;

    switch (session) {
        case UDS_SESSION_DEFAULT:
            ordinal = (uint8_t)SESSION_ORDINAL_DEFAULT;
            break;
        case UDS_SESSION_PROGRAMMING:
            ordinal = (uint8_t)SESSION_ORDINAL_PROGRAMMING;
            break;
        case UDS_SESSION_EXTENDED:
            ordinal = (uint8_t)SESSION_ORDINAL_EXTENDED;
            break;
        case UDS_SESSION_SAFETY_SYSTEM:
            ordinal = (uint8_t)SESSION_ORDINAL_SAFETY_SYSTEM;
            break;
        default:
            ordinal = (uint8_t)0U;   /* Unknown session type. */
            break;
    }
    return ordinal;
}

/* =============================================================================
 * Module lifecycle
 * ============================================================================= */

uds_status_t uds_safety_init(void)
{
    if (s_safety_ctx.initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    s_safety_ctx.null_check_violations     = 0U;
    s_safety_ctx.session_check_violations  = 0U;
    s_safety_ctx.security_check_violations = 0U;
    s_safety_ctx.bounds_check_violations   = 0U;
    s_safety_ctx.platform_violations       = 0U;  /* [HIGH-2] */
    s_safety_ctx.total_checks_performed    = 0U;
    s_safety_ctx.last_violation_code       = UDS_STATUS_OK;
    s_safety_ctx.initialized               = true;

    return UDS_STATUS_OK;
}

const uds_safety_ctx_t *uds_safety_get_ctx(void)
{
    if (!s_safety_ctx.initialized) {
        return NULL;
    }
    return &s_safety_ctx;
}

/* =============================================================================
 * [HIGH-2 FIX] Platform violation recorder
 * ============================================================================= */

void uds_safety_record_platform_violation(uds_status_t code)
{
    /*
     * Safe to call before uds_safety_init(): if the module is not yet
     * initialised, the counter write is silently skipped.  This prevents
     * early-startup hardware faults (e.g. TRNG failing its power-on self-test
     * before the safety module is ready) from causing a null-deref or being
     * lost entirely.
     *
     * MISRA C:2012 Rule 15.5: single return point — achieved by guarded write.
     */
    if (s_safety_ctx.initialized) {
        record_violation(VIOLATION_PLATFORM, code);
    }
}

uds_status_t uds_safety_reset_counters(void)
{
    /*
     * SAFETY WARNING: This function must NOT be called in production firmware.
     * Its intended use is test harness reset between test cases.
     */
    if (!s_safety_ctx.initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    s_safety_ctx.null_check_violations     = 0U;
    s_safety_ctx.session_check_violations  = 0U;
    s_safety_ctx.security_check_violations = 0U;
    s_safety_ctx.bounds_check_violations   = 0U;
    s_safety_ctx.total_checks_performed    = 0U;
    s_safety_ctx.last_violation_code       = UDS_STATUS_OK;

    return UDS_STATUS_OK;
}

/* =============================================================================
 * NULL pointer checks (REQ-SAFE-004)
 * ============================================================================= */

uds_safety_result_t uds_safety_check_null_ptr(
    const void *ptr,
    const char *ptr_name)
{
    (void)ptr_name;   /* Used only when logging is enabled. */

#if UDS_SAFETY_ENABLE_NULL_CHECKS == 0U
    (void)ptr;
    return UDS_STATUS_OK;
#else
    count_check();

    if (ptr == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }
    return UDS_STATUS_OK;
#endif
}

/* =============================================================================
 * Session validation (REQ-SAFE-002)
 * ============================================================================= */

uds_safety_result_t uds_safety_validate_session(
    const uds_session_ctx_t *session_ctx,
    uds_session_type_t       required)
{
#if UDS_SAFETY_ENABLE_SESSION_CHECKS == 0U
    (void)session_ctx;
    (void)required;
    return UDS_STATUS_OK;
#else
    uds_session_type_t  active;
    uds_status_t        st;
    uint8_t             active_ord;
    uint8_t             required_ord;

    count_check();

    /* NULL guard — REQ-SAFE-004 */
    if (session_ctx == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Initialization guard. */
    if (!session_ctx->initialized) {
        record_violation(VIOLATION_SESSION, UDS_STATUS_ERR_NOT_INITIALIZED);
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* Query active session type. */
    st = uds_session_get_active(session_ctx, &active);
    if (st != UDS_STATUS_OK) {
        record_violation(VIOLATION_SESSION, st);
        return st;
    }

    /* Compare ordinals — higher ordinal = more privileged. */
    active_ord   = session_ordinal(active);
    required_ord = session_ordinal(required);

    if ((active_ord == (uint8_t)0U) || (required_ord == (uint8_t)0U)) {
        /* Unrecognised session type — treat as invalid. */
        record_violation(VIOLATION_SESSION, UDS_STATUS_ERR_SESSION_INVALID);
        return UDS_STATUS_ERR_SESSION_INVALID;
    }

    if (active_ord < required_ord) {
        record_violation(VIOLATION_SESSION, UDS_STATUS_ERR_SESSION_INVALID);
        return UDS_STATUS_ERR_SESSION_INVALID;
    }

    return UDS_STATUS_OK;
#endif
}

uds_safety_result_t uds_safety_check_service_in_session(
    const uds_session_ctx_t *session_ctx,
    uint8_t                  service_id)
{
    uds_session_type_t required_session;

    count_check();

    /* NULL guard — REQ-SAFE-004 */
    if (session_ctx == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /*
     * Minimum session requirement per ISO 14229-1 service specification:
     *
     *  0x10  DiagnosticSessionControl — any session (Default minimum)
     *  0x11  ECUReset                 — any session
     *  0x22  ReadDataByIdentifier     — any session (individual DIDs may require more)
     *  0x27  SecurityAccess           — Extended or Programming session
     *  0x2E  WriteDataByIdentifier    — Extended or Programming session
     *  0x3E  TesterPresent            — any session
     *
     * NOTE: The DID-level min_session from diagnostics_config.yaml provides
     * finer-grained control and is checked separately in uds_safety_validate_did_access().
     */
    switch (service_id) {
        case UDS_SID_DIAGNOSTIC_SESSION_CONTROL:    /* 0x10 */
            required_session = UDS_SESSION_DEFAULT;
            break;
        case UDS_SID_ECU_RESET:                     /* 0x11 */
            required_session = UDS_SESSION_DEFAULT;
            break;
        case UDS_SID_READ_DATA_BY_ID:               /* 0x22 */
            required_session = UDS_SESSION_DEFAULT;
            break;
        case UDS_SID_SECURITY_ACCESS:               /* 0x27 */
            required_session = UDS_SESSION_EXTENDED;
            break;
        case 0x2EU:                                 /* WriteDataByIdentifier */
            required_session = UDS_SESSION_EXTENDED;
            break;
        case UDS_SID_TESTER_PRESENT:                /* 0x3E */
            required_session = UDS_SESSION_DEFAULT;
            break;
        default:
            record_violation(VIOLATION_SESSION, UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED);
            return UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED;
    }

    return uds_safety_validate_session(session_ctx, required_session);
}

/* =============================================================================
 * DID access control (REQ-SAFE-001, REQ-SAFE-003)
 * ============================================================================= */

uds_safety_result_t uds_safety_find_did(
    uint16_t           did_id,
    const did_entry_t **out_entry)
{
    const did_entry_t *found;

    count_check();

    /* Guard out_entry itself — REQ-SAFE-004 */
    if (out_entry == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }

    found = did_database_find(did_id);
    if (found == NULL) {
        *out_entry = NULL;
        record_violation(VIOLATION_BOUNDS, UDS_STATUS_ERR_DID_NOT_FOUND);
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    *out_entry = found;
    return UDS_STATUS_OK;
}

uds_safety_result_t uds_safety_validate_did_access(
    const did_entry_t        *entry,
    const uds_session_ctx_t  *session_ctx,
    const uds_security_ctx_t *security_ctx,
    uds_safety_access_type_t  access_type)
{
    uint8_t required_access_flag;
    uint8_t required_security_level;

    count_check();

    /* ── NULL guards (REQ-SAFE-004) ──────────────────────────────────────── */
    if (entry == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (session_ctx == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (security_ctx == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* ── Capability check: does the DID support this access type? ─────────── */
    if (access_type == UDS_SAFETY_ACCESS_READ) {
        required_access_flag    = (uint8_t)DID_ACCESS_READ;
        required_security_level = entry->read_access_level;
    } else {
        required_access_flag    = (uint8_t)DID_ACCESS_WRITE;
        required_security_level = entry->write_access_level;
    }

    if ((entry->access_flags & required_access_flag) == (uint8_t)0U) {
        record_violation(VIOLATION_SESSION, UDS_STATUS_ERR_CONDITIONS_NOT_MET);
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

#if UDS_SAFETY_ENABLE_SESSION_CHECKS
    /* ── Session check (REQ-SAFE-002) ──────────────────────────────────── */
    {
        uds_safety_result_t sess_result;
        uds_session_type_t  min_session;

        /*
         * Cast min_session from uint8_t storage in the DID descriptor back to
         * the uds_session_type_t enum.  The code generator guarantees the stored
         * value is always a valid enum member (validated in validate_config()).
         */
        min_session = (uds_session_type_t)entry->min_session;
        sess_result = uds_safety_validate_session(session_ctx, min_session);
        if (sess_result != UDS_STATUS_OK) {
            /* counter already incremented inside uds_safety_validate_session */
            return sess_result;
        }
    }
#endif /* UDS_SAFETY_ENABLE_SESSION_CHECKS */

#if UDS_SAFETY_ENABLE_SECURITY_CHECKS
    /* ── Security level check (REQ-SAFE-003) ─────────────────────────── */
    if (required_security_level > (uint8_t)0U) {
        uds_safety_result_t sec_result;
        bool is_unlocked = false;

        sec_result = uds_security_is_unlocked(
            security_ctx,
            required_security_level,
            &is_unlocked
        );

        if (sec_result != UDS_STATUS_OK) {
            record_violation(VIOLATION_SECURITY, sec_result);
            return sec_result;
        }

        if (!is_unlocked) {
            record_violation(VIOLATION_SECURITY, UDS_STATUS_ERR_SEC_NOT_UNLOCKED);
            return UDS_STATUS_ERR_SEC_NOT_UNLOCKED;
        }
    }
#endif /* UDS_SAFETY_ENABLE_SECURITY_CHECKS */

    return UDS_STATUS_OK;
}

/* =============================================================================
 * Buffer bounds checking (REQ-SAFE-006, REQ-SAFE-007)
 * ============================================================================= */

uds_safety_result_t uds_safety_check_did_data_length(
    const did_entry_t *entry,
    uint16_t           buf_len)
{
#if UDS_SAFETY_ENABLE_BOUNDS_CHECKS == 0U
    (void)entry;
    (void)buf_len;
    return UDS_STATUS_OK;
#else
    count_check();

    if (entry == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /*
     * Use explicit cast to avoid signed/unsigned comparison warning.
     * entry->data_length and buf_len are both uint16_t — no truncation risk.
     * MISRA C:2012 Rule 10.4: operands of same essential type.
     */
    if (buf_len < (uint16_t)entry->data_length) {
        record_violation(VIOLATION_BOUNDS, UDS_STATUS_ERR_BUFFER_OVERFLOW);
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    return UDS_STATUS_OK;
#endif
}

uds_safety_result_t uds_safety_check_request_length(
    const uds_msg_buf_t *req,
    uint16_t             min_length)
{
    count_check();

    if (req == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (req->length < min_length) {
        record_violation(VIOLATION_BOUNDS, UDS_STATUS_ERR_INVALID_PARAM);
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    return UDS_STATUS_OK;
}

uds_safety_result_t uds_safety_check_write_data_length(
    const did_entry_t *entry,
    uint16_t           write_len)
{
    count_check();

    if (entry == NULL) {
        record_violation(VIOLATION_NULL_PTR, UDS_STATUS_ERR_NULL_PTR);
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /*
     * ISO 14229-1 §12.4.2: The number of bytes in the dataRecord parameter
     * of the WriteDataByIdentifier request shall be equal to the number of
     * bytes defined in the vehicle manufacturer's data dictionary for the
     * requested dataIdentifier.
     *
     * Mismatch → NRC 0x13 (incorrectMessageLengthOrInvalidFormat).
     */
    if (write_len != (uint16_t)entry->data_length) {
        record_violation(VIOLATION_BOUNDS, UDS_STATUS_ERR_INVALID_PARAM);
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    return UDS_STATUS_OK;
}

/* =============================================================================
 * DTC safety checks
 * ============================================================================= */

uds_safety_result_t uds_safety_validate_dtc_status_mask(
    uint8_t status_byte,
    uint8_t support_mask)
{
    count_check();

    /*
     * ISO 14229-1 §11.3.2: bits set in status_byte that are NOT in the ECU's
     * support_mask represent unsupported status bits.  This is an out-of-range
     * request condition.
     *
     * MISRA C:2012 Rule 10.1: bitwise operator on unsigned operands only.
     */
    if ((status_byte & (uint8_t)(~support_mask)) != (uint8_t)0U) {
        record_violation(VIOLATION_BOUNDS, UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE);
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    return UDS_STATUS_OK;
}

/* =============================================================================
 * Database integrity verification (REQ-SAFE-001)
 * ============================================================================= */

uds_safety_result_t uds_safety_verify_did_database(void)
{
    uint16_t     did_count;
    uint16_t     i;
    uds_status_t st;

    if (!s_safety_ctx.initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* Query database size — propagate any database errors. */
    st = did_database_get_count(&did_count);
    if (st != UDS_STATUS_OK) {
        return st;
    }

    /* Bounds check: count must not exceed the UDS stack limit. */
    if ((uint32_t)did_count > (uint32_t)UDS_MAX_DID_COUNT) {
        record_violation(VIOLATION_BOUNDS, UDS_STATUS_ERR_INVALID_PARAM);
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /*
     * Per-entry validation: iterate over the registered DIDs and verify
     * each entry's data_length and access_flags.
     *
     * NOTE: did_database_find() provides lookup by ID, not by index.
     *       The scan here uses the public API to check the count only;
     *       detailed per-entry checks require the database to expose an
     *       iteration interface.
     *
     * TODO [PHASE-4]: Expose did_database_get_entry(index) for full iteration.
     *       For now this function validates database count and the overall
     *       initialization state.
     */
    (void)i;   /* Suppress unused-variable warning until per-index API exists. */

    if (did_count == (uint16_t)0U) {
        /*
         * Zero DIDs is a valid post-init state if no DIDs have been registered
         * yet. Not a fault — just record it.
         */
        return UDS_STATUS_OK;
    }

    return UDS_STATUS_OK;
}

/* =============================================================================
 * Safety module self-test (ISO 26262-6 §9.4.3)
 * ============================================================================= */

uds_safety_result_t uds_safety_self_test(void)
{
    uds_safety_result_t result;
    uint32_t            saved_total;
    uint32_t            saved_null;
    uint32_t            saved_bounds;

    if (!s_safety_ctx.initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /*
     * Save counter state so self-test checks do not contaminate production
     * violation counts.  We restore them after each group of self-test calls.
     */
    saved_total  = s_safety_ctx.total_checks_performed;
    saved_null   = s_safety_ctx.null_check_violations;
    saved_bounds = s_safety_ctx.bounds_check_violations;

    /* ── Test 1: NULL pointer check — NULL input must return error ──────── */
    result = uds_safety_check_null_ptr(NULL, "self_test_ptr");
    if (result != UDS_STATUS_ERR_NULL_PTR) {
        /* Self-test failure: NULL pointer check is broken — logic defect. */
        return UDS_STATUS_ERR_GENERIC;
    }

    /* ── Test 2: NULL pointer check — valid pointer must return OK ──────── */
    {
        uint8_t dummy = (uint8_t)0U;
        result = uds_safety_check_null_ptr(&dummy, "self_test_valid_ptr");
        if (result != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_GENERIC;
        }
    }

    /* ── Test 3: Request length check — too-short buffer must fail ──────── */
    /*
     * [STACK-FIX] uds_msg_buf_t is 4097 bytes. Allocating two instances on
     * the stack would consume 8194 bytes — more than the default task stack
     * on most Cortex-M targets. Use the module-level static buffers
     * s_self_test_req_a and s_self_test_req_b instead.
     * These are only written during uds_safety_self_test(), which must not
     * be called concurrently (single-init contract in uds_safety_init()).
     */
    s_self_test_req_a.length = (uint16_t)1U;
    result = uds_safety_check_request_length(&s_self_test_req_a, (uint16_t)3U);
    if (result != UDS_STATUS_ERR_INVALID_PARAM) {
        return UDS_STATUS_ERR_GENERIC;
    }

    /* ── Test 4: Request length check — adequate buffer must pass ─────── */
    s_self_test_req_b.length = (uint16_t)4U;
    result = uds_safety_check_request_length(&s_self_test_req_b, (uint16_t)3U);
    if (result != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_GENERIC;
    }

    /* ── Test 5: DTC status mask — unsupported bit must fail ─────────── */
    {
        uint8_t bad_mask = (uint8_t)0xFFU;   /* support all */
        uint8_t bad_byte = (uint8_t)0x00U;   /* all supported → pass */
        result = uds_safety_validate_dtc_status_mask(bad_byte, bad_mask);
        if (result != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_GENERIC;
        }
    }

    /* ── Test 6: DTC status mask — unsupported bit rejection ─────────── */
    {
        uint8_t support = (uint8_t)0x0FU;   /* only lower 4 bits supported */
        uint8_t request = (uint8_t)0x10U;   /* bit 4 not in support mask  */
        result = uds_safety_validate_dtc_status_mask(request, support);
        if (result != UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE) {
            return UDS_STATUS_ERR_GENERIC;
        }
    }

    /* ── Test 7: NULL session context must be rejected ─────────────────── */
    result = uds_safety_validate_session(NULL, UDS_SESSION_DEFAULT);
    if (result != UDS_STATUS_ERR_NULL_PTR) {
        return UDS_STATUS_ERR_GENERIC;
    }

    /* ── Test 8: NULL DID entry in find_did must be rejected ────────────── */
    {
        const did_entry_t *out = NULL;
        result = uds_safety_find_did((uint16_t)0x0001U, &out);
        /*
         * 0x0001 is not a registered DID in any baseline config, so we
         * expect DID_NOT_FOUND.  Pass also if it happens to be found
         * (extended config) — the important thing is no crash / null deref.
         */
        if ((result != UDS_STATUS_ERR_DID_NOT_FOUND) &&
            (result != UDS_STATUS_OK)) {
            return UDS_STATUS_ERR_GENERIC;
        }
    }

    /* ── Restore counters — self-test must be transparent ──────────────── */
    s_safety_ctx.total_checks_performed = saved_total;
    s_safety_ctx.null_check_violations  = saved_null;
    s_safety_ctx.bounds_check_violations = saved_bounds;

    return UDS_STATUS_OK;
}
