/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/dual_transport_ecu/src/main.c
 *
 * PURPOSE: [EDS#197] CAN and DoIP on one ECU. Both transports are producers
 *          of the diagnostic executor, the one thread that owns the UDS
 *          server (platform/zephyr/zephyr_exec.c, core/uds_exec.h).
 *
 * THREAD MODEL:
 *   main()      — init, then starts the threads below and returns.
 *   can_task    — priority 4, every 1 ms: can_producer_step() (src/
 *                 can_producer.c). Drains every queued CAN frame (EDS#558),
 *                 ticks ISO-TP, posts a reassembled request without waiting
 *                 for it, sends finished responses and routed frames. Above
 *                 the executor, so RX keeps draining while a dispatch runs.
 *   eds_exec    — priority 5: 1 ms ticks (S3, lockout, periodic), every
 *                 dispatch, round-robin over the two producers, the pending
 *                 job, the ECUReset. Feeds the watchdog after each pass.
 *   doip_thread — priority 7 (zephyr_lwip.c): eds_doip_server_run() with
 *                 eds_doip_set_executor(): posts each request and waits.
 *
 *   A session belongs to the transport that opened it (decision ⬥A): while
 *   CAN holds an extended session, DoIP's physical requests are answered
 *   7F sid 21, and the other way round.
 *
 * CAN classic or CAN FD: the example sets up its own ISO-TP channel from the
 *   generated CAN IDs (uds_generated_init() gets no CAN transport), as a CAN
 *   FD channel when CONFIG_CAN_FD_MODE=y (fd.conf), classic otherwise.
 *
 * WATCHDOG (hardware, CONFIG_WATCHDOG=y): fed by the executor after each
 *   pass, only while can_task has run within CAN_SUPERVISION_WINDOW_MS and
 *   the DoIP server has checked in within DOIP_SUPERVISION_WINDOW_MS. A
 *   stalled executor, CAN loop or DoIP thread resets the ECU.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "uds_types.h"
#include "uds_server.h"
#include "uds_session.h"
#include "uds_periodic.h"
#include "uds_security_algo.h"
#include "isotp.h"
#include "can_transport.h"
#include "zephyr_port.h"
#include "platform_api.h"
#include "platform_doip.h"
#include "doip_server.h"
#include "zephyr_exec.h"
#include "zephyr_wdt.h"
#include "eds_nvm_diag_nvs.h"
#include "uds_init.h"
#include "generated_config.h"
#include "can_producer.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#if (CONFIG_EDS_DOIP_EXAMPLE_NET_IF_MTU > 0)
#include <zephyr/net/net_if.h>
#endif

LOG_MODULE_REGISTER(dual_transport_ecu, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------*/

#define DIAG_CAN_DEV           DEVICE_DT_GET(DT_ALIAS(can0))
#define DOIP_ECU_LOGICAL_ADDR  (0xE400U)   /* diagnostics_config.yaml ecu.doip */
#define SRC_CAN                (0U)        /* EDS_DOIP_UDS_SRC is 1 */

#define CAN_TASK_STACK_SIZE    (2048U)
#define CAN_TASK_PRIORITY      (4)

/* The DoIP server's longest bounded wait between check-ins, plus margin for
 * one dispatch and its send (as basic_ecu_doip). The CAN loop runs every
 * 1 ms; 100 ms of silence means it has stopped. */
#define DOIP_SUPERVISION_WINDOW_MS  (DOIP_CHECKIN_MAX_WAIT_MS + 2000U)
#define CAN_SUPERVISION_WINDOW_MS   (100U)

#if defined(CONFIG_CAN_FD_MODE)
#define DUAL_CAN_FD_TX_DL      (64U)
#endif

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------*/

K_THREAD_STACK_DEFINE(s_can_stack, CAN_TASK_STACK_SIZE);
static struct k_thread s_can_thread;
static struct k_timer  s_can_tick;

static isotp_ctx_t    s_tp;
static can_producer_t s_can_prod;
static diag_wdt_t     s_wdt;

static atomic_t     s_doip_checkins;
static atomic_t     s_can_steps;
static atomic_val_t s_doip_seen;       /* executor thread only */
static atomic_val_t s_can_seen;        /* executor thread only */
static int64_t      s_doip_seen_ms;    /* executor thread only */
static int64_t      s_can_seen_ms;     /* executor thread only */

/* CAN: periodic messages go out as UUDT frames (decision ⬥E). DoIP has no
 * periodic TX yet, so 0x2A over DoIP answers NRC 0x22 (EDS#546). */
static const uds_exec_producer_cfg_t s_producers[] = {
    { .src = SRC_CAN,                   .periodic_tx = true  },
    { .src = (uint8_t)EDS_DOIP_UDS_SRC, .periodic_tx = false },
};

/* ---------------------------------------------------------------------------
 * Executor bindings
 * ---------------------------------------------------------------------------*/

/* An ECUReset, once the producer that asked for it has handed the response
 * to its transport and released its slot: persist, let the response leave
 * (ISO-TP's As or the TCP stack: the same 50 ms), reset. */
static void exec_ecu_reset(uint8_t reset_type)
{
    (void)eds_platform_nvm_flush();
    (void)k_msleep((int32_t)DOIP_RESET_TX_GRACE_MS);
    (void)eds_platform_ecu_reset(reset_type);
}

/* After every executor pass: feed only while both producers are alive. */
static void exec_after_pass(void)
{
    int64_t      now  = k_uptime_get();
    atomic_val_t doip = atomic_get(&s_doip_checkins);
    atomic_val_t can  = atomic_get(&s_can_steps);

    if (doip != s_doip_seen) {
        s_doip_seen    = doip;
        s_doip_seen_ms = now;
    }
    if (can != s_can_seen) {
        s_can_seen    = can;
        s_can_seen_ms = now;
    }
    if (((now - s_doip_seen_ms) < (int64_t)DOIP_SUPERVISION_WINDOW_MS) &&
        ((now - s_can_seen_ms) < (int64_t)CAN_SUPERVISION_WINDOW_MS)) {
        (void)diag_wdt_feed(&s_wdt);
    }
}

static void doip_checkin_cb(void)
{
    (void)atomic_inc(&s_doip_checkins);
}

static void s_on_session_change(uds_session_type_t old_sess, uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

/* ---------------------------------------------------------------------------
 * CAN thread: the producer, never blocked by the executor
 * ---------------------------------------------------------------------------*/

static void can_task_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    k_timer_init(&s_can_tick, NULL, NULL);
    k_timer_start(&s_can_tick, K_MSEC(1), K_MSEC(1));
    for (;;) {
        (void)k_timer_status_sync(&s_can_tick);
        can_producer_step(&s_can_prod);
        (void)atomic_inc(&s_can_steps);
    }
}

/* The CAN channel, classic or FD (see the file header). */
static uds_status_t can_channel_init(can_transport_t *can)
{
    isotp_cfg_t cfg;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.rx_can_id            = (uint32_t)GEN_CAN_RX_ID;
    cfg.tx_can_id            = (uint32_t)GEN_CAN_TX_ID;
    cfg.functional_rx_can_id = (uint32_t)GEN_CAN_FUNCTIONAL_RX_ID;
    cfg.periodic_tx_can_id   = (uint32_t)GEN_CAN_PERIODIC_TX_ID;
    cfg.block_size           = (uint8_t)ISOTP_DEFAULT_BLOCK_SIZE;
    cfg.stmin_ms             = (uint8_t)ISOTP_DEFAULT_STMIN_MS;
#if defined(CONFIG_CAN_FD_MODE)
    cfg.use_fd               = true;
    cfg.tx_dl                = (uint8_t)DUAL_CAN_FD_TX_DL;
    cfg.brs                  = true;
#endif
    cfg.can                  = can;
    return isotp_init(&s_tp, &cfg);
}

/* ---------------------------------------------------------------------------
 * main()
 * ---------------------------------------------------------------------------*/

int main(void)
{
    uds_status_t      status;
    can_transport_t  *can = NULL;
    uds_server_ctx_t *srv = NULL;

    LOG_INF("Xaloqi EDS " GEN_ECU_NAME " v" GEN_ECU_VERSION ": CAN %s + DoIP 0x%04X:%u",
            IS_ENABLED(CONFIG_CAN_FD_MODE) ? "FD" : "classic",
            (unsigned)DOIP_ECU_LOGICAL_ADDR, (unsigned)DOIP_PORT);
    LOG_WRN("[SEC] Placeholder AES keys: inject OEM keys before production.");
    (void)uds_security_algo_set_rng_cb(NULL);

    /* [EDS#408] Armed before the NVM store opens (an append-backend erase
     * feeds it); fail closed. A no-op on native_sim (CONFIG_WATCHDOG=n). */
    if (diag_wdt_init(&s_wdt) != UDS_STATUS_OK) {
        LOG_ERR("Watchdog arming failed: refusing to run unsupervised.");
        return -1;
    }

    {
        const zephyr_port_cfg_t port_cfg = {
            .can_dev        = DIAG_CAN_DEV,
            .physical_rx_id = (uint32_t)GEN_CAN_RX_ID,
        };
        status = zephyr_port_init(&port_cfg, &can);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("CAN platform init failed: 0x%02X", (unsigned)status);
            return -1;
        }
    }

    /* [EDS#200] Before uds_generated_init(): the DTC mirror sits on it. */
    status = eds_nvm_init_diag_nvs(&s_wdt);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("NVM store init failed: 0x%02X; the DTC mirror will not survive a reset.",
                (unsigned)status);
    }
    (void)diag_wdt_feed(&s_wdt);

    /* No CAN transport here: the example owns the ISO-TP channel (above). */
    status = uds_generated_init(NULL, 0U, 0U);
    srv = uds_generated_get_server();
    if ((status != UDS_STATUS_OK) || (srv == NULL)) {
        LOG_ERR("UDS stack init failed: 0x%02X", (unsigned)status);
        return -1;
    }
    status = can_channel_init(can);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("ISO-TP init failed: 0x%02X", (unsigned)status);
        return -1;
    }
    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx, s_on_session_change);
    (void)diag_wdt_feed(&s_wdt);

    /* The executor first: S3, lockout and the watchdog feed are live before
     * either transport can deliver a request. Both supervision clocks start
     * now, so a producer that never starts is caught one window later. */
    s_doip_seen_ms = k_uptime_get();
    s_can_seen_ms  = s_doip_seen_ms;
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

    /* CAN producer and its thread. */
    can_producer_init(&s_can_prod, zephyr_exec_get(), SRC_CAN, &s_tp, can);
    (void)k_thread_create(&s_can_thread, s_can_stack, K_THREAD_STACK_SIZEOF(s_can_stack),
                          can_task_entry, NULL, NULL, NULL,
                          CAN_TASK_PRIORITY, 0U, K_NO_WAIT);
    (void)k_thread_name_set(&s_can_thread, "can_task");

    /* DoIP producer. Discovery as generated (transport: both). */
    (void)eds_doip_set_executor(zephyr_exec_get(), zephyr_exec_wait_event);
    eds_doip_set_checkin_callback(doip_checkin_cb);   /* [EDS#399] */
    eds_doip_set_discovery(uds_generated_doip_discovery());

#if (CONFIG_EDS_DOIP_EXAMPLE_NET_IF_MTU > 0)
    /* [EDS#648] Cap the MSS this board advertises, before the first
     * connection (see the Kconfig help and the FRDM board conf). */
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

    status = eds_doip_platform_start(DOIP_ECU_LOGICAL_ADDR, DOIP_PORT, srv);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("DoIP platform start failed: 0x%02X", (unsigned)status);
        return -1;
    }
    LOG_INF("Ready: CAN 0x%03X/0x%03X (functional 0x%03X), DoIP port %u",
            (unsigned)GEN_CAN_RX_ID, (unsigned)GEN_CAN_TX_ID,
            (unsigned)GEN_CAN_FUNCTIONAL_RX_ID, (unsigned)DOIP_PORT);
    return 0;
}
