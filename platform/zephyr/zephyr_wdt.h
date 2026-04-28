// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_wdt.h
 *
 * PURPOSE: Watchdog timer (WDT) integration for the diagnostics poll loop.
 *
 *          The ISO 26262-6 ASIL-B claim on this stack requires that the
 *          diagnostic task is supervised by a hardware watchdog. If the poll
 *          loop stalls (e.g. due to CAN bus lockup, priority inversion, or
 *          software defect), the WDT must reset the ECU within the configured
 *          window.
 *
 *          This module installs a WDT channel and exposes a single feed
 *          function that must be called at the end of every 1 ms poll
 *          iteration. A window of CONFIG_DIAG_WDT_WINDOW_MS (default 100 ms)
 *          gives 100 iterations of slack before a reset is triggered.
 *
 * CONFIGURATION:
 *          CONFIG_DIAG_WDT_WINDOW_MS   — WDT timeout window in ms (default 100)
 *          CONFIG_WDT_DISABLE_AT_BOOT  — set to n in prj.conf to keep WDT armed
 *
 * SAFETY  : Safety-relevant. WDT must be enabled in production firmware.
 *           This module is intentionally a no-op on native_sim (no WDT driver).
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef ZEPHYR_WDT_H
#define ZEPHYR_WDT_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Default WDT window
 * -------------------------------------------------------------------------- */
#ifndef CONFIG_DIAG_WDT_WINDOW_MS
#define CONFIG_DIAG_WDT_WINDOW_MS   (100U)
#endif

/* --------------------------------------------------------------------------
 * WDT context (opaque)
 * -------------------------------------------------------------------------- */
#define DIAG_WDT_OPAQUE_SIZE   (32U)

typedef struct diag_wdt {
    uint8_t _opaque[DIAG_WDT_OPAQUE_SIZE];
} diag_wdt_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Install a WDT channel and arm the watchdog.
 *
 * Must be called once during startup, before the poll loop begins.
 * If the WDT driver is absent (e.g. native_sim), this function returns
 * UDS_STATUS_OK without installing a channel — feeding is a no-op.
 *
 * @param[out] wdt  Caller-allocated WDT context.
 *
 * @return UDS_STATUS_OK on success (or WDT driver absent — degraded mode).
 * @return UDS_STATUS_ERR_NULL_PTR if wdt is NULL.
 * @return UDS_STATUS_ERR_PLATFORM if WDT channel installation failed.
 */
uds_status_t diag_wdt_init(diag_wdt_t *wdt);

/**
 * @brief Feed the watchdog (must be called every iteration of the poll loop).
 *
 * Resets the WDT countdown. If not called within CONFIG_DIAG_WDT_WINDOW_MS,
 * the watchdog will reset the system.
 *
 * Safe to call even if diag_wdt_init() returned degraded mode — it becomes
 * a no-op in that case.
 *
 * @param[in] wdt  Initialised WDT context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if wdt is NULL.
 */
uds_status_t diag_wdt_feed(diag_wdt_t *wdt);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_WDT_H */
