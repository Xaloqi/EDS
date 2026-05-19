/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu/src/sensor_ecu.h
 *
 * PURPOSE: Shared definitions for the SensorECU example.
 *          Physical encoding constants, threshold defaults, and sensor
 *          status bitmask bit definitions used across main.c,
 *          did_handlers_impl.c, and sensor_monitor.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#ifndef SENSOR_ECU_H
#define SENSOR_ECU_H

#include <stdint.h>
#include <stdbool.h>
#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Temperature encoding
 *
 * ISO 14229 / SAE J1939 offset encoding: raw_byte = T_degC + 40
 * This allows signed temperatures from −40 °C (raw 0x00) to +215 °C (raw 0xFF)
 * to be transmitted as an unsigned uint8.
 * ---------------------------------------------------------------------------*/

#define SENSOR_TEMP_OFFSET_DEG_C        (40)
#define SENSOR_TEMP_ENCODE(t_degc)      ((uint8_t)((int16_t)(t_degc) + SENSOR_TEMP_OFFSET_DEG_C))
#define SENSOR_TEMP_DECODE(raw)         ((int16_t)(raw) - SENSOR_TEMP_OFFSET_DEG_C)

/** Default upper fault threshold: +85 °C (maximum automotive operating temp) */
#define SENSOR_TEMP_THRESHOLD_HIGH_DEFAULT_DEG_C    (85)

/** Default lower fault threshold: −40 °C (minimum automotive operating temp) */
#define SENSOR_TEMP_THRESHOLD_LOW_DEFAULT_DEG_C    (-40)

/* ---------------------------------------------------------------------------
 * Voltage encoding
 *
 * uint16 big-endian, LSB = 1 mV. Nominal ECU supply: 9–16 V.
 * ---------------------------------------------------------------------------*/

#define SENSOR_VOLTAGE_NOMINAL_LOW_MV   (9000U)    /*  9.0 V */
#define SENSOR_VOLTAGE_NOMINAL_HIGH_MV  (16000U)   /* 16.0 V */

/* ---------------------------------------------------------------------------
 * Sensor status bitmask (DID 0xD003)
 * ---------------------------------------------------------------------------*/

/** Bit 0: temperature sensor reading is valid (sensor present and responding) */
#define SENSOR_STATUS_TEMP_OK           (1U << 0U)

/** Bit 1: voltage sensor reading is valid */
#define SENSOR_STATUS_VOLTAGE_OK        (1U << 1U)

/** Bit 2: temperature reading exceeds upper threshold */
#define SENSOR_STATUS_TEMP_FAULT_HIGH   (1U << 2U)

/** Bit 3: temperature reading is below lower threshold */
#define SENSOR_STATUS_TEMP_FAULT_LOW    (1U << 3U)

/** Bit 4: supply voltage exceeds upper threshold */
#define SENSOR_STATUS_VOLTAGE_FAULT_HIGH (1U << 4U)

/** Bit 5: supply voltage is below lower threshold */
#define SENSOR_STATUS_VOLTAGE_FAULT_LOW  (1U << 5U)

/* ---------------------------------------------------------------------------
 * Shared sensor state — written by sensor_monitor thread,
 * read by DID handler callbacks.
 * Protected by sensor_state_lock (k_mutex in sensor_monitor.c).
 * ---------------------------------------------------------------------------*/

typedef struct {
    int16_t  temp_deg_c;        /**< Latest temperature reading, °C */
    uint16_t voltage_mv;        /**< Latest supply voltage reading, mV */
    uint8_t  status;            /**< Sensor status bitmask (SENSOR_STATUS_*) */

    /* NVM-backed thresholds (defaults set at init, writable via DID 0xD010/11) */
    int8_t   temp_threshold_high_deg_c;
    int8_t   temp_threshold_low_deg_c;
} sensor_state_t;

/* ---------------------------------------------------------------------------
 * API — implemented in sensor_monitor.c
 * ---------------------------------------------------------------------------*/

/**
 * @brief Initialise the sensor state and start the monitor thread.
 *        Call once from main() before uds_generated_init().
 */
void sensor_monitor_init(void);

/**
 * @brief Read a snapshot of the current sensor state.
 *        Thread-safe (acquires internal mutex).
 */
void sensor_state_get(sensor_state_t *out);

/**
 * @brief Update a threshold value (from DID write handler).
 *        Thread-safe. Changes take effect on the next monitor cycle.
 */
void sensor_set_temp_threshold_high(int8_t deg_c);
void sensor_set_temp_threshold_low(int8_t deg_c);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_ECU_H */
