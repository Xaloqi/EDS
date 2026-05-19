/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.c
 *
 * ECU       : MotorController_Inverter
 * Version   : 1.0.0
 * Generated : 2026-05-19T13:34:40Z
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

/** Stub backing store for DID 0xF190 — VehicleIdentificationNumber (17 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'VehicleIdentificationNumber'. */
static uint8_t s_mock_vehicleidentificationnumber[17U] = { 0U };

/** Stub backing store for DID 0xF18C — ECUSerialNumber (8 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECUSerialNumber'. */
static uint8_t s_mock_ecuserialnumber[8U] = { 0U };

/** Stub backing store for DID 0xF187 — VehicleManufacturerSparePartNumber (11 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'VehicleManufacturerSparePartNumber'. */
static uint8_t s_mock_vehiclemanufacturersparepartnumber[11U] = { 0U };

/** Stub backing store for DID 0xF189 — ECUSoftwareVersionNumber (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECUSoftwareVersionNumber'. */
static uint8_t s_mock_ecusoftwareversionnumber[4U] = { 0U };

/** Stub backing store for DID 0xE001 — MC_RotorSpeed_rpm (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_RotorSpeed_rpm'. */
static uint8_t s_mock_mc_rotorspeed_rpm[2U] = { 0U };

/** Stub backing store for DID 0xE002 — MC_TorqueDemand_01Nm (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_TorqueDemand_01Nm'. */
static uint8_t s_mock_mc_torquedemand_01nm[2U] = { 0U };

/** Stub backing store for DID 0xE003 — MC_TorqueActual_01Nm (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_TorqueActual_01Nm'. */
static uint8_t s_mock_mc_torqueactual_01nm[2U] = { 0U };

/** Stub backing store for DID 0xE004 — MC_RotorElectricalAngle_raw (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_RotorElectricalAngle_raw'. */
static uint8_t s_mock_mc_rotorelectricalangle_raw[2U] = { 0U };

/** Stub backing store for DID 0xE005 — MC_MechanicalPower_10W (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_MechanicalPower_10W'. */
static uint8_t s_mock_mc_mechanicalpower_10w[2U] = { 0U };

/** Stub backing store for DID 0xE101 — MC_PhaseA_Current_100mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_PhaseA_Current_100mA'. */
static uint8_t s_mock_mc_phasea_current_100ma[2U] = { 0U };

/** Stub backing store for DID 0xE102 — MC_PhaseB_Current_100mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_PhaseB_Current_100mA'. */
static uint8_t s_mock_mc_phaseb_current_100ma[2U] = { 0U };

/** Stub backing store for DID 0xE103 — MC_PhaseC_Current_100mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_PhaseC_Current_100mA'. */
static uint8_t s_mock_mc_phasec_current_100ma[2U] = { 0U };

/** Stub backing store for DID 0xE104 — MC_Iq_Current_100mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_Iq_Current_100mA'. */
static uint8_t s_mock_mc_iq_current_100ma[2U] = { 0U };

/** Stub backing store for DID 0xE105 — MC_Id_Current_100mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_Id_Current_100mA'. */
static uint8_t s_mock_mc_id_current_100ma[2U] = { 0U };

/** Stub backing store for DID 0xE201 — MC_DCLink_Voltage_mV (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_DCLink_Voltage_mV'. */
static uint8_t s_mock_mc_dclink_voltage_mv[4U] = { 0U };

/** Stub backing store for DID 0xE202 — MC_DCLink_Current_100mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_DCLink_Current_100mA'. */
static uint8_t s_mock_mc_dclink_current_100ma[2U] = { 0U };

/** Stub backing store for DID 0xE203 — MC_ModulationIndex_04pct (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_ModulationIndex_04pct'. */
static uint8_t s_mock_mc_modulationindex_04pct[1U] = { 0U };

/** Stub backing store for DID 0xE301 — MC_IGBT_PhaseA_JunctionTemp_degC (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_IGBT_PhaseA_JunctionTemp_degC'. */
static uint8_t s_mock_mc_igbt_phasea_junctiontemp_degc[1U] = { 0U };

/** Stub backing store for DID 0xE302 — MC_IGBT_PhaseB_JunctionTemp_degC (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_IGBT_PhaseB_JunctionTemp_degC'. */
static uint8_t s_mock_mc_igbt_phaseb_junctiontemp_degc[1U] = { 0U };

/** Stub backing store for DID 0xE303 — MC_IGBT_PhaseC_JunctionTemp_degC (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_IGBT_PhaseC_JunctionTemp_degC'. */
static uint8_t s_mock_mc_igbt_phasec_junctiontemp_degc[1U] = { 0U };

/** Stub backing store for DID 0xE304 — MC_MotorStatorTemp_degC (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_MotorStatorTemp_degC'. */
static uint8_t s_mock_mc_motorstatortemp_degc[1U] = { 0U };

/** Stub backing store for DID 0xE305 — MC_CoolantInletTemp_degC (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_CoolantInletTemp_degC'. */
static uint8_t s_mock_mc_coolantinlettemp_degc[1U] = { 0U };

/** Stub backing store for DID 0xE401 — MC_FaultStatusBitmask (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_FaultStatusBitmask'. */
static uint8_t s_mock_mc_faultstatusbitmask[2U] = { 0U };

/** Stub backing store for DID 0xE402 — MC_OperatingMode (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_OperatingMode'. */
static uint8_t s_mock_mc_operatingmode[1U] = { 0U };

/** Stub backing store for DID 0xE501 — MC_ResolverOffset_raw (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_ResolverOffset_raw'. */
static uint8_t s_mock_mc_resolveroffset_raw[2U] = { 0U };

/** Stub backing store for DID 0xE502 — MC_TorqueScalingFactor_01pct (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_TorqueScalingFactor_01pct'. */
static uint8_t s_mock_mc_torquescalingfactor_01pct[2U] = { 0U };

/** Stub backing store for DID 0xE503 — MC_CurrentSensorTrim_100uA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'MC_CurrentSensorTrim_100uA'. */
static uint8_t s_mock_mc_currentsensortrim_100ua[2U] = { 0U };


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
 * @brief Read handler for DID 0xF190 — VehicleIdentificationNumber.
 *
 * Stub: copies 17 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 17 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_vehicleidentificationnumber, 17U);
 *     *out_len = 17U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 17.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 17.
 */
uds_status_t did_read_vehicleidentificationnumber(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for VehicleIdentificationNumber. */
    (void)memcpy(buf, s_mock_vehicleidentificationnumber, (size_t)17U);
    *out_len = (uint16_t)17U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xF18C — ECUSerialNumber.
 *
 * Stub: copies 8 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 8 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecuserialnumber, 8U);
 *     *out_len = 8U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 8.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 8.
 */
uds_status_t did_read_ecuserialnumber(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECUSerialNumber. */
    (void)memcpy(buf, s_mock_ecuserialnumber, (size_t)8U);
    *out_len = (uint16_t)8U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xF187 — VehicleManufacturerSparePartNumber.
 *
 * Stub: copies 11 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 11 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_vehiclemanufacturersparepartnumber, 11U);
 *     *out_len = 11U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 11.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 11.
 */
uds_status_t did_read_vehiclemanufacturersparepartnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)11U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for VehicleManufacturerSparePartNumber. */
    (void)memcpy(buf, s_mock_vehiclemanufacturersparepartnumber, (size_t)11U);
    *out_len = (uint16_t)11U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xF189 — ECUSoftwareVersionNumber.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecusoftwareversionnumber, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_ecusoftwareversionnumber(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECUSoftwareVersionNumber. */
    (void)memcpy(buf, s_mock_ecusoftwareversionnumber, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE001 — MC_RotorSpeed_rpm.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_rotorspeed_rpm, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_rotorspeed_rpm(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_RotorSpeed_rpm. */
    (void)memcpy(buf, s_mock_mc_rotorspeed_rpm, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE002 — MC_TorqueDemand_01Nm.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_torquedemand_01nm, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_torquedemand_01nm(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_TorqueDemand_01Nm. */
    (void)memcpy(buf, s_mock_mc_torquedemand_01nm, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE003 — MC_TorqueActual_01Nm.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_torqueactual_01nm, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_torqueactual_01nm(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_TorqueActual_01Nm. */
    (void)memcpy(buf, s_mock_mc_torqueactual_01nm, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE004 — MC_RotorElectricalAngle_raw.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_rotorelectricalangle_raw, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_rotorelectricalangle_raw(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_RotorElectricalAngle_raw. */
    (void)memcpy(buf, s_mock_mc_rotorelectricalangle_raw, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE005 — MC_MechanicalPower_10W.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_mechanicalpower_10w, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_mechanicalpower_10w(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_MechanicalPower_10W. */
    (void)memcpy(buf, s_mock_mc_mechanicalpower_10w, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE101 — MC_PhaseA_Current_100mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_phasea_current_100ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_phasea_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_PhaseA_Current_100mA. */
    (void)memcpy(buf, s_mock_mc_phasea_current_100ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE102 — MC_PhaseB_Current_100mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_phaseb_current_100ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_phaseb_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_PhaseB_Current_100mA. */
    (void)memcpy(buf, s_mock_mc_phaseb_current_100ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE103 — MC_PhaseC_Current_100mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_phasec_current_100ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_phasec_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_PhaseC_Current_100mA. */
    (void)memcpy(buf, s_mock_mc_phasec_current_100ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE104 — MC_Iq_Current_100mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_iq_current_100ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_iq_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_Iq_Current_100mA. */
    (void)memcpy(buf, s_mock_mc_iq_current_100ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE105 — MC_Id_Current_100mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_id_current_100ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_id_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_Id_Current_100mA. */
    (void)memcpy(buf, s_mock_mc_id_current_100ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE201 — MC_DCLink_Voltage_mV.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_dclink_voltage_mv, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_mc_dclink_voltage_mv(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_DCLink_Voltage_mV. */
    (void)memcpy(buf, s_mock_mc_dclink_voltage_mv, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE202 — MC_DCLink_Current_100mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_dclink_current_100ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_dclink_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_DCLink_Current_100mA. */
    (void)memcpy(buf, s_mock_mc_dclink_current_100ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE203 — MC_ModulationIndex_04pct.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_modulationindex_04pct, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_mc_modulationindex_04pct(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_ModulationIndex_04pct. */
    (void)memcpy(buf, s_mock_mc_modulationindex_04pct, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE301 — MC_IGBT_PhaseA_JunctionTemp_degC.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_igbt_phasea_junctiontemp_degc, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_mc_igbt_phasea_junctiontemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_IGBT_PhaseA_JunctionTemp_degC. */
    (void)memcpy(buf, s_mock_mc_igbt_phasea_junctiontemp_degc, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE302 — MC_IGBT_PhaseB_JunctionTemp_degC.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_igbt_phaseb_junctiontemp_degc, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_mc_igbt_phaseb_junctiontemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_IGBT_PhaseB_JunctionTemp_degC. */
    (void)memcpy(buf, s_mock_mc_igbt_phaseb_junctiontemp_degc, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE303 — MC_IGBT_PhaseC_JunctionTemp_degC.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_igbt_phasec_junctiontemp_degc, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_mc_igbt_phasec_junctiontemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_IGBT_PhaseC_JunctionTemp_degC. */
    (void)memcpy(buf, s_mock_mc_igbt_phasec_junctiontemp_degc, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE304 — MC_MotorStatorTemp_degC.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_motorstatortemp_degc, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_mc_motorstatortemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_MotorStatorTemp_degC. */
    (void)memcpy(buf, s_mock_mc_motorstatortemp_degc, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE305 — MC_CoolantInletTemp_degC.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_coolantinlettemp_degc, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_mc_coolantinlettemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_CoolantInletTemp_degC. */
    (void)memcpy(buf, s_mock_mc_coolantinlettemp_degc, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE401 — MC_FaultStatusBitmask.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_faultstatusbitmask, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_faultstatusbitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_FaultStatusBitmask. */
    (void)memcpy(buf, s_mock_mc_faultstatusbitmask, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE402 — MC_OperatingMode.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_operatingmode, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_mc_operatingmode(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_OperatingMode. */
    (void)memcpy(buf, s_mock_mc_operatingmode, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE501 — MC_ResolverOffset_raw.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_resolveroffset_raw, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_resolveroffset_raw(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_ResolverOffset_raw. */
    (void)memcpy(buf, s_mock_mc_resolveroffset_raw, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE502 — MC_TorqueScalingFactor_01pct.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_torquescalingfactor_01pct, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_torquescalingfactor_01pct(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_TorqueScalingFactor_01pct. */
    (void)memcpy(buf, s_mock_mc_torquescalingfactor_01pct, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0xE503 — MC_CurrentSensorTrim_100uA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_mc_currentsensortrim_100ua, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_mc_currentsensortrim_100ua(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)2U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for MC_CurrentSensorTrim_100uA. */
    (void)memcpy(buf, s_mock_mc_currentsensortrim_100ua, (size_t)2U);
    *out_len = (uint16_t)2U;

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
 * @brief Write handler for DID 0xF187 — VehicleManufacturerSparePartNumber.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 11 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'VehicleManufacturerSparePartNumber'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (11 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 11.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 11.
 */
uds_status_t did_write_vehiclemanufacturersparepartnumber(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)11U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_vehiclemanufacturersparepartnumber, buf, (size_t)11U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0xE501 — MC_ResolverOffset_raw.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 2 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'MC_ResolverOffset_raw'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 2.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 2.
 */
uds_status_t did_write_mc_resolveroffset_raw(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)2U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_mc_resolveroffset_raw, buf, (size_t)2U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0xE502 — MC_TorqueScalingFactor_01pct.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 2 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'MC_TorqueScalingFactor_01pct'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 2.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 2.
 */
uds_status_t did_write_mc_torquescalingfactor_01pct(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)2U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_mc_torquescalingfactor_01pct, buf, (size_t)2U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0xE503 — MC_CurrentSensorTrim_100uA.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 2 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'MC_CurrentSensorTrim_100uA'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 2.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 2.
 */
uds_status_t did_write_mc_currentsensortrim_100ua(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)2U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_mc_currentsensortrim_100ua, buf, (size_t)2U);

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
    /* ── DID 0xF190 — VehicleIdentificationNumber ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61840U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)17U,
        .read_cb            = did_read_vehicleidentificationnumber,
        .write_cb           = NULL,
        .description        = "VehicleIdentificationNumber"
    },
    /* ── DID 0xF18C — ECUSerialNumber ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61836U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)8U,
        .read_cb            = did_read_ecuserialnumber,
        .write_cb           = NULL,
        .description        = "ECUSerialNumber"
    },
    /* ── DID 0xF187 — VehicleManufacturerSparePartNumber ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61831U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)11U,
        .read_cb            = did_read_vehiclemanufacturersparepartnumber,
        .write_cb           = did_write_vehiclemanufacturersparepartnumber,
        .description        = "VehicleManufacturerSparePartNumber"
    },
    /* ── DID 0xF189 — ECUSoftwareVersionNumber ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61833U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_ecusoftwareversionnumber,
        .write_cb           = NULL,
        .description        = "ECUSoftwareVersionNumber"
    },
    /* ── DID 0xE001 — MC_RotorSpeed_rpm ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57345U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_rotorspeed_rpm,
        .write_cb           = NULL,
        .description        = "MC_RotorSpeed_rpm"
    },
    /* ── DID 0xE002 — MC_TorqueDemand_01Nm ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57346U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_torquedemand_01nm,
        .write_cb           = NULL,
        .description        = "MC_TorqueDemand_01Nm"
    },
    /* ── DID 0xE003 — MC_TorqueActual_01Nm ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57347U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_torqueactual_01nm,
        .write_cb           = NULL,
        .description        = "MC_TorqueActual_01Nm"
    },
    /* ── DID 0xE004 — MC_RotorElectricalAngle_raw ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57348U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_rotorelectricalangle_raw,
        .write_cb           = NULL,
        .description        = "MC_RotorElectricalAngle_raw"
    },
    /* ── DID 0xE005 — MC_MechanicalPower_10W ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57349U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_mechanicalpower_10w,
        .write_cb           = NULL,
        .description        = "MC_MechanicalPower_10W"
    },
    /* ── DID 0xE101 — MC_PhaseA_Current_100mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57601U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_phasea_current_100ma,
        .write_cb           = NULL,
        .description        = "MC_PhaseA_Current_100mA"
    },
    /* ── DID 0xE102 — MC_PhaseB_Current_100mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57602U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_phaseb_current_100ma,
        .write_cb           = NULL,
        .description        = "MC_PhaseB_Current_100mA"
    },
    /* ── DID 0xE103 — MC_PhaseC_Current_100mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57603U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_phasec_current_100ma,
        .write_cb           = NULL,
        .description        = "MC_PhaseC_Current_100mA"
    },
    /* ── DID 0xE104 — MC_Iq_Current_100mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57604U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_iq_current_100ma,
        .write_cb           = NULL,
        .description        = "MC_Iq_Current_100mA"
    },
    /* ── DID 0xE105 — MC_Id_Current_100mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57605U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_id_current_100ma,
        .write_cb           = NULL,
        .description        = "MC_Id_Current_100mA"
    },
    /* ── DID 0xE201 — MC_DCLink_Voltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57857U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_mc_dclink_voltage_mv,
        .write_cb           = NULL,
        .description        = "MC_DCLink_Voltage_mV"
    },
    /* ── DID 0xE202 — MC_DCLink_Current_100mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57858U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_dclink_current_100ma,
        .write_cb           = NULL,
        .description        = "MC_DCLink_Current_100mA"
    },
    /* ── DID 0xE203 — MC_ModulationIndex_04pct ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)57859U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_mc_modulationindex_04pct,
        .write_cb           = NULL,
        .description        = "MC_ModulationIndex_04pct"
    },
    /* ── DID 0xE301 — MC_IGBT_PhaseA_JunctionTemp_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58113U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_mc_igbt_phasea_junctiontemp_degc,
        .write_cb           = NULL,
        .description        = "MC_IGBT_PhaseA_JunctionTemp_degC"
    },
    /* ── DID 0xE302 — MC_IGBT_PhaseB_JunctionTemp_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58114U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_mc_igbt_phaseb_junctiontemp_degc,
        .write_cb           = NULL,
        .description        = "MC_IGBT_PhaseB_JunctionTemp_degC"
    },
    /* ── DID 0xE303 — MC_IGBT_PhaseC_JunctionTemp_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58115U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_mc_igbt_phasec_junctiontemp_degc,
        .write_cb           = NULL,
        .description        = "MC_IGBT_PhaseC_JunctionTemp_degC"
    },
    /* ── DID 0xE304 — MC_MotorStatorTemp_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58116U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_mc_motorstatortemp_degc,
        .write_cb           = NULL,
        .description        = "MC_MotorStatorTemp_degC"
    },
    /* ── DID 0xE305 — MC_CoolantInletTemp_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58117U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_mc_coolantinlettemp_degc,
        .write_cb           = NULL,
        .description        = "MC_CoolantInletTemp_degC"
    },
    /* ── DID 0xE401 — MC_FaultStatusBitmask ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58369U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_faultstatusbitmask,
        .write_cb           = NULL,
        .description        = "MC_FaultStatusBitmask"
    },
    /* ── DID 0xE402 — MC_OperatingMode ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58370U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_mc_operatingmode,
        .write_cb           = NULL,
        .description        = "MC_OperatingMode"
    },
    /* ── DID 0xE501 — MC_ResolverOffset_raw ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58625U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_resolveroffset_raw,
        .write_cb           = did_write_mc_resolveroffset_raw,
        .description        = "MC_ResolverOffset_raw"
    },
    /* ── DID 0xE502 — MC_TorqueScalingFactor_01pct ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58626U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_torquescalingfactor_01pct,
        .write_cb           = did_write_mc_torquescalingfactor_01pct,
        .description        = "MC_TorqueScalingFactor_01pct"
    },
    /* ── DID 0xE503 — MC_CurrentSensorTrim_100uA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)58627U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_mc_currentsensortrim_100ua,
        .write_cb           = did_write_mc_currentsensortrim_100ua,
        .description        = "MC_CurrentSensorTrim_100uA"
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
 * @return UDS_STATUS_OK if all 27 DID(s) registered successfully.
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
