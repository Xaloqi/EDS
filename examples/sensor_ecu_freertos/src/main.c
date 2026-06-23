// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu_freertos/src/main.c
 *
 * PURPOSE: SensorECU — FreeRTOS port.
 *
 *          Demonstrates the complete Xaloqi EDS sensor → DID → DTC pattern
 *          on FreeRTOS, using the same diagnostics_config.yaml as
 *          examples/sensor_ecu/ (Zephyr). Only the platform layer and sensor
 *          driver calls differ.
 *
 *          Sensor pattern:
 *            sensor_monitor_task  (100 ms, priority 4)
 *              └─ sensor_read_temperature() / sensor_read_voltage()  [stub]
 *              └─ dtc_database_set_status()   → DTC active/clear
 *
 *            uds_poll_task  (1 ms, priority 5)
 *              └─ can_transport_receive()
 *              └─ isotp_process_rx_frame()
 *              └─ uds_server_process_request()
 *                   └─ DID 0xD001 / 0xD002 / 0xD003 handlers
 *                        └─ sensor_state_get()   [mutex-protected]
 *
 *          CAN:   Stub loopback for CI (QEMU ARM Cortex-M4).
 *                 Replace loopback_can_send() with HAL_CAN_AddTxMessage()
 *                 for STM32. Wire CAN RX ISR to eds_platform_can_input().
 *
 * ADAPTING TO REAL HARDWARE:
 *   1. Replace sensor_read_temperature() / sensor_read_voltage() in
 *      sensor_monitor_freertos.c with your ADC or I2C sensor driver calls.
 *   2. Replace loopback_can_send() with your MCU's CAN transmit call.
 *   3. Wire your CAN RX ISR to eds_platform_can_input(&frame).
 *   4. Everything else — UDS stack, DTC logic, threshold writes — is
 *      identical to the Zephyr version.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* EDS platform + stack */
#include "platform_api.h"
#include "uds_server.h"
#include "isotp.h"
#include "can_transport.h"
#include "freertos_can.h"
#include "uds_init.h"
#include "generated_config.h"

/* Sensor state (shared with sensor_monitor_freertos.c) */
#include "sensor_ecu.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define DIAG_RX_CAN_ID          ((uint32_t)GEN_CAN_RX_ID)
#define DIAG_TX_CAN_ID          ((uint32_t)GEN_CAN_TX_ID)

#ifndef FREERTOS_POLL_TASK_STACK_SIZE
#define FREERTOS_POLL_TASK_STACK_SIZE  (2048U / sizeof(StackType_t))
#endif

#ifndef FREERTOS_POLL_TASK_PRIORITY
#define FREERTOS_POLL_TASK_PRIORITY    (5U)
#endif

/* =============================================================================
 * Stub CAN loopback (CI / QEMU mode)
 *
 * Replace with HAL_CAN_AddTxMessage() for STM32, or equivalent.
 * =============================================================================*/

static uds_status_t loopback_can_send(const eds_can_frame_t *frame)
{
    if (frame == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    eds_platform_can_input(frame);
    return UDS_STATUS_OK;
}

/* =============================================================================
 * ISO-TP RX completion callback
 * =============================================================================*/

static uds_msg_buf_t s_req_buf;
static uds_msg_buf_t s_resp_buf;

static void on_isotp_rx_complete(
    const uint8_t *data,
    uint32_t       length,
    void          *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();
    uds_status_t      status;
    uint16_t          i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) { return; }
    if ((length == (uint32_t)0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) { return; }

    for (i = 0U; i < (uint16_t)length; i++) {
        s_req_buf.data[i] = data[i];
    }
    s_req_buf.length  = (uint16_t)length;
    s_resp_buf.length = (uint16_t)0U;

    status = uds_server_process_request(srv, &s_req_buf, &s_resp_buf);

    if ((status != UDS_STATUS_OK) && (s_resp_buf.length == (uint16_t)0U)) { return; }

    if (s_resp_buf.length > (uint16_t)0U) {
        (void)isotp_transmit(tp, s_resp_buf.data, s_resp_buf.length);
    }

    /* [P2-0x11-01] Deferred reset: response is on the wire, now reset. */
    if (srv->pending_reset_type != (uint8_t)0U) {
        uint8_t reset_type = srv->pending_reset_type;
        srv->pending_reset_type = (uint8_t)0U;
        (void)eds_platform_nvm_flush();
        vTaskDelay(pdMS_TO_TICKS(50));  /* Allow ISO-TP TX to reach the wire before reset (As timer). */
        (void)eds_platform_ecu_reset(reset_type);
        /* Does not return. */
    }
}

/* =============================================================================
 * UDS poll task  (1 ms tick — identical to basic_ecu_freertos)
 * =============================================================================*/

static void uds_poll_task(void *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    can_transport_t  *can = freertos_can_get_transport();
    isotp_ctx_t      *tp  = uds_generated_get_isotp();
    uds_can_frame_t   rx_frame;
    bool              frame_ready;
    uds_status_t      status;

    for (;;) {
        vTaskDelay((TickType_t)1U);

        frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);

        if ((status == UDS_STATUS_OK) && frame_ready) {
            (void)isotp_process_rx_frame(tp, &rx_frame,
                                          on_isotp_rx_complete, (void *)srv);
        }

        (void)isotp_tick_1ms(tp);
        (void)uds_server_tick_1ms(srv);
    }
}

/* =============================================================================
 * Static task storage (no heap)
 * =============================================================================*/

static StackType_t  s_poll_stack[FREERTOS_POLL_TASK_STACK_SIZE];
static StaticTask_t s_poll_task_tcb;

/* =============================================================================
 * main()
 * =============================================================================*/

int main(void)
{
    uds_status_t      status;
    uds_server_ctx_t *srv = NULL;

    /* ── 1. Sensor monitor (starts sensor_monitor_task, creates mutex) ───── */
    sensor_monitor_init();

    /* ── 2. EDS platform init — provide CAN send callback ────────────────── */
    status = eds_platform_init(&(eds_platform_cfg_t){
        .can_send            = loopback_can_send,
        .uds_task_stack_size = (uint32_t)FREERTOS_POLL_TASK_STACK_SIZE *
                               sizeof(StackType_t),
        .uds_task_priority   = (uint32_t)FREERTOS_POLL_TASK_PRIORITY,
    });
    if (status != UDS_STATUS_OK) {
        for (;;) { }
    }

    /* ── 3. UDS stack init (same call as Zephyr) ─────────────────────────── */
    status = uds_generated_init(
        freertos_can_get_transport(),
        DIAG_RX_CAN_ID,
        DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        for (;;) { }
    }

    srv = uds_generated_get_server();
    if (srv == NULL) { for (;;) { } }

    /* ── 4. UDS poll task ────────────────────────────────────────────────── */
    (void)xTaskCreateStatic(
        uds_poll_task,
        "uds_poll",
        (uint32_t)FREERTOS_POLL_TASK_STACK_SIZE,
        (void *)srv,
        (UBaseType_t)FREERTOS_POLL_TASK_PRIORITY,
        s_poll_stack,
        &s_poll_task_tcb
    );

    /* ── 5. Start scheduler ──────────────────────────────────────────────── */
    vTaskStartScheduler();

    for (;;) { }
    return 0;
}
