// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/bms_ecu/src/main.c
 *
 * PURPOSE: Battery Management System (BMS) ECU — application entry point.
 *
 * TARGET:  Generic automotive passenger-car BMS ECU.
 *          Tested on native_sim and nucleo_h743zi (STM32H743ZI Cortex-M7).
 *          Portable to any Zephyr-supported board with CAN peripheral.
 *
 * ECU ROLE:
 *   This example models the main controller ECU of a passenger-car BMS for
 *   a 96s Li-Ion pack (8 groups of 12 cells). The BMS is responsible for:
 *     - Cell voltage monitoring (4 group voltage DIDs, per-group aggregation)
 *     - Pack voltage and current measurement
 *     - Temperature monitoring (NTC sensors: cell stack, coolant, PCB)
 *     - State-of-Charge (SoC) and State-of-Health (SoH) estimation
 *     - Contactor control (main +/-, pre-charge) and weld detection
 *     - Passive and active cell balancing
 *     - Isolation monitoring (IMD interface)
 *     - Fault flag bitmask (quick triage DID) and DTC logging
 *
 * DIAGNOSTICS OVERVIEW:
 *   24 DIDs generated from examples/bms_ecu/diagnostics_config.yaml.
 *   All DID handlers are ASIL-B wrapped (5-step validation chain).
 *   10 DTCs covering all safety-relevant BMS faults.
 *   5 RoutineControl procedures: SelfTest, ForcePassiveBalance,
 *     ContactorFunctionalTest, ResetSoCToFullCharge, ClearFaultHistory.
 *   Diagnostic test suite: 24 per-DID test files + firmware integration tests.
 *   Total generated test artifacts: 31 files in generated/tests/.
 *
 * DID HANDLER PATTERN:
 *   Each DID handler in this file:
 *     1. Shows the production data source (sensor API, signal bus, NVM)
 *     2. Returns a deterministic, plausible stub value for testing
 *     3. Carries a TODO tag for integration with the real ECU application layer
 *   Replace each stub body with real sensor reads, CAN signal decoding, or
 *   NVM accesses before integration testing.
 *
 * THREAD MODEL:
 *   main()       — platform init → UDS init → launch diag_task → exit
 *   diag_task    — 1 ms poll loop: CAN RX → ISO-TP → UDS dispatch → CAN TX
 *
 *   Application threads (not shown here) update g_bms_state fields from:
 *     - Cell monitoring IC (e.g. LTC6811, MAX17853) via SPI/isoSPI
 *     - Current sensor (shunt + ADC, or Hall-effect)
 *     - NTC ADC channels (Zephyr ADC driver)
 *     - Isolation monitoring device (IMD, e.g. Bender ISOMETER) via UART/CAN
 *   DID handlers read g_bms_state under mutex protection.
 *
 * STACK BUDGET:
 *   CONFIG_DIAG_TASK_STACK_SIZE = 6144 bytes.
 *   BMS has 24 DIDs + 5 routines; increased vs. basic_ecu (4096) to
 *   accommodate the larger handler dispatch table and SoC estimation callbacks.
 *
 * BUILDING (simulation, no hardware needed):
 *   west build -b native_sim examples/bms_ecu \
 *     -- -DDIAG_SKIP_CODEGEN=ON \
 *     -DDTC_OVERLAY_FILE=examples/bms_ecu/boards/native_sim/native_sim.overlay
 *   ./build/zephyr/zephyr.exe
 *
 * SAFETY   : ASIL-B candidate (ASIL-D target for production BMS).
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

LOG_MODULE_REGISTER(bms_ecu, LOG_LEVEL_INF);

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

/* Log a warning once every N overrun events, not every tick. */
#define DIAG_OVERRUN_LOG_THRESHOLD    (3U)

/* =============================================================================
 * BMS Application State
 *
 * In production, these fields are updated by application threads that:
 *   - Read cell monitoring ICs via SPI (e.g. LTC6811, MAX17853)
 *   - Integrate current sensor output (ADC shunt or Hall-effect)
 *   - Poll NTC ADC channels via Zephyr ADC driver
 *   - Receive isolation monitoring status from IMD (UART or CAN)
 *
 * Thread safety: DID read handlers access these fields via the ASIL-B safety
 * wrapper, which serialises access through the UDS server mutex. Application
 * update threads must hold g_bms_mutex before writing.
 *
 * Encoding note: all physical quantities follow the encoding defined in
 * diagnostics_config.yaml and are copied byte-by-byte into DID response
 * buffers. No conversion is performed in the diagnostic handlers — conversions
 * happen at the application update layer.
 * ============================================================================= */

/** Mutex protecting g_bms_state from concurrent access between the diag task
 *  and application update threads. */
static struct k_mutex g_bms_mutex;

/** BMS controller application state — all fields are set to safe default
 *  values (pack open-circuit, 50% SoC, ambient temperature). */
static struct {

    /* Pack-level measurements */
    uint32_t pack_voltage_mV;          /*!< DID 0xDB00: uint32, mV */
    int16_t  pack_current_100mA;       /*!< DID 0xDB01: int16, 100 mA/LSB */
    int16_t  pack_power_10W;           /*!< DID 0xDB02: int16, 10 W/LSB */
    uint8_t  contactor_state;          /*!< DID 0xDB03: bitmask */
    uint16_t isolation_resistance_kOhm;/*!< DID 0xDB04: uint16, kΩ */

    /* Cell group voltages (4 groups, uint16 mV each) */
    uint16_t cell_group_mV[4U];        /*!< DIDs 0xDC01–0xDC04 */

    /* Temperature sensors (int8 offset-encoded: raw = T_degC + 40) */
    uint8_t  temp_cell_max_raw;        /*!< DID 0xDD00: max cell NTC */
    uint8_t  temp_cell_min_raw;        /*!< DID 0xDD01: min cell NTC */
    uint8_t  temp_coolant_inlet_raw;   /*!< DID 0xDD02: coolant inlet NTC */
    uint8_t  temp_bms_board_raw;       /*!< DID 0xDD03: PCB NTC */

    /* State estimation */
    uint8_t  soc_raw;                  /*!< DID 0xDE00: uint8, 0.4%/LSB */
    uint8_t  soh_raw;                  /*!< DID 0xDE01: uint8, 0.4%/LSB */
    uint8_t  balancing_state;          /*!< DID 0xDE02: bitmask */

    /* System health */
    uint16_t fault_flags;              /*!< DID 0xD900: bitmask */
    uint32_t full_charge_capacity_Wh;  /*!< DID 0xD901: uint32, Wh */

    /* Calibration thresholds (writable in extended session + SL1) */
    uint16_t ov_threshold_mV;          /*!< DID 0xD910: OV protection, mV */
    uint16_t uv_threshold_mV;          /*!< DID 0xD911: UV protection, mV */

    /* ECU identity (not from sensors — read from NVM at startup) */
    uint8_t  vin[17U];                 /*!< DID 0xF190: ISO VIN */
    uint8_t  ecu_serial[8U];           /*!< DID 0xF18C: ECU serial */
    uint8_t  spare_part_number[11U];   /*!< DID 0xF187: OEM part number */
    uint8_t  sw_version[4U];           /*!< DID 0xF189: Maj.Min.Pat.Bld */

} g_bms_state;

/* =============================================================================
 * BMS State Initialisation
 *
 * Sets all fields to plausible, safe default values.
 * In production, identity fields (VIN, serial, part number) are read from NVM
 * during startup before uds_generated_init() is called.
 * ============================================================================= */
static void bms_state_init(void)
{
    (void)k_mutex_init(&g_bms_mutex);

    /* --- Pack defaults: open-circuit voltage for 50% SoC on NMC ---
     * 8 groups × 12 cells × 3.7V nominal = 355.2V
     * At 50% SoC NMC ≈ 3.72 V/cell → 96 × 3720 mV = 357120 mV
     */
    g_bms_state.pack_voltage_mV           = 357120U;
    g_bms_state.pack_current_100mA        = 0;        /* 0 A (idle)          */
    g_bms_state.pack_power_10W            = 0;        /* 0 W (idle)          */
    g_bms_state.contactor_state           = 0x00U;    /* All open            */
    g_bms_state.isolation_resistance_kOhm = 0xFFFFU;  /* IMD not yet started */

    /* --- Cell group defaults: equal split of pack voltage ---
     * 357120 mV / 8 groups = 44640 mV per group (12 × 3720 mV)
     */
    for (uint8_t i = 0U; i < 4U; i++) {
        g_bms_state.cell_group_mV[i] = 44640U;
    }

    /* --- Temperature defaults: ambient 25 °C → raw = 25 + 40 = 65 --- */
    g_bms_state.temp_cell_max_raw      = 65U;
    g_bms_state.temp_cell_min_raw      = 65U;
    g_bms_state.temp_coolant_inlet_raw = 65U;
    g_bms_state.temp_bms_board_raw     = 65U;

    /* --- State estimation defaults: 50% SoC, 100% SoH (new pack) ---
     * SoC: 50% → raw = 50 / 0.4 = 125
     * SoH: 100% → raw = 100 / 0.4 = 250
     */
    g_bms_state.soc_raw          = 125U;
    g_bms_state.soh_raw          = 250U;
    g_bms_state.balancing_state  = 0x00U;  /* No balancing active */

    /* --- System health defaults: no faults, nominal capacity --- */
    g_bms_state.fault_flags               = 0x0000U;
    g_bms_state.full_charge_capacity_Wh   = 75000U;  /* 75 kWh nominal */

    /* --- Calibration defaults (NMC chemistry thresholds) --- */
    g_bms_state.ov_threshold_mV = 4200U;  /* 4.200 V per cell */
    g_bms_state.uv_threshold_mV = 2800U;  /* 2.800 V per cell */

    /* --- ECU identity (NVM placeholders) --- */
    /* TODO [APPLICATION]: Load VIN from NVM / OEM provisioning flash sector */
    (void)memset(g_bms_state.vin,              0x20U, sizeof(g_bms_state.vin));
    (void)memset(g_bms_state.ecu_serial,       0x00U, sizeof(g_bms_state.ecu_serial));
    (void)memset(g_bms_state.spare_part_number,0x30U, sizeof(g_bms_state.spare_part_number));
    g_bms_state.sw_version[0U] = 1U;   /* Major */
    g_bms_state.sw_version[1U] = 0U;   /* Minor */
    g_bms_state.sw_version[2U] = 0U;   /* Patch */
    g_bms_state.sw_version[3U] = 0U;   /* Build */
}

/* =============================================================================
 * DID Handler Implementations
 *
 * All handlers follow the pattern:
 *   1. Lock g_bms_mutex (production: prevents data races with update threads)
 *   2. Copy data into the response buffer (memcpy or field assignment)
 *   3. Unlock mutex and return UDS_STATUS_OK
 *
 * The mutex lock/unlock is shown as commented-out code in stub form because
 * the Zephyr mutex API is not available in the unit test harness. Enable the
 * mutex calls when integrating into the real Zephyr application.
 *
 * SAFETY: All handlers are invoked via the ASIL-B safety wrapper chain in
 * generated/did_safety_wrappers.c. Handlers must NOT validate session,
 * security level, or DID existence — the wrapper chain handles that before
 * calling into these functions.
 * ============================================================================= */

/* ---------------------------------------------------------------------------
 * A. ECU Identity Handlers (0xF190, 0xF18C, 0xF187, 0xF189)
 * --------------------------------------------------------------------------- */

/**
 * @brief DID 0xF190 — VehicleIdentificationNumber (17 bytes, read-only)
 *
 * Production: read 17-byte VIN from NVM flash sector programmed at EOL.
 * The VIN is a permanent identity; it is written once at OEM end-of-line
 * and must survive ECU replacement (stored externally in body-domain ECU).
 */
uds_status_t bms_did_vehicleidentificationnumber_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with nvm_store_read(NVM_KEY_VIN, buf, 17) */
    (void)memcpy(buf, g_bms_state.vin, 17U);
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xF18C — ECUSerialNumber (8 bytes, read-only)
 *
 * Production: read 8-byte serial from factory flash area (OTP or NVM).
 * Serial is assigned at PCB manufacturing and is unique per board.
 */
uds_status_t bms_did_ecuserialnumber_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with nvm_store_read(NVM_KEY_SERIAL, buf, 8) */
    (void)memcpy(buf, g_bms_state.ecu_serial, 8U);
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xF187 — VehicleManufacturerSparePartNumber (11 bytes, read-write)
 *
 * Production read: retrieve 11-byte OEM part number from NVM.
 * Production write: persist updated part number to NVM after ECU exchange.
 */
uds_status_t bms_did_vehiclemanufacturersparepartnumber_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with nvm_store_read(NVM_KEY_PART_NO, buf, 11) */
    (void)memcpy(buf, g_bms_state.spare_part_number, 11U);
    return UDS_STATUS_OK;
}

uds_status_t bms_did_vehiclemanufacturersparepartnumber_write(const uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with nvm_store_write(NVM_KEY_PART_NO, buf, 11) */
    (void)memcpy(g_bms_state.spare_part_number, buf, 11U);
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xF189 — ECUSoftwareVersionNumber (4 bytes: Maj.Min.Patch.Build)
 *
 * Production: read from linker-section symbol or compile-time constant embedded
 * in the firmware image (e.g. const uint8_t __attribute__((section(".version")))).
 */
uds_status_t bms_did_ecusoftwareversionnumber_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with linker-symbol read or VERSION_MAJOR etc. */
    (void)memcpy(buf, g_bms_state.sw_version, 4U);
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * B. Pack-Level Measurement Handlers (0xDB00–0xDB04)
 * --------------------------------------------------------------------------- */

/**
 * @brief DID 0xDB00 — BMS_PackVoltage_mV (4 bytes, uint32)
 *
 * Production: read summed stack voltage from cell monitoring IC chain.
 * e.g. LTC6811: read CVAR (cell voltage all registers) via SPI, sum all cells.
 * The pack voltage is also measured independently by an isolated voltage
 * divider on MCU ADC for cross-check and IMD input.
 */
uds_status_t bms_did_bms_packvoltage_mv_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with cell_monitor_ic_read_pack_voltage(&v_mV)
     * then cross-check against ADC-measured pack voltage. */
    uint32_t v = g_bms_state.pack_voltage_mV;
    buf[0U] = (uint8_t)((v >> 24U) & 0xFFU);
    buf[1U] = (uint8_t)((v >> 16U) & 0xFFU);
    buf[2U] = (uint8_t)((v >>  8U) & 0xFFU);
    buf[3U] = (uint8_t)( v         & 0xFFU);
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDB01 — BMS_PackCurrent_100mA (2 bytes, int16 signed, 100 mA/LSB)
 *
 * Production: read current sensor output.
 * Shunt-based: shunt_ADC_count * V_ref / gain / R_shunt → mA → /100 for LSB.
 * Hall-effect: hall_output_V * sensitivity_factor → A → *10 for 100mA LSB.
 * Sign convention: positive = discharge (power to inverter), negative = charge.
 */
uds_status_t bms_did_bms_packcurrent_100ma_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with current_sensor_read_100mA(&i_100mA) */
    int16_t i = g_bms_state.pack_current_100mA;
    buf[0U] = (uint8_t)(((uint16_t)i >> 8U) & 0xFFU);
    buf[1U] = (uint8_t)( (uint16_t)i        & 0xFFU);
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDB02 — BMS_PackPower_10W (2 bytes, int16 signed, 10 W/LSB)
 *
 * Production: computed from pack voltage × pack current.
 * P_10W = (V_mV * I_100mA) / (1000 * 1000 * 10) — done in fixed-point.
 * Positive = discharge power, negative = charging power drawn from grid.
 */
uds_status_t bms_did_bms_packpower_10w_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Compute from live V and I: P = V_mV * I_100mA / 10000000 */
    int16_t p = g_bms_state.pack_power_10W;
    buf[0U] = (uint8_t)(((uint16_t)p >> 8U) & 0xFFU);
    buf[1U] = (uint8_t)( (uint16_t)p        & 0xFFU);
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDB03 — BMS_ContactorState (1 byte, bitmask)
 *
 * Production: read GPIO inputs connected to contactor auxiliary contacts.
 * Auxiliary contacts are wired NO (normally open) in parallel with the main
 * contactor coil — they close when the contactor is energized.
 * Bit layout:
 *   bit 0 = MainPos closed   (GPIO: CONT_POS_AUX)
 *   bit 1 = MainNeg closed   (GPIO: CONT_NEG_AUX)
 *   bit 2 = PreCharge closed (GPIO: CONT_PC_AUX)
 *   bit 3 = HV interlock OK  (GPIO: HV_INTERLOCK_SENSE)
 *   bit 4 = Contactor weld   (latch: set by weld detection, cleared by routine)
 */
uds_status_t bms_did_bms_contactorstate_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with gpio_pin_get(cont_pos_dev, CONT_POS_AUX_PIN)
     * etc. for each contactor auxiliary contact. */
    buf[0U] = g_bms_state.contactor_state;
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDB04 — BMS_InsulationResistance_kOhm (2 bytes, uint16)
 *
 * Production: read latest IMD measurement over UART or CAN from the isolation
 * monitoring device (e.g. Bender ISOMETER IR155-3203).
 * 0xFFFF = measurement pending or IMD initialising.
 * ISO 6469-1 limit: ≥ 100 Ω/V. For 400V pack → 40 kΩ minimum.
 */
uds_status_t bms_did_bms_insulationresistance_kohm_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: Replace with imd_get_latest_measurement_kOhm(&r_kOhm) */
    uint16_t r = g_bms_state.isolation_resistance_kOhm;
    buf[0U] = (uint8_t)((r >> 8U) & 0xFFU);
    buf[1U] = (uint8_t)( r        & 0xFFU);
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * C. Cell Group Voltage Handlers (0xDC01–0xDC04, uint16 mV)
 *
 * In production, each group voltage is the summed output of 12 cell channels
 * read from the cell monitoring IC registers.
 * LTC6811 example: ltc6811_read_cvars() fills a float array; sum 12 cells
 * per group, convert to mV, store as uint16.
 * --------------------------------------------------------------------------- */

/**
 * @brief Read one cell group voltage (internal helper).
 *
 * @param buf    DID response buffer (2 bytes, big-endian)
 * @param group  Group index 0–3 (0 = group 1, 3 = group 4)
 */
static uds_status_t s_read_cell_group_voltage(uint8_t *buf, uint8_t group)
{
    uint16_t v = g_bms_state.cell_group_mV[group];
    buf[0U] = (uint8_t)((v >> 8U) & 0xFFU);
    buf[1U] = (uint8_t)( v        & 0xFFU);
    return UDS_STATUS_OK;
}

/** @brief DID 0xDC01 — BMS_CellGroup1_Voltage_mV */
uds_status_t bms_did_bms_cellgroup1_voltage_mv_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: ltc6811_sum_group_voltage(0, &v_mV) */
    return s_read_cell_group_voltage(buf, 0U);
}

/** @brief DID 0xDC02 — BMS_CellGroup2_Voltage_mV */
uds_status_t bms_did_bms_cellgroup2_voltage_mv_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: ltc6811_sum_group_voltage(1, &v_mV) */
    return s_read_cell_group_voltage(buf, 1U);
}

/** @brief DID 0xDC03 — BMS_CellGroup3_Voltage_mV */
uds_status_t bms_did_bms_cellgroup3_voltage_mv_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: ltc6811_sum_group_voltage(2, &v_mV) */
    return s_read_cell_group_voltage(buf, 2U);
}

/** @brief DID 0xDC04 — BMS_CellGroup4_Voltage_mV */
uds_status_t bms_did_bms_cellgroup4_voltage_mv_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: ltc6811_sum_group_voltage(3, &v_mV) */
    return s_read_cell_group_voltage(buf, 3U);
}

/* ---------------------------------------------------------------------------
 * D. Temperature Handlers (0xDD00–0xDD03, int8 offset-encoded)
 *
 * All temperature DIDs use the same offset encoding:
 *   raw_byte = T_degC + 40     (so −40 °C = raw 0, +215 °C = raw 255)
 * This is consistent with SAE J1939 SPN temperature encoding.
 * --------------------------------------------------------------------------- */

/**
 * @brief DID 0xDD00 — BMS_MaxCellTemperature_degC (1 byte, offset-encoded)
 *
 * Production: read all NTC ADC channels (cell stack sensors), apply Steinhart–
 * Hart equation or lookup table to convert resistance → °C, track maximum.
 * NTC resistors are typically on each cell monitoring board, read by the
 * monitoring IC auxiliary ADC (e.g. LTC6811 GPIO1–GPIO5 channels).
 */
uds_status_t bms_did_bms_maxcelltemperature_degc_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: bms_ntc_get_max_cell_temp_degC() + 40 */
    buf[0U] = g_bms_state.temp_cell_max_raw;
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDD01 — BMS_MinCellTemperature_degC (1 byte, offset-encoded)
 *
 * Production: minimum cell temperature across all NTC channels.
 * Critical for cold-start: charging below 0 °C risks lithium plating.
 * The BMS must derate charge current below 10 °C and inhibit above −10 °C.
 */
uds_status_t bms_did_bms_mincelltemperature_degc_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: bms_ntc_get_min_cell_temp_degC() + 40 */
    buf[0U] = g_bms_state.temp_cell_min_raw;
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDD02 — BMS_CoolantInletTemperature_degC (1 byte, offset-encoded)
 *
 * Production: read coolant-inlet NTC via Zephyr ADC driver.
 * adc_read() → raw count → voltage → resistance → temperature lookup table.
 * Used as feed-forward input for thermal derating model.
 */
uds_status_t bms_did_bms_coolantinlettemperature_degc_read(uint8_t *buf)
{
    /* TODO [APPLICATION]:
     *   int err = adc_read(coolant_adc_dev, &coolant_adc_seq);
     *   int16_t mv = adc_raw_to_millivolts_dt(&coolant_chan, &raw_val);
     *   buf[0] = ntc_mv_to_degC(mv) + 40;
     */
    buf[0U] = g_bms_state.temp_coolant_inlet_raw;
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDD03 — BMS_BMSBoardTemperature_degC (1 byte, offset-encoded)
 *
 * Production: read PCB-mounted NTC via Zephyr ADC driver.
 * Monitors MOSFET / power stage heating for overcurrent protection.
 */
uds_status_t bms_did_bms_bmsboardtemperature_degc_read(uint8_t *buf)
{
    /* TODO [APPLICATION]:
     *   int err = adc_read(board_ntc_dev, &board_ntc_seq);
     *   buf[0] = ntc_raw_to_degC_degC(&board_ntc_seq) + 40;
     */
    buf[0U] = g_bms_state.temp_bms_board_raw;
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * E. State Estimation Handlers (0xDE00–0xDE02)
 * --------------------------------------------------------------------------- */

/**
 * @brief DID 0xDE00 — BMS_StateOfCharge_04pct (1 byte, 0.4%/LSB)
 *
 * Production: read the latest SoC output from the SoC estimator module.
 * The estimator runs as a separate task and updates soc_percent periodically.
 * Typical algorithms: Extended Kalman Filter, coulomb counting with OCV reset,
 * or data-driven ML inference.
 *
 * LSB = 0.4 % → raw = SoC_percent / 0.4 = SoC_percent * 2.5
 * Example: 80% SoC → raw = 200
 */
uds_status_t bms_did_bms_stateofcharge_04pct_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: soc_estimator_get_raw(&raw) where raw = SoC * 2.5 */
    buf[0U] = g_bms_state.soc_raw;
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDE01 — BMS_StateOfHealth_04pct (1 byte, 0.4%/LSB)
 *
 * Production: read SoH from the SoH estimator.
 * SoH is updated after each full charge-discharge cycle by comparing
 * measured full-charge capacity to the nominal design capacity.
 * SoH = (C_measured_Ah / C_nominal_Ah) × 100 %
 */
uds_status_t bms_did_bms_stateofhealth_04pct_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: soh_estimator_get_raw(&raw) where raw = SoH * 2.5 */
    buf[0U] = g_bms_state.soh_raw;
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xDE02 — BMS_BalancingStateBitmask (1 byte, bitmask)
 *
 * Production: read balancing state from the cell balancing controller module.
 * Passive balancing: balancer_is_active() checks GPIO or SPI register on IC.
 * Active balancing: read balancing IC status register.
 */
uds_status_t bms_did_bms_balancingstatebitmask_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: buf[0] = cell_balancer_get_state_bitmask() */
    buf[0U] = g_bms_state.balancing_state;
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * F. System Health and Calibration Handlers (0xD900–0xD911)
 * --------------------------------------------------------------------------- */

/**
 * @brief DID 0xD900 — BMS_FaultFlagsBitmask (2 bytes, uint16 bitmask)
 *
 * Production: read the active fault latch register from the BMS fault manager.
 * The fault manager sets bits atomically when protection thresholds are crossed.
 * This DID provides a fast triage view without querying individual DTCs.
 */
uds_status_t bms_did_bms_faultflagsbitmask_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: buf[0..1] = fault_manager_get_active_flags() (big-endian) */
    uint16_t f = g_bms_state.fault_flags;
    buf[0U] = (uint8_t)((f >> 8U) & 0xFFU);
    buf[1U] = (uint8_t)( f        & 0xFFU);
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xD901 — BMS_FullChargeCapacity_Wh (4 bytes, uint32)
 *
 * Production: read from NVM. Updated by the SoH estimator after each full
 * charge cycle. Nominally 75000 Wh for a 75 kWh pack (decreases with aging).
 */
uds_status_t bms_did_bms_fullchargecapacity_wh_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: nvm_store_read(NVM_KEY_FULL_CAP_WH, buf, 4) */
    uint32_t c = g_bms_state.full_charge_capacity_Wh;
    buf[0U] = (uint8_t)((c >> 24U) & 0xFFU);
    buf[1U] = (uint8_t)((c >> 16U) & 0xFFU);
    buf[2U] = (uint8_t)((c >>  8U) & 0xFFU);
    buf[3U] = (uint8_t)( c         & 0xFFU);
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xD910 — BMS_OVThreshold_mV (2 bytes, uint16, read-write)
 *
 * Production read: read from NVM (set at EOL, OEM-specific value).
 * Production write: validate range [3800, 4350] mV, persist to NVM, apply to
 * protection comparator in cell monitoring IC.
 *
 * Security: write requires Level 1 security (extended session + seed/key).
 */
uds_status_t bms_did_bms_ovthreshold_mv_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: nvm_store_read(NVM_KEY_OV_THRESH_MV, buf, 2) */
    uint16_t t = g_bms_state.ov_threshold_mV;
    buf[0U] = (uint8_t)((t >> 8U) & 0xFFU);
    buf[1U] = (uint8_t)( t        & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t bms_did_bms_ovthreshold_mv_write(const uint8_t *buf)
{
    uint16_t t = (uint16_t)(((uint16_t)buf[0U] << 8U) | (uint16_t)buf[1U]);
    /* Range validation: NMC OV threshold must be 3800–4350 mV */
    if ((t < 3800U) || (t > 4350U)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
    /* TODO [APPLICATION]: nvm_store_write(NVM_KEY_OV_THRESH_MV, buf, 2)
     *   then ltc6811_set_ov_threshold(t) to apply to monitoring IC */
    g_bms_state.ov_threshold_mV = t;
    return UDS_STATUS_OK;
}

/**
 * @brief DID 0xD911 — BMS_UVThreshold_mV (2 bytes, uint16, read-write)
 *
 * Production: same pattern as OV threshold. UV threshold determines the point
 * at which the BMS opens the main contactors on deep discharge.
 *
 * Range validation: [2500, 3200] mV for NMC chemistry.
 */
uds_status_t bms_did_bms_uvthreshold_mv_read(uint8_t *buf)
{
    /* TODO [APPLICATION]: nvm_store_read(NVM_KEY_UV_THRESH_MV, buf, 2) */
    uint16_t t = g_bms_state.uv_threshold_mV;
    buf[0U] = (uint8_t)((t >> 8U) & 0xFFU);
    buf[1U] = (uint8_t)( t        & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t bms_did_bms_uvthreshold_mv_write(const uint8_t *buf)
{
    uint16_t t = (uint16_t)(((uint16_t)buf[0U] << 8U) | (uint16_t)buf[1U]);
    /* Range validation: NMC UV threshold must be 2500–3200 mV */
    if ((t < 2500U) || (t > 3200U)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
    /* TODO [APPLICATION]: nvm_store_write(NVM_KEY_UV_THRESH_MV, buf, 2)
     *   then ltc6811_set_uv_threshold(t) to apply to monitoring IC */
    g_bms_state.uv_threshold_mV = t;
    return UDS_STATUS_OK;
}

/* =============================================================================
 * Diagnostics Infrastructure
 *
 * [NEW-M2 FIX] This section replaces the stale diagnostics wiring that used
 * non-existent API functions from an earlier prototype version:
 *
 *   OLD (broken):                        NEW (current API):
 *   ─────────────────────────────────    ──────────────────────────────────────
 *   zephyr_port_init(can_dev)            zephyr_port_init(&port_cfg, &can)
 *   can_transport_get_instance()         can comes from zephyr_port_init output
 *   zephyr_can_init(t, dev, rx, tx)      replaced by zephyr_port_init
 *   isotp_init(&ch, transport)           handled internally by uds_generated_init
 *   uds_generated_init(&s_isotp_ch)      uds_generated_init(can, rx_id, tx_id)
 *   uds_server_poll()                    can_transport_receive + isotp_process_rx_frame
 *                                        + uds_server_process_request
 *
 * The pattern mirrors examples/basic_ecu/src/main.c exactly.
 * All BMS application state and DID/routine handlers above are unchanged.
 * ============================================================================= */

/* ── Static allocations ────────────────────────────────────────────────────── */

K_THREAD_STACK_DEFINE(s_diag_stack, CONFIG_DIAG_TASK_STACK_SIZE);
static struct k_thread s_diag_thread;

/* UDS message buffers — file scope to avoid stack exhaustion in thread. */
static uds_msg_buf_t s_req_buf;
static uds_msg_buf_t s_resp_buf;

/* Platform objects. */
static diag_mutex_t s_session_lock;  /**< Protects uds_session_ctx_t access.  */
static diag_mutex_t s_security_lock; /**< Protects uds_security_ctx_t access. */
static diag_timer_t s_tick_timer;    /**< 1 ms periodic tick.                 */
static diag_wdt_t   s_wdt;          /**< Hardware watchdog.                  */

/* ── Zephyr TRNG callback for UDS SecurityAccess [P1-SEC] ─────────────────── */
/*
 * Guard the entropy driver header and device handle on CONFIG_ENTROPY_GENERATOR.
 * When CONFIG_ENTROPY_GENERATOR=n (native_sim CI), the LFSR fallback inside
 * uds_security_algo.c is used automatically.
 */
#if defined(CONFIG_ENTROPY_GENERATOR)
#include <zephyr/drivers/entropy.h>
static const struct device *s_entropy_dev = NULL;

static uds_status_t diag_trng_cb(uint8_t *buf, uint8_t len)
{
    if ((s_entropy_dev == NULL) || !device_is_ready(s_entropy_dev)) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    int rc = entropy_get_entropy(s_entropy_dev, buf, (size_t)len);
    return (rc == 0) ? UDS_STATUS_OK : UDS_STATUS_ERR_PLATFORM;
}
#endif /* CONFIG_ENTROPY_GENERATOR */

/**
 * @brief Initialise TRNG (if available) and register callback.
 *
 * Must be called before uds_generated_init() so that the RNG callback is
 * in place before the first SID 0x27 seed request.
 */
static int diag_security_algo_init(void)
{
#if defined(CONFIG_ENTROPY_GENERATOR)
    s_entropy_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
    if (!device_is_ready(s_entropy_dev)) {
        LOG_WRN("[BMS][P1-SEC] TRNG device not ready — using LFSR fallback (dev only)");
        s_entropy_dev = NULL;
    } else {
        uds_security_algo_set_rng_cb(diag_trng_cb);
        LOG_INF("[BMS][P1-SEC] TRNG registered: %s", s_entropy_dev->name);
    }
#else
    LOG_WRN("[BMS][P1-SEC] No zephyr_entropy DTS node — LFSR seed fallback active. "
            "Add RNG DTS node and CONFIG_ENTROPY_GENERATOR=y for production.");
#endif /* CONFIG_ENTROPY_GENERATOR */

    /*
     * OEM KEY INJECTION PLACEHOLDER — replace before production.
     * See the Security Integration Guide (Professional tier — xaloqi.com)
     * for the full key provisioning workflow.
     */
    LOG_WRN("[BMS][P1-SEC] Using compile-time placeholder AES keys. "
            "Inject OTP keys before production.");
    return 0;
}

/* ── ISO-TP RX completion callback ────────────────────────────────────────── */
/*
 * Called from diag_task_entry() when isotp_process_rx_frame() has
 * reassembled a complete UDS PDU into a contiguous buffer.
 *
 * Acquires both session and security locks, dispatches to the UDS server,
 * and transmits the response via ISO-TP.
 *
 * TIMING: Must complete within GEN_P2_SERVER_MAX_MS (25 ms).
 */
static void on_isotp_rx_complete(
    const uint8_t *data,
    uint32_t       length,
    void          *arg)
{
    uds_server_ctx_t *srv    = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp     = uds_generated_get_isotp();
    uds_status_t      status;
    uint16_t          i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) {
        LOG_ERR("[BMS] on_isotp_rx_complete: NULL argument — PDU dropped.");
        return;
    }

    if ((length == (uint32_t)0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) {
        LOG_WRN("[BMS] on_isotp_rx_complete: invalid length %u — PDU dropped.",
                (unsigned)length);
        return;
    }

    for (i = (uint16_t)0U; i < (uint16_t)length; i++) {
        s_req_buf.data[i] = data[i];
    }
    s_req_buf.length  = (uint16_t)length;
    s_resp_buf.length = (uint16_t)0U;

    LOG_DBG("[BMS] UDS RX: SID=0x%02X len=%u",
            (unsigned)data[0], (unsigned)length);

    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    status = uds_server_process_request(srv, &s_req_buf, &s_resp_buf);
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);

    if ((status != UDS_STATUS_OK) && (s_resp_buf.length == (uint16_t)0U)) {
        LOG_ERR("[BMS] UDS server error 0x%02X — no response generated.",
                (unsigned)status);
        return;
    }

    if (s_resp_buf.length > (uint16_t)0U) {
        LOG_DBG("[BMS] UDS TX: RSID=0x%02X len=%u",
                (unsigned)s_resp_buf.data[0], (unsigned)s_resp_buf.length);
        status = isotp_transmit(tp, s_resp_buf.data, s_resp_buf.length);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("[BMS] ISO-TP TX error 0x%02X", (unsigned)status);
        }
    }

    /* [P2-0x11-01] Deferred reset: response is on the wire, now reset. */
    if (srv->pending_reset_type != (uint8_t)0U) {
        uint8_t reset_type = srv->pending_reset_type;
        srv->pending_reset_type = (uint8_t)0U;
        (void)zephyr_port_nvm_flush();
        k_msleep(50);  /* Allow ISO-TP TX to reach the wire before reset (As timer). */
        (void)zephyr_port_ecu_reset(reset_type);
        /* Does not return. */
    }
}

/* ── 1 ms tick callback ────────────────────────────────────────────────────── */

typedef struct tick_ctx {
    uds_server_ctx_t *srv;
    isotp_ctx_t      *tp;
} tick_ctx_t;

static void on_tick(void *arg)
{
    tick_ctx_t *ctx = (tick_ctx_t *)arg;

    if ((ctx == NULL) || (ctx->tp == NULL) || (ctx->srv == NULL)) {
        return;
    }

    (void)isotp_tick_1ms(ctx->tp);

    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    (void)uds_server_tick_1ms(ctx->srv);
    (void)uds_periodic_tick_1ms();
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);
}

/* =============================================================================
 * Diagnostics Task
 *
 * Runs as a dedicated Zephyr thread at CONFIG_DIAG_TASK_PRIORITY.
 * Loops at ~1 ms intervals driven by the k_timer-backed diag_timer_t.
 *
 * Each iteration:
 *   1. Wait for 1 ms tick (blocks on semaphore from timer ISR).
 *   2. Poll CAN RX queue — forward frames to ISO-TP reassembler.
 *   3. Drive ISO-TP + UDS timers via on_tick() callback.
 *   4. Feed hardware watchdog.
 *   5. Check for overruns and log if threshold exceeded.
 *
 * Thread args (p1, p2, p3) match the k_thread_create() call in main():
 *   p1 — uds_server_ctx_t *srv
 *   p2 — can_transport_t  *can
 *   p3 — isotp_ctx_t      *tp
 * ============================================================================= */
static void diag_task_entry(void *p1, void *p2, void *p3)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)p1;
    can_transport_t  *can = (can_transport_t  *)p2;
    isotp_ctx_t      *tp  = (isotp_ctx_t      *)p3;

    uds_status_t    status;
    uds_can_frame_t rx_frame;
    bool            frame_ready;
    uint32_t        overrun_count;
    uint32_t        prev_overrun = 0U;

    tick_ctx_t tick_ctx = { .srv = srv, .tp = tp };

    LOG_INF("[BMS] Diagnostics task started (stack %u bytes, priority %d).",
            (unsigned)CONFIG_DIAG_TASK_STACK_SIZE,
            (int)CONFIG_DIAG_TASK_PRIORITY);

    /* Start 1 ms periodic timer — callback is on_tick, arg is tick_ctx. */
    if (diag_timer_start(&s_tick_timer) != UDS_STATUS_OK) {
        LOG_ERR("[BMS] Failed to start 1 ms timer — aborting diag task.");
        return;
    }

    while (true) {

        /* ── [1] Wait for 1 ms tick ──────────────────────────────────────── */
        status = diag_timer_wait_tick(&s_tick_timer, 2U);
        if (status == UDS_STATUS_ERR_TIMEOUT) {
            LOG_WRN("[BMS] 1 ms tick timeout — continuing.");
        }

        /* ── [2] CAN RX poll ─────────────────────────────────────────────── */
        frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);

        if (status != UDS_STATUS_OK) {
            if (status == UDS_STATUS_ERR_CAN_BUS_OFF) {
                LOG_ERR("[BMS] CAN bus-off detected.");
                /*
                 * TODO [APPLICATION]: Implement bus-off recovery.
                 * For now log and continue — WDT will reset if bus stays off.
                 */
            } else {
                LOG_DBG("[BMS] can_transport_receive error: 0x%02X",
                        (unsigned)status);
            }
        } else if (frame_ready) {
            status = isotp_process_rx_frame(
                tp,
                &rx_frame,
                on_isotp_rx_complete,
                (void *)srv
            );
            if ((status != UDS_STATUS_OK) &&
                (status != (uds_status_t)UDS_STATUS_ERR_TP_FRAME_INVALID)) {
                LOG_WRN("[BMS] ISO-TP RX error: 0x%02X", (unsigned)status);
            }
        } else {
            /* No frame this iteration — idle. */
        }

        /* ── [3] 1 ms tick processing (ISO-TP + UDS + periodic timers) ─── */
        on_tick(&tick_ctx);

        /* ── [3a] Drain periodic push frames due this tick ───────────────── */
        {
            static uds_msg_buf_t s_periodic_frame;
            while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
                (void)isotp_transmit(tp, s_periodic_frame.data,
                                     (uint32_t)s_periodic_frame.length);
            }
        }

        /* ── [4] Watchdog feed ───────────────────────────────────────────── */
        (void)diag_wdt_feed(&s_wdt);

        /* ── [5] Overrun detection ───────────────────────────────────────── */
        (void)diag_timer_pending_ticks(&s_tick_timer, &overrun_count);
        if (overrun_count > prev_overrun) {
            uint32_t new_overruns = overrun_count - prev_overrun;
            if (new_overruns >= DIAG_OVERRUN_LOG_THRESHOLD) {
                LOG_WRN("[BMS] Poll loop overrun: %u ticks missed.",
                        (unsigned)new_overruns);
            }
            prev_overrun = overrun_count;
        }

    } /* while (true) */
}

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

/* =============================================================================
 * main() — Platform + stack initialisation, then start diagnostics thread.
 *
 * main() exits after starting the diagnostics thread. The Zephyr scheduler
 * takes over and the diagnostics thread runs indefinitely.
 *
 * [NEW-M2 FIX] Replaced stale 4-step manual CAN/ISO-TP wiring with the
 * current API:
 *   1. zephyr_port_init(&port_cfg, &can)   — CAN platform + filter install
 *   2. uds_generated_init(can, rx, tx)     — full stack init (includes ISO-TP)
 *   3. uds_generated_get_server() / get_isotp() — retrieve context pointers
 *   4. Pass srv/can/tp to diagnostics thread as p1/p2/p3
 * ============================================================================= */
int main(void)
{
    uds_status_t      st;
    can_transport_t  *can  = NULL;
    uds_server_ctx_t *srv  = NULL;
    isotp_ctx_t      *tp   = NULL;
    zephyr_port_cfg_t port_cfg;

    LOG_INF("=== BMS ECU — Xaloqi EDS v1.0.0 ===");
    LOG_INF("    24 DIDs | 10 DTCs | 5 Routines | ASIL-B");

    /* --- 1. Initialise BMS application state ----------------------------- */
    bms_state_init();
    LOG_INF("[BMS] Application state initialised.");

    /* --- 2. Platform objects init --------------------------------------- */
    if (diag_mutex_init(&s_session_lock) != UDS_STATUS_OK) {
        LOG_ERR("[BMS] Session mutex init failed.");
        return -EIO;
    }
    if (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK) {
        LOG_ERR("[BMS] Security mutex init failed.");
        return -EIO;
    }

    st = diag_wdt_init(&s_wdt);
    if (st != UDS_STATUS_OK) {
        /* Non-fatal — continue without WDT for CI/simulation. */
        LOG_WRN("[BMS] WDT init returned 0x%02X (non-fatal).", (unsigned)st);
    }

    /*
     * 1 ms timer: register on_tick callback now; start is deferred to
     * the diagnostics thread after full stack init completes.
     */
    st = diag_timer_init(&s_tick_timer, on_tick, NULL);
    if (st != UDS_STATUS_OK) {
        LOG_ERR("[BMS] Timer init failed: 0x%02X", (unsigned)st);
        return -EIO;
    }

    /* --- 3. Platform + CAN transport ------------------------------------ */
    const struct device *can_dev = DIAG_CAN_DEV;
    if (!device_is_ready(can_dev)) {
        LOG_ERR("[BMS] CAN device not ready — diagnostics disabled.");
        return -EIO;
    }

    (void)memset(&port_cfg, 0, sizeof(port_cfg));
    port_cfg.can_dev        = can_dev;
    port_cfg.physical_rx_id = 0U;  /* functional-only (0x7DF); set 0x7E0U for
                                     * physical addressing if required by OEM tool */

    st = zephyr_port_init(&port_cfg, &can);
    if (st != UDS_STATUS_OK) {
        LOG_ERR("[BMS] zephyr_port_init failed: 0x%02X", (unsigned)st);
        return -EIO;
    }
    LOG_INF("[BMS] CAN transport ready (device: %s).", can_dev->name);

    /* --- 4. Security algorithm init ------------------------------------- */
    /*
     * Must run before uds_generated_init() so that the TRNG callback and
     * any injected OEM keys are in place before the first seed request.
     */
    (void)diag_security_algo_init();

    /* --- 5. UDS stack initialisation (generated from YAML) -------------- */
    st = uds_generated_init(can, DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);
    if (st != UDS_STATUS_OK) {
        LOG_ERR("[BMS] uds_generated_init failed: 0x%02X", (unsigned)st);
        return -EIO;
    }

    srv = uds_generated_get_server();
    tp  = uds_generated_get_isotp();

    if ((srv == NULL) || (tp == NULL)) {
        LOG_ERR("[BMS] Context accessors returned NULL after init.");
        return -EIO;
    }

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    LOG_INF("[BMS] UDS stack initialised. 24 DIDs / 10 DTCs registered.");
    LOG_INF("[BMS]   RX CAN: 0x%03X  TX CAN: 0x%03X",
            (unsigned)DIAG_RX_CAN_ID, (unsigned)DIAG_TX_CAN_ID);

    /* --- 6. Launch diagnostics polling task ----------------------------- */
    /*
     * Pass srv (p1), can (p2), tp (p3) as thread arguments.
     * diag_task_entry() uses these directly — no global state needed.
     */
    (void)k_thread_create(
        &s_diag_thread,
        s_diag_stack,
        K_THREAD_STACK_SIZEOF(s_diag_stack),
        diag_task_entry,
        (void *)srv,
        (void *)can,
        (void *)tp,
        CONFIG_DIAG_TASK_PRIORITY,
        (uint32_t)0U,
        K_NO_WAIT
    );
    k_thread_name_set(&s_diag_thread, "bms_diag");

    LOG_INF("[BMS] Diagnostics task launched. ECU ready for scan-tool.");

    /* main() returns — Zephyr scheduler takes over. */
    return 0;
}
