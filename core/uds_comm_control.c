// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_comm_control.c
 *
 * PURPOSE: Communication control / DTC setting state machine.
 *          See uds_comm_control.h for full API documentation.
 *
 * SAFETY  : ASIL-B candidate. Tracks safety-relevant ECU communication state.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_comm_control.h"
#include "uds_types.h"

#include <stddef.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

static bool                         s_initialized   = false;
static uds_comm_mode_t              s_comm_mode     = UDS_COMM_MODE_ENABLE_RX_TX;
static uds_dtc_setting_mode_t       s_dtc_setting   = UDS_DTC_SETTING_ON;
static uds_comm_control_platform_cb_t s_comm_cb     = NULL;
static uds_dtc_setting_platform_cb_t  s_dtc_cb      = NULL;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

uds_status_t uds_comm_control_init(const uds_comm_control_cfg_t *cfg)
{
    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    s_comm_mode   = UDS_COMM_MODE_ENABLE_RX_TX;
    s_dtc_setting = UDS_DTC_SETTING_ON;

    if (cfg != NULL) {
        s_comm_cb = cfg->comm_cb;
        s_dtc_cb  = cfg->dtc_cb;
    } else {
        s_comm_cb = NULL;
        s_dtc_cb  = NULL;
    }

    s_initialized = true;
    return UDS_STATUS_OK;
}

uds_status_t uds_comm_control_apply(uint8_t control_type, uint8_t comm_type)
{
    uds_comm_mode_t new_mode;
    uds_status_t    status;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    switch (control_type) {
        case (uint8_t)UDS_COMM_MODE_ENABLE_RX_TX:
            new_mode = UDS_COMM_MODE_ENABLE_RX_TX;
            break;
        case (uint8_t)UDS_COMM_MODE_ENABLE_RX_DISABLE_TX:
            new_mode = UDS_COMM_MODE_ENABLE_RX_DISABLE_TX;
            break;
        case (uint8_t)UDS_COMM_MODE_DISABLE_RX_ENABLE_TX:
            new_mode = UDS_COMM_MODE_DISABLE_RX_ENABLE_TX;
            break;
        case (uint8_t)UDS_COMM_MODE_DISABLE_RX_TX:
            new_mode = UDS_COMM_MODE_DISABLE_RX_TX;
            break;
        default:
            return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (s_comm_cb != NULL) {
        status = s_comm_cb(new_mode, comm_type);
        if (status != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
        }
    }

    s_comm_mode = new_mode;
    return UDS_STATUS_OK;
}

uds_status_t uds_comm_control_set_dtc_setting(uint8_t dtc_setting)
{
    uds_dtc_setting_mode_t new_setting;
    uds_status_t           status;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    switch (dtc_setting) {
        case (uint8_t)UDS_DTC_SETTING_ON:
            new_setting = UDS_DTC_SETTING_ON;
            break;
        case (uint8_t)UDS_DTC_SETTING_OFF:
            new_setting = UDS_DTC_SETTING_OFF;
            break;
        default:
            return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (s_dtc_cb != NULL) {
        status = s_dtc_cb(new_setting);
        if (status != UDS_STATUS_OK) {
            return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
        }
    }

    s_dtc_setting = new_setting;
    return UDS_STATUS_OK;
}

uds_status_t uds_comm_control_restore_defaults(void)
{
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (s_comm_cb != NULL) {
        (void)s_comm_cb(UDS_COMM_MODE_ENABLE_RX_TX, (uint8_t)0x01U);
    }

    if (s_dtc_cb != NULL) {
        (void)s_dtc_cb(UDS_DTC_SETTING_ON);
    }

    s_comm_mode   = UDS_COMM_MODE_ENABLE_RX_TX;
    s_dtc_setting = UDS_DTC_SETTING_ON;

    return UDS_STATUS_OK;
}

uds_status_t uds_comm_control_get_mode(uds_comm_mode_t *out_mode)
{
    if (out_mode == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }
    *out_mode = s_comm_mode;
    return UDS_STATUS_OK;
}

uds_status_t uds_comm_control_get_dtc_setting(uds_dtc_setting_mode_t *out_mode)
{
    if (out_mode == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }
    *out_mode = s_dtc_setting;
    return UDS_STATUS_OK;
}

bool uds_comm_control_dtc_setting_is_on(void)
{
    if (!s_initialized) {
        return true; /* Conservative default — do not suppress DTC faults. */
    }
    return (s_dtc_setting == UDS_DTC_SETTING_ON);
}

#ifdef UNIT_TEST
/* --------------------------------------------------------------------------
 * Test-only reset function
 *
 * [MISRA 8.7] This function has external linkage so that test translation
 * units can call it via 'extern' declaration without linking production code
 * against a test header. The prototype is intentionally guarded in the
 * header by UNIT_TEST to prevent accidental production use.
 * -------------------------------------------------------------------------- */
void uds_comm_control_test_reset(void)
{
    s_initialized = false;
    s_comm_mode   = UDS_COMM_MODE_ENABLE_RX_TX;
    s_dtc_setting = UDS_DTC_SETTING_ON;
    s_comm_cb     = NULL;
    s_dtc_cb      = NULL;
}
#endif /* UNIT_TEST */

