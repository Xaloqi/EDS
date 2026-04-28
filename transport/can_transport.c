/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/can_transport.c
 *
 * PURPOSE: Platform-independent CAN transport abstraction layer.
 *          Implements the convenience wrapper API declared in can_transport.h.
 *          All hardware-specific behaviour is delegated via the ops vtable
 *          to a concrete platform implementation (e.g. zephyr_can.c).
 *
 * ARCHITECTURE:
 *   Application / UDS layer
 *       │
 *   isotp.c   ──calls──►  can_transport_transmit()  ─ops→  platform impl
 *                          can_transport_receive()   ─ops→  platform impl
 *                          can_transport_get_status()─ops→  platform impl
 *
 * SAFETY  : This file implements the sole path between the ISO-TP layer and
 *           the CAN hardware. NULL-pointer and ready-state guards are the
 *           last line of defence before a call reaches hardware registers.
 *           ASIL-B candidate — review required during safety assessment.
 *
 * MISRA   : No dynamic memory. No recursion. Fixed-width types throughout.
 *           All public functions return uds_status_t. Explicit casts used.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * VERSION : 0.1.0
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "can_transport.h"

/* =============================================================================
 * Internal helper: validate that a can_transport_t pointer is usable.
 *
 * Checks:
 *   - Pointer is non-NULL.
 *   - ops vtable pointer is non-NULL.
 *   - All mandatory ops function pointers are non-NULL.
 *   - ready flag is set.
 *
 * SAFETY: Must be called at the entry of every public function before any
 *         vtable dispatch. Prevents NULL-function-pointer dereference.
 * ============================================================================= */
static uds_status_t can_transport_validate(const can_transport_t *can)
{
    if (can == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (can->ops == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /*
     * MISRA C:2012 Rule 11.1: Function pointer validity check.
     * Each mandatory op is verified independently to produce a clear
     * fault code rather than a silent invalid-memory dereference.
     */
    if (can->ops->transmit == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (can->ops->receive == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (can->ops->get_status == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!can->ready) {
        return UDS_STATUS_ERR_CAN_NOT_READY;
    }

    return UDS_STATUS_OK;
}

/* =============================================================================
 * Public API
 * ============================================================================= */

/**
 * @brief Transmit a CAN frame via the registered ops->transmit function.
 *
 * @details Validates the transport instance and frame pointer, then delegates
 *          to the platform-specific transmit implementation. The caller must
 *          treat UDS_STATUS_ERR_CAN_TX_FAILED as a transport-layer timeout
 *          trigger (N_As timeout) in the ISO-TP state machine.
 *
 * @note TIMING: The ops->transmit implementation MUST complete within the
 *               ISO 15765-2 N_As maximum (25 ms default). If the underlying
 *               driver blocks for longer, the ISO-TP timeout state machine
 *               will be violated.
 *
 * @note SAFETY: CAN TX failure must be propagated upward. This function never
 *               silently discards a failed transmission.
 */
uds_status_t can_transport_transmit(
    can_transport_t       *can,
    const uds_can_frame_t *frame)
{
    uds_status_t status;

    /* Validate transport instance before any vtable dispatch. */
    status = can_transport_validate(can);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    if (frame == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /*
     * Defensive check: DLC must not exceed CAN classical frame maximum.
     * CAN FD support (up to 64 bytes) is deferred to a future phase.
     * MISRA C:2012 Rule 10.4: Comparison uses explicit cast.
     */
    if ((uint8_t)frame->dlc > (uint8_t)UDS_CAN_FRAME_MAX_LEN) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Dispatch to platform implementation. */
    return can->ops->transmit(can, frame);
}

/**
 * @brief Poll for a received CAN frame from the transport interface.
 *
 * @details Non-blocking poll. If no frame is available, *out_ready is set
 *          to false and UDS_STATUS_OK is returned. The ISO-TP layer must
 *          call this function regularly from its tick context to drain the
 *          hardware receive buffer.
 *
 * @note MISRA C:2012 Rule 8.13: out_ready and out_frame written on all
 *       non-error paths to prevent use of uninitialised values.
 */
uds_status_t can_transport_receive(
    can_transport_t *can,
    uds_can_frame_t *out_frame,
    bool            *out_ready)
{
    uds_status_t status;

    /* Initialise output parameters to safe values before any early return. */
    if (out_ready != NULL) {
        *out_ready = false;
    }

    status = can_transport_validate(can);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    if (out_frame == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (out_ready == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    return can->ops->receive(can, out_frame, out_ready);
}

/**
 * @brief Query the operational status of the CAN controller.
 *
 * @details Used by the ISO-TP and UDS layers to detect bus-off conditions
 *          and suppress transmission attempts during recovery. The caller
 *          should treat bus-off as a fatal transport error requiring
 *          supervisory recovery logic at the application layer.
 *
 * @note SAFETY: Bus-off detection is a safety-relevant function. If the
 *               platform implementation cannot determine controller state,
 *               it must return UDS_STATUS_ERR_CAN_NOT_READY rather than
 *               fabricating a healthy status.
 */
uds_status_t can_transport_get_status(
    can_transport_t *can,
    bool            *bus_off)
{
    uds_status_t status;

    /* Initialise output to safe (worst-case) value. */
    if (bus_off != NULL) {
        *bus_off = true;
    }

    status = can_transport_validate(can);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    if (bus_off == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    return can->ops->get_status(can, bus_off);
}
