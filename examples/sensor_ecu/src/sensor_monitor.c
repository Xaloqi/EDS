/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu/src/sensor_monitor.c
 *
 * PURPOSE: Sensor monitoring thread — reads temperature and voltage sensors
 *          via the Zephyr sensor API every 100 ms, checks thresholds, and
 *          updates DTC status via dtc_database_set_status().
 *
 * SENSOR INTEGRATION PATTERN:
 *
 *   1. Declare sensor device handles using DT_ALIAS.
 *      The board overlay maps physical sensor nodes to well-known aliases:
 *        temp-sensor-0   → on-board temperature sensor
 *        voltage-sensor-0 → supply voltage monitor
 *
 *   2. On each monitor cycle:
 *      a. sensor_sample_fetch()  — trigger a new measurement
 *      b. sensor_channel_get()   — read the measured value
 *      c. Apply threshold logic  — compare to fault limits
 *      d. dtc_database_set_status() — set/clear DTCs based on result
 *
 *   3. The latest readings are stored in s_sensor_state (protected by
 *      s_sensor_lock) and read by DID handlers via sensor_state_get().
 *
 * NATIVE_SIM BEHAVIOUR:
 *   On native_sim, no physical sensor hardware exists. The sensor stub
 *   (boards/native_sim/sensor_stub.c) simulates a sensor that cycles
 *   through: normal → over-temp fault → recovery → under-voltage → recovery.
 *   This lets you watch DTCs activate and clear without any hardware.
 *
 * FREERTOS ADAPTATION NOTE:
 *   On FreeRTOS, replace:
 *     sensor_sample_fetch()      → your_adc_read() or vendor HAL call
 *     sensor_channel_get()       → scale raw ADC counts to engineering units
 *     dtc_database_set_status()  → same call (platform-independent)
 *   The threshold logic and DTC update calls are identical on both platforms.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "sensor_ecu.h"
#include "dtc_database.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor_monitor, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * Sensor monitor thread configuration
 * ---------------------------------------------------------------------------*/

#define SENSOR_MONITOR_STACK_SIZE   (1024U)
#define SENSOR_MONITOR_PRIORITY     (7)        /* lower priority than diag task */
#define SENSOR_MONITOR_INTERVAL_MS  (100U)     /* sample every 100 ms */

K_THREAD_STACK_DEFINE(s_monitor_stack, SENSOR_MONITOR_STACK_SIZE);
static struct k_thread s_monitor_thread;

/* ---------------------------------------------------------------------------
 * Sensor device handles
 *
 * These are resolved from Device Tree aliases defined in the board overlay.
 * On native_sim the aliases point to stub sensor nodes (sensor_stub.c).
 * On real hardware they point to the physical sensor driver nodes.
 *
 * DT_ALIAS resolves at compile time — if the alias is absent from the DTS,
 * the build fails with a clear error (missing alias 'temp-sensor-0').
 * ---------------------------------------------------------------------------*/

static const struct device *s_temp_dev    = NULL;
static const struct device *s_voltage_dev = NULL;

/* ---------------------------------------------------------------------------
 * Shared sensor state + mutex
 * ---------------------------------------------------------------------------*/

static sensor_state_t s_sensor_state;
static struct k_mutex s_sensor_lock;

/* ---------------------------------------------------------------------------
 * DTC codes — must match diagnostics_config.yaml
 * ---------------------------------------------------------------------------*/

#define DTC_TEMP_HIGH     (0xD00101UL)
#define DTC_TEMP_LOW      (0xD00102UL)
#define DTC_VOLTAGE_HIGH  (0xD00201UL)
#define DTC_VOLTAGE_LOW   (0xD00202UL)

/* ---------------------------------------------------------------------------
 * Internal: read temperature from Zephyr sensor driver
 * Returns true on success, false if sensor unavailable.
 * ---------------------------------------------------------------------------*/

static bool read_temperature(int16_t *out_deg_c)
{
    struct sensor_value val;
    int rc;

    if ((s_temp_dev == NULL) || !device_is_ready(s_temp_dev)) {
        return false;
    }

    rc = sensor_sample_fetch(s_temp_dev);
    if (rc != 0) {
        LOG_WRN("Temperature fetch failed: %d", rc);
        return false;
    }

    rc = sensor_channel_get(s_temp_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
    if (rc != 0) {
        LOG_WRN("Temperature channel read failed: %d", rc);
        return false;
    }

    /* sensor_value: val1 = integer part (°C), val2 = fractional (millionths) */
    *out_deg_c = (int16_t)val.val1;
    return true;
}

/* ---------------------------------------------------------------------------
 * Internal: read supply voltage from Zephyr sensor driver
 * Returns true on success, false if sensor unavailable.
 * ---------------------------------------------------------------------------*/

static bool read_voltage(uint16_t *out_mv)
{
    struct sensor_value val;
    int rc;

    if ((s_voltage_dev == NULL) || !device_is_ready(s_voltage_dev)) {
        return false;
    }

    rc = sensor_sample_fetch(s_voltage_dev);
    if (rc != 0) {
        LOG_WRN("Voltage fetch failed: %d", rc);
        return false;
    }

    rc = sensor_channel_get(s_voltage_dev, SENSOR_CHAN_VOLTAGE, &val);
    if (rc != 0) {
        LOG_WRN("Voltage channel read failed: %d", rc);
        return false;
    }

    /*
     * Zephyr voltage channel returns volts.
     * val1 = integer volts, val2 = fractional part in millionths.
     * Convert to mV: (val1 * 1000) + (val2 / 1000).
     */
    int32_t mv = (val.val1 * 1000) + (val.val2 / 1000);
    *out_mv = (uint16_t)((mv < 0) ? 0 : (uint32_t)mv);
    return true;
}

/* ---------------------------------------------------------------------------
 * Internal: check thresholds and update DTC status
 * ---------------------------------------------------------------------------*/

static void check_thresholds(sensor_state_t *state)
{
    /* ── Temperature faults ─────────────────────────────────────────────── */
    if (state->status & SENSOR_STATUS_TEMP_OK) {

        if (state->temp_deg_c > (int16_t)state->temp_threshold_high_deg_c) {
            state->status |= SENSOR_STATUS_TEMP_FAULT_HIGH;
            state->status &= ~(uint8_t)SENSOR_STATUS_TEMP_FAULT_LOW;
            (void)dtc_database_set_status(DTC_TEMP_HIGH, true);
            (void)dtc_database_set_status(DTC_TEMP_LOW,  false);
            LOG_WRN("TEMP HIGH fault: %d °C > %d °C threshold",
                    (int)state->temp_deg_c,
                    (int)state->temp_threshold_high_deg_c);

        } else if (state->temp_deg_c < (int16_t)state->temp_threshold_low_deg_c) {
            state->status |= SENSOR_STATUS_TEMP_FAULT_LOW;
            state->status &= ~(uint8_t)SENSOR_STATUS_TEMP_FAULT_HIGH;
            (void)dtc_database_set_status(DTC_TEMP_LOW,  true);
            (void)dtc_database_set_status(DTC_TEMP_HIGH, false);
            LOG_WRN("TEMP LOW fault: %d °C < %d °C threshold",
                    (int)state->temp_deg_c,
                    (int)state->temp_threshold_low_deg_c);

        } else {
            /* In-range — clear both temp faults */
            state->status &= ~(uint8_t)(SENSOR_STATUS_TEMP_FAULT_HIGH |
                                         SENSOR_STATUS_TEMP_FAULT_LOW);
            (void)dtc_database_set_status(DTC_TEMP_HIGH, false);
            (void)dtc_database_set_status(DTC_TEMP_LOW,  false);
        }
    }

    /* ── Voltage faults ─────────────────────────────────────────────────── */
    if (state->status & SENSOR_STATUS_VOLTAGE_OK) {

        if (state->voltage_mv > SENSOR_VOLTAGE_NOMINAL_HIGH_MV) {
            state->status |= SENSOR_STATUS_VOLTAGE_FAULT_HIGH;
            state->status &= ~(uint8_t)SENSOR_STATUS_VOLTAGE_FAULT_LOW;
            (void)dtc_database_set_status(DTC_VOLTAGE_HIGH, true);
            (void)dtc_database_set_status(DTC_VOLTAGE_LOW,  false);
            LOG_WRN("VOLTAGE HIGH fault: %u mV > %u mV",
                    (unsigned)state->voltage_mv,
                    (unsigned)SENSOR_VOLTAGE_NOMINAL_HIGH_MV);

        } else if (state->voltage_mv < SENSOR_VOLTAGE_NOMINAL_LOW_MV) {
            state->status |= SENSOR_STATUS_VOLTAGE_FAULT_LOW;
            state->status &= ~(uint8_t)SENSOR_STATUS_VOLTAGE_FAULT_HIGH;
            (void)dtc_database_set_status(DTC_VOLTAGE_LOW,  true);
            (void)dtc_database_set_status(DTC_VOLTAGE_HIGH, false);
            LOG_WRN("VOLTAGE LOW fault: %u mV < %u mV",
                    (unsigned)state->voltage_mv,
                    (unsigned)SENSOR_VOLTAGE_NOMINAL_LOW_MV);

        } else {
            /* In-range — clear both voltage faults */
            state->status &= ~(uint8_t)(SENSOR_STATUS_VOLTAGE_FAULT_HIGH |
                                         SENSOR_STATUS_VOLTAGE_FAULT_LOW);
            (void)dtc_database_set_status(DTC_VOLTAGE_HIGH, false);
            (void)dtc_database_set_status(DTC_VOLTAGE_LOW,  false);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Sensor monitor thread entry
 * ---------------------------------------------------------------------------*/

static void sensor_monitor_entry(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;

    LOG_INF("Sensor monitor started (%u ms interval).",
            (unsigned)SENSOR_MONITOR_INTERVAL_MS);

    while (true) {
        k_msleep(SENSOR_MONITOR_INTERVAL_MS);

        sensor_state_t local;
        k_mutex_lock(&s_sensor_lock, K_FOREVER);
        local = s_sensor_state;
        k_mutex_unlock(&s_sensor_lock);

        /* Reset valid flags — set them back on successful reads */
        local.status &= ~(uint8_t)(SENSOR_STATUS_TEMP_OK | SENSOR_STATUS_VOLTAGE_OK);

        /* Read temperature */
        int16_t temp_deg_c = 0;
        if (read_temperature(&temp_deg_c)) {
            local.temp_deg_c = temp_deg_c;
            local.status |= SENSOR_STATUS_TEMP_OK;
            LOG_DBG("Temperature: %d °C", (int)temp_deg_c);
        } else {
            LOG_WRN("Temperature sensor unavailable.");
        }

        /* Read voltage */
        uint16_t voltage_mv = 0U;
        if (read_voltage(&voltage_mv)) {
            local.voltage_mv = voltage_mv;
            local.status |= SENSOR_STATUS_VOLTAGE_OK;
            LOG_DBG("Voltage: %u mV", (unsigned)voltage_mv);
        } else {
            LOG_WRN("Voltage sensor unavailable.");
        }

        /* Threshold check and DTC update */
        check_thresholds(&local);

        /* Write back under lock */
        k_mutex_lock(&s_sensor_lock, K_FOREVER);
        s_sensor_state = local;
        k_mutex_unlock(&s_sensor_lock);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------*/

void sensor_monitor_init(void)
{
    /* Initialise shared state with defaults */
    k_mutex_init(&s_sensor_lock);

    s_sensor_state.temp_deg_c               = 25;   /* assume room temp at boot */
    s_sensor_state.voltage_mv               = 12000U; /* assume nominal 12 V */
    s_sensor_state.status                   = 0U;
    s_sensor_state.temp_threshold_high_deg_c = SENSOR_TEMP_THRESHOLD_HIGH_DEFAULT_DEG_C;
    s_sensor_state.temp_threshold_low_deg_c  = SENSOR_TEMP_THRESHOLD_LOW_DEFAULT_DEG_C;

    /* Resolve sensor devices from Device Tree aliases */
#if DT_HAS_ALIAS(temp_sensor_0)
    s_temp_dev = DEVICE_DT_GET(DT_ALIAS(temp_sensor_0));
    if (!device_is_ready(s_temp_dev)) {
        LOG_WRN("Temperature sensor not ready: %s", s_temp_dev->name);
        s_temp_dev = NULL;
    } else {
        LOG_INF("Temperature sensor: %s", s_temp_dev->name);
    }
#else
    LOG_WRN("No 'temp-sensor-0' alias in DTS — temperature sensor disabled.");
#endif

#if DT_HAS_ALIAS(voltage_sensor_0)
    s_voltage_dev = DEVICE_DT_GET(DT_ALIAS(voltage_sensor_0));
    if (!device_is_ready(s_voltage_dev)) {
        LOG_WRN("Voltage sensor not ready: %s", s_voltage_dev->name);
        s_voltage_dev = NULL;
    } else {
        LOG_INF("Voltage sensor: %s", s_voltage_dev->name);
    }
#else
    LOG_WRN("No 'voltage-sensor-0' alias in DTS — voltage sensor disabled.");
#endif

    /* Start monitor thread */
    k_thread_create(
        &s_monitor_thread,
        s_monitor_stack,
        K_THREAD_STACK_SIZEOF(s_monitor_stack),
        sensor_monitor_entry,
        NULL, NULL, NULL,
        SENSOR_MONITOR_PRIORITY,
        0U,
        K_NO_WAIT
    );
    k_thread_name_set(&s_monitor_thread, "sensor_monitor");
}

void sensor_state_get(sensor_state_t *out)
{
    k_mutex_lock(&s_sensor_lock, K_FOREVER);
    *out = s_sensor_state;
    k_mutex_unlock(&s_sensor_lock);
}

void sensor_set_temp_threshold_high(int8_t deg_c)
{
    k_mutex_lock(&s_sensor_lock, K_FOREVER);
    s_sensor_state.temp_threshold_high_deg_c = deg_c;
    k_mutex_unlock(&s_sensor_lock);
    LOG_INF("Temp threshold high set to %d °C", (int)deg_c);
}

void sensor_set_temp_threshold_low(int8_t deg_c)
{
    k_mutex_lock(&s_sensor_lock, K_FOREVER);
    s_sensor_state.temp_threshold_low_deg_c = deg_c;
    k_mutex_unlock(&s_sensor_lock);
    LOG_INF("Temp threshold low set to %d °C", (int)deg_c);
}
