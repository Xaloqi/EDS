// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/motor_controller_ecu/src/main.c
 *
 * PURPOSE: Traction Inverter / Motor Controller ECU — application entry point.
 *
 * TARGET:  Generic automotive traction inverter ECU.
 *          Tested on native_sim and nucleo_h743zi (STM32H743ZI Cortex-M7).
 *          Portable to any Zephyr-supported board with CAN peripheral.
 *
 * ECU ROLE:
 *   This example models the main controller ECU of a 3-phase traction inverter
 *   for an IPMSM (interior permanent-magnet synchronous motor). The inverter
 *   is responsible for:
 *     - Field-oriented control (FOC): torque and flux current regulation
 *     - Rotor position / speed sensing via resolver (sin/cos → RDC)
 *     - Phase current measurement (3× inline shunts, ADC @ ≥10 kHz)
 *     - DC-link voltage monitoring and pre-charge supervision
 *     - IGBT / SiC gate-driver health (desaturation detection)
 *     - Junction temperature estimation (T_j = T_case + P_loss × R_th)
 *     - Regenerative braking coordination with BMS over CAN
 *
 *   Together with the bms_ecu example, this forms the complete EV powertrain
 *   diagnostic coverage: BMS (energy storage) + Motor Controller (conversion).
 *
 * DIAGNOSTICS OVERVIEW:
 *   27 DIDs generated from examples/motor_controller_ecu/diagnostics_config.yaml.
 *   All DID handlers are ASIL-B wrapped (5-step validation chain).
 *   10 DTCs covering all safety-relevant inverter protection faults.
 *   6 RoutineControl procedures: SelfTest, PhaseBalanceTest,
 *     GateDriverFunctionalTest, ResolverOffsetCalibration,
 *     ForceMotorInhibit, ClearFaultHistory.
 *   Diagnostic test suite: 27 per-DID + 6 per-routine + 2 service test files.
 *   Total generated test artifacts: 40 files in generated/tests/.
 *
 * DID HANDLER PATTERN:
 *   Each DID handler in this file:
 *     1. Shows the production data source (FOC signals, sensor APIs, CAN bus)
 *     2. Returns a deterministic, plausible stub value for testing
 *     3. Carries a TODO tag for integration with the real application layer
 *   Replace each stub body with real sensor reads, CAN signal decoding, or
 *   NVM accesses before integration testing.
 *
 * THREAD MODEL:
 *   main()       — platform init → UDS init → launch diag_task → exit
 *   diag_task    — 1 ms poll loop: CAN RX → ISO-TP → UDS dispatch → CAN TX
 *   foc_task     — 100 µs FOC ISR/task updating g_mc_state (not shown here)
 *
 *   FOC runs at ≥ 10 kHz; DID reads are background priority and non-blocking.
 *   g_mc_mutex serialises access between the diagnostic task and FOC update.
 *
 * BUILDING (simulation, no hardware needed):
 *   west build -b native_sim examples/motor_controller_ecu \
 *     -- -DDIAG_SKIP_CODEGEN=ON \
 *     -DDTC_OVERLAY_FILE=examples/motor_controller_ecu/boards/native_sim/native_sim.overlay
 *   ./build/zephyr/zephyr.exe
 *
 * SAFETY   : ASIL-B candidate.
 * STANDARD : MISRA C:2012 alignment intended.
 * LICENSE  : Apache-2.0
 * VERSION  : 1.0.0
 * =============================================================================
 */

/* --------------------------------------------------------------------------
 * Diagnostics stack headers
 * -------------------------------------------------------------------------- */
#include "uds_types.h"
#include "uds_server.h"
#include "uds_security_algo.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "isotp.h"
#include "can_transport.h"
#include "zephyr_port.h"
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"

/* --------------------------------------------------------------------------
 * Generated headers (from diagnostics_config.yaml via codegen.py)
 * -------------------------------------------------------------------------- */
#include "uds_init.h"
#include "generated_config.h"
#include "did_handlers.h"

/* --------------------------------------------------------------------------
 * Zephyr headers
 * -------------------------------------------------------------------------- */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(motor_controller_ecu, LOG_LEVEL_INF);

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define DIAG_CAN_DEV         DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID       ((uint32_t)GEN_CAN_RX_ID)   /* 0x7DF: functional */
#define DIAG_TX_CAN_ID       ((uint32_t)GEN_CAN_TX_ID)   /* 0x7E8: physical    */

#ifndef CONFIG_DIAG_TASK_STACK_SIZE
#define CONFIG_DIAG_TASK_STACK_SIZE   (6144U)
#endif

#ifndef CONFIG_DIAG_TASK_PRIORITY
#define CONFIG_DIAG_TASK_PRIORITY     (5)
#endif

#define DIAG_OVERRUN_LOG_THRESHOLD    (3U)

/* =============================================================================
 * Motor Controller Application State
 *
 * In production these fields are updated by the FOC interrupt service routine
 * or a dedicated high-priority task (typically a Zephyr k_timer callback
 * scheduled at the PWM period, e.g. every 100 µs for 10 kHz switching).
 *
 * Data sources per field group:
 *   Motor electrical  — FOC algorithm outputs (torque model, angle decoder)
 *   Phase currents    — ADC DMA complete callback (3× shunt, differential ADC)
 *   DC link           — Isolated ADC channel on HV bus; current shunt on DC rail
 *   Temperatures      — NTC ADC channels polled at 10 ms; junction estimation
 *   FOC controller    — Direct FOC variable reads (Iq, Id, modulation index)
 *   Calibration       — NVM reads at startup; writable in extended session + SL1
 *
 * Thread safety: DID read handlers access these fields via the ASIL-B safety
 * wrapper. FOC update context must hold g_mc_mutex before writing. The DID
 * read handlers hold the mutex for the minimum time needed to copy the value.
 *
 * Encoding: all values follow the encoding defined in diagnostics_config.yaml.
 * No conversion is performed in the diagnostic handlers — conversions happen
 * in the application update layer before writing to this struct.
 * ============================================================================= */

/** Mutex protecting g_mc_state from concurrent access. */
static struct k_mutex g_mc_mutex;

/** Motor controller application state — initialised to safe standstill values. */
static struct {

    /* Motor electrical measurements */
    int16_t  rotor_speed_rpm;          /*!< DID 0xE001: int16, 1 rpm/LSB */
    int16_t  torque_demand_01Nm;       /*!< DID 0xE002: int16, 0.1 Nm/LSB */
    int16_t  torque_actual_01Nm;       /*!< DID 0xE003: int16, 0.1 Nm/LSB */
    uint16_t rotor_angle_raw;          /*!< DID 0xE004: uint16, angle raw */
    int16_t  mech_power_10W;           /*!< DID 0xE005: int16, 10 W/LSB */

    /* Phase current measurements */
    int16_t  phase_a_100mA;            /*!< DID 0xE101: int16, 100 mA/LSB */
    int16_t  phase_b_100mA;            /*!< DID 0xE102: int16, 100 mA/LSB */
    int16_t  phase_c_100mA;            /*!< DID 0xE103: derived Ic=-(Ia+Ib) */
    int16_t  iq_100mA;                 /*!< DID 0xE104: torque-producing */
    int16_t  id_100mA;                 /*!< DID 0xE105: flux-producing */

    /* DC-link measurements */
    uint32_t dclink_voltage_mV;        /*!< DID 0xE201: uint32, mV */
    int16_t  dclink_current_100mA;     /*!< DID 0xE202: int16, 100 mA/LSB */
    uint8_t  modulation_index_04pct;   /*!< DID 0xE203: uint8, 0.4%/LSB */

    /* Thermal measurements (int8 offset: raw = T_degC + 40) */
    uint8_t  igbt_temp_a_raw;          /*!< DID 0xE301: Phase A junction */
    uint8_t  igbt_temp_b_raw;          /*!< DID 0xE302: Phase B junction */
    uint8_t  igbt_temp_c_raw;          /*!< DID 0xE303: Phase C junction */
    uint8_t  stator_temp_raw;          /*!< DID 0xE304: motor winding NTC */
    uint8_t  coolant_temp_raw;         /*!< DID 0xE305: coolant inlet NTC */

    /* FOC controller state */
    uint16_t fault_flags;              /*!< DID 0xE401: bitmask */
    uint8_t  operating_mode;           /*!< DID 0xE402: enum 0x00–0xFF */

    /* Calibration parameters (writable in extended session + SL1) */
    uint16_t resolver_offset_raw;      /*!< DID 0xE501: angle offset */
    uint16_t torque_scaling_01pct;     /*!< DID 0xE502: scaling trim */
    int16_t  current_sensor_trim_100uA;/*!< DID 0xE503: zero-offset trim */

    /* ECU identity (read from NVM at startup) */
    uint8_t  vin[17U];                 /*!< DID 0xF190: ISO VIN */
    uint8_t  ecu_serial[8U];           /*!< DID 0xF18C: ECU serial */
    uint8_t  spare_part_number[11U];   /*!< DID 0xF187: OEM part number */
    uint8_t  sw_version[4U];           /*!< DID 0xF189: Maj.Min.Pat.Bld */

} g_mc_state = {
    /* Standstill defaults — safe values for all read paths before FOC starts */
    .rotor_speed_rpm         = 0,
    .torque_demand_01Nm      = 0,
    .torque_actual_01Nm      = 0,
    .rotor_angle_raw         = 0U,
    .mech_power_10W          = 0,
    .phase_a_100mA           = 0,
    .phase_b_100mA           = 0,
    .phase_c_100mA           = 0,
    .iq_100mA                = 0,
    .id_100mA                = 0,
    .dclink_voltage_mV       = 400000UL,  /* 400 V open-circuit default */
    .dclink_current_100mA    = 0,
    .modulation_index_04pct  = 0U,
    .igbt_temp_a_raw         = (uint8_t)(25 + 40),   /* 25 °C */
    .igbt_temp_b_raw         = (uint8_t)(25 + 40),
    .igbt_temp_c_raw         = (uint8_t)(25 + 40),
    .stator_temp_raw         = (uint8_t)(25 + 40),
    .coolant_temp_raw        = (uint8_t)(20 + 40),   /* 20 °C coolant */
    .fault_flags             = 0U,
    .operating_mode          = 0x01U,   /* Idle */
    .resolver_offset_raw     = 0U,
    .torque_scaling_01pct    = 1000U,   /* 100.0% — nominal */
    .current_sensor_trim_100uA = 0,
    .vin                     = "00000000000000000",
    .ecu_serial              = "MC000001",
    .spare_part_number       = "00000000000",
    .sw_version              = {1U, 0U, 0U, 0U},
};

/* =============================================================================
 * DID Handler Implementations
 *
 * Handler naming convention (from generated/did_handlers.h):
 *   did_read_XXXX()   — read handler for DID 0xXXXX
 *   did_write_XXXX()  — write handler for DID 0xXXXX (where writable)
 *
 * All handlers:
 *   - Hold g_mc_mutex for the duration of the copy
 *   - Write exactly data_length bytes into buffer
 *   - Return UDS_STATUS_OK on success
 *   - Return UDS_STATUS_ERR_GENERIC if sensor data is unavailable
 *
 * TODO for integration: replace each stub body with real sensor reads,
 * FOC variable access, or NVM reads as appropriate.
 * ============================================================================= */

/* --------------------------------------------------------------------------
 * A. ECU Identity
 * -------------------------------------------------------------------------- */

uds_status_t did_read_F190(uint8_t *buf)
{
    /* TODO: read VIN from NVM at startup and cache in g_mc_state.vin */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    (void)memcpy(buf, g_mc_state.vin, 17U);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_F18C(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    (void)memcpy(buf, g_mc_state.ecu_serial, 8U);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_F187(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    (void)memcpy(buf, g_mc_state.spare_part_number, 11U);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_write_F187(const uint8_t *buf, uint16_t len)
{
    if (len != 11U) { return UDS_STATUS_ERR_INVALID_PARAM; }
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    (void)memcpy(g_mc_state.spare_part_number, buf, 11U);
    /* TODO: persist to NVM via nvm_store_write() */
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_F189(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    (void)memcpy(buf, g_mc_state.sw_version, 4U);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * B. Motor Electrical Measurements
 * -------------------------------------------------------------------------- */

uds_status_t did_read_E001(uint8_t *buf)
{
    /* TODO: read rotor_speed_rpm from resolver decoder output register */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)((uint16_t)g_mc_state.rotor_speed_rpm >> 8U);
    buf[1U] = (uint8_t)((uint16_t)g_mc_state.rotor_speed_rpm & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E002(uint8_t *buf)
{
    /* TODO: read torque_demand from CAN receive buffer (VSC torque request) */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)((uint16_t)g_mc_state.torque_demand_01Nm >> 8U);
    buf[1U] = (uint8_t)((uint16_t)g_mc_state.torque_demand_01Nm & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E003(uint8_t *buf)
{
    /* TODO: compute torque_actual from FOC model:
     *   T = 1.5 * poles * (lambda_f * Iq + (Ld - Lq) * Id * Iq) */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)((uint16_t)g_mc_state.torque_actual_01Nm >> 8U);
    buf[1U] = (uint8_t)((uint16_t)g_mc_state.torque_actual_01Nm & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E004(uint8_t *buf)
{
    /* TODO: read raw resolver angle from RDC output register */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)(g_mc_state.rotor_angle_raw >> 8U);
    buf[1U] = (uint8_t)(g_mc_state.rotor_angle_raw & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E005(uint8_t *buf)
{
    /* TODO: compute P_mech = T_actual × ω (convert rpm to rad/s first) */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)((uint16_t)g_mc_state.mech_power_10W >> 8U);
    buf[1U] = (uint8_t)((uint16_t)g_mc_state.mech_power_10W & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * C. Phase Currents
 * -------------------------------------------------------------------------- */

static uds_status_t s_read_i16_be(uint8_t *buf, int16_t val)
{
    buf[0U] = (uint8_t)((uint16_t)val >> 8U);
    buf[1U] = (uint8_t)((uint16_t)val & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E101(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_i16_be(buf, g_mc_state.phase_a_100mA);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E102(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_i16_be(buf, g_mc_state.phase_b_100mA);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E103(uint8_t *buf)
{
    /* Ic = -(Ia + Ib) — stored as derived value in state struct */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_i16_be(buf, g_mc_state.phase_c_100mA);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E104(uint8_t *buf)
{
    /* TODO: read Iq from FOC current regulator output */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_i16_be(buf, g_mc_state.iq_100mA);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E105(uint8_t *buf)
{
    /* TODO: read Id from FOC flux controller output */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_i16_be(buf, g_mc_state.id_100mA);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

/* --------------------------------------------------------------------------
 * D. DC-Link Measurements
 * -------------------------------------------------------------------------- */

uds_status_t did_read_E201(uint8_t *buf)
{
    /* TODO: read from isolated ADC channel monitoring HV bus */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)((g_mc_state.dclink_voltage_mV >> 24U) & 0xFFU);
    buf[1U] = (uint8_t)((g_mc_state.dclink_voltage_mV >> 16U) & 0xFFU);
    buf[2U] = (uint8_t)((g_mc_state.dclink_voltage_mV >>  8U) & 0xFFU);
    buf[3U] = (uint8_t)( g_mc_state.dclink_voltage_mV         & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E202(uint8_t *buf)
{
    /* TODO: read DC bus current from shunt measurement on battery rail */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_i16_be(buf, g_mc_state.dclink_current_100mA);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E203(uint8_t *buf)
{
    /* TODO: read SVPWM modulation index from FOC modulator output */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = g_mc_state.modulation_index_04pct;
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * E. Thermal Measurements
 * -------------------------------------------------------------------------- */

static uds_status_t s_read_temp_raw(uint8_t *buf, uint8_t raw)
{
    buf[0U] = raw;
    return UDS_STATUS_OK;
}

uds_status_t did_read_E301(uint8_t *buf)
{
    /* TODO: T_j(A) = T_case(A) + P_sw_A × R_th(j-c) */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_temp_raw(buf, g_mc_state.igbt_temp_a_raw);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E302(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_temp_raw(buf, g_mc_state.igbt_temp_b_raw);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E303(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_temp_raw(buf, g_mc_state.igbt_temp_c_raw);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E304(uint8_t *buf)
{
    /* TODO: read stator winding NTC via Zephyr ADC driver */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_temp_raw(buf, g_mc_state.stator_temp_raw);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_read_E305(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_temp_raw(buf, g_mc_state.coolant_temp_raw);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

/* --------------------------------------------------------------------------
 * F. FOC Controller State
 * -------------------------------------------------------------------------- */

uds_status_t did_read_E401(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)(g_mc_state.fault_flags >> 8U);
    buf[1U] = (uint8_t)(g_mc_state.fault_flags & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E402(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = g_mc_state.operating_mode;
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * G. Calibration Parameters
 * -------------------------------------------------------------------------- */

uds_status_t did_read_E501(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)(g_mc_state.resolver_offset_raw >> 8U);
    buf[1U] = (uint8_t)(g_mc_state.resolver_offset_raw & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_write_E501(const uint8_t *buf, uint16_t len)
{
    if (len != 2U) { return UDS_STATUS_ERR_INVALID_PARAM; }
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    g_mc_state.resolver_offset_raw =
        (uint16_t)(((uint16_t)buf[0U] << 8U) | (uint16_t)buf[1U]);
    /* TODO: persist resolver offset to NVM and reload into FOC resolver decoder */
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E502(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    buf[0U] = (uint8_t)(g_mc_state.torque_scaling_01pct >> 8U);
    buf[1U] = (uint8_t)(g_mc_state.torque_scaling_01pct & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_write_E502(const uint8_t *buf, uint16_t len)
{
    if (len != 2U) { return UDS_STATUS_ERR_INVALID_PARAM; }
    uint16_t val = (uint16_t)(((uint16_t)buf[0U] << 8U) | (uint16_t)buf[1U]);
    /* Enforce valid range: 850–1150 (85.0–115.0%) */
    if ((val < 850U) || (val > 1150U)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    g_mc_state.torque_scaling_01pct = val;
    /* TODO: apply scaling factor to FOC torque demand path */
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

uds_status_t did_read_E503(uint8_t *buf)
{
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    uds_status_t s = s_read_i16_be(buf, g_mc_state.current_sensor_trim_100uA);
    k_mutex_unlock(&g_mc_mutex);
    return s;
}

uds_status_t did_write_E503(const uint8_t *buf, uint16_t len)
{
    if (len != 2U) { return UDS_STATUS_ERR_INVALID_PARAM; }
    int16_t val = (int16_t)(((uint16_t)buf[0U] << 8U) | (uint16_t)buf[1U]);
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    g_mc_state.current_sensor_trim_100uA = val;
    /* TODO: apply trim to ADC current sensor calibration coefficients */
    k_mutex_unlock(&g_mc_mutex);
    return UDS_STATUS_OK;
}

/* =============================================================================
 * Routine handlers
 *
 * Each routine callback implements one sub-function: start_cb, stop_cb,
 * or results_cb. The handlers are stubs that log the invocation and return
 * UDS_STATUS_OK. Replace with real procedure logic for production integration.
 * ============================================================================= */

uds_status_t routine_CC00_start(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)opt; (void)opt_len; (void)result_cap;
    LOG_INF("[MC] SelfTest started");
    /* TODO: run RAM march, ROM CRC, gate-driver SPI, resolver plausibility */
    result[0U]    = 0x00U;   /* pass = 0x00, fail = 0x01 */
    *result_len_out = 1U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC00_results(
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)result_cap;
    result[0U]    = 0x00U;   /* last self-test result: pass */
    *result_len_out = 1U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC01_start(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)opt; (void)opt_len; (void)result_cap;
    LOG_INF("[MC] PhaseBalanceTest started");
    /* TODO: apply controlled motoring setpoint; measure Ia, Ib, Ic RMS;
     *       compute per-phase deviation from mean. */
    result[0U] = 0U;   /* Phase A deviation: 0 mA (stub) */
    result[1U] = 0U;   /* Phase B deviation: 0 mA */
    result[2U] = 0U;   /* Phase C deviation: 0 mA */
    *result_len_out = 3U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC01_results(
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)result_cap;
    result[0U] = 0U; result[1U] = 0U; result[2U] = 0U;
    *result_len_out = 3U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC02_start(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)opt; (void)opt_len; (void)result_cap;
    LOG_INF("[MC] GateDriverFunctionalTest started");
    /* TODO: sequence each gate driver channel; check DESAT response;
     *       set result bitmask bits on failure. */
    result[0U]    = 0x3FU;  /* all 6 channels pass (bits 0-5 = 1) */
    *result_len_out = 1U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC02_results(
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)result_cap;
    result[0U]    = 0x3FU;
    *result_len_out = 1U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC10_start(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)opt; (void)opt_len; (void)result_cap;
    LOG_INF("[MC] ResolverOffsetCalibration started");
    /* TODO: with rotor locked, read raw resolver angle and store as offset
     *       in DID 0xE501 (MC_ResolverOffset_raw) and NVM. */
    uint16_t measured_offset = g_mc_state.rotor_angle_raw;  /* stub */
    result[0U]    = (uint8_t)(measured_offset >> 8U);
    result[1U]    = (uint8_t)(measured_offset & 0xFFU);
    *result_len_out = 2U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC10_results(
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)result_cap;
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    result[0U] = (uint8_t)(g_mc_state.resolver_offset_raw >> 8U);
    result[1U] = (uint8_t)(g_mc_state.resolver_offset_raw & 0xFFU);
    k_mutex_unlock(&g_mc_mutex);
    *result_len_out = 2U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC11_start(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)opt; (void)opt_len; (void)result_cap;
    LOG_WRN("[MC] ForceMotorInhibit ACTIVATED — gate-driver disabled");
    /* TODO: assert gate-driver disable pin (GPIO output) */
    result[0U]    = 0x01U;  /* inhibit active */
    *result_len_out = 1U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC11_stop(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)opt; (void)opt_len; (void)result_cap;
    LOG_WRN("[MC] ForceMotorInhibit DEACTIVATED — gate-driver re-enabled");
    /* TODO: de-assert gate-driver disable pin */
    result[0U]    = 0x00U;  /* inhibit released */
    *result_len_out = 1U;
    return UDS_STATUS_OK;
}

uds_status_t routine_CC12_start(
    const uint8_t *opt, uint8_t opt_len,
    uint8_t *result, uint8_t result_cap, uint8_t *result_len_out)
{
    (void)opt; (void)opt_len; (void)result_cap;
    LOG_INF("[MC] ClearFaultHistory — clearing all DTCs and fault latches");
    /* TODO: call uds_dtc_clear_all() and reset g_mc_state.fault_flags */
    (void)k_mutex_lock(&g_mc_mutex, K_FOREVER);
    g_mc_state.fault_flags = 0U;
    k_mutex_unlock(&g_mc_mutex);
    *result_len_out = 0U;
    return UDS_STATUS_OK;
}

/* =============================================================================
 * DTC Registration helpers
 * Called from uds_generated_init() via generated/uds_init.c
 * ============================================================================= */

/* Forward declaration — implemented in generated/uds_init.c */
extern void register_dtcs(void);

/* =============================================================================
 * Diagnostics task (1 ms poll loop)
 * ============================================================================= */

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

static void diag_task_fn(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;

    const struct device   *can_dev = DIAG_CAN_DEV;
    can_transport_t       *can     = NULL;
    zephyr_port_cfg_t      port_cfg;
    uds_status_t           status;
    uint32_t               overrun_count = 0U;

    /* ── Platform init ────────────────────────────────────────────────────── */
    if (!device_is_ready(can_dev)) {
        LOG_ERR("[MC] CAN device not ready");
        return;
    }

    (void)memset(&port_cfg, 0, sizeof(port_cfg));
    port_cfg.can_dev = can_dev;

    status = zephyr_port_init(&port_cfg, &can);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("[MC] zephyr_port_init failed: 0x%02X", (unsigned)status);
        return;
    }

    /* ── UDS stack init ────────────────────────────────────────────────────── */
    status = uds_generated_init(can,
                                 (uint32_t)DIAG_RX_CAN_ID,
                                 (uint32_t)DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("[MC] uds_generated_init failed: 0x%02X", (unsigned)status);
        return;
    }

    {
        uds_server_ctx_t *mc_srv = uds_generated_get_server();
        if (mc_srv != NULL) {
            (void)uds_periodic_init();
            (void)uds_session_register_change_cb(mc_srv->cfg.session_ctx,
                                                  s_on_session_change);
        }
    }

    LOG_INF("[MC] Motor controller diagnostics running");
    LOG_INF("[MC] RX=0x%03X TX=0x%03X", DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);

    /* ── 1 ms poll loop ───────────────────────────────────────────────────── */
    int64_t next_tick = k_uptime_get() + 1LL;

    while (true) {
        int64_t now = k_uptime_get();

        if (now >= next_tick) {
            if ((now - next_tick) > (int64_t)DIAG_OVERRUN_LOG_THRESHOLD) {
                overrun_count++;
                if ((overrun_count % (uint32_t)10U) == (uint32_t)1U) {
                    LOG_WRN("[MC] Tick overrun (×%u)", (unsigned)overrun_count);
                }
            }
            next_tick = now + 1LL;

            /* Drive all 1 ms tick functions */
            (void)uds_server_tick_1ms(NULL);
            (void)uds_periodic_tick_1ms();
        }

        {
            static uds_msg_buf_t s_periodic_frame;
            isotp_ctx_t *mc_tp = uds_generated_get_isotp();
            if (mc_tp != NULL) {
                while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
                    (void)isotp_transmit(mc_tp, s_periodic_frame.data,
                                        (uint32_t)s_periodic_frame.length);
                }
            }
        }

        k_sleep(K_USEC(500));
    }
}

K_THREAD_DEFINE(diag_task, CONFIG_DIAG_TASK_STACK_SIZE,
                diag_task_fn, NULL, NULL, NULL,
                CONFIG_DIAG_TASK_PRIORITY, 0, 0);

/* =============================================================================
 * main()
 * ============================================================================= */

int main(void)
{
    LOG_INF("[MC] Motor Controller ECU — Xaloqi EDS v1.0.0");
    LOG_INF("[MC] 27 DIDs | 10 DTCs | 6 Routines | ASIL-B safety wrappers");

    /* Initialise application mutex */
    (void)k_mutex_init(&g_mc_mutex);

    /* Security algorithm: register TRNG callback before any seed generation.
     * TODO: replace with real TRNG source (e.g. Zephyr entropy driver).
     * The reference implementation uses a software counter as a nonce seed. */
    uds_security_algo_reset();

    LOG_INF("[MC] Application init complete. Diagnostics task starting.");
    /* diag_task starts automatically via K_THREAD_DEFINE */
    return 0;
}
