/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/canfd_ecu/src/main.c
 *
 * PURPOSE: CAN FD example ECU (EDS#557, EDS#546) — application entry point.
 *
 *          diagnostics_config.yaml sets can.fd: true, so the generated
 *          uds_init.c makes the ISO-TP channel a CAN FD channel (use_fd,
 *          TX_DL 64, BRS). Nothing in this file is FD-specific: the loop is
 *          the same as every other example's. What changes is on the wire:
 *          every frame is an FD frame, responses up to 62 bytes are one
 *          escape Single Frame, longer ones FF + 64-byte CFs, and the 0x2A
 *          periodic record of 0xF201 (20 bytes) is one 24-byte FD frame.
 *
 *          The only application code is did_read_fdperiodicstatus() for
 *          0xF201 (source: app): uptime in ms, then a fixed pattern.
 *
 * THREAD MODEL (same as basic_ecu):
 *   main()        → init + start diag_task → returns
 *   diag_task     → 1 ms poll loop (ISO-TP + UDS dispatch)
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "uds_types.h"
#include "uds_server.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "isotp.h"
#include "can_transport.h"
#include "zephyr_port.h"
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"
#include "nvm_store.h"
#include "uds_init.h"
#include "uds_security_algo.h"
#include "generated_config.h"
#include "did_handlers.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(canfd_ecu, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------*/

#define DIAG_CAN_DEV            DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID          ((uint32_t)GEN_CAN_RX_ID)   /* 0x7E0, physical */
#define DIAG_TX_CAN_ID          ((uint32_t)GEN_CAN_TX_ID)   /* 0x7E8 */

#ifndef CONFIG_DIAG_TASK_STACK_SIZE
#define CONFIG_DIAG_TASK_STACK_SIZE  (4096U)
#endif

#ifndef CONFIG_DIAG_TASK_PRIORITY
#define CONFIG_DIAG_TASK_PRIORITY    (5)
#endif

/* ---------------------------------------------------------------------------
 * Static allocations
 * ---------------------------------------------------------------------------*/

K_THREAD_STACK_DEFINE(s_diag_stack, CONFIG_DIAG_TASK_STACK_SIZE);
static struct k_thread s_diag_thread;

static uds_msg_buf_t s_req_buf;
static uds_msg_buf_t s_resp_buf;

static diag_mutex_t s_session_lock;
static diag_mutex_t s_security_lock;
static diag_timer_t s_tick_timer;
static diag_wdt_t   s_wdt;

/* ---------------------------------------------------------------------------
 * 0xF201 FdPeriodicStatus (source: app): uptime in ms (uint32 BE), then
 * 0xA0..0xAF. Read by 0x22 and, once subscribed, by SID 0x2A.
 * ---------------------------------------------------------------------------*/

#define FD_PERIODIC_STATUS_LEN  (20U)

uds_status_t did_read_fdperiodicstatus(uint8_t *buf, uint16_t buf_len,
                                       uint16_t *out_len)
{
    uint32_t up;
    uint16_t i;

    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)FD_PERIODIC_STATUS_LEN) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    up = k_uptime_get_32();
    buf[0] = (uint8_t)(up >> 24U);
    buf[1] = (uint8_t)(up >> 16U);
    buf[2] = (uint8_t)(up >> 8U);
    buf[3] = (uint8_t)up;
    for (i = 4U; i < (uint16_t)FD_PERIODIC_STATUS_LEN; i++) {
        buf[i] = (uint8_t)(0xA0U + (uint8_t)(i - 4U));
    }
    *out_len = (uint16_t)FD_PERIODIC_STATUS_LEN;
    return UDS_STATUS_OK;
}

/* ---------------------------------------------------------------------------
 * TRNG / security init
 * ---------------------------------------------------------------------------*/

#if defined(CONFIG_ENTROPY_GENERATOR)
#include <zephyr/drivers/entropy.h>
static const struct device *s_entropy_dev = NULL;

static uds_status_t trng_cb(uint8_t *buf, uint8_t len)
{
    if ((s_entropy_dev == NULL) || !device_is_ready(s_entropy_dev)) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    return (entropy_get_entropy(s_entropy_dev, buf, (size_t)len) == 0)
           ? UDS_STATUS_OK : UDS_STATUS_ERR_PLATFORM;
}
#endif

static void security_init(void)
{
#if defined(CONFIG_ENTROPY_GENERATOR)
    s_entropy_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
    if (device_is_ready(s_entropy_dev)) {
        uds_security_algo_set_rng_cb(trng_cb);
        LOG_INF("TRNG registered: %s", s_entropy_dev->name);
    } else {
        LOG_WRN("TRNG not ready — LFSR fallback (dev/CI only).");
    }
#else
    LOG_WRN("No entropy device — LFSR fallback (dev/CI only).");
#endif
    /*
     * OEM key injection placeholder.
     * Replace with keys loaded from secure storage or provisioned at
     * manufacturing time before deploying to production hardware.
     */
    LOG_WRN("Placeholder AES keys active — inject real keys before production.");
}

/* ---------------------------------------------------------------------------
 * ISO-TP RX completion callback
 * ---------------------------------------------------------------------------*/

static void on_isotp_rx_complete(const uint8_t *data, uint32_t length, void *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();
    uds_status_t      rc;
    uint16_t          i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) { return; }
    if ((length == 0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) { return; }

    for (i = 0U; i < (uint16_t)length; i++) { s_req_buf.data[i] = data[i]; }
    s_req_buf.length  = (uint16_t)length;
    s_resp_buf.length = 0U;

    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    /* [EDS-toolchain#165] A functional request (ISO 15765-4) must not draw
     * the NRCs ISO 14229-1 Table 4 says to suppress. */
    uds_addressing_t addressing = UDS_ADDRESSING_PHYSICAL;
    (void)isotp_get_rx_addressing(tp, &addressing);
    rc = uds_server_process_request_ex(srv, &s_req_buf, &s_resp_buf, addressing);
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);

    if (s_resp_buf.length > 0U) {
        (void)isotp_transmit(tp, s_resp_buf.data, s_resp_buf.length);
    } else if (rc != UDS_STATUS_OK) {
        LOG_ERR("UDS error 0x%02X, no response.", (unsigned)rc);
    }

    /* [P2-0x11-01] Deferred reset: response is on the wire, now reset. */
    if (srv->pending_reset_type != (uint8_t)0U) {
        uint8_t reset_type = srv->pending_reset_type;
        srv->pending_reset_type = (uint8_t)0U;
        (void)zephyr_port_nvm_flush();
        k_msleep(50);  /* Allow ISO-TP TX to reach the wire before reset (As timer). */
        (void)zephyr_port_ecu_reset(reset_type);
        /* Does not return. */
    }
}

/* ---------------------------------------------------------------------------
 * 1 ms tick
 * ---------------------------------------------------------------------------*/

typedef struct { uds_server_ctx_t *srv; isotp_ctx_t *tp; } tick_ctx_t;

static void on_tick(void *arg)
{
    tick_ctx_t *ctx = (tick_ctx_t *)arg;
    if ((ctx == NULL) || (ctx->tp == NULL) || (ctx->srv == NULL)) { return; }
    (void)isotp_tick_1ms(ctx->tp);
    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    (void)uds_server_tick_1ms(ctx->srv);
    (void)uds_periodic_tick_1ms();
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);
}

/* ---------------------------------------------------------------------------
 * Diagnostics task
 * ---------------------------------------------------------------------------*/

static void diag_task_entry(void *p1, void *p2, void *p3)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)p1;
    can_transport_t  *can = (can_transport_t  *)p2;
    isotp_ctx_t      *tp  = (isotp_ctx_t      *)p3;

    uds_status_t    rc;
    uds_can_frame_t rx_frame;
    bool            frame_ready;
    tick_ctx_t      tick_ctx = { .srv = srv, .tp = tp };

    if (diag_timer_start(&s_tick_timer) != UDS_STATUS_OK) {
        LOG_ERR("Timer start failed."); return;
    }

    LOG_INF("Diagnostics task running.");

    while (true) {
        (void)diag_timer_wait_tick(&s_tick_timer, 2U);

        /* [EDS#558] Drain every queued frame, at most the queue depth. The Flow
         * Control BlockSize keeps a burst within the RX queue only if each
         * iteration empties it; one frame per tick loses CFs of long requests. */
        frame_ready = true;
        for (uint32_t rx_n = 0U;
             frame_ready && (rx_n < (uint32_t)EDS_CAN_RX_QUEUE_DEPTH); rx_n++) {
            frame_ready = false;
            rc = can_transport_receive(can, &rx_frame, &frame_ready);

            if (rc == UDS_STATUS_ERR_CAN_BUS_OFF) {
                LOG_ERR("CAN bus-off.");
            } else if ((rc == UDS_STATUS_OK) && frame_ready) {
                (void)isotp_process_rx_frame(tp, &rx_frame,
                                              on_isotp_rx_complete, (void *)srv);
            }
        }

        on_tick(&tick_ctx);

        {
            static uds_msg_buf_t s_periodic_frame;
            while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
                /* [EDS#546] A periodic message is a UUDT frame on its own CAN ID
                 * (ISO 14229-3 §8.5): it bypasses the ISO-TP channel, so it can
                 * go out while a multi-frame response is being sent. On this
                 * FD channel [pDID + 20 bytes] is one 24-byte FD frame.
                 * [EDS#194] Re-queue if the CAN driver refuses it instead of
                 * silently dropping this period; retry on the next 1ms tick and
                 * stop draining this tick. */
                if (isotp_transmit_uudt(tp, s_periodic_frame.data,
                                   (uint32_t)s_periodic_frame.length) != UDS_STATUS_OK) {
                    (void)uds_periodic_requeue_last();
                    break;
                }
            }
        }

        (void)diag_wdt_feed(&s_wdt);
    }
}

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

/* ---------------------------------------------------------------------------
 * main()
 * ---------------------------------------------------------------------------*/

int main(void)
{
    uds_status_t      rc;
    can_transport_t  *can = NULL;
    uds_server_ctx_t *srv = NULL;
    isotp_ctx_t      *tp  = NULL;

    LOG_INF("=============================================================");
    LOG_INF("Xaloqi EDS  v" GEN_ECU_VERSION "  —  CanFdEcu");
    LOG_INF("  DIDs: %u   CAN FD: TX_DL %u, BRS %u",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_CAN_FD_TX_DL, (unsigned)GEN_CAN_FD_BRS);
    LOG_INF("  0xF190 (20 B answer: one FD SF) · 0x0100-0x0102 (64 B each: FF + CFs)");
    LOG_INF("  0x2A on 0xF201: 20-byte record, one FD frame on 0x5E8");
    LOG_INF("=============================================================");

    /* Mutex init */
    if ((diag_mutex_init(&s_session_lock)  != UDS_STATUS_OK) ||
        (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK)) {
        LOG_ERR("Mutex init failed."); return -1;
    }

    /* Watchdog */
    (void)diag_wdt_init(&s_wdt);

    /* 1 ms timer */
    rc = diag_timer_init(&s_tick_timer, on_tick, NULL);
    if (rc != UDS_STATUS_OK) {
        LOG_ERR("Timer init failed: 0x%02X", (unsigned)rc); return -1;
    }

    /* CAN transport */
    const zephyr_port_cfg_t port_cfg = {
        .can_dev        = DIAG_CAN_DEV,
        .physical_rx_id = DIAG_RX_CAN_ID,   /* alongside the 0x7DF functional filter */
    };
    rc = zephyr_port_init(&port_cfg, &can);
    if (rc != UDS_STATUS_OK) {
        LOG_ERR("Platform init failed: 0x%02X", (unsigned)rc); return -1;
    }

    /* NVM store */
    /*
     * [EDS#200] Must run before uds_generated_init() (dtc_mirror_init() at
     * Step 3.5 sits on top of nvm_store and silently no-ops until this
     * succeeds). This example only targets native_sim, where
     * platform/zephyr/nvm_store_mock.c (RAM-backed) is linked instead of
     * the real NVS backend — its nvm_store_init() ignores cfg entirely, so
     * NULL is correct here (see nvm_store.h: "may be NULL on host").
     */
    rc = nvm_store_init(NULL);
    if (rc != UDS_STATUS_OK) {
        LOG_ERR("NVM store init failed: 0x%02X — DTC mirror will not "
                "survive reset this run.", (unsigned)rc);
    }

    /* Security */
    security_init();

    /* UDS stack */
    rc = uds_generated_init(can, DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);
    if (rc != UDS_STATUS_OK) {
        LOG_ERR("UDS init failed: 0x%02X", (unsigned)rc); return -1;
    }

    srv = uds_generated_get_server();
    tp  = uds_generated_get_isotp();
    if ((srv == NULL) || (tp == NULL)) {
        LOG_ERR("NULL context after init."); return -1;
    }

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    LOG_INF("UDS stack ready — awaiting diagnostic requests on CAN 0x7E0 (physical) / 0x7DF (functional)");

    /* Start diagnostics thread */
    (void)k_thread_create(
        &s_diag_thread, s_diag_stack,
        K_THREAD_STACK_SIZEOF(s_diag_stack),
        diag_task_entry,
        (void *)srv, (void *)can, (void *)tp,
        CONFIG_DIAG_TASK_PRIORITY, 0U, K_NO_WAIT
    );
    k_thread_name_set(&s_diag_thread, "diag_task");

    return 0;
}
