/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/robot_joint_controller_ecu/src/main.c
 *
 * PURPOSE: Robot joint controller ECU — application entry point.
 *
 *          Single-axis robot joint controller exposing live joint state
 *          (position, velocity, torque, motor temperature) and fault
 *          history over ISO 14229 UDS / ISO-TP on CAN.
 *
 *          This example demonstrates that Xaloqi EDS works identically
 *          in robotics and automotive contexts. The UDS protocol, ASIL-B
 *          safety wrappers, and YAML-driven codegen are unchanged. Only
 *          the DID handler implementations change to read from your motor
 *          controller API instead of an automotive ECU signal bus.
 *
 * INTEGRATION PATTERN:
 *
 *   Your motor controller / robot OS provides:
 *     - Joint position (encoder counts → millidegrees)
 *     - Joint velocity (rad/s or rpm)
 *     - Motor torque (Nm)
 *     - Motor temperature (°C)
 *
 *   You wire these into the DID handlers in generated/did_handlers.c.
 *   The UDS session management, security access, DTC persistence, and
 *   ISO-TP framing are handled entirely by Xaloqi EDS.
 *
 * SUPPORTED INTERFACES:
 *   Zephyr: motor driver API (CONFIG_MOTOR=y), ADC for temperature,
 *           encoder via QDEC driver, or custom CAN-based servo driver
 *   FreeRTOS: replace Zephyr APIs in did_handlers.c with your vendor HAL.
 *             See examples/basic_ecu_freertos/ for FreeRTOS platform wiring.
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

LOG_MODULE_REGISTER(robot_ecu, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------*/

#define DIAG_CAN_DEV            DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID          ((uint32_t)GEN_CAN_RX_ID)   /* 0x7DF */
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
     * OEM / robot manufacturer key injection placeholder.
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
    rc = uds_server_process_request(srv, &s_req_buf, &s_resp_buf);
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);

    if (s_resp_buf.length > 0U) {
        (void)isotp_transmit(tp, s_resp_buf.data, s_resp_buf.length);
    } else if (rc != UDS_STATUS_OK) {
        LOG_ERR("UDS error 0x%02X, no response.", (unsigned)rc);
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

        frame_ready = false;
        rc = can_transport_receive(can, &rx_frame, &frame_ready);

        if (rc == UDS_STATUS_ERR_CAN_BUS_OFF) {
            LOG_ERR("CAN bus-off.");
        } else if ((rc == UDS_STATUS_OK) && frame_ready) {
            (void)isotp_process_rx_frame(tp, &rx_frame,
                                          on_isotp_rx_complete, (void *)srv);
        }

        on_tick(&tick_ctx);
        (void)diag_wdt_feed(&s_wdt);
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
    LOG_INF("Xaloqi EDS  v" GEN_ECU_VERSION "  —  RobotJointController");
    LOG_INF("  DIDs: %u   DTCs: %u",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT);
    LOG_INF("  Axis 0: position 0xA000 · velocity 0xA001 · torque 0xA002");
    LOG_INF("  Faults: 0x19 subfunction 0x02 to read active DTCs");
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
    const zephyr_port_cfg_t port_cfg = { .can_dev = DIAG_CAN_DEV };
    rc = zephyr_port_init(&port_cfg, &can);
    if (rc != UDS_STATUS_OK) {
        LOG_ERR("Platform init failed: 0x%02X", (unsigned)rc); return -1;
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

    LOG_INF("UDS stack ready — awaiting diagnostic requests on CAN 0x7DF");

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
