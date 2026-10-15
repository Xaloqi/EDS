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
 *            - Tick loop: eds_elapsed_consume() (EDS#411, not yet added).
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

/** Elapsed-time state. Treat as opaque; use the functions below. */
typedef struct {
    uint32_t anchor_ms; /**< Clock value the interval is measured from. */
    uint32_t carry_ms;  /**< Reserved for eds_elapsed_consume() (EDS#411). */
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

#ifdef __cplusplus
}
#endif

#endif /* EDS_ELAPSED_H */
