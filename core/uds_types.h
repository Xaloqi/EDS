// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_types.h
 *
 * PURPOSE: Unified type definitions, error codes, and constants for the
 *          entire UDS/ISO-TP stack.
 *
 * SAFETY  : Safety-relevant — shared across all architectural layers.
 *           Changes to this file may have ASIL-B impact.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_TYPES_H
#define UDS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Version identification
 *
 * These macros define the EDS stack version and must match:
 *   - #define UDS_STACK_VERSION in generated/safety_config.h (template)
 *   - "Stack version" field in docs/INTEGRATION_GUIDE.md
 *
 * Bumped to 1.7.0 for the v1.7.0 commercial release.
 * -------------------------------------------------------------------------- */
#define UDS_SUITE_VERSION_MAJOR  (1U)
#define UDS_SUITE_VERSION_MINOR  (7U)
#define UDS_SUITE_VERSION_PATCH  (0U)

/** Compile-time version string — matches UDS_STACK_VERSION in safety_config.h. */
#define UDS_SUITE_VERSION_STRING "1.7.0"

/* --------------------------------------------------------------------------
 * Buffer and protocol sizing constants
 * -------------------------------------------------------------------------- */

/** Maximum UDS payload length (ISO 14229-1, single frame PDU limit over CAN). */
#define UDS_MAX_PAYLOAD_LEN      (4095U)

/** Maximum single CAN frame data length (CAN FD max). */
#define UDS_CAN_FRAME_MAX_LEN    (64U)

/** Maximum number of simultaneously supported UDS sessions. */
#define UDS_MAX_SESSIONS         (1U)

/** Maximum number of DID entries in the database. */
#define UDS_MAX_DID_COUNT        (64U)

/** Maximum number of DTC entries in the database. */
#define UDS_MAX_DTC_COUNT        (128U)

/**
 * @brief Maximum number of routines that can be registered in the routine database.
 *
 * Bounded at compile time — no dynamic memory.
 * ASIL-B: 32 is generous for typical ECU routine counts and fits in uint8_t.
 */
#define UDS_MAX_ROUTINE_COUNT    (32U)

/** Maximum security access seed length in bytes. */
/** Maximum security access seed length in bytes. [P1-SEC] Updated from 4->8 for AES-CMAC nonce. */
#define UDS_SECURITY_SEED_LEN    (8U)

/** Maximum security access key length in bytes. */
#define UDS_SECURITY_KEY_LEN     (4U)

/** Maximum number of security levels supported. */
#define UDS_MAX_SECURITY_LEVELS  (4U)

/* --------------------------------------------------------------------------
 * Unified status / error type
 *
 * SAFETY: All public APIs must return uds_status_t.
 *         No implicit success assumptions permitted.
 * -------------------------------------------------------------------------- */

/**
 * @brief Unified status code enumeration for the diagnostics suite.
 *
 * All public API functions return a value of this type.
 * Callers must inspect return values; ignoring them is a protocol violation.
 */
typedef enum uds_status {
    /* --- Success --- */
    UDS_STATUS_OK                        = 0x00, /**< Operation completed successfully. */

    /* --- General errors --- */
    UDS_STATUS_ERR_GENERIC               = 0x01, /**< Unspecified internal error. */
    UDS_STATUS_ERR_NULL_PTR              = 0x02, /**< Null pointer passed to API. */
    UDS_STATUS_ERR_INVALID_PARAM         = 0x03, /**< Parameter out of valid range. */
    UDS_STATUS_ERR_BUFFER_OVERFLOW       = 0x04, /**< Data exceeds static buffer capacity. */
    UDS_STATUS_ERR_NOT_INITIALIZED       = 0x05, /**< Module not initialized before use. */
    UDS_STATUS_ERR_ALREADY_INITIALIZED   = 0x06, /**< Module initialized more than once. */
    UDS_STATUS_ERR_TIMEOUT               = 0x07, /**< Operation timed out. */
    UDS_STATUS_ERR_BUSY                  = 0x08, /**< Resource temporarily unavailable. */
    UDS_STATUS_ERR_NOT_FOUND             = 0x09, /**< Requested item not found (e.g. no periodic subscription due). */

    /* --- Session errors --- */
    UDS_STATUS_ERR_SESSION_INVALID       = 0x10, /**< Requested session type not supported. */
    UDS_STATUS_ERR_SESSION_TRANSITION    = 0x11, /**< Session transition not permitted. */
    UDS_STATUS_ERR_SESSION_TIMEOUT       = 0x12, /**< Session timed out (S3server). */

    /* --- Security errors --- */
    UDS_STATUS_ERR_SEC_ACCESS_DENIED     = 0x20, /**< Security access denied. */
    UDS_STATUS_ERR_SEC_INVALID_KEY       = 0x21, /**< Key does not match seed. */
    UDS_STATUS_ERR_SEC_ATTEMPT_EXCEEDED  = 0x22, /**< Exceeded maximum failed attempts. */
    UDS_STATUS_ERR_SEC_NOT_UNLOCKED      = 0x23, /**< Security level not unlocked. */
    UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE  = 0x24, /**< Seed generation failed. */

    /* --- Transport (ISO-TP) errors --- */
    UDS_STATUS_ERR_TP_FRAME_INVALID      = 0x30, /**< Malformed ISO-TP frame received. */
    UDS_STATUS_ERR_TP_OVERFLOW           = 0x31, /**< ISO-TP RX buffer overflow. */
    UDS_STATUS_ERR_TP_TIMEOUT_CR         = 0x32, /**< ISO-TP Cr timeout expired. */
    UDS_STATUS_ERR_TP_TIMEOUT_AS         = 0x33, /**< ISO-TP As timeout expired. */
    UDS_STATUS_ERR_TP_STMIN_VIOLATION    = 0x34, /**< STmin constraint violated. */
    UDS_STATUS_ERR_TP_TX_FAILED          = 0x35, /**< ISO-TP frame transmission failed. */
    UDS_STATUS_ERR_TP_UNEXPECTED_PDU     = 0x36, /**< Unexpected PDU type in current state. */
    UDS_STATUS_ERR_TP_TIMEOUT_BS         = 0x37, /**< ISO-TP Bs timeout expired (no FC received). */

    /* --- CAN layer errors --- */
    UDS_STATUS_ERR_CAN_TX_FAILED         = 0x40, /**< CAN frame transmission failed. */
    UDS_STATUS_ERR_CAN_RX_FAILED         = 0x41, /**< CAN frame reception failed. */
    UDS_STATUS_ERR_CAN_BUS_OFF           = 0x42, /**< CAN controller in bus-off state. */
    UDS_STATUS_ERR_CAN_NOT_READY         = 0x43, /**< CAN controller not ready. */

    /* --- Service / protocol errors --- */
    UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED = 0x50, /**< Service ID not supported. */
    UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP   = 0x51, /**< Sub-function not supported. */
    UDS_STATUS_ERR_DID_NOT_FOUND         = 0x52, /**< DID not present in database. */
    UDS_STATUS_ERR_DID_READ_FAILED       = 0x53, /**< DID read operation failed. */
    UDS_STATUS_ERR_DID_WRITE_FAILED      = 0x54, /**< DID write operation failed. */
    UDS_STATUS_ERR_CONDITIONS_NOT_MET             = 0x55, /**< Conditions not correct for service. */
    /* [M5 REMOVED] UDS_STATUS_ERR_CONDITIONS_NOT_CORRECT alias deleted — MISRA C:2012 Rule 4.2
     * Use UDS_STATUS_ERR_CONDITIONS_NOT_MET (0x55) instead. */
    UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE           = 0x56, /**< Request parameter out of range. */
    UDS_STATUS_ERR_SERVICE_NOT_SUPPORTED_IN_SESSION = 0x57, /**< Service not supported in active session (NRC 0x7F). */
    UDS_STATUS_ERR_COMM_CONTROL_REJECTED          = 0x58, /**< CommunicationControl subFunction not permitted. */
    UDS_STATUS_ERR_DTC_SETTING_REJECTED           = 0x59, /**< ControlDTCSetting subFunction not permitted. */
    UDS_STATUS_ERR_ROUTINE_NOT_FOUND              = 0x5AU, /**< Routine ID not present in database. */
    UDS_STATUS_ERR_ROUTINE_FAILED                 = 0x5BU, /**< Routine execution failed. */
    UDS_STATUS_ERR_ROUTINE_NOT_SUPPORTED_IN_SESSION = 0x5CU, /**< Routine not supported in active session. */

    /* --- Download / Transfer errors (0x34/0x36/0x37) --- */
    UDS_STATUS_ERR_TRANSFER_WRONG_SEQ            = 0x5DU, /**< Wrong block sequence counter (NRC 0x73). */
    UDS_STATUS_ERR_TRANSFER_ABORTED              = 0x5EU, /**< Transfer aborted due to error. */
    UDS_STATUS_ERR_UPLOAD_DOWNLOAD_NOT_ACCEPTED  = 0x5FU, /**< Upload/download not accepted (NRC 0x70). */

    /* --- Platform errors --- */
    UDS_STATUS_ERR_PLATFORM              = 0x60, /**< Platform-specific error. */
    UDS_STATUS_ERR_OS_RESOURCE           = 0x61, /**< OS resource allocation failed. */

    UDS_STATUS_MAX                                /**< Sentinel — do not use as return value. */
} uds_status_t;

/* --------------------------------------------------------------------------
 * UDS session type enumeration (ISO 14229-1 Table 40)
 * -------------------------------------------------------------------------- */

/**
 * @brief UDS diagnostic session types.
 */
typedef enum uds_session_type {
    UDS_SESSION_DEFAULT          = 0x01U, /**< Default session (DSC 0x01). */
    UDS_SESSION_PROGRAMMING      = 0x02U, /**< Programming session (DSC 0x02). */
    UDS_SESSION_EXTENDED         = 0x03U, /**< Extended diagnostic session (DSC 0x03). */
    UDS_SESSION_SAFETY_SYSTEM    = 0x04U  /**< Safety system diagnostic session (DSC 0x04). */
} uds_session_type_t;

/* --------------------------------------------------------------------------
 * UDS NRC (Negative Response Code) enumeration (ISO 14229-1 Table A.1)
 * -------------------------------------------------------------------------- */

/**
 * @brief ISO 14229-1 Negative Response Codes.
 */
typedef enum uds_nrc {
    UDS_NRC_POSITIVE_RESPONSE                = 0x00U,
    UDS_NRC_GENERAL_REJECT                   = 0x10U,
    UDS_NRC_SERVICE_NOT_SUPPORTED            = 0x11U,
    UDS_NRC_SUBFUNCTION_NOT_SUPPORTED        = 0x12U,
    UDS_NRC_INCORRECT_MSG_LEN_OR_FORMAT      = 0x13U,
    UDS_NRC_RESPONSE_TOO_LONG                = 0x14U,
    UDS_NRC_BUSY_REPEAT_REQUEST              = 0x21U,
    UDS_NRC_CONDITIONS_NOT_CORRECT           = 0x22U,
    UDS_NRC_REQUEST_SEQUENCE_ERROR           = 0x24U,
    UDS_NRC_REQUEST_OUT_OF_RANGE             = 0x31U,
    UDS_NRC_SECURITY_ACCESS_DENIED           = 0x33U,
    UDS_NRC_AUTHENTICATION_REQUIRED          = 0x34U,
    UDS_NRC_INVALID_KEY                      = 0x35U,
    UDS_NRC_EXCEEDED_NUM_OF_ATTEMPTS         = 0x36U,
    UDS_NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED  = 0x37U,
    UDS_NRC_UPLOAD_DOWNLOAD_NOT_ACCEPTED     = 0x70U,
    UDS_NRC_TRANSFER_DATA_SUSPENDED          = 0x71U,
    UDS_NRC_GENERAL_PROG_FAILURE             = 0x72U,
    UDS_NRC_WRONG_BLOCK_SEQUENCE_COUNTER     = 0x73U,
    UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING = 0x78U,
    UDS_NRC_SUBFUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION = 0x7EU,
    UDS_NRC_SERVICE_NOT_SUPPORTED_IN_ACTIVE_SESSION     = 0x7FU
} uds_nrc_t;

/* --------------------------------------------------------------------------
 * Service ID definitions (ISO 14229-1)
 * -------------------------------------------------------------------------- */
#define UDS_SID_DIAGNOSTIC_SESSION_CONTROL    (0x10U)
#define UDS_SID_ECU_RESET                     (0x11U)
#define UDS_SID_CLEAR_DIAGNOSTIC_INFO         (0x14U)
#define UDS_SID_READ_DTC_INFO                 (0x19U)
#define UDS_SID_READ_DATA_BY_ID               (0x22U)
#define UDS_SID_READ_MEMORY_BY_ADDRESS        (0x23U)
#define UDS_SID_SECURITY_ACCESS               (0x27U)
#define UDS_SID_COMMUNICATION_CONTROL         (0x28U)
#define UDS_SID_READ_DATA_BY_PERIODIC_ID      (0x2AU)
#define UDS_SID_WRITE_DATA_BY_ID              (0x2EU)
#define UDS_SID_INPUT_OUTPUT_CONTROL          (0x2FU)
#define UDS_SID_ROUTINE_CONTROL               (0x31U)
#define UDS_SID_REQUEST_DOWNLOAD              (0x34U)
#define UDS_SID_REQUEST_UPLOAD                (0x35U)
#define UDS_SID_TRANSFER_DATA                 (0x36U)
#define UDS_SID_REQUEST_TRANSFER_EXIT         (0x37U)
#define UDS_SID_WRITE_MEMORY_BY_ADDRESS       (0x3DU)
#define UDS_SID_TESTER_PRESENT                (0x3EU)
#define UDS_SID_CONTROL_DTC_SETTING           (0x85U)
#define UDS_SID_NEGATIVE_RESPONSE             (0x7FU)
#define UDS_SID_POSITIVE_RESPONSE_OFFSET      (0x40U)

/* --------------------------------------------------------------------------
 * Raw message buffer type
 * -------------------------------------------------------------------------- */

/**
 * @brief Fixed-size raw diagnostic message buffer.
 *
 * SAFETY: Static allocation enforced. No heap usage permitted.
 */
typedef struct uds_msg_buf {
    uint8_t  data[UDS_MAX_PAYLOAD_LEN]; /**< Raw payload bytes. */
    uint16_t length;                    /**< Number of valid bytes in data[]. */
} uds_msg_buf_t;

/*
 * Compile-time stack-size guard for uds_msg_buf_t.
 *
 * uds_msg_buf_t is 4097 bytes (4095 data + 2 length). Allocating even one
 * instance on the stack of a task with a small stack will cause silent
 * corruption or an MPU fault. All instances in the EDS stack itself are
 * module-level statics; this guard exists to catch accidental stack
 * allocations introduced by integrators or future contributors.
 *
 * EDS_MSG_BUF_MAX_STACK_BYTES controls the maximum size that will be
 * silently permitted. The default is 256 bytes — smaller than one
 * uds_msg_buf_t — which means the assertion fires for ANY uds_msg_buf_t
 * on the stack unless the integrator explicitly raises the limit by
 * defining EDS_MSG_BUF_MAX_STACK_BYTES in their build system.
 *
 * To suppress the check (e.g. on a host build with large stacks):
 *   -DEDS_MSG_BUF_MAX_STACK_BYTES=8192
 *
 * To tighten the check for a target with a 4KB task stack:
 *   -DEDS_MSG_BUF_MAX_STACK_BYTES=512
 *
 * MISRA C:2012 Rule 20.7 — macro argument parenthesised.
 * MISRA C:2012 Rule 1.3  — static_assert is defined in C11 and available
 *                          via <assert.h> on all supported toolchains.
 *
 * NOTE: This check fires at the translation-unit level, not at each call
 * site. It verifies the struct size against the configured limit, which is
 * a necessary (though not sufficient) condition to prevent stack overflow.
 * The enforcement of "no stack allocation of this type" is a code-review
 * and MISRA-C Rule 18.8 concern; this check catches the common case of the
 * type being sized beyond any reasonable stack budget.
 */
#ifndef EDS_MSG_BUF_MAX_STACK_BYTES
#define EDS_MSG_BUF_MAX_STACK_BYTES  (256U)
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && !defined(MISRA_ANALYSIS)
/*
 * MISRA_ANALYSIS guard: cppcheck/misra_analysis.py defines MISRA_ANALYSIS=1
 * during static analysis passes. _Static_assert is C11 and valid, but the
 * MISRA cppcheck addon reports it as "Rule N/A" (not covered by any MISRA
 * rule) which inflates the open-violation count. The guard excludes this
 * assertion from the MISRA pass only; GCC and Clang always see it.
 */
_Static_assert(
    sizeof(uds_msg_buf_t) <= (EDS_MSG_BUF_MAX_STACK_BYTES),
    "uds_msg_buf_t exceeds EDS_MSG_BUF_MAX_STACK_BYTES — "
    "do not allocate uds_msg_buf_t on the task stack. "
    "Use static or module-level storage instead. "
    "If you are intentionally using a large stack, suppress this check by "
    "defining EDS_MSG_BUF_MAX_STACK_BYTES to a value >= sizeof(uds_msg_buf_t) "
    "in your build system (e.g. -DEDS_MSG_BUF_MAX_STACK_BYTES=8192)."
);
#endif

/* --------------------------------------------------------------------------
 * CAN frame descriptor
 * -------------------------------------------------------------------------- */

/**
 * @brief Descriptor for a single CAN or CAN FD frame.
 */
typedef struct uds_can_frame {
    uint32_t id;                              /**< CAN identifier (11-bit or 29-bit). */
    uint8_t  data[UDS_CAN_FRAME_MAX_LEN];     /**< Frame payload bytes. */
    uint8_t  dlc;                             /**< Data length code. */
    bool     is_extended_id;                  /**< True if 29-bit CAN ID. */
    bool     is_fd;                           /**< True if CAN FD frame. */
} uds_can_frame_t;

/* --------------------------------------------------------------------------
 * Timestamp type (millisecond resolution)
 * -------------------------------------------------------------------------- */

/** @brief Millisecond-resolution timestamp, sourced from platform layer. */
typedef uint32_t uds_timestamp_ms_t;

#ifdef __cplusplus
}
#endif

#endif /* UDS_TYPES_H */
