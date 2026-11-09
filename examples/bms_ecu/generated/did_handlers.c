/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.c
 *
 * ECU       : BMS_MainController
 * Version   : 1.0.0
 * Generated : 2026-11-09T12:29:04Z
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
static uint8_t s_mock_vehicleidentificationnumber[17U] = { 0x42U, 0x4DU, 0x53U, 0x45U, 0x58U, 0x41U, 0x4DU, 0x50U, 0x4CU, 0x45U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x31U };

/** Stub backing store for DID 0xF18C — ECUSerialNumber (8 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECUSerialNumber'. */
static uint8_t s_mock_ecuserialnumber[8U] = { 0x42U, 0x4DU, 0x53U, 0x30U, 0x30U, 0x30U, 0x30U, 0x31U };

/** Stub backing store for DID 0xF187 — VehicleManufacturerSparePartNumber (11 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'VehicleManufacturerSparePartNumber'. */
static uint8_t s_mock_vehiclemanufacturersparepartnumber[11U] = { 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U, 0x30U };

/** Stub backing store for DID 0xF189 — ECUSoftwareVersionNumber (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECUSoftwareVersionNumber'. */
static uint8_t s_mock_ecusoftwareversionnumber[4U] = { 0x01U, 0x00U, 0x00U, 0x00U };

/* =============================================================================
 * Application-owned DIDs (source: app, EDS-toolchain#201)
 *
 * No backing store and no handler body is generated for these. The
 * application defines the functions declared in did_handlers.h; a missing
 * definition is a link error. The registration table and the safety wrappers
 * below still reference them, so session and security gating is unchanged.
 * ============================================================================= */
/*   DID 0xDB00 — BMS_PackVoltage_mV: did_read_bms_packvoltage_mv() */
/*   DID 0xDB01 — BMS_PackCurrent_100mA: did_read_bms_packcurrent_100ma() */
/*   DID 0xDB02 — BMS_PackPower_10W: did_read_bms_packpower_10w() */
/*   DID 0xDB03 — BMS_ContactorState: did_read_bms_contactorstate() */
/*   DID 0xDB04 — BMS_InsulationResistance_kOhm: did_read_bms_insulationresistance_kohm() */
/*   DID 0xDC01 — BMS_CellGroup1_Voltage_mV: did_read_bms_cellgroup1_voltage_mv() */
/*   DID 0xDC02 — BMS_CellGroup2_Voltage_mV: did_read_bms_cellgroup2_voltage_mv() */
/*   DID 0xDC03 — BMS_CellGroup3_Voltage_mV: did_read_bms_cellgroup3_voltage_mv() */
/*   DID 0xDC04 — BMS_CellGroup4_Voltage_mV: did_read_bms_cellgroup4_voltage_mv() */
/*   DID 0xDD00 — BMS_MaxCellTemperature_degC: did_read_bms_maxcelltemperature_degc() */
/*   DID 0xDD01 — BMS_MinCellTemperature_degC: did_read_bms_mincelltemperature_degc() */
/*   DID 0xDD02 — BMS_CoolantInletTemperature_degC: did_read_bms_coolantinlettemperature_degc() */
/*   DID 0xDD03 — BMS_BMSBoardTemperature_degC: did_read_bms_bmsboardtemperature_degc() */
/*   DID 0xDE00 — BMS_StateOfCharge_04pct: did_read_bms_stateofcharge_04pct() */
/*   DID 0xDE01 — BMS_StateOfHealth_04pct: did_read_bms_stateofhealth_04pct() */
/*   DID 0xDE02 — BMS_BalancingStateBitmask: did_read_bms_balancingstatebitmask() */
/*   DID 0xD900 — BMS_FaultFlagsBitmask: did_read_bms_faultflagsbitmask() */
/*   DID 0xD901 — BMS_FullChargeCapacity_Wh: did_read_bms_fullchargecapacity_wh() */
/*   DID 0xD910 — BMS_OVThreshold_mV: did_read_bms_ovthreshold_mv() did_write_bms_ovthreshold_mv() */
/*   DID 0xD911 — BMS_UVThreshold_mV: did_read_bms_uvthreshold_mv() did_write_bms_uvthreshold_mv() */


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
    /* ── DID 0xDB00 — BMS_PackVoltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56064U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_bms_packvoltage_mv,
        .write_cb           = NULL,
        .description        = "BMS_PackVoltage_mV"
    },
    /* ── DID 0xDB01 — BMS_PackCurrent_100mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56065U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_packcurrent_100ma,
        .write_cb           = NULL,
        .description        = "BMS_PackCurrent_100mA"
    },
    /* ── DID 0xDB02 — BMS_PackPower_10W ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56066U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_packpower_10w,
        .write_cb           = NULL,
        .description        = "BMS_PackPower_10W"
    },
    /* ── DID 0xDB03 — BMS_ContactorState ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56067U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_bms_contactorstate,
        .write_cb           = NULL,
        .description        = "BMS_ContactorState"
    },
    /* ── DID 0xDB04 — BMS_InsulationResistance_kOhm ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56068U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_insulationresistance_kohm,
        .write_cb           = NULL,
        .description        = "BMS_InsulationResistance_kOhm"
    },
    /* ── DID 0xDC01 — BMS_CellGroup1_Voltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56321U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_cellgroup1_voltage_mv,
        .write_cb           = NULL,
        .description        = "BMS_CellGroup1_Voltage_mV"
    },
    /* ── DID 0xDC02 — BMS_CellGroup2_Voltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56322U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_cellgroup2_voltage_mv,
        .write_cb           = NULL,
        .description        = "BMS_CellGroup2_Voltage_mV"
    },
    /* ── DID 0xDC03 — BMS_CellGroup3_Voltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56323U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_cellgroup3_voltage_mv,
        .write_cb           = NULL,
        .description        = "BMS_CellGroup3_Voltage_mV"
    },
    /* ── DID 0xDC04 — BMS_CellGroup4_Voltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56324U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_cellgroup4_voltage_mv,
        .write_cb           = NULL,
        .description        = "BMS_CellGroup4_Voltage_mV"
    },
    /* ── DID 0xDD00 — BMS_MaxCellTemperature_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56576U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_bms_maxcelltemperature_degc,
        .write_cb           = NULL,
        .description        = "BMS_MaxCellTemperature_degC"
    },
    /* ── DID 0xDD01 — BMS_MinCellTemperature_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56577U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_bms_mincelltemperature_degc,
        .write_cb           = NULL,
        .description        = "BMS_MinCellTemperature_degC"
    },
    /* ── DID 0xDD02 — BMS_CoolantInletTemperature_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56578U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_bms_coolantinlettemperature_degc,
        .write_cb           = NULL,
        .description        = "BMS_CoolantInletTemperature_degC"
    },
    /* ── DID 0xDD03 — BMS_BMSBoardTemperature_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56579U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_bms_bmsboardtemperature_degc,
        .write_cb           = NULL,
        .description        = "BMS_BMSBoardTemperature_degC"
    },
    /* ── DID 0xDE00 — BMS_StateOfCharge_04pct ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56832U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_bms_stateofcharge_04pct,
        .write_cb           = NULL,
        .description        = "BMS_StateOfCharge_04pct"
    },
    /* ── DID 0xDE01 — BMS_StateOfHealth_04pct ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56833U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_bms_stateofhealth_04pct,
        .write_cb           = NULL,
        .description        = "BMS_StateOfHealth_04pct"
    },
    /* ── DID 0xDE02 — BMS_BalancingStateBitmask ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)56834U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_bms_balancingstatebitmask,
        .write_cb           = NULL,
        .description        = "BMS_BalancingStateBitmask"
    },
    /* ── DID 0xD900 — BMS_FaultFlagsBitmask ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)55552U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_faultflagsbitmask,
        .write_cb           = NULL,
        .description        = "BMS_FaultFlagsBitmask"
    },
    /* ── DID 0xD901 — BMS_FullChargeCapacity_Wh ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)55553U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_bms_fullchargecapacity_wh,
        .write_cb           = NULL,
        .description        = "BMS_FullChargeCapacity_Wh"
    },
    /* ── DID 0xD910 — BMS_OVThreshold_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)55568U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_ovthreshold_mv,
        .write_cb           = did_write_bms_ovthreshold_mv,
        .description        = "BMS_OVThreshold_mV"
    },
    /* ── DID 0xD911 — BMS_UVThreshold_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)55569U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_bms_uvthreshold_mv,
        .write_cb           = did_write_bms_uvthreshold_mv,
        .description        = "BMS_UVThreshold_mV"
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
 * @return UDS_STATUS_OK if all 24 DID(s) registered successfully.
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
