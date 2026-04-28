// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_wdt.c
 *
 * PURPOSE: Watchdog timer integration using Zephyr WDT driver API.
 *
 *          Installs a WDT channel via wdt_install_timeout() and arms it
 *          via wdt_setup(). Feeds via wdt_feed() each poll iteration.
 *
 *          Degrades gracefully when the WDT driver is absent (native_sim)
 *          — feed calls become no-ops and a LOG_WRN is emitted at init.
 *
 * SAFETY  : ASIL-B candidate.
 *           WDT_OPT_PAUSE_IN_SLEEP is intentionally NOT set — the watchdog
 *           must continue ticking even if the CPU enters a low-power state,
 *           to catch scheduler lockup during sleep.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "zephyr_wdt.h"
#include "uds_types.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <string.h>

/* [FIX] Was LOG_MODULE_DECLARE(basic_ecu) — see transport/zephyr_can.c for rationale. */
LOG_MODULE_REGISTER(zephyr_wdt, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Internal context embedded in opaque storage
 * -------------------------------------------------------------------------- */
typedef struct diag_wdt_internal {
    const struct device *wdt_dev;   /**< Zephyr WDT device, or NULL if absent. */
    int                  channel;   /**< WDT channel ID returned by wdt_install_timeout. */
    bool                 active;    /**< True if WDT channel is armed. */
} diag_wdt_internal_t;

BUILD_ASSERT(sizeof(diag_wdt_internal_t) <= DIAG_WDT_OPAQUE_SIZE,
             "DIAG_WDT_OPAQUE_SIZE too small — increase in zephyr_wdt.h");

static diag_wdt_internal_t *intern(diag_wdt_t *w)
{
    return (diag_wdt_internal_t *)(void *)w->_opaque;  /* NOLINT */
}

/* --------------------------------------------------------------------------
 * WDT expiry callback
 *
 * Invoked when the WDT channel fires (after timeout with no feed).
 * Logs the event. A hardware reset will follow within one WDT window.
 *
 * SAFETY NOTE: Do NOT perform any safety-critical state saves here.
 *              The system is in an undefined state — reset is imminent.
 * -------------------------------------------------------------------------- */
static void wdt_expiry_cb(const struct device *dev, int channel_id)
{
    (void)dev;
    (void)channel_id;
    LOG_ERR("WDT: Diagnostics poll loop stalled — hardware reset imminent.");
}

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t diag_wdt_init(diag_wdt_t *wdt)
{
    diag_wdt_internal_t          *wi;
    const struct device          *dev;
    struct wdt_timeout_cfg        cfg;
    int                           channel;
    int                           rc;

    if (wdt == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    (void)memset(wdt->_opaque, 0, sizeof(wdt->_opaque));
    wi = intern(wdt);
    wi->active  = false;
    wi->channel = -1;

    /* Try to get the WDT device — may not exist on native_sim. */
    dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0));
    if ((dev == NULL) || !device_is_ready(dev)) {
        LOG_WRN("WDT: No watchdog device available — running without HW supervision.");
        wi->wdt_dev = NULL;
        return UDS_STATUS_OK;  /* Degraded mode — not fatal */
    }

    wi->wdt_dev = dev;

    /*
     * Configure WDT timeout:
     *   window.min = 0       — no minimum window (feed any time)
     *   window.max = CONFIG_DIAG_WDT_WINDOW_MS  — must feed within this window
     *   flags      = 0       — WDT_OPT_PAUSE_IN_SLEEP NOT set (intentional)
     *   callback   = wdt_expiry_cb — log before reset
     */
    cfg.callback    = wdt_expiry_cb;
    cfg.flags       = WDT_FLAG_RESET_SOC;
    cfg.window.min  = (uint32_t)0U;
    cfg.window.max  = (uint32_t)(CONFIG_DIAG_WDT_WINDOW_MS);

    channel = wdt_install_timeout(dev, &cfg);
    if (channel < 0) {
        LOG_ERR("WDT: Failed to install timeout channel: %d", channel);
        return UDS_STATUS_ERR_PLATFORM;
    }

    wi->channel = channel;

    /*
     * Arm the watchdog.
     * WDT_OPT_PAUSE_HALTED_BY_DBG: allow debugger to halt without triggering
     * reset — useful during development, harmless in production.
     */
    rc = wdt_setup(dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (rc != 0) {
        LOG_ERR("WDT: wdt_setup failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    wi->active = true;
    LOG_INF("WDT: Armed with %u ms window (channel %d).",
            (unsigned)CONFIG_DIAG_WDT_WINDOW_MS, channel);

    return UDS_STATUS_OK;
}

uds_status_t diag_wdt_feed(diag_wdt_t *wdt)
{
    diag_wdt_internal_t *wi;

    if (wdt == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    wi = intern(wdt);

    if (!wi->active || (wi->wdt_dev == NULL)) {
        return UDS_STATUS_OK;  /* No-op in degraded mode. */
    }

    (void)wdt_feed(wi->wdt_dev, wi->channel);

    return UDS_STATUS_OK;
}
