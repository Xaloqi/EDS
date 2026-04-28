// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_comm_control.h
 *
 * PURPOSE: Platform-agnostic state machine for CommunicationControl (SID 0x28)
 *          and ControlDTCSetting (SID 0x85).
 *
 *          Both services change ECU-wide operating modes that must be:
 *            1. Applied immediately when the request arrives.
 *            2. Automatically restored when the diagnostic session returns
 *               to Default (ISO 14229-1 §14.1 and §15.1).
 *
 *          This module tracks the current comm-control mode and DTC-setting
 *          mode, and provides the restore hooks called by service_0x10.c
 *          when the session transitions back to Default.
 *
 * PLATFORM CALLBACK:
 *   Applications register a platform_comm_cb and platform_dtc_cb during
 *   init. These callbacks translate the abstract mode enum into hardware
 *   actions (e.g. Zephyr CAN filter configuration, application-level
 *   message gating flags).
 *
 *   For host-side unit tests the callbacks are no-ops — state is tracked
 *   in RAM only.
 *
 * THREAD SAFETY:
 *   Not thread-safe. Must be called exclusively from the UDS task context.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_COMM_CONTROL_H
#define UDS_COMM_CONTROL_H

#include "uds_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * CommunicationControl mode (ISO 14229-1 Table 207)
 * -------------------------------------------------------------------------- */

/**
 * @brief Communication control mode enumeration.
 */
typedef enum uds_comm_mode {
    UDS_COMM_MODE_ENABLE_RX_TX          = 0x00U, /**< Normal operation. */
    UDS_COMM_MODE_ENABLE_RX_DISABLE_TX  = 0x01U, /**< RX enabled, TX disabled. */
    UDS_COMM_MODE_DISABLE_RX_ENABLE_TX  = 0x02U, /**< RX disabled, TX enabled. */
    UDS_COMM_MODE_DISABLE_RX_TX         = 0x03U  /**< Both RX and TX disabled. */
} uds_comm_mode_t;

/* --------------------------------------------------------------------------
 * DTC setting mode (ISO 14229-1 §15.2)
 * -------------------------------------------------------------------------- */

/**
 * @brief DTC setting control mode.
 */
typedef enum uds_dtc_setting_mode {
    UDS_DTC_SETTING_ON   = 0x01U, /**< DTC setting enabled (default). */
    UDS_DTC_SETTING_OFF  = 0x02U  /**< DTC setting disabled. */
} uds_dtc_setting_mode_t;

/* --------------------------------------------------------------------------
 * Platform callback types
 * -------------------------------------------------------------------------- */

/**
 * @brief Platform callback: apply communication control mode.
 *
 * @param[in] mode       Requested communication mode.
 * @param[in] comm_type  Communication type byte from the request.
 *
 * @return UDS_STATUS_OK if mode was applied successfully.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if the platform cannot comply.
 */
typedef uds_status_t (*uds_comm_control_platform_cb_t)(
    uds_comm_mode_t mode,
    uint8_t         comm_type);

/**
 * @brief Platform callback: apply DTC setting mode.
 *
 * @param[in] mode  Requested DTC setting mode.
 *
 * @return UDS_STATUS_OK if mode was applied successfully.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if the platform cannot comply.
 */
typedef uds_status_t (*uds_dtc_setting_platform_cb_t)(
    uds_dtc_setting_mode_t mode);

/* --------------------------------------------------------------------------
 * Configuration structure
 * -------------------------------------------------------------------------- */

/**
 * @brief Configuration for the comm-control / DTC-setting module.
 *
 * Both callbacks are optional (may be NULL). If NULL, state is tracked
 * in RAM but no hardware action is taken — suitable for host-side tests.
 */
typedef struct uds_comm_control_cfg {
    uds_comm_control_platform_cb_t comm_cb; /**< Platform comm-control callback (may be NULL). */
    uds_dtc_setting_platform_cb_t  dtc_cb;  /**< Platform DTC-setting callback (may be NULL). */
} uds_comm_control_cfg_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the communication control module.
 *
 * Registers optional platform callbacks and resets state to:
 *   - Comm mode: UDS_COMM_MODE_ENABLE_RX_TX
 *   - DTC setting: UDS_DTC_SETTING_ON
 *
 * @param[in] cfg  Configuration (may be NULL for no-callback mode).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if already initialized.
 */
uds_status_t uds_comm_control_init(const uds_comm_control_cfg_t *cfg);

/**
 * @brief Apply a communication control mode (called by service_0x28_handler).
 *
 * Updates internal state and invokes the platform callback if registered.
 *
 * @param[in] control_type  Sub-function value (0x00..0x03).
 * @param[in] comm_type     Communication type byte from request.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if init not called.
 * @return UDS_STATUS_ERR_INVALID_PARAM if control_type is out of range.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if platform callback failed.
 */
uds_status_t uds_comm_control_apply(uint8_t control_type, uint8_t comm_type);

/**
 * @brief Apply a DTC setting mode (called by service_0x85_handler).
 *
 * @param[in] dtc_setting  DTC setting sub-function (0x01 = ON, 0x02 = OFF).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if init not called.
 * @return UDS_STATUS_ERR_INVALID_PARAM if dtc_setting is not 0x01 or 0x02.
 * @return UDS_STATUS_ERR_CONDITIONS_NOT_MET if platform callback failed.
 */
uds_status_t uds_comm_control_set_dtc_setting(uint8_t dtc_setting);

/**
 * @brief Restore default communication and DTC-setting modes.
 *
 * Must be called when the diagnostic session returns to Default session
 * (ISO 14229-1 §14.1, §15.1). Resets both modes to their power-on
 * defaults without error if already in the default state.
 *
 * @return UDS_STATUS_OK on success (always succeeds even if callbacks fail).
 */
uds_status_t uds_comm_control_restore_defaults(void);

/**
 * @brief Get the current communication control mode.
 *
 * @param[out] out_mode  Receives the current comm mode.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if out_mode is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if init not called.
 */
uds_status_t uds_comm_control_get_mode(uds_comm_mode_t *out_mode);

/**
 * @brief Get the current DTC setting mode.
 *
 * @param[out] out_mode  Receives the current DTC setting mode.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if out_mode is NULL.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED if init not called.
 */
uds_status_t uds_comm_control_get_dtc_setting(uds_dtc_setting_mode_t *out_mode);

/**
 * @brief Check whether DTC setting is currently enabled.
 *
 * Convenience wrapper — avoids callers having to interpret the enum.
 *
 * @return true  if DTC setting mode is ON (default).
 * @return false if DTC setting mode is OFF (disabled by tester).
 */
bool uds_comm_control_dtc_setting_is_on(void);

#ifdef UNIT_TEST
/**
 * @brief Reset module state to power-on defaults.
 *
 * FOR TEST USE ONLY. Not available in production builds.
 * [MISRA 8.7] Prototype provided here so all callers have a visible
 * declaration; guarded so the symbol is unreachable in production.
 */
void uds_comm_control_test_reset(void);
#endif /* UNIT_TEST */

#ifdef __cplusplus
}
#endif

#endif /* UDS_COMM_CONTROL_H */
