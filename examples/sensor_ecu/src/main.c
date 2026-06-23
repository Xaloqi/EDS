/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu/src/main.c
 *
 * PURPOSE: SensorECU — zone controller with real sensor integration.
 *
 *          Demonstrates the complete Xaloqi EDS sensor pattern:
 *
 *            Zephyr sensor API (temp, voltage)
 *              ↓
 *            sensor_monitor thread (100 ms cycle)
 *              ↓ dtc_database_set_status()
 *            DTC database (0xD00101, 0xD00102, 0xD00201, 0xD00202)
 *              ↓
 *            UDS 0x19 ReadDTCInformation → tester reads active faults
 *            UDS 0x22 ReadDataByIdentifier → tester reads live sensor values
 *
 *          Two threads run concurrently:
 *            diag_task     — 1 ms poll loop (ISO-TP + UDS dispatch)
 *            sensor_monitor — 100 ms sensor read + DTC update
 *
 * THREAD MODEL:
 *   main()
 *     ├─ sensor_monitor_init()   → starts sensor_monitor thread
 *     ├─ uds_generated_init()    → initialises UDS stack
 *     └─ diag task created       → starts diagnostic poll loop
 *
 *   sensor_monitor thread (priority 7, 100 ms sleep):
 *     sensor_sample_fetch() → dtc_database_set_status() → k_mutex_unlock()
 *
 *   diag_task thread (priority 5, 1 ms tick):
 *     can_transport_receive() → isotp_process_rx_frame() → uds_server_process_request()
 *     DID 0xD001/0xD002/0xD003 handlers call sensor_state_get() (mutex-protected)
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "sensor_ecu.h"
#include "uds_types.h"
#include "uds_server.h"
#include "isotp.h"
#include "can_transport.h"
#include "zephyr_port.h"
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"
#include "uds_init.h"
#include "uds_security_algo.h"
#include "generated_config.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor_ecu, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------*/

#define DIAG_CAN_DEV            DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID          ((uint32_t)GEN_CAN_RX_ID)
#define DIAG_TX_CAN_ID          ((uint32_t)GEN_CAN_TX_ID)

#ifndef CONFIG_DIAG_TASK_STACK_SIZE
#define CONFIG_DIAG_TASK_STACK_SIZE  (4096U)
#endif

#ifndef CONFIG_DIAG_TASK_PRIORITY
#define CONFIG_DIAG_TASK_PRIORITY    (5)
#endif

#define DIAG_OVERRUN_LOG_THRESHOLD   (3U)

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
 * TRNG callback (same pattern as basic_ecu)
 * ---------------------------------------------------------------------------*/

#if defined(CONFIG_ENTROPY_GENERATOR)
#include <zephyr/drivers/entropy.h>
static const struct device *s_entropy_dev = NULL;

static uds_status_t diag_trng_cb(uint8_t *buf, uint8_t len)
{
    if ((s_entropy_dev == NULL) || !device_is_ready(s_entropy_dev)) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    int rc = entropy_get_entropy(s_entropy_dev, buf, (size_t)len);
    return (rc == 0) ? UDS_STATUS_OK : UDS_STATUS_ERR_PLATFORM;
}
#endif

static int diag_security_algo_init(void)
{
#if defined(CONFIG_ENTROPY_GENERATOR)
    s_entropy_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
    if (!device_is_ready(s_entropy_dev)) {
        LOG_WRN("TRNG not ready — LFSR fallback.");
        s_entropy_dev = NULL;
    } else {
        uds_security_algo_set_rng_cb(diag_trng_cb);
        LOG_INF("TRNG: %s", s_entropy_dev->name);
    }
#else
    LOG_WRN("No entropy device — LFSR fallback (dev/CI only).");
#endif
    LOG_WRN("Placeholder AES keys active — inject OEM keys before production.");
    return 0;
}

/* ---------------------------------------------------------------------------
 * ISO-TP RX completion callback
 * ---------------------------------------------------------------------------*/

static void on_isotp_rx_complete(const uint8_t *data, uint32_t length, void *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();
    uds_status_t      status;
    uint16_t          i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) {
        return;
    }
    if ((length == 0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) {
        return;
    }

    for (i = 0U; i < (uint16_t)length; i++) {
        s_req_buf.data[i] = data[i];
    }
    s_req_buf.length  = (uint16_t)length;
    s_resp_buf.length = 0U;

    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    status = uds_server_process_request(srv, &s_req_buf, &s_resp_buf);
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);

    if (s_resp_buf.length > 0U) {
        (void)isotp_transmit(tp, s_resp_buf.data, s_resp_buf.length);
    } else if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS error 0x%02X, no response.", (unsigned)status);
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

    uds_status_t    status;
    uds_can_frame_t rx_frame;
    bool            frame_ready;
    uint32_t        overrun_count;
    uint32_t        prev_overrun = 0U;

    tick_ctx_t tick_ctx = { .srv = srv, .tp = tp };

    LOG_INF("Diagnostics task started.");

    if (diag_timer_start(&s_tick_timer) != UDS_STATUS_OK) {
        LOG_ERR("Timer start failed.");
        return;
    }

    while (true) {
        status = diag_timer_wait_tick(&s_tick_timer, 2U);
        if (status == UDS_STATUS_ERR_TIMEOUT) {
            LOG_WRN("1 ms tick timeout.");
        }

        frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);

        if (status == UDS_STATUS_ERR_CAN_BUS_OFF) {
            LOG_ERR("CAN bus-off.");
        } else if ((status == UDS_STATUS_OK) && frame_ready) {
            (void)isotp_process_rx_frame(tp, &rx_frame,
                                          on_isotp_rx_complete, (void *)srv);
        }

        on_tick(&tick_ctx);
        (void)diag_wdt_feed(&s_wdt);

        (void)diag_timer_pending_ticks(&s_tick_timer, &overrun_count);
        if ((overrun_count - prev_overrun) >= DIAG_OVERRUN_LOG_THRESHOLD) {
            LOG_WRN("Poll loop overrun: %u ticks missed.",
                    (unsigned)(overrun_count - prev_overrun));
            prev_overrun = overrun_count;
        }
    }
}

/* ---------------------------------------------------------------------------
 * main()
 * ---------------------------------------------------------------------------*/

int main(void)
{
    uds_status_t      status;
    can_transport_t  *can = NULL;
    uds_server_ctx_t *srv = NULL;
    isotp_ctx_t      *tp  = NULL;

    LOG_INF("=============================================================");
    LOG_INF("Xaloqi EDS  v" GEN_ECU_VERSION "  —  SensorECU");
    LOG_INF("  DIDs: %u   DTCs: %u",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT);
    LOG_INF("  RX: 0x%03X   TX: 0x%03X",
            (unsigned)DIAG_RX_CAN_ID, (unsigned)DIAG_TX_CAN_ID);
    LOG_INF("=============================================================");

    /* ── Sensor monitor (starts sensor_monitor thread) ───────────────────── */
    sensor_monitor_init();
    LOG_INF("Sensor monitor initialised.");

    /* ── Mutex init ──────────────────────────────────────────────────────── */
    if ((diag_mutex_init(&s_session_lock)  != UDS_STATUS_OK) ||
        (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK)) {
        LOG_ERR("Mutex init failed.");
        return -1;
    }

    /* ── Watchdog ────────────────────────────────────────────────────────── */
    (void)diag_wdt_init(&s_wdt);

    /* ── Timer ───────────────────────────────────────────────────────────── */
    status = diag_timer_init(&s_tick_timer, on_tick, NULL);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("Timer init failed.");
        return -1;
    }

    /* ── CAN transport ───────────────────────────────────────────────────── */
    const zephyr_port_cfg_t port_cfg = { .can_dev = DIAG_CAN_DEV };
    status = zephyr_port_init(&port_cfg, &can);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("Platform init failed.");
        return -1;
    }

    /* ── Security algo ───────────────────────────────────────────────────── */
    (void)diag_security_algo_init();

    /* ── UDS stack ───────────────────────────────────────────────────────── */
    status = uds_generated_init(can, DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS init failed: 0x%02X", (unsigned)status);
        return -1;
    }

    srv = uds_generated_get_server();
    tp  = uds_generated_get_isotp();

    if ((srv == NULL) || (tp == NULL)) {
        LOG_ERR("NULL context after init.");
        return -1;
    }

    LOG_INF("UDS stack ready. Sensor DIDs: 0xD001 (temp), 0xD002 (voltage), 0xD003 (status)");
    LOG_INF("Active DTCs visible via UDS 0x19 subfunction 0x02.");

    /* ── Diagnostics thread ──────────────────────────────────────────────── */
    (void)k_thread_create(
        &s_diag_thread, s_diag_stack,
        K_THREAD_STACK_SIZEOF(s_diag_stack),
        diag_task_entry,
        (void *)srv, (void *)can, (void *)tp,
        CONFIG_DIAG_TASK_PRIORITY, 0U, K_NO_WAIT
    );
    k_thread_name_set(&s_diag_thread, "diag_task");

    LOG_INF("SensorECU running.");
    return 0;
}
