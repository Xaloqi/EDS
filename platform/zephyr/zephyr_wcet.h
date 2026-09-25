// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_wcet.h
 *
 * PURPOSE: Minimal DWT-cycle-counter instrumentation for the issue #31 WCET
 *          measurement campaign. NOT part of any shipped build — every call
 *          site this compiles at is wrapped in `#if defined(DIAG_WCET_MEASURE)`,
 *          a bare preprocessor flag passed only via a dedicated measurement
 *          build's `-D`, never defined by any example's own CMakeLists.txt
 *          or prj.conf. Production and every existing CI build are
 *          completely unaffected: this header is not even included unless
 *          DIAG_WCET_MEASURE is defined at the call site.
 *
 * METHOD: Zephyr's timing_* API (zephyr/timing/timing.h), which on Cortex-M
 *         reads the DWT cycle counter (arch/arm/core/cortex_m/timing.c) —
 *         the same DWT_CYCCNT issue #31 itself specifies, via Zephyr's own
 *         portable wrapper rather than a raw register poke. Requires
 *         CONFIG_TIMING_FUNCTIONS=y in the measurement build's board conf.
 *
 * USAGE:
 *   static wcet_stats_t s_stats;
 *   ...
 *   #if defined(DIAG_WCET_MEASURE)
 *   { timing_t t0 = timing_counter_get();
 *   #endif
 *       do_the_work();
 *   #if defined(DIAG_WCET_MEASURE)
 *   timing_t t1 = timing_counter_get();
 *   wcet_stats_record(&s_stats, timing_cycles_get(&t0, &t1)); }
 *   #endif
 *
 * READOUT: logs a line on every NEW maximum observed (see
 * wcet_stats_record()), not a GDB read afterward. A GDB read requires an SWD
 * attach, and every attach path this board accepts needs a reset first to
 * get past a Cortex-M7 DBGMCU examine timing quirk seen throughout this
 * project's hardware bring-up — which wipes the very RAM-resident
 * accumulator being measured. Logging on new-max is also inherently
 * low-frequency after the first few calls (a real WCET distribution's max
 * stops moving quickly), so it barely perturbs the timed region itself, and
 * it shows the max's convergence live rather than a single opaque endpoint.
 *
 * SAFETY: This module and every call site it touches are measurement-only
 *         scaffolding, not part of any safety-relevant build.
 * =============================================================================
 */

#ifndef ZEPHYR_WCET_H
#define ZEPHYR_WCET_H

#if defined(DIAG_WCET_MEASURE)

#include <zephyr/timing/timing.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t count;
    uint64_t min_cycles;
    uint64_t max_cycles;
    uint64_t sum_cycles;
} wcet_stats_t;

static inline void wcet_stats_init(wcet_stats_t *s)
{
    s->count      = 0U;
    s->min_cycles = UINT64_MAX;
    s->max_cycles = 0U;
    s->sum_cycles = 0U;
}

/**
 * @brief Record one sample. Returns true iff this call set a new maximum —
 * callers LOG_INF() on that (with their own file's log module, not this
 * shared header's) rather than this function doing the logging itself.
 */
static inline bool wcet_stats_record(wcet_stats_t *s, uint64_t cycles)
{
    bool is_new_max = (cycles > s->max_cycles);

    s->count++;
    s->sum_cycles += cycles;
    if (cycles < s->min_cycles) {
        s->min_cycles = cycles;
    }
    if (is_new_max) {
        s->max_cycles = cycles;
    }
    return is_new_max;
}

#endif /* DIAG_WCET_MEASURE */

#endif /* ZEPHYR_WCET_H */
