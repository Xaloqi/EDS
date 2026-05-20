/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.c
 *
 * ECU       : ARDEP_IOController
 * Version   : 1.0.0
 * Generated : 2026-05-20T07:21:48Z
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

/** Stub backing store for DID 0xF197 — SystemSupplierIdentifierDataIdentifier (5 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'SystemSupplierIdentifierDataIdentifier'. */
static uint8_t s_mock_systemsupplieridentifierdataidentifier[5U] = { 0U };

/** Stub backing store for DID 0x2001 — PowerIO_OutputStateBitmask (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_OutputStateBitmask'. */
static uint8_t s_mock_powerio_outputstatebitmask[1U] = { 0U };

/** Stub backing store for DID 0x2002 — PowerIO_Output1_Current_mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Output1_Current_mA'. */
static uint8_t s_mock_powerio_output1_current_ma[2U] = { 0U };

/** Stub backing store for DID 0x2003 — PowerIO_Output2_Current_mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Output2_Current_mA'. */
static uint8_t s_mock_powerio_output2_current_ma[2U] = { 0U };

/** Stub backing store for DID 0x2004 — PowerIO_Output3_Current_mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Output3_Current_mA'. */
static uint8_t s_mock_powerio_output3_current_ma[2U] = { 0U };

/** Stub backing store for DID 0x2005 — PowerIO_Output4_Current_mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Output4_Current_mA'. */
static uint8_t s_mock_powerio_output4_current_ma[2U] = { 0U };

/** Stub backing store for DID 0x2006 — PowerIO_Output5_Current_mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Output5_Current_mA'. */
static uint8_t s_mock_powerio_output5_current_ma[2U] = { 0U };

/** Stub backing store for DID 0x2007 — PowerIO_Output6_Current_mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Output6_Current_mA'. */
static uint8_t s_mock_powerio_output6_current_ma[2U] = { 0U };

/** Stub backing store for DID 0x2010 — PowerIO_OutputControl (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_OutputControl'. */
static uint8_t s_mock_powerio_outputcontrol[1U] = { 0U };

/** Stub backing store for DID 0x2101 — PowerIO_InputStateBitmask (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_InputStateBitmask'. */
static uint8_t s_mock_powerio_inputstatebitmask[1U] = { 0U };

/** Stub backing store for DID 0x2102 — PowerIO_Input1_Voltage_mV (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Input1_Voltage_mV'. */
static uint8_t s_mock_powerio_input1_voltage_mv[2U] = { 0U };

/** Stub backing store for DID 0x2103 — PowerIO_Input2_Voltage_mV (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Input2_Voltage_mV'. */
static uint8_t s_mock_powerio_input2_voltage_mv[2U] = { 0U };

/** Stub backing store for DID 0x2104 — PowerIO_Input3_Voltage_mV (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_Input3_Voltage_mV'. */
static uint8_t s_mock_powerio_input3_voltage_mv[2U] = { 0U };

/** Stub backing store for DID 0x2201 — CAN_BusStatus (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'CAN_BusStatus'. */
static uint8_t s_mock_can_busstatus[2U] = { 0U };

/** Stub backing store for DID 0x2202 — CAN_RxFrameCount (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'CAN_RxFrameCount'. */
static uint8_t s_mock_can_rxframecount[4U] = { 0U };

/** Stub backing store for DID 0x2203 — CAN_TxFrameCount (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'CAN_TxFrameCount'. */
static uint8_t s_mock_can_txframecount[4U] = { 0U };

/** Stub backing store for DID 0x2211 — LIN_BusStatus (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'LIN_BusStatus'. */
static uint8_t s_mock_lin_busstatus[1U] = { 0U };

/** Stub backing store for DID 0x2212 — LIN_SlaveCount (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'LIN_SlaveCount'. */
static uint8_t s_mock_lin_slavecount[1U] = { 0U };

/** Stub backing store for DID 0x2301 — ECU_SupplyVoltage_mV (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECU_SupplyVoltage_mV'. */
static uint8_t s_mock_ecu_supplyvoltage_mv[2U] = { 0U };

/** Stub backing store for DID 0x2302 — ECU_InternalTemperature_degC (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECU_InternalTemperature_degC'. */
static uint8_t s_mock_ecu_internaltemperature_degc[1U] = { 0U };

/** Stub backing store for DID 0x2303 — ECU_UptimeSeconds (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECU_UptimeSeconds'. */
static uint8_t s_mock_ecu_uptimeseconds[4U] = { 0U };

/** Stub backing store for DID 0x2304 — ECU_ResetCounter (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECU_ResetCounter'. */
static uint8_t s_mock_ecu_resetcounter[2U] = { 0U };

/** Stub backing store for DID 0x2305 — ECU_LastResetReason (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECU_LastResetReason'. */
static uint8_t s_mock_ecu_lastresetreason[1U] = { 0U };

/** Stub backing store for DID 0x2306 — ECU_DiagnosticStackVersion (3 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'ECU_DiagnosticStackVersion'. */
static uint8_t s_mock_ecu_diagnosticstackversion[3U] = { 0U };

/** Stub backing store for DID 0x2401 — CAN_BusBitrate_kbps (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'CAN_BusBitrate_kbps'. */
static uint8_t s_mock_can_busbitrate_kbps[2U] = { 0U };

/** Stub backing store for DID 0x2402 — LIN_BusBaudrate_bps (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'LIN_BusBaudrate_bps'. */
static uint8_t s_mock_lin_busbaudrate_bps[2U] = { 0U };

/** Stub backing store for DID 0x2403 — PowerIO_OvercurrentThreshold_mA (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'PowerIO_OvercurrentThreshold_mA'. */
static uint8_t s_mock_powerio_overcurrentthreshold_ma[2U] = { 0U };

/** Stub backing store for DID 0x2404 — WatchdogTimeout_ms (2 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'WatchdogTimeout_ms'. */
static uint8_t s_mock_watchdogtimeout_ms[2U] = { 0U };

/** Stub backing store for DID 0x2501 — FirmwareVersion_Active (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'FirmwareVersion_Active'. */
static uint8_t s_mock_firmwareversion_active[4U] = { 0U };

/** Stub backing store for DID 0x2502 — FirmwareVersion_Pending (4 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'FirmwareVersion_Pending'. */
static uint8_t s_mock_firmwareversion_pending[4U] = { 0U };

/** Stub backing store for DID 0x2503 — FirmwareUpdateStatus (1 byte(s)). */
/* TODO [APPLICATION]: Replace with real data source for 'FirmwareUpdateStatus'. */
static uint8_t s_mock_firmwareupdatestatus[1U] = { 0U };


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
 * @brief Read handler for DID 0xF197 — SystemSupplierIdentifierDataIdentifier.
 *
 * Stub: copies 5 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 5 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_systemsupplieridentifierdataidentifier, 5U);
 *     *out_len = 5U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 5.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 5.
 */
uds_status_t did_read_systemsupplieridentifierdataidentifier(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)5U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for SystemSupplierIdentifierDataIdentifier. */
    (void)memcpy(buf, s_mock_systemsupplieridentifierdataidentifier, (size_t)5U);
    *out_len = (uint16_t)5U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2001 — PowerIO_OutputStateBitmask.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_outputstatebitmask, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_powerio_outputstatebitmask(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_OutputStateBitmask. */
    (void)memcpy(buf, s_mock_powerio_outputstatebitmask, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2002 — PowerIO_Output1_Current_mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_output1_current_ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_output1_current_ma(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Output1_Current_mA. */
    (void)memcpy(buf, s_mock_powerio_output1_current_ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2003 — PowerIO_Output2_Current_mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_output2_current_ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_output2_current_ma(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Output2_Current_mA. */
    (void)memcpy(buf, s_mock_powerio_output2_current_ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2004 — PowerIO_Output3_Current_mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_output3_current_ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_output3_current_ma(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Output3_Current_mA. */
    (void)memcpy(buf, s_mock_powerio_output3_current_ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2005 — PowerIO_Output4_Current_mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_output4_current_ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_output4_current_ma(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Output4_Current_mA. */
    (void)memcpy(buf, s_mock_powerio_output4_current_ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2006 — PowerIO_Output5_Current_mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_output5_current_ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_output5_current_ma(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Output5_Current_mA. */
    (void)memcpy(buf, s_mock_powerio_output5_current_ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2007 — PowerIO_Output6_Current_mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_output6_current_ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_output6_current_ma(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Output6_Current_mA. */
    (void)memcpy(buf, s_mock_powerio_output6_current_ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2010 — PowerIO_OutputControl.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_outputcontrol, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_powerio_outputcontrol(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_OutputControl. */
    (void)memcpy(buf, s_mock_powerio_outputcontrol, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2101 — PowerIO_InputStateBitmask.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_inputstatebitmask, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_powerio_inputstatebitmask(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_InputStateBitmask. */
    (void)memcpy(buf, s_mock_powerio_inputstatebitmask, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2102 — PowerIO_Input1_Voltage_mV.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_input1_voltage_mv, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_input1_voltage_mv(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Input1_Voltage_mV. */
    (void)memcpy(buf, s_mock_powerio_input1_voltage_mv, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2103 — PowerIO_Input2_Voltage_mV.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_input2_voltage_mv, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_input2_voltage_mv(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Input2_Voltage_mV. */
    (void)memcpy(buf, s_mock_powerio_input2_voltage_mv, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2104 — PowerIO_Input3_Voltage_mV.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_input3_voltage_mv, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_input3_voltage_mv(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_Input3_Voltage_mV. */
    (void)memcpy(buf, s_mock_powerio_input3_voltage_mv, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2201 — CAN_BusStatus.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_can_busstatus, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_can_busstatus(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for CAN_BusStatus. */
    (void)memcpy(buf, s_mock_can_busstatus, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2202 — CAN_RxFrameCount.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_can_rxframecount, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_can_rxframecount(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for CAN_RxFrameCount. */
    (void)memcpy(buf, s_mock_can_rxframecount, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2203 — CAN_TxFrameCount.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_can_txframecount, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_can_txframecount(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for CAN_TxFrameCount. */
    (void)memcpy(buf, s_mock_can_txframecount, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2211 — LIN_BusStatus.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_lin_busstatus, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_lin_busstatus(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for LIN_BusStatus. */
    (void)memcpy(buf, s_mock_lin_busstatus, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2212 — LIN_SlaveCount.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_lin_slavecount, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_lin_slavecount(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for LIN_SlaveCount. */
    (void)memcpy(buf, s_mock_lin_slavecount, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2301 — ECU_SupplyVoltage_mV.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecu_supplyvoltage_mv, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_ecu_supplyvoltage_mv(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECU_SupplyVoltage_mV. */
    (void)memcpy(buf, s_mock_ecu_supplyvoltage_mv, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2302 — ECU_InternalTemperature_degC.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecu_internaltemperature_degc, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_ecu_internaltemperature_degc(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECU_InternalTemperature_degC. */
    (void)memcpy(buf, s_mock_ecu_internaltemperature_degc, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2303 — ECU_UptimeSeconds.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecu_uptimeseconds, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_ecu_uptimeseconds(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECU_UptimeSeconds. */
    (void)memcpy(buf, s_mock_ecu_uptimeseconds, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2304 — ECU_ResetCounter.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecu_resetcounter, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_ecu_resetcounter(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECU_ResetCounter. */
    (void)memcpy(buf, s_mock_ecu_resetcounter, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2305 — ECU_LastResetReason.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecu_lastresetreason, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_ecu_lastresetreason(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECU_LastResetReason. */
    (void)memcpy(buf, s_mock_ecu_lastresetreason, (size_t)1U);
    *out_len = (uint16_t)1U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2306 — ECU_DiagnosticStackVersion.
 *
 * Stub: copies 3 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 3 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_ecu_diagnosticstackversion, 3U);
 *     *out_len = 3U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 3.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 3.
 */
uds_status_t did_read_ecu_diagnosticstackversion(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (buf_len < (uint16_t)3U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for ECU_DiagnosticStackVersion. */
    (void)memcpy(buf, s_mock_ecu_diagnosticstackversion, (size_t)3U);
    *out_len = (uint16_t)3U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2401 — CAN_BusBitrate_kbps.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_can_busbitrate_kbps, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_can_busbitrate_kbps(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for CAN_BusBitrate_kbps. */
    (void)memcpy(buf, s_mock_can_busbitrate_kbps, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2402 — LIN_BusBaudrate_bps.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_lin_busbaudrate_bps, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_lin_busbaudrate_bps(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for LIN_BusBaudrate_bps. */
    (void)memcpy(buf, s_mock_lin_busbaudrate_bps, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2403 — PowerIO_OvercurrentThreshold_mA.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_powerio_overcurrentthreshold_ma, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_powerio_overcurrentthreshold_ma(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for PowerIO_OvercurrentThreshold_mA. */
    (void)memcpy(buf, s_mock_powerio_overcurrentthreshold_ma, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2404 — WatchdogTimeout_ms.
 *
 * Stub: copies 2 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 0
 * DATA LENGTH  : 2 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_watchdogtimeout_ms, 2U);
 *     *out_len = 2U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 2.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 2.
 */
uds_status_t did_read_watchdogtimeout_ms(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for WatchdogTimeout_ms. */
    (void)memcpy(buf, s_mock_watchdogtimeout_ms, (size_t)2U);
    *out_len = (uint16_t)2U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2501 — FirmwareVersion_Active.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_firmwareversion_active, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_firmwareversion_active(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for FirmwareVersion_Active. */
    (void)memcpy(buf, s_mock_firmwareversion_active, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2502 — FirmwareVersion_Pending.
 *
 * Stub: copies 4 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 4 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_firmwareversion_pending, 4U);
 *     *out_len = 4U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 4.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 4.
 */
uds_status_t did_read_firmwareversion_pending(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for FirmwareVersion_Pending. */
    (void)memcpy(buf, s_mock_firmwareversion_pending, (size_t)4U);
    *out_len = (uint16_t)4U;

    return UDS_STATUS_OK;
}

/**
 * @brief Read handler for DID 0x2503 — FirmwareUpdateStatus.
 *
 * Stub: copies 1 zero byte(s) into buf.
 *
 * MIN SESSION  : UDS_SESSION_DEFAULT
 * SEC LEVEL    : 0
 * DATA LENGTH  : 1 byte(s)
 *
 * TODO [APPLICATION]: Replace the memcpy below with real data access.
 *   Example: read from NVM or driver:
 *     (void)memcpy(buf, real_data_source_for_firmwareupdatestatus, 1U);
 *     *out_len = 1U;
 *
 * @param[out] buf      Buffer to receive DID data (caller-allocated).
 * @param[in]  buf_len  Capacity of buf in bytes. Must be >= 1.
 * @param[out] out_len  Set to number of bytes written on success.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf or out_len is NULL.
 * @return UDS_STATUS_ERR_BUFFER_OVERFLOW if buf_len < 1.
 */
uds_status_t did_read_firmwareupdatestatus(
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

    /* TODO [APPLICATION]: Replace with real sensor/NVM access for FirmwareUpdateStatus. */
    (void)memcpy(buf, s_mock_firmwareupdatestatus, (size_t)1U);
    *out_len = (uint16_t)1U;

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
 * @brief Write handler for DID 0x2010 — PowerIO_OutputControl.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 1 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'PowerIO_OutputControl'.
 *
 * SAFETY: Write handlers that affect persistent calibration or actuators
 *         must include range checks and error handling. Review required.
 *
 * @param[in] buf  Pointer to new DID data (1 byte(s)).
 * @param[in] len  Number of bytes in buf. Must equal 1.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if buf is NULL.
 * @return UDS_STATUS_ERR_INVALID_PARAM if len != 1.
 */
uds_status_t did_write_powerio_outputcontrol(
    const uint8_t *buf,
    uint16_t       len)
{
    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (len != (uint16_t)1U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* TODO [APPLICATION]: Validate range, then write to NVM/actuator. */
    (void)memcpy(s_mock_powerio_outputcontrol, buf, (size_t)1U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0x2401 — CAN_BusBitrate_kbps.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 2 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'CAN_BusBitrate_kbps'.
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
uds_status_t did_write_can_busbitrate_kbps(
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
    (void)memcpy(s_mock_can_busbitrate_kbps, buf, (size_t)2U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0x2402 — LIN_BusBaudrate_bps.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 2 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'LIN_BusBaudrate_bps'.
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
uds_status_t did_write_lin_busbaudrate_bps(
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
    (void)memcpy(s_mock_lin_busbaudrate_bps, buf, (size_t)2U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0x2403 — PowerIO_OvercurrentThreshold_mA.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 2 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'PowerIO_OvercurrentThreshold_mA'.
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
uds_status_t did_write_powerio_overcurrentthreshold_ma(
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
    (void)memcpy(s_mock_powerio_overcurrentthreshold_ma, buf, (size_t)2U);

    return UDS_STATUS_OK;
}

/**
 * @brief Write handler for DID 0x2404 — WatchdogTimeout_ms.
 *
 * Stub: copies buf into the mock backing store.
 *
 * MIN SESSION  : UDS_SESSION_EXTENDED
 * SEC LEVEL    : 1
 * DATA LENGTH  : 2 byte(s) (exact length required)
 *
 * TODO [APPLICATION]: Replace stub body with NVM write, actuator command,
 *                     or calibration record update for 'WatchdogTimeout_ms'.
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
uds_status_t did_write_watchdogtimeout_ms(
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
    (void)memcpy(s_mock_watchdogtimeout_ms, buf, (size_t)2U);

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
    /* ── DID 0xF197 — SystemSupplierIdentifierDataIdentifier ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)61847U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)5U,
        .read_cb            = did_read_systemsupplieridentifierdataidentifier,
        .write_cb           = NULL,
        .description        = "SystemSupplierIdentifierDataIdentifier"
    },
    /* ── DID 0x2001 — PowerIO_OutputStateBitmask ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8193U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_powerio_outputstatebitmask,
        .write_cb           = NULL,
        .description        = "PowerIO_OutputStateBitmask"
    },
    /* ── DID 0x2002 — PowerIO_Output1_Current_mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8194U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_output1_current_ma,
        .write_cb           = NULL,
        .description        = "PowerIO_Output1_Current_mA"
    },
    /* ── DID 0x2003 — PowerIO_Output2_Current_mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8195U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_output2_current_ma,
        .write_cb           = NULL,
        .description        = "PowerIO_Output2_Current_mA"
    },
    /* ── DID 0x2004 — PowerIO_Output3_Current_mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8196U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_output3_current_ma,
        .write_cb           = NULL,
        .description        = "PowerIO_Output3_Current_mA"
    },
    /* ── DID 0x2005 — PowerIO_Output4_Current_mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8197U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_output4_current_ma,
        .write_cb           = NULL,
        .description        = "PowerIO_Output4_Current_mA"
    },
    /* ── DID 0x2006 — PowerIO_Output5_Current_mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8198U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_output5_current_ma,
        .write_cb           = NULL,
        .description        = "PowerIO_Output5_Current_mA"
    },
    /* ── DID 0x2007 — PowerIO_Output6_Current_mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8199U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_output6_current_ma,
        .write_cb           = NULL,
        .description        = "PowerIO_Output6_Current_mA"
    },
    /* ── DID 0x2010 — PowerIO_OutputControl ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8208U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_powerio_outputcontrol,
        .write_cb           = did_write_powerio_outputcontrol,
        .description        = "PowerIO_OutputControl"
    },
    /* ── DID 0x2101 — PowerIO_InputStateBitmask ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8449U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_powerio_inputstatebitmask,
        .write_cb           = NULL,
        .description        = "PowerIO_InputStateBitmask"
    },
    /* ── DID 0x2102 — PowerIO_Input1_Voltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8450U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_input1_voltage_mv,
        .write_cb           = NULL,
        .description        = "PowerIO_Input1_Voltage_mV"
    },
    /* ── DID 0x2103 — PowerIO_Input2_Voltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8451U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_input2_voltage_mv,
        .write_cb           = NULL,
        .description        = "PowerIO_Input2_Voltage_mV"
    },
    /* ── DID 0x2104 — PowerIO_Input3_Voltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8452U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_input3_voltage_mv,
        .write_cb           = NULL,
        .description        = "PowerIO_Input3_Voltage_mV"
    },
    /* ── DID 0x2201 — CAN_BusStatus ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8705U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_can_busstatus,
        .write_cb           = NULL,
        .description        = "CAN_BusStatus"
    },
    /* ── DID 0x2202 — CAN_RxFrameCount ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8706U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_can_rxframecount,
        .write_cb           = NULL,
        .description        = "CAN_RxFrameCount"
    },
    /* ── DID 0x2203 — CAN_TxFrameCount ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8707U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_can_txframecount,
        .write_cb           = NULL,
        .description        = "CAN_TxFrameCount"
    },
    /* ── DID 0x2211 — LIN_BusStatus ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8721U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_lin_busstatus,
        .write_cb           = NULL,
        .description        = "LIN_BusStatus"
    },
    /* ── DID 0x2212 — LIN_SlaveCount ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8722U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_lin_slavecount,
        .write_cb           = NULL,
        .description        = "LIN_SlaveCount"
    },
    /* ── DID 0x2301 — ECU_SupplyVoltage_mV ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8961U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_ecu_supplyvoltage_mv,
        .write_cb           = NULL,
        .description        = "ECU_SupplyVoltage_mV"
    },
    /* ── DID 0x2302 — ECU_InternalTemperature_degC ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8962U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_ecu_internaltemperature_degc,
        .write_cb           = NULL,
        .description        = "ECU_InternalTemperature_degC"
    },
    /* ── DID 0x2303 — ECU_UptimeSeconds ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8963U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_ecu_uptimeseconds,
        .write_cb           = NULL,
        .description        = "ECU_UptimeSeconds"
    },
    /* ── DID 0x2304 — ECU_ResetCounter ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8964U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_ecu_resetcounter,
        .write_cb           = NULL,
        .description        = "ECU_ResetCounter"
    },
    /* ── DID 0x2305 — ECU_LastResetReason ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8965U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_ecu_lastresetreason,
        .write_cb           = NULL,
        .description        = "ECU_LastResetReason"
    },
    /* ── DID 0x2306 — ECU_DiagnosticStackVersion ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)8966U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)3U,
        .read_cb            = did_read_ecu_diagnosticstackversion,
        .write_cb           = NULL,
        .description        = "ECU_DiagnosticStackVersion"
    },
    /* ── DID 0x2401 — CAN_BusBitrate_kbps ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)9217U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_can_busbitrate_kbps,
        .write_cb           = did_write_can_busbitrate_kbps,
        .description        = "CAN_BusBitrate_kbps"
    },
    /* ── DID 0x2402 — LIN_BusBaudrate_bps ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)9218U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_lin_busbaudrate_bps,
        .write_cb           = did_write_lin_busbaudrate_bps,
        .description        = "LIN_BusBaudrate_bps"
    },
    /* ── DID 0x2403 — PowerIO_OvercurrentThreshold_mA ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)9219U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_powerio_overcurrentthreshold_ma,
        .write_cb           = did_write_powerio_overcurrentthreshold_ma,
        .description        = "PowerIO_OvercurrentThreshold_mA"
    },
    /* ── DID 0x2404 — WatchdogTimeout_ms ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)9220U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ | DID_ACCESS_WRITE
                              ),
        .min_session        = (uint8_t)UDS_SESSION_EXTENDED,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)1U,
        .data_length        = (uint16_t)2U,
        .read_cb            = did_read_watchdogtimeout_ms,
        .write_cb           = did_write_watchdogtimeout_ms,
        .description        = "WatchdogTimeout_ms"
    },
    /* ── DID 0x2501 — FirmwareVersion_Active ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)9473U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_firmwareversion_active,
        .write_cb           = NULL,
        .description        = "FirmwareVersion_Active"
    },
    /* ── DID 0x2502 — FirmwareVersion_Pending ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)9474U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)4U,
        .read_cb            = did_read_firmwareversion_pending,
        .write_cb           = NULL,
        .description        = "FirmwareVersion_Pending"
    },
    /* ── DID 0x2503 — FirmwareUpdateStatus ─────────────────────────────────────────── */
    {
        .did_id             = (uint16_t)9475U,
        .access_flags       = (uint8_t)(
                                  DID_ACCESS_READ
                              ),
        .min_session        = (uint8_t)UDS_SESSION_DEFAULT,
        .read_access_level  = (uint8_t)0U,
        .write_access_level = (uint8_t)0U,
        .data_length        = (uint16_t)1U,
        .read_cb            = did_read_firmwareupdatestatus,
        .write_cb           = NULL,
        .description        = "FirmwareUpdateStatus"
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
 * @return UDS_STATUS_OK if all 35 DID(s) registered successfully.
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
