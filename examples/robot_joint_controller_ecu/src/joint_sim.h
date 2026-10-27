/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/robot_joint_controller_ecu/src/joint_sim.h
 *
 * PURPOSE: Simulated robot joint behind the five live Axis 0 DIDs.
 *
 *          The DIDs 0xA000-0xA004 are declared "source: app" in
 *          diagnostics_config.yaml, so codegen emits no handler body for them.
 *          This module defines did_read_axis0position() and its four
 *          siblings (declared in generated/did_handlers.h) and sets the
 *          over-temperature DTC.
 *
 *          Everything is a pure function of the time since boot, so a run is
 *          reproducible. There is no physical joint, motor or encoder.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#ifndef JOINT_SIM_H
#define JOINT_SIM_H

#include <stdint.h>

/* Sweep: triangle wave between -/+ JOINT_SIM_SWEEP_AMPL_MDEG, one full cycle
 * (out and back) per JOINT_SIM_SWEEP_PERIOD_MS. Inside the soft limits the
 * YAML gives 0xA011/0xA012 (+/-170 degrees). */
#define JOINT_SIM_SWEEP_AMPL_MDEG     (90000)
#define JOINT_SIM_SWEEP_PERIOD_MS     (20000U)

/* Motor temperature: JOINT_SIM_TEMP_START_C at boot, +1 degC per second,
 * saturating at JOINT_SIM_TEMP_MAX_C. The fault threshold is the YAML's 85 degC
 * ("above this"), first exceeded at 86 degC, 56 s after boot. */
#define JOINT_SIM_TEMP_START_C        (30)
#define JOINT_SIM_TEMP_MAX_C          (100)
#define JOINT_SIM_TEMP_FAULT_C        (85)
#define JOINT_SIM_TEMP_FAULT_AT_MS    (56000U)

/** DTC 0xA00101, Axis 0 motor over-temperature (diagnostics_config.yaml). */
#define JOINT_SIM_DTC_OVERTEMP        (0xA00101UL)

/**
 * @brief Advance the simulation to now_ms and update the DTC.
 *
 * Call from the thread that also runs the UDS dispatch (diag_task): the DID
 * readers and this function then never run concurrently, and no lock is
 * needed. The first call fixes the time origin.
 *
 * @param[in] now_ms  Monotonic milliseconds (wraps at 2^32, handled).
 */
void joint_sim_step(uint32_t now_ms);

#endif /* JOINT_SIM_H */
