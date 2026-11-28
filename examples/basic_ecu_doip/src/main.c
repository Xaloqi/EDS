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
 * THREAD MODEL [EDS#197]:
 *   main()        — init → uds_generated_init → start the executor + DoIP → return
 *   eds_exec      — the diagnostic executor (platform/zephyr/zephyr_exec.c),
 *                   the one thread that owns the UDS server. Woken every
 *                   1 ms by a k_timer, it runs uds_server_tick_1ms() and
 *                   uds_periodic_tick_1ms() once per millisecond actually
 *                   elapsed [EDS#401, EDS#411]; [EDS#191] in a DoIP-only
 *                   build nothing else progresses S3 and the SecurityAccess
 *                   lockout. It replaces uds_tick_task.
 *   doip_thread   — started via a semaphore (zephyr_lwip.c), not a fixed
 *                   delay [EDS#192]; runs eds_doip_server_run(), a producer
 *                   of the executor (eds_doip_set_executor(), EDS#197 J6):
 *                   it posts each request and waits for its answer, and
 *                   makes no UDS call itself. No mutex, no lock callbacks.
 *
 * WATCHDOG SUPERVISION [EDS#399] (real hardware, CONFIG_WATCHDOG=y; a
 * no-op on native_sim unless the selftest's counter watchdog is built in):
 *   The watchdog is armed in main() before the NVM store opens, and from
 *   the executor's start it is fed only by the executor loop, after each
 *   pass (exec_after_pass()). It feeds only while doip_thread has checked
 *   in, through eds_doip_set_checkin_callback(), within
 *   DOIP_SUPERVISION_WINDOW_MS. A stalled executor (a hung tick, a hung
 *   call, a DoIP dispatch hung while it holds the server), or doip_thread
 *   wedged on its socket or exited, all reset the ECU. An idle server with
 *   no client checks in every accept poll (5 s), so it is never reset for
 *   being idle. tests/zephyr_doip_selftest (WDT_STALL) shows the trip.
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

/* --------------------------------------------------------------------------
 * DoIP platform headers
 * -------------------------------------------------------------------------- */
#include "platform_api.h"    /* eds_platform_uptime_ms() */
#include "platform_doip.h"   /* eds_doip_platform_start() */
#include "doip_server.h"     /* DOIP_PORT */
#include "zephyr_exec.h"     /* [EDS#197] the diagnostic executor */
#include "zephyr_wdt.h"
#if defined(DIAG_DOIP_FLASH_BENCH)
#include "zephyr_flash_ops.h" /* [EDS#423] bench flash download */
#include "services.h"
#endif
#if defined(DIAG_BENCH_SLOW_ROUTINE)
#include "bench_slow_routine.h"   /* [EDS#282] bench slow routine */
#endif
#include "nvm_store.h"
#include "eds_nvm_diag_nvs.h"
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
#if (CONFIG_EDS_DOIP_EXAMPLE_NET_IF_MTU > 0)
#include <zephyr/net/net_if.h>
#endif

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

/* [EDS#399] Fed only by the executor loop, and only while doip_thread has
 * checked in recently. The window is the longest wait the server bounds
 * between check-ins (the accept poll with no client), plus margin for one
 * dispatch and its response send. See "WATCHDOG SUPERVISION" above. */
#define DOIP_SUPERVISION_WINDOW_MS  (DOIP_CHECKIN_MAX_WAIT_MS + 2000U)
static diag_wdt_t   s_wdt;
static atomic_t     s_doip_checkins;
static atomic_val_t s_doip_seen;        /* executor thread only */
static int64_t      s_doip_checkin_ms;  /* executor thread only */

/* [EDS#197] The one producer: DoIP. No periodic TX (decision E: 0x2A over
 * DoIP answers NRC 0x22 until EDS#546). */
static const uds_exec_producer_cfg_t s_producers[] = {
    { .src = (uint8_t)EDS_DOIP_UDS_SRC, .periodic_tx = false },
};

static void doip_checkin_cb(void)
{
    (void)atomic_inc(&s_doip_checkins);
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
 * Executor supervision [EDS#399, EDS#197]
 *
 * Runs on the executor thread after every pass (1 ms or sooner). A pass
 * that never ends (a hung dispatch, call or tick) stops the feed, and the
 * watchdog resets the ECU.
 * ============================================================================= */

/* [EDS#197] The executor performs an ECUReset once DoIP has handed the
 * response to the stack and released its slot: persist, let the response
 * leave, reset (EDS#434's sequence). Does not return for a supported type. */
static void exec_ecu_reset(uint8_t reset_type)
{
    (void)eds_platform_nvm_flush();
    (void)k_msleep((int32_t)DOIP_RESET_TX_GRACE_MS);
    (void)eds_platform_ecu_reset(reset_type);
}

static void exec_after_pass(void)
{
    int64_t      now_ms   = k_uptime_get();
    atomic_val_t doip_now = atomic_get(&s_doip_checkins);

    if (doip_now != s_doip_seen) {
        s_doip_seen       = doip_now;
        s_doip_checkin_ms = now_ms;
    }
    if ((now_ms - s_doip_checkin_ms) < (int64_t)DOIP_SUPERVISION_WINDOW_MS) {
        (void)diag_wdt_feed(&s_wdt);
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
     * [EDS#408] Arm the watchdog before the NVM store opens. On the append
     * backend nvm_store_init() may erase a 128 KB bank (up to 4 s on STM32H7)
     * on first boot or at compaction; the backend feeds the watchdog across
     * that erase (nvm_store_append_set_wdt, called by eds_nvm_init_diag_nvs()),
     * but only a watchdog that is armed and registered can be fed. Fail closed:
     * Zephyr's boot-time arming is off (boards/nucleo_h753zi/
     * nucleo_h753zi_doip.conf), so ignoring a failure here would run
     * unsupervised while appearing supervised (EDS#306's class). On native_sim
     * (CONFIG_WATCHDOG=n) this returns OK in degraded mode. From here on, a
     * `return -1` leaves no DoIP check-ins and ends in a watchdog reset on
     * hardware. The window is kept by the feeds after the NVM and UDS init
     * below, then by the executor loop.
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
     * [EDS#200] Must run before uds_generated_init() (dtc_mirror_init() at
     * Step 3.5 sits on top of nvm_store and silently no-ops until this
     * succeeds). eds_nvm_init_diag_nvs() resolves the board's diag_nvs
     * partition on hardware (append backend: STM32H7, MCX N947; EDS#408) and
     * is nvm_store_init(NULL) on native_sim and with -DDIAG_USE_MOCK_NVM=ON,
     * where the RAM mock ignores cfg.
     */
    status = eds_nvm_init_diag_nvs(&s_wdt);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("NVM store init failed: 0x%02X — DTC mirror will not "
                "survive reset this run.", (unsigned)status);
    }
    (void)diag_wdt_feed(&s_wdt);

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
     * runs in steps after a 0x78; the DoIP server drives those steps with
     * uds_server_poll() and sends the final response [EDS#282]. Registered
     * before uds_generated_init(), as zephyr_flash_ops.h requires. */
    LOG_WRN("[BENCH] Flash-download build: 0x34/0x36/0x37 write flash bank 2.");
    if (zephyr_flash_ops_init() != UDS_STATUS_OK) {
        LOG_ERR("Flash ops init failed.");
        return -1;
    }
#endif

    status = uds_generated_init(NULL, 0U, 0U);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS stack init failed: 0x%02X", (unsigned)status);
        return -1;
    }
    (void)diag_wdt_feed(&s_wdt);   /* [EDS#408] the executor, the first feeder, starts below */

#if defined(DIAG_BENCH_SLOW_ROUTINE)
    /* [EDS#282] Bench only: RID 0xF0F0 answers 0x78 (bench_slow_routine.h);
     * the DoIP server drives it with uds_server_poll(). */
    LOG_WRN("[BENCH] Slow routine 0xF0F0 registered (response-pending bench).");
    if (bench_slow_routine_register() != UDS_STATUS_OK) {
        LOG_ERR("Bench slow routine registration failed.");
        return -1;
    }
#endif

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

    /*
     * [EDS#197] Start the executor before the DoIP server, so S3/lockout
     * timing and the watchdog feed are live from the moment a connection
     * can arrive. Time of the last DoIP check-in starts now, so a DoIP
     * thread that never starts is caught one window after this point.
     */
    s_doip_seen       = atomic_get(&s_doip_checkins);
    s_doip_checkin_ms = k_uptime_get();
    {
        const zephyr_exec_cfg_t ec = {
            .srv            = srv,
            .producers      = s_producers,
            .producer_count = (uint8_t)ARRAY_SIZE(s_producers),
            .reset          = exec_ecu_reset,
            .after_pass     = exec_after_pass,
        };

        status = zephyr_exec_start(&ec);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("Executor start failed: 0x%02X", (unsigned)status);
            return -1;
        }
    }
    (void)eds_doip_set_executor(zephyr_exec_get(), zephyr_exec_wait_event);
    eds_doip_set_checkin_callback(doip_checkin_cb);   /* [EDS#399] */

    /*
     * [EDS#392] Vehicle discovery: on by default (CONFIG_EDS_DOIP_DISCOVERY),
     * configured from ecu.doip.discovery in diagnostics_config.yaml. The
     * generated config reads the VIN through the 0xF190 safety wrapper, on
     * the executor, like UDS dispatch; if 0xF190 is restricted the response
     * says "VIN not available".
     */
    eds_doip_set_discovery(uds_generated_doip_discovery());

#if (CONFIG_EDS_DOIP_EXAMPLE_NET_IF_MTU > 0)
    /*
     * [EDS#648] Cap the MSS this board advertises, before the first
     * connection: Zephyr sends MTU - 40 in every SYN-ACK. See the Kconfig
     * help and boards/frdm_mcxn947/frdm_mcxn947_doip.conf for the value.
     */
    {
        struct net_if *iface = net_if_get_default();

        if (iface == NULL) {
            LOG_ERR("No default network interface for the MTU of EDS#648");
            return -1;
        }
        net_if_set_mtu(iface, (uint16_t)CONFIG_EDS_DOIP_EXAMPLE_NET_IF_MTU);
        LOG_INF("Network interface MTU set to %u", (unsigned)net_if_get_mtu(iface));
    }
#endif

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
