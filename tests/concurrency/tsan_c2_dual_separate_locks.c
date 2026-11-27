// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test (ThreadSanitizer)
 * FILE: tests/concurrency/tsan_c2_dual_separate_locks.c
 * MODULE UNDER TEST: core/uds_server.c with two producers
 *
 * PURPOSE: [EDS#197] Scenario C2: a CAN and a DoIP producer, each on its own
 *   lock (dual_producers.h). This is the #197 race, and the fail-first test
 *   for the executor theme.
 *
 *   TSAN_EXPECT: XFAIL
 *   run_tsan.sh reads the marker above. Today every run must show both
 *   data races > 0 AND request_count < expected (lost updates in the
 *   server's own counter, within C2_MAX_BATCHES batches). A run that does
 *   not show both fails the job, so an XFAIL that stops racing cannot pass
 *   silently. The executor (J3) turns this into an expected pass and
 *   removes the marker.
 * =============================================================================
 */

#define DUAL_SHARED_LOCK 0
#include "dual_producers.h"

/* A lost update needs both producers inside the counter's increment at
 * once; with several CPUs a 40 000-request batch shows only a handful, and
 * a CI run once showed none (with races reported). Up to five batches,
 * stopping at the first loss, so a false XPASS needs five clean batches. */
#define C2_MAX_BATCHES (5U)

int main(void)
{
    (void)dual_producers_run("tsan_c2_dual_separate_locks", C2_MAX_BATCHES);
    return 0;   /* run_tsan.sh judges the XFAIL from the races and the count */
}
