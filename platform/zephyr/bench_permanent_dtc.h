// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/bench_permanent_dtc.h
 *
 * PURPOSE: [EDS#580] Bench-only: mark one DTC permanent as soon as it is
 *          confirmed, so the behaviour EDS#534 / #537 fixed can be shown on
 *          native_sim or a board: DTC set -> 14 FF FF FF -> still reported
 *          -> reboot -> still reported.
 *
 *          The permanent flag is not persisted (by decision, see
 *          docs/INTEGRATION_GUIDE.md); an application sets it again at boot.
 *          This does exactly that: every call looks at the DTC and, if its
 *          status has the confirmed bit (set live, or restored from NVM at
 *          boot), marks it permanent. Call it from the diag task.
 *
 * On native_sim, EDS_BENCH_NO_PERMANENT=1 in the environment turns the marking
 * off (the control run).
 *
 * NOT FOR PRODUCTION: built only with -DDIAG_BENCH_PERMANENT_DTC=ON. Marking
 * a DTC permanent is the application's decision (sub-function 0x15,
 * reportDTCWithPermanentStatus, lists them); this makes it for you.
 * =============================================================================
 */

#ifndef BENCH_PERMANENT_DTC_H
#define BENCH_PERMANENT_DTC_H

#include <stdint.h>

/** Mark @p dtc_code permanent once it is confirmed. Cheap; call every loop. */
void bench_permanent_dtc_step(uint32_t dtc_code);

#endif /* BENCH_PERMANENT_DTC_H */
