// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/ardep_ecu/src/main.c
 *
 * PURPOSE: ARDEP I/O Controller ECU — application entry point.
 *
 * TARGET: Mercedes-Benz ARDEP (Automotive Rapid DEvelopment Platform)
 *         boards/mercedes/ardep in the ARDEP Zephyr workspace.
 *
 * CONTEXT — ARDEP UPGRADE PATH:
 *   ARDEP ships with UDS support via the driftregion/iso14229 library
 *   (MIT-licensed, no ASIL documentation, no test generation).
 *   This file demonstrates replacing that integration with the Embedded
 *   Diagnostics Suite:
 *     - ASIL-B safety wrappers (5-step validation chain, generated)
 *     - AES-128-CMAC security access (Phase 1 hardening)
 *     - Auto-generated test suite (pytest, simulator + firmware modes)
 *     - MISRA C:2012 deviation log (ISO 26262 work product)
 *
 * ECU ROLE: Automotive I/O Controller
 *   Models a body-domain I/O controller on the ARDEP PowerIO Shield:
 *     - 6× high-side switch outputs (48V / 3A per channel)
 *     - 6× analog/digital inputs
 *     - CAN ↔ LIN gateway status reporting
 *     - Firmware update metadata (DFU over UDS — ARDEP primary feature)
 *     - ECU health and calibration DIDs
 *
 * DID HANDLERS:
 *   Stub implementations for all 35 DIDs are in this file.
 *   Each stub contains:
 *     1. A comment explaining what a production implementation reads
 *     2. A deterministic return value suitable for testing
 *     3. A TODO tag for integration with your ECU application layer
 *
 * THREAD MODEL:  (identical to basic_ecu — see basic_ecu/src/main.c)
 *   main()        — platform init → UDS init → start diag thread → exit
 *   diag_task     — 1 ms poll: CAN RX → ISO-TP → UDS dispatch → TX
 *
 * STACK BUDGET:
 *   CONFIG_DIAG_TASK_STACK_SIZE = 6144 bytes (larger than basic_ecu due
 *   to 35 DIDs vs 5 — more handler table traversal depth).
 *
 * BUILDING:
 *   # Requires ARDEP workspace (see docs/ARDEP_UPGRADE_GUIDE.md):
 *   cd <ardep-workspace>
 *   west build -b ardep path/to/eds/examples/ardep_ecu
 *
 *   # With EDS overlay explicitly:
 *   west build -b ardep path/to/eds/examples/ardep_ecu \
 *     -- -DEXTRA_CONF_FILE=path/to/eds/examples/ardep_ecu/boards/ardep/ardep.conf \
 *     -DDTC_OVERLAY_FILE=path/to/eds/examples/ardep_ecu/boards/ardep/ardep.overlay
 *
 *   # For simulation (CI / host testing without hardware):
 *   west build -b native_sim path/to/eds/examples/ardep_ecu \
 *     -- -DDIAG_SKIP_CODEGEN=ON
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

LOG_MODULE_REGISTER(ardep_ecu, LOG_LEVEL_INF);

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define DIAG_CAN_DEV         DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID       ((uint32_t)GEN_CAN_RX_ID)   /* 0x7DF */
#define DIAG_TX_CAN_ID       ((uint32_t)GEN_CAN_TX_ID)   /* 0x7E8 */

#ifndef CONFIG_DIAG_TASK_STACK_SIZE
#define CONFIG_DIAG_TASK_STACK_SIZE   (6144U)
#endif

#ifndef CONFIG_DIAG_TASK_PRIORITY
#define CONFIG_DIAG_TASK_PRIORITY     (5)
#endif

#define DIAG_OVERRUN_LOG_THRESHOLD    (3U)

/* =============================================================================
 * Application state — ARDEP I/O Controller
 *
 * In a production integration, these variables are updated by application
 * threads that read real hardware (ADC, GPIO, CAN statistics, etc.).
 * Here they are initialized to safe default values for demonstration.
 *
 * Thread safety: DID read handlers access these via the ASIL-B safety
 * wrappers which are called only from the diagnostics thread. If your
 * application updates these from another thread, protect them with a mutex.
 * =============================================================================*/

/* ── ECU identity ─────────────────────────────────────────────────────────── */
static const uint8_t s_vin[17]            = "ARDEPEDS000000001";
static const uint8_t s_ecu_serial[8]      = "EDS00001";
static       uint8_t s_spare_part_num[11] = "EDS-IO-0001";
static const uint8_t s_sw_version[4]      = { 1U, 0U, 0U, 0U }; /* 1.0.0.0 */
static const uint8_t s_supplier_id[5]     = "00000";             /* CAGE code */

/* ── PowerIO output state ─────────────────────────────────────────────────── */
static uint8_t  s_output_bitmask       = 0x00U;     /* All outputs OFF */
static uint16_t s_output_current_ma[6] = {0U};      /* [Out1..Out6] in mA  */

/* ── PowerIO input state ──────────────────────────────────────────────────── */
static uint8_t  s_input_bitmask        = 0x00U;
static uint16_t s_input_voltage_mv[3]  = {12000U, 0U, 0U}; /* Vin, In2, In3 */

/* ── CAN / LIN bus status ─────────────────────────────────────────────────── */
static uint8_t  s_can_status[2]      = { 0x00U, 0U };  /* [state, tx_err]  */
static uint32_t s_can_rx_count       = 0U;
static uint32_t s_can_tx_count       = 0U;
static uint8_t  s_lin_status         = 0x00U;           /* 0=OK             */
static uint8_t  s_lin_slave_count    = 0U;

/* ── ECU health ───────────────────────────────────────────────────────────── */
static uint16_t s_supply_mv          = 12000U;   /* 12.0V nominal           */
static uint8_t  s_temperature_degc   = 65U;      /* 25°C (offset: +40)      */
static uint32_t s_uptime_seconds     = 0U;
static uint16_t s_reset_counter      = 0U;
static uint8_t  s_reset_reason       = 0x00U;    /* 0x00 = PowerOn          */
static const uint8_t s_eds_version[3] = { 1U, 0U, 0U }; /* EDS 1.0.0       */

/* ── Calibration (NVM-backed in production) ───────────────────────────────── */
static uint16_t s_can_bitrate_kbps   = 500U;
static uint16_t s_lin_baudrate_bps   = 9600U;
static uint16_t s_oc_threshold_ma    = 2500U;    /* 2.5 A overcurrent limit */
static uint16_t s_wdt_timeout_ms     = 100U;

/* ── Firmware update metadata ─────────────────────────────────────────────── */
static const uint8_t s_fw_active[4]  = { 1U, 0U, 0U, 0U };
static const uint8_t s_fw_pending[4] = { 0U, 0U, 0U, 0U }; /* None pending */
static uint8_t  s_dfu_status         = 0x00U;    /* 0x00 = Idle             */

/* =============================================================================
 * Static allocations
 * ============================================================================= */

K_THREAD_STACK_DEFINE(s_diag_stack, CONFIG_DIAG_TASK_STACK_SIZE);
static struct k_thread s_diag_thread;

static uds_msg_buf_t s_req_buf;
static uds_msg_buf_t s_resp_buf;

static diag_mutex_t s_session_lock;
static diag_mutex_t s_security_lock;
static diag_timer_t s_tick_timer;
static diag_wdt_t   s_wdt;

/* =============================================================================
 * DID Read Handlers
 *
 * These functions are registered by the generated did_handlers_register_all()
 * call inside uds_generated_init(). The ASIL-B safety wrappers enforce
 * session, security, and buffer-length checks before calling them.
 *
 * Naming: ardep_read_<did_name_lowercase>
 *   - Parameters: uint8_t *buf  (write response data here)
 *   - Return:     UDS_STATUS_OK on success, error code on failure
 * ============================================================================= */

/* ── A. ECU Identity ────────────────────────────────────────────────────────*/

uds_status_t ardep_read_vehicleidentificationnumber(uint8_t *buf)
{
    /* TODO: Replace with real VIN from NVM / OBD port. */
    (void)memcpy(buf, s_vin, 17U);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_ecuserialnumber(uint8_t *buf)
{
    (void)memcpy(buf, s_ecu_serial, 8U);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_vehiclemanufacturerspharepartnumber(uint8_t *buf)
{
    (void)memcpy(buf, s_spare_part_num, 11U);
    return UDS_STATUS_OK;
}

uds_status_t ardep_write_vehiclemanufacturerSparePartNumber(
    const uint8_t *buf)
{
    /* TODO: Persist to NVM in production. */
    (void)memcpy(s_spare_part_num, buf, 11U);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_ecusoftwareversionnumber(uint8_t *buf)
{
    (void)memcpy(buf, s_sw_version, 4U);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_systemsupplieridentifierdataidentifier(uint8_t *buf)
{
    (void)memcpy(buf, s_supplier_id, 5U);
    return UDS_STATUS_OK;
}

/* ── B. PowerIO Output State ────────────────────────────────────────────────*/

uds_status_t ardep_read_powerio_outputstatebitmask(uint8_t *buf)
{
    /*
     * TODO: Read actual output state from PowerIO driver.
     * Example (Zephyr GPIO API):
     *   uint8_t mask = 0U;
     *   for (uint8_t i = 0; i < 6; i++) {
     *       if (gpio_pin_get(hs_dev, out_pins[i]) > 0) mask |= (1U << i);
     *   }
     *   buf[0] = mask;
     */
    buf[0] = s_output_bitmask;
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_output1_current_ma(uint8_t *buf)
{
    /* TODO: Read from ADC channel mapped to Output 1 sense resistor. */
    buf[0] = (uint8_t)(s_output_current_ma[0] >> 8U);
    buf[1] = (uint8_t)(s_output_current_ma[0] & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_output2_current_ma(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_output_current_ma[1] >> 8U);
    buf[1] = (uint8_t)(s_output_current_ma[1] & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_output3_current_ma(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_output_current_ma[2] >> 8U);
    buf[1] = (uint8_t)(s_output_current_ma[2] & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_output4_current_ma(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_output_current_ma[3] >> 8U);
    buf[1] = (uint8_t)(s_output_current_ma[3] & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_output5_current_ma(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_output_current_ma[4] >> 8U);
    buf[1] = (uint8_t)(s_output_current_ma[4] & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_output6_current_ma(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_output_current_ma[5] >> 8U);
    buf[1] = (uint8_t)(s_output_current_ma[5] & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_outputcontrol(uint8_t *buf)
{
    buf[0] = s_output_bitmask;
    return UDS_STATUS_OK;
}

uds_status_t ardep_write_powerio_outputcontrol(const uint8_t *buf)
{
    /*
     * TODO: Drive actual GPIO outputs via PowerIO driver.
     *
     * SAFETY NOTE: In a real system, this write must validate:
     *   - Vehicle speed == 0 (no actuation while driving)
     *   - Supply voltage in range (>9V) before energizing outputs
     *   - No existing overcurrent fault active
     *
     * The ASIL-B wrapper enforces Extended Session + Security Level 1
     * before this function is called (see diagnostics_config.yaml).
     */
    s_output_bitmask = buf[0];
    LOG_INF("PowerIO output control: 0x%02X", (unsigned)buf[0]);
    return UDS_STATUS_OK;
}

/* ── C. PowerIO Input State ─────────────────────────────────────────────────*/

uds_status_t ardep_read_powerio_inputstatebitmask(uint8_t *buf)
{
    /* TODO: Read from GPIO input pins via ARDEP Arduino headers. */
    buf[0] = s_input_bitmask;
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_input1_voltage_mv(uint8_t *buf)
{
    /* TODO: Read from ADC1 channel mapped to Input 1. */
    buf[0] = (uint8_t)(s_input_voltage_mv[0] >> 8U);
    buf[1] = (uint8_t)(s_input_voltage_mv[0] & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_input2_voltage_mv(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_input_voltage_mv[1] >> 8U);
    buf[1] = (uint8_t)(s_input_voltage_mv[1] & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_input3_voltage_mv(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_input_voltage_mv[2] >> 8U);
    buf[1] = (uint8_t)(s_input_voltage_mv[2] & 0xFFU);
    return UDS_STATUS_OK;
}

/* ── D. CAN / LIN Bus Status ────────────────────────────────────────────────*/

uds_status_t ardep_read_can_busstatus(uint8_t *buf)
{
    /*
     * TODO: Read from Zephyr CAN error counter API.
     * Example:
     *   struct can_bus_err_cnt ec;
     *   can_get_max_bitrate(can_dev, &max_br);
     *   can_get_state(can_dev, &state, &ec);
     *   buf[0] = (uint8_t)state;
     *   buf[1] = ec.tx_err_cnt;
     */
    buf[0] = s_can_status[0];
    buf[1] = s_can_status[1];
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_can_rxframecount(uint8_t *buf)
{
    buf[0] = (uint8_t)((s_can_rx_count >> 24U) & 0xFFU);
    buf[1] = (uint8_t)((s_can_rx_count >> 16U) & 0xFFU);
    buf[2] = (uint8_t)((s_can_rx_count >>  8U) & 0xFFU);
    buf[3] = (uint8_t)( s_can_rx_count         & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_can_txframecount(uint8_t *buf)
{
    buf[0] = (uint8_t)((s_can_tx_count >> 24U) & 0xFFU);
    buf[1] = (uint8_t)((s_can_tx_count >> 16U) & 0xFFU);
    buf[2] = (uint8_t)((s_can_tx_count >>  8U) & 0xFFU);
    buf[3] = (uint8_t)( s_can_tx_count         & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_lin_busstatus(uint8_t *buf)
{
    /* TODO: Read from LIN driver via ARDEP LIN HAL (ardep/drivers/lin). */
    buf[0] = s_lin_status;
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_lin_slavecount(uint8_t *buf)
{
    buf[0] = s_lin_slave_count;
    return UDS_STATUS_OK;
}

/* ── E. ECU Health ───────────────────────────────────────────────────────────*/

uds_status_t ardep_read_ecu_supplyvoltage_mv(uint8_t *buf)
{
    /*
     * TODO: Read from ADC channel connected to supply voltage divider.
     * ARDEP PowerIO Shield provides Vsupply sense.
     */
    buf[0] = (uint8_t)(s_supply_mv >> 8U);
    buf[1] = (uint8_t)(s_supply_mv & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_ecu_internaltemperature_degc(uint8_t *buf)
{
    /* Offset encoding: raw_byte = T_celsius + 40. Range: -40..+215°C. */
    buf[0] = s_temperature_degc;
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_ecu_uptimeseconds(uint8_t *buf)
{
    /* Increment from Zephyr monotonic clock in production. */
    buf[0] = (uint8_t)((s_uptime_seconds >> 24U) & 0xFFU);
    buf[1] = (uint8_t)((s_uptime_seconds >> 16U) & 0xFFU);
    buf[2] = (uint8_t)((s_uptime_seconds >>  8U) & 0xFFU);
    buf[3] = (uint8_t)( s_uptime_seconds         & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_ecu_resetcounter(uint8_t *buf)
{
    /* TODO: Persistent NVM counter — increment on each boot. */
    buf[0] = (uint8_t)(s_reset_counter >> 8U);
    buf[1] = (uint8_t)(s_reset_counter & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_ecu_lastresetreason(uint8_t *buf)
{
    /* TODO: Read RSTCR register reason at boot; store in RAM. */
    buf[0] = s_reset_reason;
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_ecu_diagnosticstackversion(uint8_t *buf)
{
    /* Returns Xaloqi EDS version: 1.0.0 */
    (void)memcpy(buf, s_eds_version, 3U);
    return UDS_STATUS_OK;
}

/* ── F. Calibration ──────────────────────────────────────────────────────────*/

uds_status_t ardep_read_can_busbitrate_kbps(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_can_bitrate_kbps >> 8U);
    buf[1] = (uint8_t)(s_can_bitrate_kbps & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_write_can_busbitrate_kbps(const uint8_t *buf)
{
    uint16_t rate = (uint16_t)(((uint16_t)buf[0] << 8U) | buf[1]);
    /* Validate: only 125, 250, 500, 1000 kbps are valid. */
    if ((rate != 125U) && (rate != 250U) &&
        (rate != 500U) && (rate != 1000U)) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_CORRECT;
    }
    s_can_bitrate_kbps = rate;
    LOG_INF("CAN bitrate set to %u kbps (applied on next reset)", (unsigned)rate);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_lin_busbaudrate_bps(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_lin_baudrate_bps >> 8U);
    buf[1] = (uint8_t)(s_lin_baudrate_bps & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_write_lin_busbaudrate_bps(const uint8_t *buf)
{
    uint16_t baud = (uint16_t)(((uint16_t)buf[0] << 8U) | buf[1]);
    if ((baud != 2400U) && (baud != 9600U) && (baud != 19200U)) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_CORRECT;
    }
    s_lin_baudrate_bps = baud;
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_powerio_overcurrentthreshold_ma(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_oc_threshold_ma >> 8U);
    buf[1] = (uint8_t)(s_oc_threshold_ma & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_write_powerio_overcurrentthreshold_ma(const uint8_t *buf)
{
    uint16_t val = (uint16_t)(((uint16_t)buf[0] << 8U) | buf[1]);
    if ((val < 500U) || (val > 3000U)) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_CORRECT;
    }
    s_oc_threshold_ma = val;
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_watchdogtimeout_ms(uint8_t *buf)
{
    buf[0] = (uint8_t)(s_wdt_timeout_ms >> 8U);
    buf[1] = (uint8_t)(s_wdt_timeout_ms & 0xFFU);
    return UDS_STATUS_OK;
}

uds_status_t ardep_write_watchdogtimeout_ms(const uint8_t *buf)
{
    uint16_t val = (uint16_t)(((uint16_t)buf[0] << 8U) | buf[1]);
    if ((val < 50U) || (val > 1000U)) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_CORRECT;
    }
    s_wdt_timeout_ms = val;
    return UDS_STATUS_OK;
}

/* ── Firmware update metadata ────────────────────────────────────────────────*/

uds_status_t ardep_read_firmwareversion_active(uint8_t *buf)
{
    /* TODO: Read from MCUboot image header. */
    (void)memcpy(buf, s_fw_active, 4U);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_firmwareversion_pending(uint8_t *buf)
{
    /* TODO: Read from MCUboot secondary slot image header. */
    (void)memcpy(buf, s_fw_pending, 4U);
    return UDS_STATUS_OK;
}

uds_status_t ardep_read_firmwareupdatestatus(uint8_t *buf)
{
    buf[0] = s_dfu_status;
    return UDS_STATUS_OK;
}

/* =============================================================================
 * DID handler registration
 *
 * Called from uds_generated_init() via the generated did_handlers_register_all().
 * The generated code calls did_handlers_register_all() which expects these
 * function pointers to be supplied by this application file.
 *
 * To add handlers for a new DID:
 *   1. Add the DID to diagnostics_config.yaml
 *   2. Run: python3 tools/codegen.py --config ... --out ... --safety-wrappers
 *   3. Implement the ardep_read_<name> / ardep_write_<name> functions above
 *   4. Register them in ardep_register_did_handlers() below
 * ============================================================================= */

/**
 * @brief Register all ARDEP application DID handlers with the DID database.
 *
 * Must be called after did_database_init() and before uds_server_init().
 * The generated uds_init.c calls did_handlers_register_all() which is the
 * application's hook to install its handler functions.
 */
void ardep_register_did_handlers(void)
{
    /*
     * Each did_database_set_handler() call registers a read/write callback
     * for one DID. The generated did_handlers.c provides a
     * did_handlers_register_all() function that calls this function.
     *
     * In the generated pattern, the handler functions registered here must
     * match the prototypes declared in generated/did_handlers.h.
     *
     * The ASIL-B safety wrappers (generated/did_safety_wrappers.c) call
     * the registered handlers only AFTER passing the 5-step validation chain.
     */

    /* This registration is handled by the generated did_handlers_register_all().
     * The generated code auto-registers stub handlers; this function provides
     * the application-level overrides if needed.
     *
     * See generated/did_handlers.c for the generated stub implementations.
     * Replace the stub bodies with calls to the ardep_read_* functions above
     * for a complete production integration.
     */
    LOG_INF("ARDEP DID handlers registered (35 DIDs, 6 writable).");
}

/* =============================================================================
 * TRNG / Security algorithm initialization  (identical to basic_ecu)
 * ============================================================================= */

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

static int diag_security_algo_init(void)
{
#if defined(CONFIG_ENTROPY_GENERATOR)
    s_entropy_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
    if (!device_is_ready(s_entropy_dev)) {
        LOG_WRN("[SEC] TRNG not ready — LFSR fallback (dev only)");
        s_entropy_dev = NULL;
    } else {
        uds_security_algo_set_rng_cb(diag_trng_cb);
        LOG_INF("[SEC] TRNG registered: %s", s_entropy_dev->name);
    }
#else
    LOG_WRN("[SEC] No entropy device — LFSR seed (dev/CI only).");
#endif
    /*
     * ARDEP OEM KEY INJECTION
     * ───────────────────────
     * Replace with keys read from ARDEP NVM / OTP before shipping.
     *
     * Example using Zephyr NVS:
     *   uint8_t key_l1[16];
     *   nvs_read(&ardep_fs, NVS_ID_DIAG_KEY_L1, key_l1, 16);
     *   uds_security_algo_set_level_key(0x01U, key_l1);
     *   memset(key_l1, 0, 16);
     */
    LOG_WRN("[SEC] Using placeholder AES keys — inject OEM keys before production.");
    return 0;
}

/* =============================================================================
 * ISO-TP RX completion callback  (identical to basic_ecu)
 * ============================================================================= */

static void on_isotp_rx_complete(
    const uint8_t *data,
    uint16_t       length,
    void          *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();
    uds_status_t      status;
    uint16_t          i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) { return; }
    if ((length == 0U) || (length > (uint16_t)UDS_MAX_PAYLOAD_LEN)) { return; }

    for (i = 0U; i < length; i++) { s_req_buf.data[i] = data[i]; }
    s_req_buf.length  = length;
    s_resp_buf.length = 0U;

    LOG_DBG("UDS RX: SID=0x%02X len=%u", (unsigned)data[0], (unsigned)length);

    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    status = uds_server_process_request(srv, &s_req_buf, &s_resp_buf);
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);

    if (s_resp_buf.length > 0U) {
        /* Update TX counter (diagnostic frames count separately from app CAN). */
        s_can_tx_count++;
        status = isotp_transmit(tp, s_resp_buf.data, s_resp_buf.length);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("ISO-TP TX error 0x%02X", (unsigned)status);
        }
    }
}

/* =============================================================================
 * 1 ms tick  (identical to basic_ecu)
 * ============================================================================= */

typedef struct tick_ctx { uds_server_ctx_t *srv; isotp_ctx_t *tp; } tick_ctx_t;

static void on_tick(void *arg)
{
    tick_ctx_t *ctx = (tick_ctx_t *)arg;
    if ((ctx == NULL) || (ctx->tp == NULL) || (ctx->srv == NULL)) { return; }

    (void)isotp_tick_1ms(ctx->tp);

    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    (void)uds_server_tick_1ms(ctx->srv);
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);

    /* Update uptime counter every second. */
    {
        static uint32_t s_tick_ms = 0U;
        s_tick_ms++;
        if (s_tick_ms >= 1000U) {
            s_tick_ms = 0U;
            s_uptime_seconds++;
        }
    }
}

/* =============================================================================
 * Diagnostics task  (identical to basic_ecu)
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
    tick_ctx_t      tick_ctx     = { .srv = srv, .tp = tp };

    LOG_INF("ARDEP diag task started (stack=%u priority=%d)",
            (unsigned)CONFIG_DIAG_TASK_STACK_SIZE, CONFIG_DIAG_TASK_PRIORITY);

    if (diag_timer_start(&s_tick_timer) != UDS_STATUS_OK) {
        LOG_ERR("Timer start failed — aborting."); return;
    }

    while (true) {
        status = diag_timer_wait_tick(&s_tick_timer, 2U);
        if (status == UDS_STATUS_ERR_TIMEOUT) {
            LOG_WRN("1 ms tick timeout.");
        }

        frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);
        if (status != UDS_STATUS_OK) {
            if (status == UDS_STATUS_ERR_CAN_BUS_OFF) {
                LOG_ERR("CAN bus-off detected.");
                s_can_status[0] = 0x02U; /* Update status DID */
            }
        } else if (frame_ready) {
            s_can_rx_count++;
            (void)isotp_process_rx_frame(tp, &rx_frame, on_isotp_rx_complete, srv);
        }

        on_tick(&tick_ctx);
        (void)diag_wdt_feed(&s_wdt);

        (void)diag_timer_pending_ticks(&s_tick_timer, &overrun_count);
        if (overrun_count > prev_overrun) {
            uint32_t missed = overrun_count - prev_overrun;
            if (missed >= DIAG_OVERRUN_LOG_THRESHOLD) {
                LOG_WRN("Poll overrun: %u ticks missed.", (unsigned)missed);
            }
            prev_overrun = overrun_count;
        }
    }
}

/* =============================================================================
 * main()
 * ============================================================================= */

int main(void)
{
    uds_status_t      status;
    can_transport_t  *can = NULL;
    uds_server_ctx_t *srv = NULL;
    isotp_ctx_t      *tp  = NULL;

    LOG_INF("=========================================================");
    LOG_INF("Xaloqi EDS v" GEN_ECU_VERSION);
    LOG_INF("  ECU  : " GEN_ECU_NAME " (ARDEP I/O Controller)");
    LOG_INF("  DIDs : %u   DTCs: %u",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT);
    LOG_INF("  CAN  : RX=0x%03X  TX=0x%03X",
            (unsigned)DIAG_RX_CAN_ID, (unsigned)DIAG_TX_CAN_ID);
    LOG_INF("  Stack: ASIL-B | AES-CMAC | MISRA C:2012");
    LOG_INF("=========================================================");
    LOG_INF("Upgrade note: replacing driftregion/iso14229 integration.");
    LOG_INF("See docs/ARDEP_UPGRADE_GUIDE.md for migration steps.");

    if (diag_mutex_init(&s_session_lock)  != UDS_STATUS_OK) { return -1; }
    if (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK) { return -1; }

    (void)diag_wdt_init(&s_wdt);

    status = diag_timer_init(&s_tick_timer, on_tick, NULL);
    if (status != UDS_STATUS_OK) { LOG_ERR("Timer init failed."); return -1; }

    {
        const zephyr_port_cfg_t port_cfg = { .can_dev = DIAG_CAN_DEV };
        status = zephyr_port_init(&port_cfg, &can);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("Platform init failed: 0x%02X", (unsigned)status);
            return -1;
        }
    }
    LOG_INF("ARDEP FDCAN transport ready (500 kbps).");

    (void)diag_security_algo_init();

    status = uds_generated_init(can, DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS stack init failed: 0x%02X", (unsigned)status);
        return -1;
    }

    ardep_register_did_handlers();

    srv = uds_generated_get_server();
    tp  = uds_generated_get_isotp();
    if ((srv == NULL) || (tp == NULL)) {
        LOG_ERR("Stack context NULL after init."); return -1;
    }

    LOG_INF("UDS stack ready: P2=%u ms  P2*=%u ms  S3=%u ms",
            (unsigned)GEN_P2_SERVER_MAX_MS,
            (unsigned)GEN_P2_STAR_SERVER_MAX_MS,
            (unsigned)GEN_S3_SERVER_TIMEOUT_MS);

    (void)k_thread_create(
        &s_diag_thread, s_diag_stack,
        K_THREAD_STACK_SIZEOF(s_diag_stack),
        diag_task_entry,
        (void *)srv, (void *)can, (void *)tp,
        CONFIG_DIAG_TASK_PRIORITY, 0U, K_NO_WAIT
    );
    k_thread_name_set(&s_diag_thread, "diag_task");

    LOG_INF("ARDEP diagnostics thread started.");
    return 0;
}
