// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/safeboot_freertos_ecu/src/main.c
 *
 * PURPOSE: SafeBootFreeRTOSECU — OTA DFU on STM32H743ZI + FreeRTOS.
 *
 *          FreeRTOS companion to examples/safeboot_ecu/ (Zephyr + MCUboot).
 *          Demonstrates the complete UDS download pipeline on a bare FreeRTOS
 *          target without MCUboot:
 *
 *            0x34 RequestDownload  — erase OTA staging area (Bank 2)
 *            0x36 TransferData ×N  — write firmware blocks (256 B each)
 *            0x37 RequestTransferExit — CRC-32 verify, accept image
 *            0x11 ECUReset         — trigger bank switch (customer bootloader)
 *
 *          Flash driver: STM32H743ZI dual-bank HAL, contributed by
 *          chenyurong22 (Siemens) in GitHub issue #28, adapted to the
 *          uds_flash_ops_t interface in platform/freertos/freertos_flash_ops.c.
 *
 * THREAD MODEL:
 *
 *   main()                      — initialise platform + UDS stack, then
 *                                 create tasks and start FreeRTOS scheduler
 *
 *   uds_poll_task (1 ms, prio 5)
 *     └─ can_transport_receive()
 *     └─ isotp_process_rx_frame()
 *          └─ on_isotp_rx_complete() → uds_server_process_request()
 *               └─ service 0x34/0x36/0x37 → freertos_flash_ops callbacks
 *     └─ isotp_tick_1ms()
 *     └─ uds_server_tick_1ms()
 *
 * ADAPTING TO REAL HARDWARE:
 *   1. Replace loopback_can_send() with HAL_FDCAN_AddMessageToTxFifoQ().
 *   2. In your FDCAN RX ISR, call eds_platform_can_input(&frame).
 *   3. Define STM32H743xx in your build so freertos_flash_ops.c uses
 *      the real HAL instead of the RAM stub.
 *   4. Add STM32H7 HAL sources to your build (see CMakeLists.txt comment).
 *
 * BANK SWAP:
 *   After RequestTransferExit succeeds, the new image sits in Bank 2.
 *   The UDS service sends ECUReset (0x11) to trigger the switch.
 *   The customer's bootloader or the EDS Developer A/B feature handles
 *   the actual bank swap at boot time.  In this public example, eds_platform_ecu_reset()
 *   resets the device and the customer's bootloader decides which bank to boot.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* EDS platform + stack */
#include "platform_api.h"
#include "uds_server.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "isotp.h"
#include "can_transport.h"
#include "freertos_can.h"
#include "uds_init.h"
#include "generated_config.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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
 * Stub CAN loopback — CI / QEMU
 *
 * Replace with HAL_FDCAN_AddMessageToTxFifoQ() for STM32H743.
 * Wire the FDCAN RX ISR to eds_platform_can_input().
 *
 * Example STM32H743 CAN TX:
 *   FDCAN_TxHeaderTypeDef tx_hdr = {
 *       .Identifier          = frame->id,
 *       .IdType              = FDCAN_STANDARD_ID,
 *       .TxFrameType         = FDCAN_DATA_FRAME,
 *       .DataLength          = FDCAN_DLC_BYTES_8,
 *       .ErrorStateIndicator = FDCAN_ESI_ACTIVE,
 *       .BitRateSwitch       = FDCAN_BRS_OFF,
 *       .FDFormat            = FDCAN_CLASSIC_CAN,
 *       .TxEventFifoControl  = FDCAN_NO_TX_EVENTS,
 *       .MessageMarker       = 0,
 *   };
 *   HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &tx_hdr, frame->data);
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

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) {
        return;
    }
    if ((length == (uint32_t)0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) {
        return;
    }

    for (i = (uint16_t)0U; i < (uint16_t)length; i++) {
        s_req_buf.data[i] = data[i];
    }
    s_req_buf.length  = (uint16_t)length;
    s_resp_buf.length = (uint16_t)0U;

    status = uds_server_process_request(srv, &s_req_buf, &s_resp_buf);

    if ((status != UDS_STATUS_OK) && (s_resp_buf.length == (uint16_t)0U)) {
        return;
    }

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
 * UDS poll task — 1 ms tick
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
        (void)uds_periodic_tick_1ms();

        {
            static uds_msg_buf_t s_periodic_frame;
            while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
                (void)isotp_transmit(tp, s_periodic_frame.data,
                                     (uint32_t)s_periodic_frame.length);
            }
        }
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

/* =============================================================================
 * Static task storage — no heap
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

    /* ── 1. EDS platform init — provide CAN send callback ────────────────── */
    status = eds_platform_init(&(eds_platform_cfg_t){
        .can_send            = loopback_can_send,
        .uds_task_stack_size = (uint32_t)FREERTOS_POLL_TASK_STACK_SIZE *
                               sizeof(StackType_t),
        .uds_task_priority   = (uint32_t)FREERTOS_POLL_TASK_PRIORITY,
    });
    if (status != UDS_STATUS_OK) {
        for (;;) { }
    }

    /*
     * ── 2. UDS stack init ─────────────────────────────────────────────────
     *
     * uds_generated_init() calls freertos_flash_ops_init() at Step 5.7
     * (generated in uds_init.c by codegen when safeboot.platform: freertos).
     *
     * freertos_flash_ops_init() registers the STM32H743 dual-bank flash
     * ops table so that 0x34/0x36/0x37 requests are accepted.
     *
     * On CI (QEMU, no STM32H7xx defined), the RAM stub in
     * freertos_flash_ops.c is used — compile + link succeeds, flash
     * operations run in a static buffer (not persistent).
     */
    status = uds_generated_init(
        freertos_can_get_transport(),
        DIAG_RX_CAN_ID,
        DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        for (;;) { }
    }

    srv = uds_generated_get_server();
    if (srv == NULL) {
        for (;;) { }
    }

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    /* ── 3. UDS poll task ────────────────────────────────────────────────── */
    (void)xTaskCreateStatic(
        uds_poll_task,
        "uds_poll",
        (uint32_t)FREERTOS_POLL_TASK_STACK_SIZE,
        (void *)srv,
        (UBaseType_t)FREERTOS_POLL_TASK_PRIORITY,
        s_poll_stack,
        &s_poll_task_tcb
    );

    /* ── 4. Start FreeRTOS scheduler ─────────────────────────────────────── */
    vTaskStartScheduler();

    /* Should never reach here. */
    for (;;) { }
    return 0;
}
