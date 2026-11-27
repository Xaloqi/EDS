// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/eds_race_point.h
 *
 * PURPOSE: [EDS#197] A named point in the stack where a test can force a
 *          thread switch, so an interleaving that ThreadSanitizer finds only
 *          now and then can be replayed every run (the forced-interleaving
 *          layer of the #197 test plan).
 *
 *          EDS_RACE_POINT(id) expands to nothing unless UNIT_TEST is
 *          defined. Under UNIT_TEST it calls eds_race_point_hook (when a
 *          test has set it) with the point's id; the test that sets the hook
 *          defines that variable. A production build has no hook, no call
 *          and no symbol: scripts/verify_no_test_hooks.py checks this on
 *          every PR.
 *
 *          The macro is defined here and nowhere else. A build that defines
 *          it first (on the command line, say) stops with #error, so it
 *          cannot be turned on behind this header's back.
 *
 * SAFETY  : No malloc. No recursion. Header-only. Empty in production.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef EDS_RACE_POINT_H
#define EDS_RACE_POINT_H

#if defined(EDS_RACE_POINT)
#error "EDS_RACE_POINT is defined by core/eds_race_point.h only"
#endif

#if defined(UNIT_TEST)

#include <stddef.h>
#include <stdint.h>

/** Test hook: called at each race point while non-NULL. Defined by the test. */
extern void (*eds_race_point_hook)(uint32_t id);

#define EDS_RACE_POINT(id)                              \
    do {                                                \
        if (eds_race_point_hook != NULL) {              \
            eds_race_point_hook((uint32_t)(id));        \
        }                                               \
    } while (0)

#else

#define EDS_RACE_POINT(id) do { } while (0)

#endif /* UNIT_TEST */

#endif /* EDS_RACE_POINT_H */
