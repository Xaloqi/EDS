// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu_freertos/src/sensor_monitor_freertos.c
 *
 * PURPOSE: Sensor monitoring task — FreeRTOS port of
 *          examples/sensor_ecu/src/sensor_monitor.c.
 *
 *          Runs as a FreeRTOS task at 100 ms intervals. Reads temperature
 *          and supply voltage from stub sensor functions (same pattern as the
 *          Zephyr native_sim stub), applies threshold logic, and updates DTC
 *          status via dtc_database_set_status().
 *
 * SENSOR ADAPTATION:
 *   On hardware, replace sensor_read_temperature() and sensor_read_voltage()
 *   with your MCU's ADC or I2C sensor read calls. The threshold logic,
 *   DTC update calls, and shared-state mutex are identical to the Zephyr
 *   version — only the sensor driver API differs.
 *
 *   Zephyr → FreeRTOS mapping:
 *     k_mutex_init()       →  xSemaphoreCreateMutexStatic()
 *     k_mutex_lock()       →  xSemaphoreTake()
 *     k_mutex_unlock()     →  xSemaphoreGive()
 *     k_msleep()           →  vTaskDelay(pdMS_TO_TICKS())
 *     k_thread_create()    →  xTaskCreateStatic()
 *     sensor_sample_fetch()→  your_adc_read() / vendor HAL call
 *     sensor_channel_get() →  scale raw counts to engineering units
 *     dtc_database_set_status() → same call (platform-independent)
 *
 * STUB SENSOR BEHAVIOUR (native_sim / QEMU equivalent):
 *   A static counter drives the simulated sensor through a 5-cycle sequence:
 *     Cycles 0–4  : temp = 25 °C, voltage = 12,000 mV  (normal)
 *     Cycles 5–9  : temp = 95 °C, voltage = 12,000 mV  (over-temp fault)
 *     Cycles 10–14: temp = 25 °C, voltage = 7,500 mV   (under-voltage fault)
 *     Cycles 15–19: temp = 25 °C, voltage = 12,000 mV  (recovery)
 *     (then repeats)
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "sensor_ecu.h"
#include "dtc_database.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Task configuration
 * ---------------------------------------------------------------------------*/

#define SENSOR_MONITOR_STACK_WORDS  (512U)
#define SENSOR_MONITOR_PRIORITY     (4U)   /* lower than UDS poll task (5) */
#define SENSOR_MONITOR_INTERVAL_MS  (100U)

/* ---------------------------------------------------------------------------
 * DTC codes — must match diagnostics_config.yaml
 * ---------------------------------------------------------------------------*/

#define DTC_TEMP_HIGH     (0xD00101UL)
#define DTC_TEMP_LOW      (0xD00102UL)
#define DTC_VOLTAGE_HIGH  (0xD00201UL)
#define DTC_VOLTAGE_LOW   (0xD00202UL)

/* ---------------------------------------------------------------------------
 * Shared sensor state + mutex
 * ---------------------------------------------------------------------------*/

static sensor_state_t      s_sensor_state;
static StaticSemaphore_t   s_mutex_buf;
static SemaphoreHandle_t   s_mutex;

/* ---------------------------------------------------------------------------
 * Static task storage (no heap)
 * ---------------------------------------------------------------------------*/

static StackType_t  s_monitor_stack[SENSOR_MONITOR_STACK_WORDS];
static StaticTask_t s_monitor_tcb;

/* ---------------------------------------------------------------------------
 * Stub sensor reads
 *
 * Simulates a cyclic fault pattern identical to the Zephyr native_sim stub.
 * Replace with real sensor driver calls for hardware targets.
 * ---------------------------------------------------------------------------*/

static uint32_t s_stub_cycle = 0U;

static bool sensor_read_temperature(int16_t *out_deg_c)
{
    uint32_t phase = (s_stub_cycle / 5U) % 4U;
    *out_deg_c = (phase == 1U) ? 95 : 25;   /* over-temp on phase 1 */
    return true;
}

static bool sensor_read_voltage(uint16_t *out_mv)
{
    uint32_t phase = (s_stub_cycle / 5U) % 4U;
    *out_mv = (phase == 2U) ? 7500U : 12000U;   /* under-voltage on phase 2 */
    return true;
}

/* ---------------------------------------------------------------------------
 * Internal: threshold check + DTC update (identical logic to Zephyr version)
 * ---------------------------------------------------------------------------*/

static void check_thresholds(sensor_state_t *state)
{
    /* ── Temperature faults ─────────────────────────────────────────────── */
    if ((state->status & SENSOR_STATUS_TEMP_OK) != 0U) {

        if (state->temp_deg_c > (int16_t)state->temp_threshold_high_deg_c) {
            state->status |= SENSOR_STATUS_TEMP_FAULT_HIGH;
            state->status &= ~(uint8_t)SENSOR_STATUS_TEMP_FAULT_LOW;
            (void)dtc_database_set_status(DTC_TEMP_HIGH, true);
            (void)dtc_database_set_status(DTC_TEMP_LOW,  false);

        } else if (state->temp_deg_c < (int16_t)state->temp_threshold_low_deg_c) {
            state->status |= SENSOR_STATUS_TEMP_FAULT_LOW;
            state->status &= ~(uint8_t)SENSOR_STATUS_TEMP_FAULT_HIGH;
            (void)dtc_database_set_status(DTC_TEMP_LOW,  true);
            (void)dtc_database_set_status(DTC_TEMP_HIGH, false);

        } else {
            state->status &= ~(uint8_t)(SENSOR_STATUS_TEMP_FAULT_HIGH |
                                         SENSOR_STATUS_TEMP_FAULT_LOW);
            (void)dtc_database_set_status(DTC_TEMP_HIGH, false);
            (void)dtc_database_set_status(DTC_TEMP_LOW,  false);
        }
    }

    /* ── Voltage faults ─────────────────────────────────────────────────── */
    if ((state->status & SENSOR_STATUS_VOLTAGE_OK) != 0U) {

        if (state->voltage_mv > SENSOR_VOLTAGE_NOMINAL_HIGH_MV) {
            state->status |= SENSOR_STATUS_VOLTAGE_FAULT_HIGH;
            state->status &= ~(uint8_t)SENSOR_STATUS_VOLTAGE_FAULT_LOW;
            (void)dtc_database_set_status(DTC_VOLTAGE_HIGH, true);
            (void)dtc_database_set_status(DTC_VOLTAGE_LOW,  false);

        } else if (state->voltage_mv < SENSOR_VOLTAGE_NOMINAL_LOW_MV) {
            state->status |= SENSOR_STATUS_VOLTAGE_FAULT_LOW;
            state->status &= ~(uint8_t)SENSOR_STATUS_VOLTAGE_FAULT_HIGH;
            (void)dtc_database_set_status(DTC_VOLTAGE_LOW,  true);
            (void)dtc_database_set_status(DTC_VOLTAGE_HIGH, false);

        } else {
            state->status &= ~(uint8_t)(SENSOR_STATUS_VOLTAGE_FAULT_HIGH |
                                         SENSOR_STATUS_VOLTAGE_FAULT_LOW);
            (void)dtc_database_set_status(DTC_VOLTAGE_HIGH, false);
            (void)dtc_database_set_status(DTC_VOLTAGE_LOW,  false);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Sensor monitor task entry
 * ---------------------------------------------------------------------------*/

static void sensor_monitor_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_MONITOR_INTERVAL_MS));
        s_stub_cycle++;

        sensor_state_t local;

        /* Snapshot shared state under mutex */
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
        (void)memcpy(&local, &s_sensor_state, sizeof(sensor_state_t));
        xSemaphoreGive(s_mutex);

        /* Reset valid flags — repopulate on successful reads */
        local.status &= ~(uint8_t)(SENSOR_STATUS_TEMP_OK | SENSOR_STATUS_VOLTAGE_OK);

        int16_t  temp_deg_c = 0;
        uint16_t voltage_mv = 0U;

        if (sensor_read_temperature(&temp_deg_c)) {
            local.temp_deg_c  = temp_deg_c;
            local.status     |= SENSOR_STATUS_TEMP_OK;
        }

        if (sensor_read_voltage(&voltage_mv)) {
            local.voltage_mv  = voltage_mv;
            local.status     |= SENSOR_STATUS_VOLTAGE_OK;
        }

        /* Threshold check — updates DTCs directly */
        check_thresholds(&local);

        /* Write back under mutex */
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
        (void)memcpy(&s_sensor_state, &local, sizeof(sensor_state_t));
        xSemaphoreGive(s_mutex);
    }
}

/* ---------------------------------------------------------------------------
 * Public API (matches sensor_monitor.c interface — caller is identical)
 * ---------------------------------------------------------------------------*/

void sensor_monitor_init(void)
{
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
    configASSERT(s_mutex != NULL);

    /* Initialise shared state with boot defaults */
    (void)memset(&s_sensor_state, 0, sizeof(sensor_state_t));
    s_sensor_state.temp_deg_c                = 25;
    s_sensor_state.voltage_mv                = 12000U;
    s_sensor_state.status                    = 0U;
    s_sensor_state.temp_threshold_high_deg_c = SENSOR_TEMP_THRESHOLD_HIGH_DEFAULT_DEG_C;
    s_sensor_state.temp_threshold_low_deg_c  = SENSOR_TEMP_THRESHOLD_LOW_DEFAULT_DEG_C;

    /* Create sensor monitor task (static storage — no heap) */
    (void)xTaskCreateStatic(
        sensor_monitor_task,
        "sensor_mon",
        SENSOR_MONITOR_STACK_WORDS,
        NULL,
        (UBaseType_t)SENSOR_MONITOR_PRIORITY,
        s_monitor_stack,
        &s_monitor_tcb
    );
}

void sensor_state_get(sensor_state_t *out)
{
    configASSERT(out != NULL);
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    (void)memcpy(out, &s_sensor_state, sizeof(sensor_state_t));
    xSemaphoreGive(s_mutex);
}

void sensor_set_temp_threshold_high(int8_t deg_c)
{
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_sensor_state.temp_threshold_high_deg_c = deg_c;
    xSemaphoreGive(s_mutex);
}

void sensor_set_temp_threshold_low(int8_t deg_c)
{
    (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_sensor_state.temp_threshold_low_deg_c = deg_c;
    xSemaphoreGive(s_mutex);
}
