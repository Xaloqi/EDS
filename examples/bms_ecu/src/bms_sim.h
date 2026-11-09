// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/bms_ecu/src/bms_sim.h
 *
 * PURPOSE: Simulated battery pack behind the live DIDs of the BMS example.
 *
 *          The DIDs 0xD900/0xD901, 0xD910/0xD911, 0xDB00-0xDB04, 0xDC01-0xDC04,
 *          0xDD00-0xDD03 and 0xDE00-0xDE02 are declared "source: app" in
 *          diagnostics_config.yaml, so codegen emits no handler body for them.
 *          This module defines their did_read_<name>() / did_write_<name>()
 *          functions (declared in generated/did_handlers.h). The identity DIDs
 *          (0xF1xx) keep their generated default values.
 *
 *          The values are fixed at boot (an idle 50 % SoC pack at 25 degC); the
 *          two thresholds can be written over UDS. There is no cell monitor,
 *          current sensor or contactor behind them: replace the reads with your
 *          measurement layer.
 *
 *          Everything runs on the diag task thread, so no lock is needed. If
 *          you add measurement threads that update the state, guard it.
 * =============================================================================
 */

#ifndef BMS_SIM_H
#define BMS_SIM_H

/** Set the boot values. Call once before the UDS server starts. */
void bms_sim_init(void);

#endif /* BMS_SIM_H */
