/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.h
 *
 * ECU       : ARDEP_IOController
 * Version   : 1.0.0
 * Generated : 2026-05-19T13:34:41Z
 *
 * PURPOSE: Declarations for all generated DID read/write handler functions
 *          and the generated DID registration entry point.
 *
 *          Architecture:
 *            diagnostics_config.yaml (dids section)
 *              -> tools/codegen.py (build_did_handlers_context)
 *                -> tools/templates/did_handlers.h.j2
 *                  -> generated/did_handlers.h  (this file)
 *
 *          This design allows DID handler stubs to be automatically generated
 *          from YAML configuration without modifying any core stack headers.
 *
 * WARNING: DO NOT EDIT MANUALLY.
 *          Regenerate: python3 tools/codegen.py --config <yaml> --out generated/
 *
 * SAFETY  : DID handlers provide read access to ECU measurement data and
 *           write access to configuration data. Session and security gating
 *           is enforced at the service layer (service_0x22.c / service_0x2E.c).
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef DID_HANDLERS_H
#define DID_HANDLERS_H

#include "uds_types.h"
#include "did_database.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Generated DID count for this ECU configuration.
 * Must match GEN_DID_COUNT in generated_config.h.
 */
#define GEN_DID_HANDLER_COUNT (35U)

/* --------------------------------------------------------------------------
 * Generated DID read handler declarations
 *
 * One function per DID that has 'read' in its access list.
 * All conform to the did_read_cb_fn prototype from did_database.h.
 * -------------------------------------------------------------------------- */

/**
 * @brief Read handler for DID 0xF190 — VehicleIdentificationNumber.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 17 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for VehicleIdentificationNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 17).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_vehicleidentificationnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xF18C — ECUSerialNumber.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 8 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECUSerialNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 8).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecuserialnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xF187 — VehicleManufacturerSparePartNumber.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 11 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for VehicleManufacturerSparePartNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 11).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_vehiclemanufacturersparepartnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xF189 — ECUSoftwareVersionNumber.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECUSoftwareVersionNumber.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecusoftwareversionnumber(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xF197 — SystemSupplierIdentifierDataIdentifier.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 5 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for SystemSupplierIdentifierDataIdentifier.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 5).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_systemsupplieridentifierdataidentifier(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2001 — PowerIO_OutputStateBitmask.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_OutputStateBitmask.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_outputstatebitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2002 — PowerIO_Output1_Current_mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Output1_Current_mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_output1_current_ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2003 — PowerIO_Output2_Current_mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Output2_Current_mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_output2_current_ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2004 — PowerIO_Output3_Current_mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Output3_Current_mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_output3_current_ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2005 — PowerIO_Output4_Current_mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Output4_Current_mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_output4_current_ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2006 — PowerIO_Output5_Current_mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Output5_Current_mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_output5_current_ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2007 — PowerIO_Output6_Current_mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Output6_Current_mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_output6_current_ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2010 — PowerIO_OutputControl.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_OutputControl.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_outputcontrol(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2101 — PowerIO_InputStateBitmask.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_InputStateBitmask.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_inputstatebitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2102 — PowerIO_Input1_Voltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Input1_Voltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_input1_voltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2103 — PowerIO_Input2_Voltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Input2_Voltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_input2_voltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2104 — PowerIO_Input3_Voltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_Input3_Voltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_input3_voltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2201 — CAN_BusStatus.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for CAN_BusStatus.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_can_busstatus(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2202 — CAN_RxFrameCount.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for CAN_RxFrameCount.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_can_rxframecount(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2203 — CAN_TxFrameCount.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for CAN_TxFrameCount.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_can_txframecount(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2211 — LIN_BusStatus.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for LIN_BusStatus.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_lin_busstatus(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2212 — LIN_SlaveCount.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for LIN_SlaveCount.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_lin_slavecount(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2301 — ECU_SupplyVoltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECU_SupplyVoltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecu_supplyvoltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2302 — ECU_InternalTemperature_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECU_InternalTemperature_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecu_internaltemperature_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2303 — ECU_UptimeSeconds.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECU_UptimeSeconds.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecu_uptimeseconds(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2304 — ECU_ResetCounter.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECU_ResetCounter.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecu_resetcounter(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2305 — ECU_LastResetReason.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECU_LastResetReason.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecu_lastresetreason(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2306 — ECU_DiagnosticStackVersion.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 3 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for ECU_DiagnosticStackVersion.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 3).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_ecu_diagnosticstackversion(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2401 — CAN_BusBitrate_kbps.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for CAN_BusBitrate_kbps.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_can_busbitrate_kbps(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2402 — LIN_BusBaudrate_bps.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for LIN_BusBaudrate_bps.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_lin_busbaudrate_bps(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2403 — PowerIO_OvercurrentThreshold_mA.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for PowerIO_OvercurrentThreshold_mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_powerio_overcurrentthreshold_ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2404 — WatchdogTimeout_ms.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for WatchdogTimeout_ms.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_watchdogtimeout_ms(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2501 — FirmwareVersion_Active.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for FirmwareVersion_Active.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_firmwareversion_active(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2502 — FirmwareVersion_Pending.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for FirmwareVersion_Pending.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_firmwareversion_pending(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0x2503 — FirmwareUpdateStatus.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for FirmwareUpdateStatus.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_firmwareupdatestatus(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/* --------------------------------------------------------------------------
 * Generated DID write handler declarations
 * -------------------------------------------------------------------------- */

/**
 * @brief Write handler for DID 0xF187 — VehicleManufacturerSparePartNumber.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 11 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for VehicleManufacturerSparePartNumber.
 *
 * @param[in] buf  Buffer containing new DID data (11 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_vehiclemanufacturersparepartnumber(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0x2010 — PowerIO_OutputControl.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for PowerIO_OutputControl.
 *
 * @param[in] buf  Buffer containing new DID data (1 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_powerio_outputcontrol(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0x2401 — CAN_BusBitrate_kbps.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for CAN_BusBitrate_kbps.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_can_busbitrate_kbps(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0x2402 — LIN_BusBaudrate_bps.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for LIN_BusBaudrate_bps.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_lin_busbaudrate_bps(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0x2403 — PowerIO_OvercurrentThreshold_mA.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for PowerIO_OvercurrentThreshold_mA.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_powerio_overcurrentthreshold_ma(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0x2404 — WatchdogTimeout_ms.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for WatchdogTimeout_ms.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_watchdogtimeout_ms(
    const uint8_t *buf,
    uint16_t       len);

/* --------------------------------------------------------------------------
 * Registration entry point
 * -------------------------------------------------------------------------- */

/**
 * @brief Register all generated DID entries with the DID database.
 *
 * Must be called after did_database_init() and before any UDS requests
 * are processed. Called by uds_generated_init().
 *
 * @return UDS_STATUS_OK if all DIDs registered successfully.
 * @return UDS_STATUS_ERR_* if any registration fails.
 */
uds_status_t did_handlers_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* DID_HANDLERS_H */
