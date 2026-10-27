/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.c
 *
 * ECU       : RobotJointController
 * Version   : 1.0.0
 * Generated : 2026-10-27T13:40:58Z
 *
 * PURPOSE: DID read/write handler stubs and static DID registration table.
 *          Each handler returns deterministic stub data (zeros) until the
 *          application developer replaces the stub body with real sensor or
 *          NVM access code.
 *
 *          Architecture:
 *            diagnostics_config.yaml (dids section)
 *              -> tools/codegen.py (build_did_handlers_context)
 *                -> tools/templates/did_handlers.c.j2
 *                  -> generated/did_handlers.c  (this file)
 *
 *          This design allows the DID table to be YAML-driven without
 *          modifying the core DID database engine (config/did_database.c).
 *
 * WARNING: DO NOT EDIT MANUALLY.
 *          Regenerate: python3 tools/codegen.py --config <yaml> --out generated/
 *
 * SAFETY  : Stub implementations return zeros, which is safe but not
 *           functionally correct. Replace each handler body before integration
 *           testing. Handlers that expose safety-relevant signals must be
 *           assessed for the required ASIL level.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "did_handlers.h"
#include "did_database.h"
#include "uds_types.h"
#include "generated_config.h"

#include <string.h>
#include <stddef.h>

/* =============================================================================
 * Stub sensor / NVM backing stores
 *
 * One static array per readable DID. These arrays act as the "sensor" in stub
 * mode. In production, replace the memcpy in each handler with a call to the
 * real hardware driver, signal bus (e.g. AUTOSAR Port interface), or NVM driver.
 *
 * MISRA C:2012 Rule 8.9: These are at file scope rather than function scope to
 * allow test harnesses to inject mock values from outside this translation unit.
 * ============================================================================= */

/** Stub backing store for DID 0xF190 — RobotSerialNumber (17 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'RobotSerialNumber'. */
static uint8_t s_mock_robotserialnumber[17U] = { 0x58U, 0x41U, 0x4CU, 0x4FU, 0x51U, 0x49U, 0x52U, 0x4FU, 0x42U, 0x4FU, 0x54U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x31U };

/** Stub backing store for DID 0xF18C — ControllerSerialNumber (8 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ControllerSerialNumber'. */
static uint8_t s_mock_controllerserialnumber[8U] = { 0x4AU, 0x43U, 0x2DU, 0x30U, 0x30U, 0x30U, 0x30U, 0x31U };

/** Stub backing store for DID 0xA010 — Axis0PositionOffset (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'Axis0PositionOffset'. */
static uint8_t s_mock_axis0positionoffset[4U] = { 0U };

/** Stub backing store for DID 0xA011 — Axis0SoftLimitPositive (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'Axis0SoftLimitPositive'. */
static uint8_t s_mock_axis0softlimitpositive[4U] = { 0x00U, 0x02U, 0x98U, 0x10U };

/** Stub backing store for DID 0xA012 — Axis0SoftLimitNegative (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'Axis0SoftLimitNegative'. */
static uint8_t s_mock_axis0softlimitnegative[4U] = { 0xFFU, 0xFDU, 0x67U, 0xF0U };

/* =============================================================================
 * Application-owned DIDs (source: app, EDS-toolchain#201)
 *
 * No backing store and no handler body is generated for these. The
 * application defines the functions declared in did_handlers.h; a missing
 * definition is a link error. The registration table and the safety wrappers
 * below still reference them, so session and security gating is unchanged.
 * ============================================================================= */
/*   DID 0xA000 — Axis0Position: did_read_axis0position() */
/*   DID 0xA001 — Axis0Velocity: did_read_axis0velocity() */
/*   DID 0xA002 — Axis0Torque: did_read_axis0torque() */
/*   DID 0xA003 — Axis0MotorTemperature: did_read_axis0motortemperature() */
/*   DID 0xA004 — Axis0StatusBitmask: did_read_axis0statusbitmask() */


/* =============================================================================
 * DID Read handlers
 *
 * Each function conforms to the did_read_cb_fn prototype:
 *   uds_status_t fn(uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
 *
 * The UDS service_0x22.c handler calls these callbacks after verifying session
 * and security level constraints. Callbacks need only return data — access
 * control is enforced by the service layer.
 * ============================================================================= */

/**
 * @brief Read handler for DID 0xF190 — RobotSerialNumber.
 *
 * Stub: copies 17 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 17 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_robotserialnumber, 17U);
 *     *out_len = 17U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 17.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 17.
 */
uds_status_t did_read_robotserialnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)17U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for RobotSerialNumber. */
    (void)memcpy(buf, s_mock_robotserialnumber, (size_t)17U);
    *out_len = (uint16_t)17U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xF18C — ControllerSerialNumber.
 *
 * Stub: copies 8 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 8 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_controllerserialnumber, 8U);
 *     *out_len = 8U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 8.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 8.
 */
uds_status_t did_read_controllerserialnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)8U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ControllerSerialNumber. */
    (void)memcpy(buf, s_mock_controllerserialnumber, (size_t)8U);
    *out_len = (uint16_t)8U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xA010 — Axis0PositionOffset.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_axis0positionoffset, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_axis0positionoffset(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)4U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for Axis0PositionOffset. */
    (void)memcpy(buf, s_mock_axis0positionoffset, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xA011 — Axis0SoftLimitPositive.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_axis0softlimitpositive, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_axis0softlimitpositive(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)4U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for Axis0SoftLimitPositive. */
    (void)memcpy(buf, s_mock_axis0softlimitpositive, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xA012 — Axis0SoftLimitNegative.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_axis0softlimitnegative, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_axis0softlimitnegative(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)4U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for Axis0SoftLimitNegative. */
    (void)memcpy(buf, s_mock_axis0softlimitnegative, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}


/* =============================================================================
 * DID Write handlers
 *
 * Each function conforms to the did_write_cb_fn prototype:
 *   uds_status_t fn(const uint8_t *buf, uint16_t len)
 *
 * Stub implementations store the value into the mock backing store.
 * Production implementations must validate the incoming data and write to NVM
 * or apply the value to the appropriate actuator or calibration record.
 * ============================================================================= */

/**
 * @brief Write handler for DID 0xA010 — Axis0PositionOffset.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 4 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'Axis0PositionOffset'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (4 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 4.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 4.
 */
uds_status_t did_write_axis0positionoffset(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)4U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_axis0positionoffset, buf, (size_t)4U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0xA011 — Axis0SoftLimitPositive.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 4 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'Axis0SoftLimitPositive'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (4 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 4.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 4.
 */
uds_status_t did_write_axis0softlimitpositive(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)4U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_axis0softlimitpositive, buf, (size_t)4U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0xA012 — Axis0SoftLimitNegative.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 4 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'Axis0SoftLimitNegative'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (4 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 4.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 4.
 */
uds_status_t did_write_axis0softlimitnegative(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)4U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_axis0softlimitnegative, buf, (size_t)4U);

    return UDS_STATUS_OK;
}


/* =============================================================================
 * Static DID registration table
 *
 * Maps DID identifiers to their handlers and access control metadata.
 * Passed to the DID database during initialisation via did_handlers_register_all().
 *
 * MISRA C:2012 Rule 8.4: Definition matches extern declaration in did_handlers.h.
 * ============================================================================= */

static const did_entry_t s_generated_did_table[GEN_DID_HANDLER_COUNT] = {
    /* ── DID 0xF190 — RobotSerialNumber ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61840U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)17U,
        .read_cb            = did_read_robotserialnumber,
        .write_cb           = NULL,
        .description        = "RobotSerialNumber"
    },
    /* ── DID 0xF18C — ControllerSerialNumber ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61836U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)8U,
        .read_cb            = did_read_controllerserialnumber,
        .write_cb           = NULL,
        .description        = "ControllerSerialNumber"
    },
    /* ── DID 0xA000 — Axis0Position ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)40960U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_axis0position,
        .write_cb           = NULL,
        .description        = "Axis0Position"
    },
    /* ── DID 0xA001 — Axis0Velocity ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)40961U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_axis0velocity,
        .write_cb           = NULL,
        .description        = "Axis0Velocity"
    },
    /* ── DID 0xA002 — Axis0Torque ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)40962U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_axis0torque,
        .write_cb           = NULL,
        .description        = "Axis0Torque"
    },
    /* ── DID 0xA003 — Axis0MotorTemperature ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)40963U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_axis0motortemperature,
        .write_cb           = NULL,
        .description        = "Axis0MotorTemperature"
    },
    /* ── DID 0xA004 — Axis0StatusBitmask ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)40964U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_axis0statusbitmask,
        .write_cb           = NULL,
        .description        = "Axis0StatusBitmask"
    },
    /* ── DID 0xA010 — Axis0PositionOffset ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)40976U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_axis0positionoffset,
        .write_cb           = did_write_axis0positionoffset,
        .description        = "Axis0PositionOffset"
    },
    /* ── DID 0xA011 — Axis0SoftLimitPositive ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)40977U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_axis0softlimitpositive,
        .write_cb           = did_write_axis0softlimitpositive,
        .description        = "Axis0SoftLimitPositive"
    },
    /* ── DID 0xA012 — Axis0SoftLimitNegative ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)40978U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_axis0softlimitnegative,
        .write_cb           = did_write_axis0softlimitnegative,
        .description        = "Axis0SoftLimitNegative"
    }
};

/* =============================================================================
 * DID registration entry point
 * ============================================================================= */

/**
 * @brief Register all generated DIDs with the DID database.
 *
 * Iterates s_generated_did_table[] and calls did_database_register() for
 * each entry. Must be called from uds_generated_init() after did_database_init()
 * and before any UDS request processing.
 *
 * @return UDS_STATUS_OK if all 10 DID(s) registered successfully.
 * @return UDS_STATUS_ERR_ALREADY_INITIALIZED if database already contains this DID.
 * @return UDS_STATUS_ERR_* on other registration failure.
 */
uds_status_t did_handlers_register_all(void)
{
    uint16_t     i;
    uds_status_t status;

    for (i = (uint16_t)0U; i < (uint16_t)GEN_DID_HANDLER_COUNT; i++) {
        status = did_database_register(&s_generated_did_table[i]);
        if (status != UDS_STATUS_OK) {
            return status;
        }
    }

    return UDS_STATUS_OK;
}
