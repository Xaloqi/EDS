// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/vehicle_state_ecu/src/vehicle_state.h
 *
 * PURPOSE: Application logic for VehicleStateECU — the indicator output
 *          state machine, the two DTC monitors, and the C bodies behind the
 *          RoutineControl actuation path (RID 0xC100 / 0xC101).
 *
 *          This is the *diagnostics layer* a plain vehicle-state demo
 *          application (two inputs -> one visual output) does not have.
 *
 * THREADING: vehicle_state_monitor_tick() is called from diag_task_entry()'s
 *          own loop in src/main.c — the SAME thread that dispatches UDS
 *          requests (including the 0x2E writes to the two input DIDs). This
 *          is deliberate: generated/did_handlers.c's backing stores have no
 *          locking of their own (every shipped example assumes single-
 *          threaded access from the diag task), so calling the generated
 *          did_read_*() accessors from a second thread — the pattern
 *          examples/sensor_ecu uses for its own monitor — would race against
 *          the UDS dispatch thread's did_write_*() calls on the same static
 *          arrays. Folding the monitor into the existing tick avoids that
 *          race without touching generated/did_handlers.c at all.
 * =============================================================================
 */

#ifndef VEHICLE_STATE_H
#define VEHICLE_STATE_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One-time initialisation. Call once from main() before the diag
 *        task starts.
 */
void vehicle_state_init(void);

/**
 * @brief Monitor + indicator tick. Call once every ~100 ms from
 *        diag_task_entry()'s loop (see src/main.c).
 *
 * Reads the two vehicle-state DIDs back through their generated accessors
 * (did_read_vehiclestate_enginespeed_rpm / did_read_vehiclestate_vehiclespeed_kph
 * — the same backing store a tester's 0x2E write lands in), runs the two
 * DTC monitors, and steps the indicator state machine.
 */
void vehicle_state_monitor_tick(void);

/**
 * @brief Release an active IndicatorLampCheck override, if one is active.
 *
 * Call from the UDS session-change callback on transition to the Default
 * session (including on S3server timeout) — a diagnostic client that holds
 * an actuator override and then disappears must not leave the output
 * stuck. See src/main.c's s_on_session_change().
 */
void vehicle_state_release_override(void);

/* --------------------------------------------------------------------------
 * RoutineControl bodies — called from generated/routine_handlers.c's four
 * stub bodies (the one hand-edited "generated/ delta" this example makes;
 * see CMakeLists.txt and the banner at the top of that file).
 * -------------------------------------------------------------------------- */

/** RID 0xC100 startRoutine — begin the asynchronous output self-test walk. */
uds_status_t vehicle_state_selftest_start(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/** RID 0xC100 requestRoutineResults — poll the self-test walk's verdict. */
uds_status_t vehicle_state_selftest_results(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/** RID 0xC101 startRoutine — take over the output, hold the lamp-check pattern. */
uds_status_t vehicle_state_lampcheck_start(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

/** RID 0xC101 stopRoutine — release the override, resume the state machine. */
uds_status_t vehicle_state_lampcheck_stop(
    uint8_t *result_buf, uint8_t result_buf_len, uint8_t *result_len);

#ifdef __cplusplus
}
#endif

#endif /* VEHICLE_STATE_H */
