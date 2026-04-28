// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_can.h
 *
 * PURPOSE: FreeRTOS CAN shim — public API.
 *
 *          Provides the can_transport_t instance used by the ISO-TP layer,
 *          and the eds_platform_can_input() entry point for the customer's
 *          CAN RX interrupt handler.
 *
 *          The CAN send path uses the function pointer registered via
 *          eds_platform_init(cfg->can_send). The CAN receive path uses
 *          an internal FreeRTOS queue: the customer's ISR calls
 *          eds_platform_can_input() which posts to the queue; the UDS
 *          poll task calls freertos_can_receive() which dequeues.
 *
 * SAFETY  : ASIL-B candidate. CAN errors must be propagated.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef FREERTOS_CAN_H
#define FREERTOS_CAN_H

#include "platform_api.h"
#include "can_transport.h"
#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the FreeRTOS CAN shim.
 *
 * Must be called from eds_platform_init() before uds_generated_init().
 * Creates the internal RX queue and wires the can_send callback into
 * the can_transport_ops_t vtable.
 *
 * @param[in] can_send  Customer-provided CAN transmit function. Must not
 *                      be NULL — validation is the caller's responsibility.
 */
void freertos_can_init(eds_can_send_fn_t can_send);

/**
 * @brief Return the CAN transport instance for use by the ISO-TP layer.
 *
 * Call after freertos_can_init(). Pass the returned pointer to
 * uds_generated_init() as the CAN transport argument.
 *
 * @return Pointer to the static can_transport_t instance.
 *         Never NULL after freertos_can_init() completes.
 */
can_transport_t *freertos_can_get_transport(void);

/**
 * @brief Feed a received CAN frame into the EDS stack.
 *
 * Call this from the customer's CAN RX interrupt handler or callback
 * whenever a frame matching the diagnostic CAN IDs (0x7DF or physical
 * RX ID) is received.
 *
 * Safe to call from interrupt context — posts to a FreeRTOS queue
 * using xQueueSendFromISR(). Does not call the UDS stack directly.
 *
 * If the queue is full (8 frames), the oldest frame is silently dropped.
 * This matches the Zephyr CAN implementation's k_msgq behaviour.
 *
 * @param[in] frame  Received CAN frame. Must not be NULL.
 */
void freertos_can_input(const eds_can_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_CAN_H */
