// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_can.c
 *
 * PURPOSE: FreeRTOS CAN transport shim.
 *
 *          Implements can_transport_ops_t (transmit, receive, get_status)
 *          for FreeRTOS builds where the customer provides their own CAN
 *          peripheral driver.
 *
 *          ARCHITECTURE:
 *
 *            Customer CAN RX ISR / callback
 *              │  eds_platform_can_input(frame)
 *              ▼
 *            freertos_can_input()
 *              │  xQueueSendFromISR() — ISR-safe, non-blocking
 *              ▼
 *            s_rx_queue  (FreeRTOS queue, depth CAN_FREERTOS_RX_QUEUE_DEPTH)
 *              │
 *              ▼
 *            freertos_can_receive()  ← UDS poll task calls can_transport_receive()
 *              │  xQueueReceive(K_NO_WAIT equivalent)
 *              ▼
 *            uds_can_frame_t  →  isotp_process_rx_frame()
 *
 *          TX path:
 *            isotp_transmit() → can_transport_transmit() → s_can_send_fn()
 *              │  (customer's MCU CAN send function)
 *              ▼
 *            CAN peripheral
 *
 *          BUS-OFF:
 *            s_bus_off is set by the customer via freertos_can_set_bus_off().
 *            The customer CAN error interrupt calls this to signal bus-off.
 *            freertos_can_get_status() reads it. The poll task logs and
 *            handles recovery using can_transport_get_status().
 *
 * SAFETY  : ASIL-B candidate. CAN errors must be propagated faithfully.
 *           s_bus_off is written from ISR context — access is guarded by
 *           taskENTER_CRITICAL / taskEXIT_CRITICAL.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "freertos_can.h"
#include "platform_api.h"
#include "can_transport.h"
#include "uds_types.h"
#include "isotp.h"         /* ISOTP_ENABLE_CAN_FD compile-time gate. */

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

/**
 * @brief RX queue depth in frames.
 *
 * 8 frames matches the Zephyr implementation's k_msgq depth. Enough to
 * buffer a burst of diagnostic frames between poll iterations.
 * Increase if the poll loop runs slower than 1 ms on the target.
 */
#ifndef CAN_FREERTOS_RX_QUEUE_DEPTH
#define CAN_FREERTOS_RX_QUEUE_DEPTH  (8U)
#endif

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

/** Customer-provided CAN send function, registered via freertos_can_init(). */
static eds_can_send_fn_t s_can_send_fn = NULL;

/** FreeRTOS queue for incoming CAN frames (ISR → poll task). */
static QueueHandle_t s_rx_queue = NULL;

/** Static queue storage — no heap allocation. */
static StaticQueue_t            s_rx_queue_struct;
static eds_can_frame_t          s_rx_queue_storage[CAN_FREERTOS_RX_QUEUE_DEPTH];

/** Bus-off flag. Written from ISR via freertos_can_set_bus_off(). */
static volatile bool s_bus_off = false;

/** True after freertos_can_init() completes. */
static bool s_initialized = false;

/* --------------------------------------------------------------------------
 * can_transport_ops_t — transmit
 * -------------------------------------------------------------------------- */

static uds_status_t freertos_can_transmit(
    can_transport_t       *self,
    const uds_can_frame_t *frame)
{
    eds_can_frame_t ef;

    (void)self;

    if (frame == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized || (s_can_send_fn == NULL)) {
        return UDS_STATUS_ERR_CAN_NOT_READY;
    }

    taskENTER_CRITICAL();
    if (s_bus_off) {
        taskEXIT_CRITICAL();
        return UDS_STATUS_ERR_CAN_BUS_OFF;
    }
    taskEXIT_CRITICAL();

#if ISOTP_ENABLE_CAN_FD
    if (frame->dlc > (uint8_t)64U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
#else
    if (frame->dlc > (uint8_t)8U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
#endif

    /* Convert uds_can_frame_t → eds_can_frame_t for the customer send callback. */
    (void)memset(&ef, 0, sizeof(ef));
    ef.id          = frame->id;
    ef.dlc         = frame->dlc;
    ef.is_extended = frame->is_extended_id;
#if ISOTP_ENABLE_CAN_FD
    ef.is_fd       = frame->is_fd;
#endif
    (void)memcpy(ef.data, frame->data, (size_t)frame->dlc);

    return s_can_send_fn(&ef);
}

/* --------------------------------------------------------------------------
 * can_transport_ops_t — receive (non-blocking poll)
 * -------------------------------------------------------------------------- */

static uds_status_t freertos_can_receive(
    can_transport_t  *self,
    uds_can_frame_t  *out_frame,
    bool             *out_ready)
{
    eds_can_frame_t ef;
    BaseType_t      rc;

    (void)self;

    if ((out_frame == NULL) || (out_ready == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    *out_ready = false;

    if (!s_initialized || (s_rx_queue == NULL)) {
        return UDS_STATUS_ERR_CAN_NOT_READY;
    }

    /* Non-blocking dequeue. */
    rc = xQueueReceive(s_rx_queue, &ef, (TickType_t)0U);
    if (rc != pdTRUE) {
        /* Queue empty — no frame this iteration. */
        return UDS_STATUS_OK;
    }

    /*
     * Convert eds_can_frame_t → uds_can_frame_t.
     */
    (void)memset(out_frame, 0, sizeof(*out_frame));
    out_frame->id              = ef.id;
    out_frame->dlc             = ef.dlc;
    out_frame->is_extended_id  = ef.is_extended;
#if ISOTP_ENABLE_CAN_FD
    out_frame->is_fd = ef.is_fd;
#else
    out_frame->is_fd = false;
#endif
    (void)memcpy(out_frame->data, ef.data, (size_t)ef.dlc);

    *out_ready = true;

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * can_transport_ops_t — get_status
 * -------------------------------------------------------------------------- */

static uds_status_t freertos_can_get_status(
    can_transport_t *self,
    bool            *bus_off)
{
    (void)self;

    if (bus_off == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    taskENTER_CRITICAL();
    *bus_off = s_bus_off;
    taskEXIT_CRITICAL();

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * CAN transport instance
 * -------------------------------------------------------------------------- */

static const can_transport_ops_t s_freertos_can_ops = {
    .transmit   = freertos_can_transmit,
    .receive    = freertos_can_receive,
    .get_status = freertos_can_get_status,
};

static can_transport_t s_freertos_can_transport = {
    .ops      = &s_freertos_can_ops,
    .platform = NULL,
    .ready    = false,
};

/* ============================================================================
 * Public API
 * ========================================================================== */

void freertos_can_init(eds_can_send_fn_t can_send)
{
    /* can_send is validated by the caller (eds_platform_init). */
    s_can_send_fn = can_send;
    s_bus_off     = false;

    /*
     * Create the static RX queue (no heap allocation).
     * xQueueCreateStatic() cannot fail when given valid storage pointers.
     */
    s_rx_queue = xQueueCreateStatic(
        (UBaseType_t)CAN_FREERTOS_RX_QUEUE_DEPTH,
        (UBaseType_t)sizeof(eds_can_frame_t),
        (uint8_t *)s_rx_queue_storage,
        &s_rx_queue_struct
    );

    s_freertos_can_transport.ops      = &s_freertos_can_ops;
    s_freertos_can_transport.platform = NULL;
    s_freertos_can_transport.ready    = true;

    s_initialized = true;
}

can_transport_t *freertos_can_get_transport(void)
{
    return &s_freertos_can_transport;
}

void freertos_can_input(const eds_can_frame_t *frame)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if ((frame == NULL) || (s_rx_queue == NULL)) {
        return;
    }

    /*
     * Use the FromISR variant — safe to call from both ISR and task context.
     * If the queue is full, the oldest frame is NOT overwritten (pdFALSE is
     * returned silently). This matches the Zephyr k_msgq_put behaviour
     * with K_NO_WAIT: new frames are dropped when the buffer is full.
     *
     * The diagnostic protocol can recover from a dropped frame via the
     * ISO-TP N_Cr timeout — the tester will retransmit.
     */
    (void)xQueueSendFromISR(s_rx_queue, frame, &higher_priority_task_woken);

    /*
     * If posting the frame unblocked a higher-priority task that is waiting
     * on this queue (e.g. the UDS poll task in a blocking receive model),
     * request a context switch. For a non-blocking poll model this is a
     * no-op.
     */
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/**
 * @brief Signal bus-off condition from the customer's CAN error interrupt.
 *
 * Call this from the CAN error interrupt handler when the controller enters
 * bus-off state. The UDS poll task reads this flag via can_transport_get_status()
 * and handles recovery.
 *
 * @param[in] is_bus_off  True to set bus-off, false to clear after recovery.
 */
void freertos_can_set_bus_off(bool is_bus_off)
{
    /*
     * taskENTER_CRITICAL / taskEXIT_CRITICAL are safe from both task and
     * ISR context on Cortex-M (they mask interrupts via BASEPRI).
     * The FROM_ISR variants require a saved-mask argument and are only
     * needed when the return value must be passed back — not needed here.
     */
    taskENTER_CRITICAL();
    s_bus_off = is_bus_off;
    taskEXIT_CRITICAL();
}
