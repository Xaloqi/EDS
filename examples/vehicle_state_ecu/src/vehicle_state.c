// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/vehicle_state_ecu/src/vehicle_state.c
 *
 * PURPOSE: Indicator state machine, DTC monitors, and RoutineControl bodies
 *          for VehicleStateECU. See vehicle_state.h for the threading note —
 *          everything in this file runs on the diag task thread only.
 *
 * INDICATOR STATES (stand in for the LED strip a plain vehicle-state demo
 * application would drive directly):
 *   0x00 IDLE    — engine 0 rpm, speed 0
 *   0x01 RUNNING — engine > 0 rpm, speed <= 5.0 km/h
 *   0x02 CRUISE  — speed > 5.0 km/h
 *   0x03 WARN    — either DTC monitor below is currently asserting a fault
 *   0x04 LAMPCHECK — IndicatorLampCheck (RID 0xC101) override is active;
 *                    not a state the ordinary state machine ever produces.
 *
 * DTC TRIGGERS (both evaluated every monitor tick, independently of any
 * active routine — a routine overrides what the output *shows*, never
 * whether a fault is detected):
 *
 *   U0100-00 "lost communication with engine control module" —
 *     engine_rpm pinned above VSE_ENGINE_RPM_IMPLAUSIBLE_MAX for
 *     VSE_DEBOUNCE_TICKS_U0100 consecutive ticks. Modelled as an
 *     out-of-range/pinned value rather than "value hasn't changed": this
 *     ECU's two inputs are tester-injected (there is no vehicle bus on
 *     native_sim), so an injected value holding steady is the NORMAL
 *     resting state between writes, not a fault signature — unlike
 *     examples/sensor_ecu, which has a real background sensor cycling a
 *     fresh value every tick and can legitimately treat "stopped changing"
 *     as "lost comms". Sustained out-of-range is the fault model that
 *     actually fits a write-injected signal.
 *
 *   P0500-64 "vehicle speed signal implausible against engine speed" —
 *     speed > VSE_SPEED_MOVING_KPH_X10_MIN (5.0 km/h) while engine_rpm == 0,
 *     for VSE_DEBOUNCE_TICKS_P0500 consecutive ticks. A genuinely
 *     cross-signal check: moving with the engine reading exactly stopped.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "vehicle_state.h"

#include "did_handlers.h"      /* generated — did_read_vehiclestate_*() accessors */
#include "dtc_database.h"
#include "uds_types.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(vehicle_state, LOG_LEVEL_INF);

/* =============================================================================
 * Tunables
 * ============================================================================= */

/* Monitor tick cadence: vehicle_state_monitor_tick() is called once every
 * this many diag_task loop iterations. The loop iterates at ~1 ms (driven by
 * the 1 ms tick timer in src/main.c's on_tick()), so 100 ticks ~= 100 ms. */
#define VSE_MONITOR_TICK_DIVISOR        (100U)

#define VSE_ENGINE_RPM_IMPLAUSIBLE_MAX  (8000U)  /* above this: pinned/stuck */
#define VSE_SPEED_MOVING_KPH_X10_MIN    (50U)    /* 5.0 km/h, in 0.1 km/h units */

#define VSE_DEBOUNCE_TICKS_U0100        (5U)     /* ~500 ms at 100 ms/tick */
#define VSE_DEBOUNCE_TICKS_P0500        (10U)    /* ~1000 ms at 100 ms/tick */

#define VSE_DTC_U0100                   (0xC10000UL)
#define VSE_DTC_P0500                   (0x050064UL)

/* Indicator state byte values — also the wire-visible routine result bytes. */
#define VSE_STATE_IDLE                  (0x00U)
#define VSE_STATE_RUNNING                (0x01U)
#define VSE_STATE_CRUISE                 (0x02U)
#define VSE_STATE_WARN                   (0x03U)
#define VSE_STATE_LAMPCHECK              (0x04U)

#define VSE_SELFTEST_ACK_STARTED         (0x01U)  /* startRoutine response byte */
#define VSE_SELFTEST_RESULT_PASS         (0x00U)  /* requestResults byte 0 */
#define VSE_SELFTEST_RESULT_FAIL         (0x01U)
#define VSE_SELFTEST_RESULT_IN_PROGRESS  (0x02U)

#define VSE_SELFTEST_PATTERN_COUNT       (4U)     /* walks IDLE..WARN, one per tick */

/* =============================================================================
 * State — single-threaded (diag task only, see vehicle_state.h)
 * ============================================================================= */

static uint8_t  s_indicator_state;       /* last computed/reported state byte */
static bool     s_lampcheck_override;    /* RID 0xC101 override active */

static uint8_t  s_u0100_debounce;        /* consecutive ticks condition held */
static uint8_t  s_p0500_debounce;
static uint8_t  s_u0100_heal_debounce;
static uint8_t  s_p0500_heal_debounce;
static bool     s_u0100_active;
static bool     s_p0500_active;

static bool     s_selftest_active;
static uint8_t  s_selftest_step;         /* 0..VSE_SELFTEST_PATTERN_COUNT */
static uint8_t  s_selftest_patterns_tested; /* bitmask, bit per pattern walked */
static uint8_t  s_selftest_result;       /* valid once !s_selftest_active */

/* =============================================================================
 * Helpers
 * ============================================================================= */

static uint16_t vse_read_be16(did_read_cb_fn read_fn)
{
    uint8_t  buf[2];
    uint16_t out_len = 0U;

    if (read_fn(buf, (uint16_t)sizeof(buf), &out_len) != UDS_STATUS_OK) {
        return 0U;
    }
    if (out_len != (uint16_t)sizeof(buf)) {
        return 0U;
    }
    return (uint16_t)(((uint16_t)buf[0] << 8U) | (uint16_t)buf[1]);
}

static uint8_t vse_compute_state(uint16_t engine_rpm, uint16_t speed_kph_x10,
                                  bool any_dtc_active)
{
    if (any_dtc_active) {
        return VSE_STATE_WARN;
    }
    if (speed_kph_x10 > (uint16_t)VSE_SPEED_MOVING_KPH_X10_MIN) {
        return VSE_STATE_CRUISE;
    }
    if (engine_rpm > 0U) {
        return VSE_STATE_RUNNING;
    }
    return VSE_STATE_IDLE;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

void vehicle_state_init(void)
{
    s_indicator_state         = VSE_STATE_IDLE;
    s_lampcheck_override       = false;
    s_u0100_debounce            = 0U;
    s_p0500_debounce            = 0U;
    s_u0100_heal_debounce       = 0U;
    s_p0500_heal_debounce       = 0U;
    s_u0100_active              = false;
    s_p0500_active              = false;
    s_selftest_active            = false;
    s_selftest_step              = 0U;
    s_selftest_patterns_tested   = 0U;
    s_selftest_result            = VSE_SELFTEST_RESULT_PASS;
}

void vehicle_state_monitor_tick(void)
{
    static uint32_t s_divisor_count;
    uint16_t engine_rpm;
    uint16_t speed_kph_x10;
    bool     any_dtc_active;
    uint8_t  new_state;

    s_divisor_count++;
    if ((s_divisor_count % VSE_MONITOR_TICK_DIVISOR) != 0U) {
        return;
    }

    engine_rpm    = vse_read_be16(did_read_vehiclestate_enginespeed_rpm);
    speed_kph_x10 = vse_read_be16(did_read_vehiclestate_vehiclespeed_kph);

    /* ── U0100-00: engine signal pinned above the plausible range ──────── */
    if (engine_rpm > (uint16_t)VSE_ENGINE_RPM_IMPLAUSIBLE_MAX) {
        s_u0100_heal_debounce = 0U;
        if (s_u0100_debounce < (uint8_t)VSE_DEBOUNCE_TICKS_U0100) {
            s_u0100_debounce++;
        }
        if ((!s_u0100_active) && (s_u0100_debounce >= (uint8_t)VSE_DEBOUNCE_TICKS_U0100)) {
            s_u0100_active = true;
            LOG_WRN("DTC U0100-00 set: engine_rpm=%u exceeds plausible max", engine_rpm);
            (void)dtc_database_set_status(VSE_DTC_U0100,
                (uint8_t)(DTC_STATUS_TEST_FAILED |
                          DTC_STATUS_TEST_FAILED_THIS_OPERATION_CYCLE |
                          DTC_STATUS_CONFIRMED_DTC));
        }
    } else {
        s_u0100_debounce = 0U;
        if (s_u0100_heal_debounce < (uint8_t)VSE_DEBOUNCE_TICKS_U0100) {
            s_u0100_heal_debounce++;
        }
        if (s_u0100_active && (s_u0100_heal_debounce >= (uint8_t)VSE_DEBOUNCE_TICKS_U0100)) {
            s_u0100_active = false;
            LOG_INF("DTC U0100-00 cleared: engine_rpm=%u back in range", engine_rpm);
            (void)dtc_database_set_status(VSE_DTC_U0100, (uint8_t)0x00U);
        }
    }
    (void)dtc_database_set_fault_counter(VSE_DTC_U0100, s_u0100_debounce);

    /* ── P0500-64: moving with the engine reading exactly stopped ──────── */
    if ((speed_kph_x10 > (uint16_t)VSE_SPEED_MOVING_KPH_X10_MIN) && (engine_rpm == 0U)) {
        s_p0500_heal_debounce = 0U;
        if (s_p0500_debounce < (uint8_t)VSE_DEBOUNCE_TICKS_P0500) {
            s_p0500_debounce++;
        }
        if ((!s_p0500_active) && (s_p0500_debounce >= (uint8_t)VSE_DEBOUNCE_TICKS_P0500)) {
            s_p0500_active = true;
            LOG_WRN("DTC P0500-64 set: speed=%u (0.1 km/h) with engine_rpm=0", speed_kph_x10);
            (void)dtc_database_set_status(VSE_DTC_P0500,
                (uint8_t)(DTC_STATUS_TEST_FAILED |
                          DTC_STATUS_TEST_FAILED_THIS_OPERATION_CYCLE |
                          DTC_STATUS_CONFIRMED_DTC |
                          DTC_STATUS_WARNING_INDICATOR_REQUESTED));
        }
    } else {
        s_p0500_debounce = 0U;
        if (s_p0500_heal_debounce < (uint8_t)VSE_DEBOUNCE_TICKS_P0500) {
            s_p0500_heal_debounce++;
        }
        if (s_p0500_active && (s_p0500_heal_debounce >= (uint8_t)VSE_DEBOUNCE_TICKS_P0500)) {
            s_p0500_active = false;
            LOG_INF("DTC P0500-64 cleared: plausible again");
            (void)dtc_database_set_status(VSE_DTC_P0500, (uint8_t)0x00U);
        }
    }
    (void)dtc_database_set_fault_counter(VSE_DTC_P0500, s_p0500_debounce);

    any_dtc_active = s_u0100_active || s_p0500_active;

    /* ── Self-test walk: steps the output through all patterns regardless
     * of what the ordinary state machine would currently show. ────────── */
    if (s_selftest_active) {
        s_selftest_patterns_tested |= (uint8_t)(1U << s_selftest_step);
        /* native_sim: no physical driver feedback exists; synthesised as
         * always-healthy. On real hardware, read the actuator driver's
         * fault/feedback line here and fold a failure into s_selftest_result. */
        s_selftest_step++;
        if (s_selftest_step >= (uint8_t)VSE_SELFTEST_PATTERN_COUNT) {
            s_selftest_active = false;
            s_selftest_result = VSE_SELFTEST_RESULT_PASS;
            LOG_INF("Self-test walk complete: patterns_tested=0x%02X result=PASS",
                    s_selftest_patterns_tested);
        }
        new_state = s_selftest_step % (uint8_t)VSE_SELFTEST_PATTERN_COUNT;
    } else if (s_lampcheck_override) {
        new_state = VSE_STATE_LAMPCHECK;
    } else {
        new_state = vse_compute_state(engine_rpm, speed_kph_x10, any_dtc_active);
    }

    if (new_state != s_indicator_state) {
        LOG_INF("Indicator: 0x%02X -> 0x%02X (engine=%u rpm, speed=%u.%u km/h)",
                s_indicator_state, new_state,
                engine_rpm, (unsigned)(speed_kph_x10 / 10U), (unsigned)(speed_kph_x10 % 10U));
        s_indicator_state = new_state;
    }
}

void vehicle_state_release_override(void)
{
    if (s_lampcheck_override) {
        LOG_INF("IndicatorLampCheck override released (session returned to Default)");
        s_lampcheck_override = false;
    }
}

uds_status_t vehicle_state_selftest_start(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    s_selftest_active          = true;
    s_selftest_step             = 0U;
    s_selftest_patterns_tested  = 0U;
    s_selftest_result           = VSE_SELFTEST_RESULT_IN_PROGRESS;

    result_buf[0] = (uint8_t)VSE_SELFTEST_ACK_STARTED;
    *result_len   = 1U;
    return UDS_STATUS_OK;
}

uds_status_t vehicle_state_selftest_results(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)3U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    result_buf[0] = s_selftest_active ? (uint8_t)VSE_SELFTEST_RESULT_IN_PROGRESS
                                       : s_selftest_result;
    result_buf[1] = s_selftest_patterns_tested;
    result_buf[2] = s_indicator_state;
    *result_len   = 3U;
    return UDS_STATUS_OK;
}

uds_status_t vehicle_state_lampcheck_start(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    s_lampcheck_override = true;
    LOG_INF("IndicatorLampCheck: override engaged, holding pattern 0x%02X",
            (unsigned)VSE_STATE_LAMPCHECK);

    result_buf[0] = (uint8_t)VSE_STATE_LAMPCHECK;
    *result_len   = 1U;
    return UDS_STATUS_OK;
}

uds_status_t vehicle_state_lampcheck_stop(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len)
{
    if ((result_buf == NULL) || (result_len == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (result_buf_len < (uint8_t)1U) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    vehicle_state_release_override();

    result_buf[0] = s_indicator_state;
    *result_len   = 1U;
    return UDS_STATUS_OK;
}
