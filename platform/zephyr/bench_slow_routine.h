// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/bench_slow_routine.h
 *
 * PURPOSE: [EDS#282] Bench-only RoutineControl that answers NRC 0x78 and
 *          busy-waits in bounded steps, for measuring the response-pending
 *          path (the 0x78 cadence, P2/P2*) without flash wear.
 *
 *            31 01 <RID_hi> <RID_lo> <N steps> <step ms / 10>
 *
 *          -> 7F 31 78, keepalives, then 71 01 <RID> <N>.
 *          N is 1..255; step is 10..BENCH_SLOW_ROUTINE_STEP_MAX_MS ms.
 *          Anything else is NRC 0x31.
 *
 * NOT FOR PRODUCTION: built only with -DDIAG_BENCH_SLOW_ROUTINE=ON.
 * =============================================================================
 */

#ifndef BENCH_SLOW_ROUTINE_H
#define BENCH_SLOW_ROUTINE_H

#include "uds_types.h"

#ifndef BENCH_SLOW_ROUTINE_RID
#define BENCH_SLOW_ROUTINE_RID (0xF0F0U)
#endif

/** Longest step the routine accepts, and the step_max_ms it declares. */
#ifndef BENCH_SLOW_ROUTINE_STEP_MAX_MS
#define BENCH_SLOW_ROUTINE_STEP_MAX_MS (2000U)
#endif

/**
 * @brief Register the routine (extended session, no security) and make it
 *        deferrable. Call after uds_generated_init().
 */
uds_status_t bench_slow_routine_register(void);

#endif /* BENCH_SLOW_ROUTINE_H */
