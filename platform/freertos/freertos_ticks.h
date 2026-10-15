// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_ticks.h
 *
 * PURPOSE: FreeRTOS tick count to milliseconds, exact at any
 *          configTICK_RATE_HZ (EDS#418).
 *
 *          eds_platform_uptime_ms() used ticks * portTICK_PERIOD_MS, and
 *          portTICK_PERIOD_MS is the integer 1000 / configTICK_RATE_HZ:
 *            - 300 Hz: 3 instead of 3.33 ms per tick, so the clock
 *              under-reports and every timer on it runs ~11% long;
 *            - above 1000 Hz: 0, so the clock never advances and no UDS
 *              timer ever expires.
 *          This computes ticks * 1000 / rate in 64-bit instead, as the
 *          lwIP port's sys_now() already does.
 *
 *          Pure arithmetic with no FreeRTOS includes, so it is host-tested
 *          (tests/unit_runnable/test_freertos_ticks.c).
 *
 * WRAP    : the result wraps with the 32-bit ms value. When 1000 is a
 *           multiple of the rate it is also continuous across the 32-bit
 *           TICK counter wrap. Otherwise the ms value jumps once there
 *           (300 Hz: after ~166 days; 2000 Hz: after ~24.8 days), and one
 *           elapsed-time measurement spanning that instant is wrong.
 *
 * SAFETY  : No malloc. No recursion. Header-only.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef EDS_FREERTOS_TICKS_H
#define EDS_FREERTOS_TICKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Convert a tick count to milliseconds.
 *
 * @param[in] ticks         Tick count (xTaskGetTickCount()).
 * @param[in] tick_rate_hz  configTICK_RATE_HZ. 0 returns 0.
 * @return ticks * 1000 / tick_rate_hz, truncated to 32 bits.
 */
static inline uint32_t eds_freertos_ticks_to_ms(uint32_t ticks, uint32_t tick_rate_hz)
{
    uint32_t ms = 0U;

    if (tick_rate_hz != 0U) {
        ms = (uint32_t)(((uint64_t)ticks * (uint64_t)1000U) / (uint64_t)tick_rate_hz);
    }
    return ms;
}

#ifdef __cplusplus
}
#endif

#endif /* EDS_FREERTOS_TICKS_H */
