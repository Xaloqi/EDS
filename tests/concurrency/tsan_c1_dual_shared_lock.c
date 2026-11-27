// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS — host concurrency test (ThreadSanitizer)
 * FILE: tests/concurrency/tsan_c1_dual_shared_lock.c
 * MODULE UNDER TEST: core/uds_server.c with two producers
 *
 * PURPOSE: [EDS#197] Scenario C1: a CAN and a DoIP producer share one lock
 *   (dual_producers.h). Expected: no data race, and request_count is
 *   exactly 2 * ROUNDS (the exit status is non-zero otherwise).
 * =============================================================================
 */

#define DUAL_SHARED_LOCK 1
#include "dual_producers.h"

int main(void)
{
    return (dual_producers_run("tsan_c1_dual_shared_lock") == (2U * ROUNDS)) ? 0 : 1;
}
