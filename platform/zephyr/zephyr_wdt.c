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

/* [FIX #302] See the matching guard in zephyr_mutex.c. */
BUILD_ASSERT(_Alignof(diag_wdt_t) >= _Alignof(diag_wdt_internal_t),
             "diag_wdt_t's _opaque alignment is insufficient for "
             "diag_wdt_internal_t — see issue #302");

/* [FIX #306] Fail closed instead of degrading silently.
 *
 * diag_wdt_init()'s "no watchdog device" path exists for native_sim, which
 * sets CONFIG_WATCHDOG=n. A build that asks for watchdog support but whose
 * devicetree exposes no watchdog is a misconfiguration, not a degraded
 * target — and #306 is precisely what that looks like when it is allowed
 * to pass silently: every supported board took the degraded path for the
 * entire life of this file, so the ISO 26262-6 ASIL-B supervision claim in
 * this file's header was unmet on real hardware while CI (cross-compile
 * only, never executed) stayed green.
 *
 * Refuse to build that combination. Anything that reaches this assert is
 * either missing a `watchdog0` alias in its devicetree, or should be
 * setting CONFIG_WATCHDOG=n and accepting documented degraded mode.
 * Mirrors the fail-closed precedent in platform/freertos/freertos_flash_ops.c
 * (EDS#215), which likewise refuses to compile a production build that
 * would silently use a stub. */
#if defined(CONFIG_WATCHDOG)
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(watchdog0)) ||
             DT_NODE_EXISTS(DT_NODELABEL(wdt0)),
             /* ASCII only: non-ASCII in a _Static_assert message is emitted
              * as octal escapes by GCC, which makes the build error unreadable. */
             "CONFIG_WATCHDOG=y but this board's devicetree exposes no "
             "watchdog: no `watchdog0` alias and no `wdt0` node label. "
             "diag_wdt_feed() would silently become a no-op and the ASIL-B "
             "hardware-supervision claim would not hold - see issue #306. "
             "Add a watchdog0 alias, or build with CONFIG_WATCHDOG=n.");
#endif

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

    /* Try to get the WDT device — may not exist on native_sim.
     *
     * [FIX #306] This used to be DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0)).
     * No board this project targets defines a node *label* `wdt0`:
     *
     *   nucleo_h743zi / nucleo_h753zi   watchdog0 = &iwdg   (STM32 IWDG)
     *   frdm_mcxn947                    watchdog0 = &wwdt0
     *   mr_canhubk3                     watchdog0 = &fs26_wdt
     *
     * DEVICE_DT_GET_OR_NULL() resolves a nonexistent node to NULL silently
     * and at compile time, so this built cleanly everywhere and took the
     * degraded path below on every single real target — meaning the ASIL-B
     * hardware-supervision claim in this file's header was never actually
     * satisfied on hardware, with only a <wrn> to show for it. Worse on
     * STM32, where Zephyr's own IWDG driver arms the watchdog at boot
     * (CONFIG_IWDG_STM32_INITIAL_TIMEOUT, default 100 ms) unless
     * CONFIG_WDT_DISABLE_AT_BOOT=y: the watchdog was armed and never fed,
     * resetting the board every ~100 ms forever. See issue #306.
     *
     * `watchdog0` is Zephyr's standard alias for "the system watchdog" and
     * is what every board above defines. The DT_NODELABEL(wdt0) fallback is
     * kept for boards that do use that label (e.g. several Nordic ones), so
     * this resolves on both conventions. */
#if DT_NODE_EXISTS(DT_ALIAS(watchdog0))
    dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
#else
    dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0));
#endif
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

    /* [FIX #306] The pre-reset callback is OPTIONAL — the reset is not.
     *
     * Several watchdogs have no pre-reset interrupt at all and their drivers
     * reject a non-NULL callback outright. The STM32 IWDG is one:
     * drivers/watchdog/wdt_iwdg_stm32.c's install_timeout() opens with
     *
     *     if (config->callback != NULL) { return -ENOTSUP; }
     *
     * so this call previously failed with -134 (-ENOTSUP) on every STM32
     * target, diag_wdt_init() returned ERR_PLATFORM, and the ECU ran with no
     * watchdog supervision at all — while, on STM32, Zephyr's boot-time
     * arming left the IWDG running and unfed (see the #306 note above).
     *
     * wdt_expiry_cb() only emits a log line; its own SAFETY NOTE forbids
     * doing anything safety-critical there, precisely because the system is
     * already in an undefined state. So when the driver refuses the
     * callback, retry without it: we lose a diagnostic nicety and keep the
     * safety function. Failing the whole init over an optional log would be
     * exactly the wrong trade. */
    if (channel == -ENOTSUP) {
        LOG_WRN("WDT: Pre-reset callback unsupported on this target — "
                "installing timeout without it (reset still armed).");
        cfg.callback = NULL;
        channel = wdt_install_timeout(dev, &cfg);
    }

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
