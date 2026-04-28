// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * HOST TEST STUB: tests/mocks/zephyr_port_mock.c
 *
 * Provides stub implementations of ALL platform functions for host-side unit
 * tests. Replaces (post-restructure paths):
 *   platform/zephyr/zephyr_port.c
 *   platform/zephyr/zephyr_mutex.c
 *   platform/zephyr/zephyr_timer.c
 *   platform/zephyr/zephyr_wdt.c
 *   platform/zephyr/zephyr_platform_api.c  (eds_platform_* stubs added below)
 *
 * Phase 6A additions:
 *   - diag_mutex_*  stubs (no-op — tests run single-threaded)
 *   - diag_timer_*  stubs (tick never fires; tests drive timing manually)
 *   - diag_wdt_*    stubs (no-op — no hardware on host)
 *
 * [RESTRUCTURE] CMakeLists adds both platform/ and platform/zephyr/ to
 *   include dirs, so these headers resolve without path prefixes.
 *
 * SAFETY: Test-only. NOT for production firmware.
 * =============================================================================
 */

#include "zephyr_port.h"    /* found via platform/zephyr/ include path */
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"
#include "platform_api.h"   /* eds_platform_* — stubs added at end of this file */
#include "uds_types.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── zephyr_port stubs ─────────────────────────────────────────────────── */

uds_status_t zephyr_port_init(
    const zephyr_port_cfg_t *cfg,
    can_transport_t        **out_transport)
{
    (void)cfg;
    (void)out_transport;
    return UDS_STATUS_OK;
}

uds_timestamp_ms_t zephyr_port_get_time_ms(void)
{
    return (uds_timestamp_ms_t)0U;
}

void zephyr_port_delay_ms(uint32_t ms)
{
    (void)ms;
}

uds_status_t zephyr_port_ecu_reset(uint8_t reset_type)
{
    (void)reset_type;
    /* Stub: pretend reset succeeded without actually resetting. */
    return UDS_STATUS_OK;
}

uds_status_t zephyr_port_nvm_flush(void)
{
    /* Stub: no NVM on host test environment. */
    return UDS_STATUS_OK;
}

uint32_t zephyr_port_enter_critical(void)
{
    return 0U;
}

void zephyr_port_exit_critical(uint32_t key)
{
    (void)key;
}

/* ── diag_mutex stubs ──────────────────────────────────────────────────── */
/* Tests are single-threaded — all mutex operations are no-ops. */

uds_status_t diag_mutex_init(diag_mutex_t *m)
{
    if (m == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    (void)memset(m->_opaque, 0, sizeof(m->_opaque));
    return UDS_STATUS_OK;
}

uds_status_t diag_mutex_lock(diag_mutex_t *m)
{
    if (m == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    return UDS_STATUS_OK;
}

uds_status_t diag_mutex_unlock(diag_mutex_t *m)
{
    if (m == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    return UDS_STATUS_OK;
}

uds_status_t diag_mutex_trylock(diag_mutex_t *m, bool *acquired)
{
    if ((m == NULL) || (acquired == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    *acquired = true;
    return UDS_STATUS_OK;
}

/* ── diag_timer stubs ──────────────────────────────────────────────────── */
/*
 * Tests drive timing by calling isotp_tick_1ms() / uds_server_tick_1ms()
 * directly. The timer stubs do nothing — diag_timer_wait_tick() returns
 * TIMEOUT immediately so it never blocks.
 */

/* Internal: store callback for tests that want to invoke it directly. */
typedef struct {
    diag_timer_tick_cb cb;
    void              *arg;
    bool               started;
} mock_timer_t;

uds_status_t diag_timer_init(diag_timer_t *t, diag_timer_tick_cb cb, void *arg)
{
    mock_timer_t *mt;
    if (t == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    (void)memset(t->_opaque, 0, sizeof(t->_opaque));
    mt = (mock_timer_t *)(void *)t->_opaque;
    mt->cb      = cb;
    mt->arg     = arg;
    mt->started = false;
    return UDS_STATUS_OK;
}

uds_status_t diag_timer_start(diag_timer_t *t)
{
    mock_timer_t *mt;
    if (t == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    mt = (mock_timer_t *)(void *)t->_opaque;
    if (mt->started) { return UDS_STATUS_ERR_ALREADY_INITIALIZED; }
    mt->started = true;
    return UDS_STATUS_OK;
}

uds_status_t diag_timer_stop(diag_timer_t *t)
{
    mock_timer_t *mt;
    if (t == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    mt = (mock_timer_t *)(void *)t->_opaque;
    mt->started = false;
    return UDS_STATUS_OK;
}

uds_status_t diag_timer_wait_tick(diag_timer_t *t, uint32_t timeout_ms)
{
    /* On host: return TIMEOUT immediately — no blocking. */
    (void)t;
    (void)timeout_ms;
    return UDS_STATUS_ERR_TIMEOUT;
}

uds_status_t diag_timer_pending_ticks(const diag_timer_t *t, uint32_t *out_count)
{
    if ((t == NULL) || (out_count == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    *out_count = 0U;
    return UDS_STATUS_OK;
}

/* ── diag_wdt stubs ────────────────────────────────────────────────────── */

uds_status_t diag_wdt_init(diag_wdt_t *wdt)
{
    if (wdt == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    (void)memset(wdt->_opaque, 0, sizeof(wdt->_opaque));
    return UDS_STATUS_OK;
}

uds_status_t diag_wdt_feed(diag_wdt_t *wdt)
{
    if (wdt == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    return UDS_STATUS_OK;
}

/* ── eds_platform_* stubs (platform_api.h) ─────────────────────────────── */
/* These are the platform-neutral functions called by the integration layer.
 * On the host test environment all are no-ops or return fixed safe values. */

uds_status_t eds_platform_init(const eds_platform_cfg_t *cfg)
{
    (void)cfg;
    return UDS_STATUS_OK;
}

uds_status_t eds_platform_ecu_reset(uint8_t reset_type)
{
    (void)reset_type;
    /* Stub: record that a reset was requested without actually resetting. */
    return UDS_STATUS_OK;
}

uds_status_t eds_platform_nvm_flush(void)
{
    /* Stub: no NVM on host test environment. */
    return UDS_STATUS_OK;
}

uint32_t eds_platform_uptime_ms(void)
{
    return 0U;
}

void eds_platform_can_input(const eds_can_frame_t *frame)
{
    (void)frame;
}
