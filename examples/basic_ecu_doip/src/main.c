// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip/src/main.c
 *
 * PURPOSE: BasicECU_DoIP — identical DID/DTC set as basic_ecu, served over
 *          DoIP (ISO 13400-2) instead of ISO-TP CAN.
 *
 *          Demonstrates the minimum integration to add DoIP to an EDS ECU:
 *            1. Include zephyr_lwip.h / platform_doip.h
 *            2. Call eds_doip_platform_start() after uds_generated_init()
 *            3. Add Networking Kconfig and the doip: block to diagnostics_config.yaml
 *
 *          The CAN diagnostic task (diag_task) is NOT started here — this
 *          example is DoIP-only. For a production "both" transport ECU,
 *          add the CAN thread from basic_ecu alongside the DoIP init.
 *
 * THREAD MODEL:
 *   main()        — init → uds_generated_init → start tick + DoIP → return
 *   uds_tick_task — 1 ms poll loop: uds_server_tick_1ms() + uds_periodic_tick_1ms(),
 *                   once per millisecond actually elapsed [EDS#401, EDS#411].
 *                   [EDS#191] A DoIP-only build has no CAN diag_task, so
 *                   without this thread the S3 session timeout and the
 *                   SecurityAccess lockout countdown never progress — a
 *                   session/unlock, once entered, never expires on its own.
 *   doip_thread   — started via a semaphore (zephyr_lwip.c), not a fixed
 *                   delay [EDS#192]; runs eds_doip_server_run()
 *
 * WATCHDOG SUPERVISION [EDS#399] (real hardware, CONFIG_WATCHDOG=y; a
 * no-op on native_sim):
 *   The IWDG is armed in main() just before uds_tick_task starts, and fed
 *   only from uds_tick_task. That thread feeds only while doip_thread has
 *   checked in, through eds_doip_set_checkin_callback(), within
 *   DOIP_SUPERVISION_WINDOW_MS. A hung tick loop, a dispatch hung holding
 *   the UDS locks, or doip_thread wedged on its socket or exited all reset
 *   the ECU. An idle server with no client checks in every accept poll
 *   (5 s), so it is never reset for being idle.
 *   NOT supervised: boot before the arming point (pre-main network wait
 *   and the init in main()), and a dead Ethernet link (accept still times
 *   out and checks in).
 *
 * TARGET: native_sim (CI — loopback networking) and any Ethernet-capable
 *         Zephyr board (e.g. FRDM-K64F, STM32H7 with ETH).
 *
 * BUILDING:
 *   west build -b native_sim examples/basic_ecu_doip
 *
 * TESTING (once built):
 *   # In one terminal:
 *   ./build/zephyr/zephyr.exe
 *   # In another:
 *   python3 -c "
 *   import asyncio
 *   from xaloqi.tester import UdsTester, DoipBus, Session
 *   async def main():
 *       async with UdsTester(DoipBus('127.0.0.1'), rx_id=0xE400, tx_id=0x0E00) as ecu:
 *           vin = await ecu.read_did(0xF190)
 *           print('VIN:', vin)
 *   asyncio.run(main())
 *   "
 *
 * SAFETY   : ASIL-B candidate.
 * STANDARD : MISRA C:2012 alignment intended.
 * LICENSE  : Apache-2.0
 * =============================================================================
 */

/* --------------------------------------------------------------------------
 * EDS stack headers
 * -------------------------------------------------------------------------- */
#include "uds_types.h"
#include "uds_server.h"
#include "uds_security_algo.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "eds_elapsed.h"     /* [EDS#411] elapsed-time ticking */

/* --------------------------------------------------------------------------
 * DoIP platform headers
 * -------------------------------------------------------------------------- */
#include "platform_api.h"    /* eds_platform_uptime_ms() */
#include "platform_doip.h"   /* eds_doip_platform_start() */
#include "doip_server.h"     /* DOIP_PORT */
#include "zephyr_mutex.h"
#include "zephyr_wdt.h"
#if defined(DIAG_DOIP_FLASH_BENCH)
#include "zephyr_flash_ops.h" /* [EDS#423] bench flash download */
#include "services.h"         /* uds_service_0x34_pending_tick() */
#endif
#include "nvm_store.h"
#if defined(DIAG_WCET_MEASURE)
#include "zephyr_wcet.h"     /* [#31] see header comment */
#endif

/* --------------------------------------------------------------------------
 * Generated headers (from diagnostics_config.yaml via codegen.py)
 * -------------------------------------------------------------------------- */
#include "uds_init.h"
#include "generated_config.h"
#include "did_handlers.h"

/* --------------------------------------------------------------------------
 * Zephyr headers
 * -------------------------------------------------------------------------- */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(basic_ecu_doip, LOG_LEVEL_INF);

/* =============================================================================
 * DoIP ECU configuration
 *
 * logical_address: 0xE400 — standard xaloqi-tester default ECU address.
 * source_address:  0x0E00 — standard tester address (accepted during
 *                            routing activation in doip_server.c).
 *
 * These values must match the DoipBus constructor arguments in the
 * xaloqi-tester integration tests (conftest.py).
 * ============================================================================= */

#define DOIP_ECU_LOGICAL_ADDR   (0xE400U)

/* =============================================================================
 * UDS tick task configuration [EDS#191]
 *
 * Drives uds_server_tick_1ms() and uds_periodic_tick_1ms() every 1 ms —
 * the only thing progressing the S3 session timeout and SecurityAccess
 * lockout countdown in a DoIP-only build (no isotp_tick_1ms(): there is
 * no ISO-TP/CAN transport here). Priority matches basic_ecu's diag_task
 * (5) — higher priority than doip_thread's default (7,
 * zephyr_lwip.c) so UDS timer progression is never starved by DoIP
 * request handling, same rationale as the CAN example's own priority split.
 * ============================================================================= */

/* [EDS#411] Most 1 ms ticks one loop iteration runs, which bounds the work
 * done while the session/security locks are held. Time beyond it is not
 * lost: eds_elapsed_consume() carries it into the next iterations (up to
 * EDS_ELAPSED_MAX_CARRY_MS). */
#define DIAG_TICK_MAX_PER_LOOP_MS         (100U)

#ifndef CONFIG_DIAG_TICK_TASK_STACK_SIZE
#define CONFIG_DIAG_TICK_TASK_STACK_SIZE   (1024U)
#endif
#ifndef CONFIG_DIAG_TICK_TASK_PRIORITY
#define CONFIG_DIAG_TICK_TASK_PRIORITY     (5)
#endif

K_THREAD_STACK_DEFINE(s_tick_stack, CONFIG_DIAG_TICK_TASK_STACK_SIZE);
static struct k_thread s_tick_thread;

/* [EDS#353] uds_tick_task and doip_thread both touch uds_server_ctx_t
 * (session/security state) with no prior synchronization — the same
 * s_session_lock/s_security_lock pair the CAN examples already wrap
 * both their tick and dispatch call sites with, extended here to DoIP.
 * See docs/threading_guide.md's "Dual-Transport Concurrency" section:
 * the design intent was always "the same lock at every call site that
 * touches uds_server_ctx_t," never actually applied to this example. */
static diag_mutex_t s_session_lock;
static diag_mutex_t s_security_lock;

/* [EDS#399] Fed only by uds_tick_task, and only while doip_thread has
 * checked in recently. The window is the longest wait the server bounds
 * between check-ins (the accept poll with no client), plus margin for one
 * dispatch and its response send. See "WATCHDOG SUPERVISION" above. */
#define DOIP_SUPERVISION_WINDOW_MS  (DOIP_CHECKIN_MAX_WAIT_MS + 2000U)
static diag_wdt_t   s_wdt;
static atomic_t     s_doip_checkins;

static void doip_checkin_cb(void)
{
    (void)atomic_inc(&s_doip_checkins);
}

static void doip_lock_cb(void)
{
    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
}

static void doip_unlock_cb(void)
{
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);
}

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

/* =============================================================================
 * UDS tick task [EDS#191]
 * ============================================================================= */

static void uds_tick_task_entry(void *p1, void *p2, void *p3)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)p1;
    (void)p2;
    (void)p3;

    LOG_INF("UDS tick task started");

    /* [EDS#399] Time of the last DoIP check-in seen. Starts now, so a DoIP
     * thread that never starts is caught one window after arming. */
    atomic_val_t doip_seen       = atomic_get(&s_doip_checkins);
    int64_t      doip_checkin_ms = k_uptime_get();

    /* [EDS#401] One loop iteration is NOT 1 ms: k_msleep(1) rounds up and
     * adds a tick (~2 ms at 1 kHz), and a dispatch holding the locks below
     * delays it further. Counting iterations ran every UDS timer (S3,
     * SecurityAccess lockout, periodic) at about half speed. Run one tick
     * per millisecond actually elapsed instead, via the shared helper
     * [EDS#411]. */
    eds_elapsed_t tick_clock;
    eds_elapsed_init(&tick_clock, eds_platform_uptime_ms());

    while (true) {
        k_msleep(1);
        int64_t  now_ms  = k_uptime_get();
        uint32_t elapsed = eds_elapsed_consume(&tick_clock, eds_platform_uptime_ms(),
                                               DIAG_TICK_MAX_PER_LOOP_MS);

        /* [EDS#353] Same lock doip_lock_cb()/doip_unlock_cb() wrap
         * uds_server_process_request() with below. */
        (void)diag_mutex_lock(&s_session_lock);
        (void)diag_mutex_lock(&s_security_lock);
        for (uint32_t i = 0U; i < elapsed; i++) {
            (void)uds_server_tick_1ms(srv);
            (void)uds_periodic_tick_1ms();
        }
        (void)diag_mutex_unlock(&s_security_lock);
        (void)diag_mutex_unlock(&s_session_lock);

        /* [EDS#399] Feed only while the DoIP server loop is alive. Placed
         * after the lock pair, so a dispatch hung holding the locks stops
         * the feed too. */
        atomic_val_t doip_now = atomic_get(&s_doip_checkins);
        if (doip_now != doip_seen) {
            doip_seen       = doip_now;
            doip_checkin_ms = now_ms;
        }
        if ((now_ms - doip_checkin_ms) < (int64_t)DOIP_SUPERVISION_WINDOW_MS) {
            (void)diag_wdt_feed(&s_wdt);
        }
    }
}

/* =============================================================================
 * main()
 * ============================================================================= */

int main(void)
{
    uds_status_t      status;
    uds_server_ctx_t *srv = NULL;

#if defined(DIAG_WCET_MEASURE)
    /* [#31] Must run before any timing_counter_get() call site — see
     * transport/doip/doip_server.c's instrumented dispatch call. */
    timing_init();
    timing_start();
#endif

    LOG_INF("Xaloqi EDS BasicECU_DoIP starting (v1.6.0)");
    LOG_INF("DoIP logical address: 0x%04X  port: %u",
            (unsigned)DOIP_ECU_LOGICAL_ADDR, (unsigned)DOIP_PORT);

    /*
     * Security algorithm init.
     * For production: inject TRNG callback and OEM level keys.
     * See docs/SECURITY_NOTICE.md.
     */
    LOG_WRN("[SEC] Using placeholder AES keys — inject OEM keys before production.");
    (void)uds_security_algo_set_rng_cb(NULL);

    /*
     * [EDS#200] Must run before uds_generated_init() (dtc_mirror_init() at
     * Step 3.5 sits on top of nvm_store and silently no-ops until this
     * succeeds). This example only targets native_sim, where
     * platform/zephyr/nvm_store_mock.c (RAM-backed) is linked instead of
     * the real NVS backend — its nvm_store_init() ignores cfg entirely, so
     * NULL is correct here (see nvm_store.h: "may be NULL on host").
     */
    status = nvm_store_init(NULL);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("NVM store init failed: 0x%02X — DTC mirror will not "
                "survive reset this run.", (unsigned)status);
    }

    /*
     * UDS stack init — DoIP build passes NULL for the CAN transport
     * because the stack will be driven by the DoIP server thread, not
     * a CAN poll loop. uds_generated_init() in a DoIP-only build does
     * not call isotp_init(); the generated code checks the transport
     * field from diagnostics_config.yaml.
     *
     * For a "transport: both" build, pass the real CAN transport here.
     */
#if defined(DIAG_DOIP_FLASH_BENCH)
    /* [EDS#423] Bench only: RequestDownload writes to real flash (bank 2,
     * partition image_1, see nucleo_h753zi_flash_bench.overlay). The erase
     * runs in steps after a 0x78; the pending callback lets the DoIP server
     * drive those steps and send the final response. Registered before
     * uds_generated_init(), as zephyr_flash_ops.h requires. */
    LOG_WRN("[BENCH] Flash-download build: 0x34/0x36/0x37 write flash bank 2.");
    if (zephyr_flash_ops_init() != UDS_STATUS_OK) {
        LOG_ERR("Flash ops init failed.");
        return -1;
    }
    eds_doip_set_pending_callback(uds_service_0x34_pending_tick);
#endif

    status = uds_generated_init(NULL, 0U, 0U);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS stack init failed: 0x%02X", (unsigned)status);
        return -1;
    }

    srv = uds_generated_get_server();
    if (srv == NULL) {
        LOG_ERR("UDS server context NULL after init.");
        return -1;
    }

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    LOG_INF("UDS stack ready: %u DIDs  %u DTCs",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT);

    /* [EDS#353] Must be initialised and registered before the tick task
     * and DoIP server start below — both race on uds_server_ctx_t from
     * the moment either thread runs. */
    if ((diag_mutex_init(&s_session_lock)  != UDS_STATUS_OK) ||
        (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK)) {
        LOG_ERR("Mutex init failed.");
        return -1;
    }
    eds_doip_set_lock_callbacks(doip_lock_cb, doip_unlock_cb);
    eds_doip_set_checkin_callback(doip_checkin_cb);   /* [EDS#399] */


    /*
     * [EDS#399] Arm the watchdog here, immediately before its only feeder
     * starts. Not earlier: on this example's NVS backend, nvm_store_init()
     * above may erase a flash sector, which can block far past the 100 ms
     * window, and there is no feeder yet. Fail closed: Zephyr's boot-time
     * arming is off (boards/nucleo_h753zi/nucleo_h753zi_doip.conf), so
     * ignoring a failure here would run unsupervised while appearing
     * supervised (EDS#306's class). On native_sim (CONFIG_WATCHDOG=n) this
     * returns OK in degraded mode. From here on, a `return -1` leaves no
     * DoIP check-ins and ends in a watchdog reset on hardware.
     */
    if (diag_wdt_init(&s_wdt) != UDS_STATUS_OK) {
        LOG_ERR("Watchdog arming failed — refusing to run unsupervised.");
        return -1;
    }
#if defined(DIAG_DOIP_FLASH_BENCH)
    /* [EDS#423] Feed across each bounded erase step (#312). */
    zephyr_flash_ops_set_wdt(&s_wdt);
#endif

    /*
     * [EDS#191] Start the 1 ms UDS tick task before the DoIP server so
     * S3/lockout timing is live from the moment a connection can arrive.
     */
    (void)k_thread_create(
        &s_tick_thread, s_tick_stack,
        K_THREAD_STACK_SIZEOF(s_tick_stack),
        uds_tick_task_entry,
        (void *)srv, NULL, NULL,
        CONFIG_DIAG_TICK_TASK_PRIORITY, 0U, K_NO_WAIT
    );
    k_thread_name_set(&s_tick_thread, "uds_tick_task");

    /*
     * Start DoIP server.
     * This registers the Zephyr BSD-socket platform ops with doip_server.c
     * and signals doip_thread (zephyr_lwip.c) to start — a semaphore, not
     * a fixed delay [EDS#192]. The thread then accepts TCP connections on
     * DOIP_ECU_LOGICAL_ADDR / DOIP_PORT.
     */
    status = eds_doip_platform_start(DOIP_ECU_LOGICAL_ADDR, DOIP_PORT, srv);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("DoIP platform start failed: 0x%02X", (unsigned)status);
        return -1;
    }

    LOG_INF("DoIP server activated — awaiting TCP connections on port %u",
            (unsigned)DOIP_PORT);

    /*
     * main() returns here. The Zephyr kernel continues scheduling the
     * doip_thread. In native_sim the process stays alive until killed.
     */
    return 0;
}
