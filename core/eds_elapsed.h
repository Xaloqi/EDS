// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/eds_elapsed.h
 *
 * PURPOSE: Elapsed time from a millisecond clock, in place of counting
 *          loop iterations or events as a stand-in for time (EDS#411,
 *          EDS#412).
 *
 *          Pure arithmetic on caller-supplied timestamps: no clock access,
 *          no OS dependency, so it is identical on Zephyr and FreeRTOS and
 *          host-testable. Feed it eds_platform_uptime_ms() (platform_api.h)
 *          or, in transport code, the clock its platform ops expose.
 *
 *          Two uses, one instance each:
 *            - Deadline: eds_elapsed_init() when the interval starts, then
 *              eds_elapsed_since() to compare against a limit (EDS#412).
 *            - Tick loop: eds_elapsed_init() once, then
 *              eds_elapsed_consume() every iteration (EDS#411).
 *          Do not mix the two on one instance: consume moves the anchor.
 *
 *          All arithmetic is unsigned and wraps with the 32-bit clock, so an
 *          interval is measured correctly across a clock wrap as long as it
 *          is shorter than 2^32 ms (~49.7 days).
 *
 * SAFETY  : No malloc. No recursion. Header-only.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef EDS_ELAPSED_H
#define EDS_ELAPSED_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * [EDS#411] Most time eds_elapsed_consume() will hold back to hand out on
 * later calls. Carrying the remainder keeps timers from running long after
 * a real stall (a slow NVM write, a blocked send). Saturating it here stops
 * a debugger halt, during which the platform clock keeps running, from
 * being replayed as a burst of timer ticks on resume: S3 expiring, lockout
 * state churning, periodic scheduling flooding. 5 s is longer than any
 * stall EDS itself bounds (DOIP_TCP_SEND_TIMEOUT_MS is 2 s) and far shorter
 * than a halt. Time beyond it is dropped. This is the timer counterpart of
 * WDT_OPT_PAUSE_HALTED_BY_DBG, which zephyr_wdt.c already uses.
 */
#ifndef EDS_ELAPSED_MAX_CARRY_MS
#define EDS_ELAPSED_MAX_CARRY_MS (5000U)
#endif

#if (EDS_ELAPSED_MAX_CARRY_MS == 0U)
#error "EDS_ELAPSED_MAX_CARRY_MS must be non-zero (EDS#411)"
#endif

/** Elapsed-time state. Treat as opaque; use the functions below. */
typedef struct {
    uint32_t anchor_ms; /**< Clock value the interval is measured from. */
    uint32_t carry_ms;  /**< Time owed by eds_elapsed_consume() (EDS#411). */
} eds_elapsed_t;

/**
 * @brief Start measuring from now_ms.
 *
 * @param[out] e       State to initialise. NULL is ignored.
 * @param[in]  now_ms  Current clock value in ms.
 */
static inline void eds_elapsed_init(eds_elapsed_t *e, uint32_t now_ms)
{
    if (e != NULL) {
        e->anchor_ms = now_ms;
        e->carry_ms  = 0U;
    }
}

/**
 * @brief Milliseconds since eds_elapsed_init(), wrap-safe.
 *
 * @param[in] e       State from eds_elapsed_init(). NULL returns 0.
 * @param[in] now_ms  Current clock value in ms, from the same clock.
 * @return now_ms - anchor in unsigned 32-bit arithmetic.
 */
static inline uint32_t eds_elapsed_since(const eds_elapsed_t *e, uint32_t now_ms)
{
    uint32_t since = 0U;

    if (e != NULL) {
        since = now_ms - e->anchor_ms;
    }
    return since;
}

/**
 * @brief Milliseconds a tick loop should process now.  [EDS#411]
 *
 * Adds the time since the previous call (or eds_elapsed_init()) to what is
 * still owed, saturating the total at EDS_ELAPSED_MAX_CARRY_MS, and returns
 * at most cap_ms of it. The rest is carried to later calls, never dropped,
 * so a stall drains over several iterations, each doing bounded work. The
 * caller runs its own 1 ms ticks that many times.
 *
 * @param[in,out] e       State from eds_elapsed_init(). NULL returns 0.
 * @param[in]     now_ms  Current clock value in ms, from the same clock.
 * @param[in]     cap_ms  Most ms to hand out in this call (bounds work).
 * @return ms to process now, <= cap_ms.
 */
static inline uint32_t eds_elapsed_consume(eds_elapsed_t *e, uint32_t now_ms, uint32_t cap_ms)
{
    uint32_t out = 0U;

    if (e != NULL) {
        uint32_t owed    = (uint32_t)EDS_ELAPSED_MAX_CARRY_MS;
        uint32_t elapsed = now_ms - e->anchor_ms;

        e->anchor_ms = now_ms;
        /* carry_ms <= EDS_ELAPSED_MAX_CARRY_MS always, so the sum below
         * cannot wrap once elapsed is known to be under the limit. */
        if (elapsed < (uint32_t)EDS_ELAPSED_MAX_CARRY_MS) {
            owed = e->carry_ms + elapsed;
            if (owed > (uint32_t)EDS_ELAPSED_MAX_CARRY_MS) {
                owed = (uint32_t)EDS_ELAPSED_MAX_CARRY_MS;
            }
        }
        out = (owed < cap_ms) ? owed : cap_ms;
        e->carry_ms = owed - out;
    }
    return out;
}

#ifdef __cplusplus
}
#endif

#endif /* EDS_ELAPSED_H */
