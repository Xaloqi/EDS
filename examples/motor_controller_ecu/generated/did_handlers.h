/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: GENERATED — did_handlers.h
 *
 * ECU       : MotorController_Inverter
 * Version   : 1.0.0
 * Generated : 2026-05-19T13:34:40Z
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
#define GEN_DID_HANDLER_COUNT (27U)

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
 * @brief Read handler for DID 0xE001 — MC_RotorSpeed_rpm.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_RotorSpeed_rpm.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_rotorspeed_rpm(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE002 — MC_TorqueDemand_01Nm.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_TorqueDemand_01Nm.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_torquedemand_01nm(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE003 — MC_TorqueActual_01Nm.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_TorqueActual_01Nm.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_torqueactual_01nm(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE004 — MC_RotorElectricalAngle_raw.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_RotorElectricalAngle_raw.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_rotorelectricalangle_raw(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE005 — MC_MechanicalPower_10W.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_MechanicalPower_10W.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_mechanicalpower_10w(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE101 — MC_PhaseA_Current_100mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_PhaseA_Current_100mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_phasea_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE102 — MC_PhaseB_Current_100mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_PhaseB_Current_100mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_phaseb_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE103 — MC_PhaseC_Current_100mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_PhaseC_Current_100mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_phasec_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE104 — MC_Iq_Current_100mA.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_Iq_Current_100mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_iq_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE105 — MC_Id_Current_100mA.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_Id_Current_100mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_id_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE201 — MC_DCLink_Voltage_mV.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 4 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_DCLink_Voltage_mV.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 4).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_dclink_voltage_mv(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE202 — MC_DCLink_Current_100mA.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_DCLink_Current_100mA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_dclink_current_100ma(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE203 — MC_ModulationIndex_04pct.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_ModulationIndex_04pct.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_modulationindex_04pct(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE301 — MC_IGBT_PhaseA_JunctionTemp_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_IGBT_PhaseA_JunctionTemp_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_igbt_phasea_junctiontemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE302 — MC_IGBT_PhaseB_JunctionTemp_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_IGBT_PhaseB_JunctionTemp_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_igbt_phaseb_junctiontemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE303 — MC_IGBT_PhaseC_JunctionTemp_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_IGBT_PhaseC_JunctionTemp_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_igbt_phasec_junctiontemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE304 — MC_MotorStatorTemp_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_MotorStatorTemp_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_motorstatortemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE305 — MC_CoolantInletTemp_degC.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_CoolantInletTemp_degC.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_coolantinlettemp_degc(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE401 — MC_FaultStatusBitmask.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_FaultStatusBitmask.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_faultstatusbitmask(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE402 — MC_OperatingMode.
 *
 * Min session  : UDS_SESSION_DEFAULT
 * Security level: 0
 * Data length  : 1 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_OperatingMode.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 1).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_operatingmode(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE501 — MC_ResolverOffset_raw.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_ResolverOffset_raw.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_resolveroffset_raw(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE502 — MC_TorqueScalingFactor_01pct.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_TorqueScalingFactor_01pct.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_torquescalingfactor_01pct(
    uint8_t  *buf,
    uint16_t  buf_len,
    uint16_t *out_len);

/**
 * @brief Read handler for DID 0xE503 — MC_CurrentSensorTrim_100uA.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 0
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to return current sensor / ECU data for MC_CurrentSensorTrim_100uA.
 *
 * @param[out] buf      Output buffer for DID data.
 * @param[in]  buf_len  Size of buf in bytes (must be >= 2).
 * @param[out] out_len  Set to number of bytes written.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_read_mc_currentsensortrim_100ua(
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
 * @brief Write handler for DID 0xE501 — MC_ResolverOffset_raw.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for MC_ResolverOffset_raw.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_mc_resolveroffset_raw(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0xE502 — MC_TorqueScalingFactor_01pct.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for MC_TorqueScalingFactor_01pct.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_mc_torquescalingfactor_01pct(
    const uint8_t *buf,
    uint16_t       len);

/**
 * @brief Write handler for DID 0xE503 — MC_CurrentSensorTrim_100uA.
 *
 * Min session  : UDS_SESSION_EXTENDED
 * Security level: 1
 * Data length  : 2 byte(s)
 *
 * TODO [APPLICATION]: Implement to apply new value for MC_CurrentSensorTrim_100uA.
 *
 * @param[in] buf  Buffer containing new DID data (2 byte(s)).
 * @param[in] len  Number of bytes in buf.
 * @return UDS_STATUS_OK on success.
 */
uds_status_t did_write_mc_currentsensortrim_100ua(
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
