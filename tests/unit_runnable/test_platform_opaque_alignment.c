// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * FILE: tests/unit_runnable/test_platform_opaque_alignment.c
 *
 * PURPOSE: Regression test for issue #302 — diag_mutex_t / diag_timer_t /
 *          diag_wdt_t embed a Zephyr kernel object (struct k_mutex,
 *          k_timer+k_sem, a WDT context struct) inside a raw uint8_t[]
 *          "opaque storage" array, then cast that storage directly to the
 *          real struct pointer type. A plain uint8_t[] has 1-byte alignment,
 *          which is undefined behaviour to reinterpret as a pointer- or
 *          dlist-containing struct — and on real Cortex-M7 hardware
 *          (NUCLEO-H753ZI bring-up, 2026-09-25) it faulted outright:
 *          sys_dlist_init() performs an LDRD/STRD dual-word access on the
 *          reinterpreted storage, which traps on ARMv7-M if the address
 *          isn't word-aligned.
 *
 *          This is a host-testable property (plain C11 _Alignof, no real
 *          Zephyr kernel headers needed) even though the fault itself only
 *          ever manifested on the real target — a plain compile check or a
 *          native_sim/QEMU run on an x86 host doesn't exercise ARM's
 *          alignment traps.
 *
 * TEST CASES:
 *   TC-ALIGN-001  diag_mutex_t's _opaque is word-aligned
 *   TC-ALIGN-002  diag_timer_t's _opaque is word-aligned
 *   TC-ALIGN-003  diag_wdt_t's _opaque is word-aligned
 *
 * FRAMEWORK: Zephyr Ztest (host shim)
 * =============================================================================
 */

#include <zephyr/ztest.h>
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"

ZTEST_SUITE(test_platform_opaque_alignment, NULL, NULL, NULL, NULL, NULL);

/* Word/pointer alignment (4 bytes on the real ARM target) is the actual
 * architectural minimum that avoids the LDRD/STRD fault; the fix reserves
 * 8 bytes for headroom against any 64-bit-aligned member Zephyr's kernel
 * objects might ever need. Assert the concrete guarantee the fix makes,
 * not just ">1", so a future regression that weakens it back toward
 * byte-alignment is still caught. */
#define EXPECTED_MIN_ALIGN 8U

ZTEST(test_platform_opaque_alignment, tc001_diag_mutex_opaque_aligned)
{
    zassert_true(_Alignof(diag_mutex_t) >= EXPECTED_MIN_ALIGN,
        "diag_mutex_t must be word-aligned — see issue #302");
}

ZTEST(test_platform_opaque_alignment, tc002_diag_timer_opaque_aligned)
{
    zassert_true(_Alignof(diag_timer_t) >= EXPECTED_MIN_ALIGN,
        "diag_timer_t must be word-aligned — see issue #302");
}

ZTEST(test_platform_opaque_alignment, tc003_diag_wdt_opaque_aligned)
{
    zassert_true(_Alignof(diag_wdt_t) >= EXPECTED_MIN_ALIGN,
        "diag_wdt_t must be word-aligned — see issue #302");
}

extern void test_platform_opaque_alignment__tc001_diag_mutex_opaque_aligned(void);
extern void test_platform_opaque_alignment__tc002_diag_timer_opaque_aligned(void);
extern void test_platform_opaque_alignment__tc003_diag_wdt_opaque_aligned(void);

void run_all_tests(void)
{
    RUN_TEST(test_platform_opaque_alignment__tc001_diag_mutex_opaque_aligned);
    RUN_TEST(test_platform_opaque_alignment__tc002_diag_timer_opaque_aligned);
    RUN_TEST(test_platform_opaque_alignment__tc003_diag_wdt_opaque_aligned);
}
