// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/vehicle_state_ecu/src/main.c
 *
 * PURPOSE: VehicleStateECU Zephyr application entry point.
 *
 *          Two vehicle-state input DIDs (engine speed, vehicle speed),
 *          two DTCs on faults those two signals can produce, and a
 *          RoutineControl actuation path over an indicator output stage —
 *          see src/vehicle_state.c for the application logic behind all
 *          three.
 *
 * TARGET:  native_sim only. This example does not ship hardware board
 *          overlays — see README.md.
 *
 * THREAD MODEL:
 *   main()       — platform init -> UDS init -> launch diag_task -> return 0
 *   diag_task    — 1 ms poll loop: CAN RX -> ISO-TP -> UDS dispatch -> CAN TX,
 *                  plus a 100 ms-cadence call into vehicle_state_monitor_tick()
 *                  (see vehicle_state.h for why that logic lives on this
 *                  thread rather than a separate one).
 *
 * BUILDING:
 *   west build -b native_sim examples/vehicle_state_ecu
 *
 * SAFETY   : Example application — not safety-assessed.
 * STANDARD : MISRA C:2012 alignment intended.
 * LICENSE  : Apache-2.0
 * =============================================================================
 */

/* --------------------------------------------------------------------------
 * Diagnostics stack headers
 * -------------------------------------------------------------------------- */
#include "uds_types.h"
#include "uds_server.h"
#include "uds_security_algo.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "isotp.h"
#include "can_transport.h"
#include "zephyr_port.h"
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"
#include "nvm_store.h"

/* --------------------------------------------------------------------------
 * Generated headers (from diagnostics_config.yaml via codegen.py)
 * -------------------------------------------------------------------------- */
#include "uds_init.h"
#include "generated_config.h"

/* --------------------------------------------------------------------------
 * Application logic
 * -------------------------------------------------------------------------- */
#include "vehicle_state.h"

/* --------------------------------------------------------------------------
 * Zephyr headers
 * -------------------------------------------------------------------------- */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(vehicle_state_ecu, LOG_LEVEL_INF);

#if !defined(CONFIG_BOARD_NATIVE_SIM)
#error "examples/vehicle_state_ecu targets native_sim only -- see README.md"
#endif

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define DIAG_CAN_DEV         DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID       ((uint32_t)GEN_CAN_RX_ID)   /* 0x7E0 */
#define DIAG_TX_CAN_ID       ((uint32_t)GEN_CAN_TX_ID)   /* 0x7E8 */

#ifndef CONFIG_DIAG_TASK_STACK_SIZE
#define CONFIG_DIAG_TASK_STACK_SIZE   (4096U)
#endif

#ifndef CONFIG_DIAG_TASK_PRIORITY
#define CONFIG_DIAG_TASK_PRIORITY     (5)
#endif

/* =============================================================================
 * Static allocations
 * ============================================================================= */

K_THREAD_STACK_DEFINE(s_diag_stack, CONFIG_DIAG_TASK_STACK_SIZE);
static struct k_thread s_diag_thread;

static diag_mutex_t s_session_lock;
static diag_mutex_t s_security_lock;
static diag_timer_t s_tick_timer;
static diag_wdt_t   s_wdt;

/* =============================================================================
 * ISO-TP RX completion callback
 *
 * Called by isotp_process_rx_frame() when a complete UDS PDU is reassembled.
 * Holds the session lock for the duration of dispatch + response transmission.
 * ============================================================================= */

static void on_isotp_rx_complete(
    const uint8_t *data,
    uint32_t       length,
    void          *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();

    /* Static buffers — no stack allocation of uds_msg_buf_t. */
    static uds_msg_buf_t s_req;
    static uds_msg_buf_t s_resp;

    uint16_t i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) { return; }
    if ((length == 0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) { return; }

    (void)diag_mutex_lock(&s_session_lock);

    for (i = 0U; i < (uint16_t)length; i++) {
        s_req.data[i] = data[i];
    }
    s_req.length  = (uint16_t)length;
    s_resp.length = 0U;

    (void)uds_server_process_request(srv, &s_req, &s_resp);

    if (s_resp.length > 0U) {
        (void)isotp_transmit(tp, s_resp.data, s_resp.length);
    }

    (void)diag_mutex_unlock(&s_session_lock);

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

/* =============================================================================
 * 1 ms tick callback — drives ISO-TP and UDS session timers
 * ============================================================================= */

typedef struct { uds_server_ctx_t *srv; isotp_ctx_t *tp; } tick_ctx_t;

static void on_tick(void *arg)
{
    tick_ctx_t *ctx = (tick_ctx_t *)arg;
    if (ctx == NULL) { return; }
    (void)isotp_tick_1ms(ctx->tp);
    (void)uds_server_tick_1ms(ctx->srv);
    (void)uds_periodic_tick_1ms();
}

/* =============================================================================
 * Diagnostics task
 *
 * Folds vehicle_state_monitor_tick() into this same loop, on this same
 * thread, immediately after on_tick() — see vehicle_state.h for why that
 * matters (it's the thread that also dispatches the 0x2E writes to the two
 * input DIDs, and the generated DID backing stores have no locking of
 * their own).
 * ============================================================================= */

static void diag_task_entry(void *p1, void *p2, void *p3)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)p1;
    can_transport_t  *can = (can_transport_t  *)p2;
    isotp_ctx_t      *tp  = (isotp_ctx_t      *)p3;

    uds_status_t    status;
    uds_can_frame_t rx_frame;
    bool            frame_ready;
    tick_ctx_t      tick_ctx = { .srv = srv, .tp = tp };

    LOG_INF("VehicleStateECU diag task started");

    if (diag_timer_start(&s_tick_timer) != UDS_STATUS_OK) {
        LOG_ERR("Timer start failed."); return;
    }

    while (true) {
        (void)diag_timer_wait_tick(&s_tick_timer, 2U);

        frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);
        if ((status == UDS_STATUS_OK) && frame_ready) {
            (void)isotp_process_rx_frame(tp, &rx_frame,
                                          on_isotp_rx_complete, srv);
        }

        on_tick(&tick_ctx);
        vehicle_state_monitor_tick();

        {
            static uds_msg_buf_t s_periodic_frame;
            while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
                /* [EDS#194] Re-queue on ISO-TP busy instead of silently dropping
                 * this period -- isotp_transmit() can legitimately return BUSY
                 * mid multi-frame send; retry on the next 1ms tick rather than
                 * losing the period, and stop draining this tick (the same busy
                 * channel would just fail the next pop too). */
                if (isotp_transmit(tp, s_periodic_frame.data,
                                   (uint32_t)s_periodic_frame.length) != UDS_STATUS_OK) {
                    (void)uds_periodic_requeue_last();
                    break;
                }
            }
        }

        (void)diag_wdt_feed(&s_wdt);
    }
}

/* =============================================================================
 * Session change callback — cancels periodic subscriptions and releases any
 * held IndicatorLampCheck override on return to the Default session
 * (including on S3server timeout — a diagnostic client that holds an
 * actuator override and then disappears must not leave the output stuck).
 * ============================================================================= */

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
        vehicle_state_release_override();
    }
}

/* =============================================================================
 * main()
 * ============================================================================= */

int main(void)
{
    uds_status_t      status;
    can_transport_t  *can = NULL;
    uds_server_ctx_t *srv = NULL;
    isotp_ctx_t      *tp  = NULL;

    LOG_INF("Xaloqi EDS VehicleStateECU starting");

    vehicle_state_init();

    if (diag_mutex_init(&s_session_lock)  != UDS_STATUS_OK) { return -1; }
    if (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK) { return -1; }

    (void)diag_wdt_init(&s_wdt);

    status = diag_timer_init(&s_tick_timer, on_tick, NULL);
    if (status != UDS_STATUS_OK) { LOG_ERR("Timer init failed."); return -1; }

    {
        const zephyr_port_cfg_t port_cfg = {
            .can_dev = DIAG_CAN_DEV,
        };
        status = zephyr_port_init(&port_cfg, &can);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("Platform init failed: 0x%02X", (unsigned)status);
            return -1;
        }
    }

    /* native_sim only (enforced above): RAM-backed mock NVM, no real flash
     * partition to resolve. See platform/zephyr/nvm_store_mock.c. */
    status = nvm_store_init(NULL);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("NVM store init failed: 0x%02X — DTC mirror will not "
                "survive reset this run.", (unsigned)status);
    }

    /*
     * Security algorithm init — no TRNG on native_sim (CI / development
     * build). For production: call uds_security_algo_set_rng_cb() with a
     * real TRNG callback and uds_security_algo_set_level_key() with OEM
     * keys from OTP. See docs/SECURITY_NOTICE.md.
     */
    LOG_WRN("[SEC] Using placeholder AES keys — inject OEM keys before production.");
    (void)uds_security_algo_set_rng_cb(NULL);

    status = uds_generated_init(can, DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS stack init failed: 0x%02X", (unsigned)status);
        return -1;
    }

    srv = uds_generated_get_server();
    tp  = uds_generated_get_isotp();
    if ((srv == NULL) || (tp == NULL)) {
        LOG_ERR("Stack context NULL after init."); return -1;
    }

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    LOG_INF("UDS stack ready: %u DIDs  %u DTCs  RX=0x%03X TX=0x%03X",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT,
            (unsigned)DIAG_RX_CAN_ID, (unsigned)DIAG_TX_CAN_ID);

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
