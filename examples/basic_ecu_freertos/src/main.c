// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_freertos/src/main.c
 *
 * PURPOSE: BasicECU FreeRTOS application entry point.
 *
 *          Demonstrates Xaloqi EDS on FreeRTOS using the same YAML
 *          configuration as examples/basic_ecu/ (Zephyr). The same
 *          diagnostics_config.yaml, the same codegen, the same 14 UDS
 *          services — only the platform layer differs.
 *
 *          STUB CAN LOOPBACK:
 *            For CI (QEMU ARM Cortex-M4) a stub CAN implementation is used:
 *            every frame sent via loopback_can_send() is immediately fed back
 *            into eds_platform_can_input(). This allows the ISO-TP and UDS
 *            stack to be exercised end-to-end without a real CAN peripheral.
 *
 *          PRODUCTION INTEGRATION:
 *            Replace loopback_can_send() with your MCU's CAN transmit call
 *            (e.g. HAL_CAN_AddTxMessage for STM32). Wire your CAN RX ISR
 *            to call eds_platform_can_input(&frame). See platform_api.h.
 *
 *          INTEGRATION SEQUENCE (4 steps):
 *
 *            1. eds_platform_init()     — provide CAN send + optional NVM ops
 *            2. uds_generated_init()    — start the UDS stack
 *            3. eds_freertos_start()    — create the UDS poll task
 *            4. vTaskStartScheduler()   — hand control to FreeRTOS
 *
 *          The UDS poll task body, ISO-TP RX callback, and static task storage
 *          are encapsulated inside eds_freertos_start(). Nothing to copy.
 *          See platform/freertos/freertos_platform_api.c for the implementation.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "FreeRTOS.h"
#include "task.h"

#include "platform_api.h"
#include "freertos_can.h"
#include "uds_init.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "uds_server.h"
#include "generated_config.h"

#include <stdint.h>
#include <stdbool.h>

/* CAN IDs from generated_config.h */
#define DIAG_RX_CAN_ID  ((uint32_t)GEN_CAN_RX_ID)   /* 0x7DF */
#define DIAG_TX_CAN_ID  ((uint32_t)GEN_CAN_TX_ID)   /* 0x7E8 */

/* =============================================================================
 * Stub CAN loopback (CI / QEMU)
 *
 * Every transmitted frame is fed straight back into the RX queue.
 * Replace with your MCU's CAN send function for hardware targets.
 * ============================================================================= */

static uds_status_t loopback_can_send(const eds_can_frame_t *frame)
{
    if (frame == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    eds_platform_can_input(frame);
    return UDS_STATUS_OK;
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
 * main() — four steps
 * ============================================================================= */

int main(void)
{
    uds_status_t status;

    /* Step 1 — Initialise the EDS platform layer. */
    status = eds_platform_init(&(eds_platform_cfg_t){
        .can_send            = loopback_can_send,
        .uds_task_stack_size = 2048U,
        .uds_task_priority   = 5U,
    });
    if (status != UDS_STATUS_OK) {
        for (;;) { }
    }

    /* Step 2 — Initialise the UDS stack (identical call to Zephyr). */
    status = uds_generated_init(
        freertos_can_get_transport(),
        DIAG_RX_CAN_ID,
        DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        for (;;) { }
    }

    {
        uds_server_ctx_t *srv = uds_generated_get_server();
        if (srv != NULL) {
            (void)uds_periodic_init();
            (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                                  s_on_session_change);
        }
    }

    /* Step 3 — Create the UDS poll task (static allocation, no heap).
     *
     * eds_freertos_start() encapsulates the poll task body, ISO-TP RX
     * completion callback, and static storage — nothing to copy from
     * this file into your own application. */
    status = eds_freertos_start();
    if (status != UDS_STATUS_OK) {
        for (;;) { }
    }

    /* Step 4 — Start the FreeRTOS scheduler. Does not return. */
    vTaskStartScheduler();

    for (;;) { }   /* unreachable */
    return 0;
}
