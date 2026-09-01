// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/can_transport.h
 *
 * PURPOSE: CAN transport abstraction interface.
 *          Defines the hardware-independent CAN interface used by the
 *          ISO-TP layer. Concrete implementations (e.g. Zephyr CAN driver)
 *          are registered via function pointer tables.
 *
 * SAFETY  : Hardware interface — must be validated against target CAN
 *           controller specification. ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef CAN_TRANSPORT_H
#define CAN_TRANSPORT_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * CAN controller configuration
 * -------------------------------------------------------------------------- */

/** Default diagnostic CAN bus bitrate (500 kbps standard). */
#ifndef CAN_DIAG_BITRATE_BPS
#define CAN_DIAG_BITRATE_BPS (500000U)
#endif

/** Maximum number of CAN RX filter entries. */
#ifndef CAN_MAX_RX_FILTERS
#define CAN_MAX_RX_FILTERS (4U)
#endif

/* --------------------------------------------------------------------------
 * CAN transport interface (vtable pattern)
 * -------------------------------------------------------------------------- */

/* Forward declaration. */
typedef struct can_transport can_transport_t;

/**
 * @brief Prototype for CAN frame transmission function.
 *
 * @param[in] self   Pointer to the CAN transport instance.
 * @param[in] frame  CAN frame to transmit.
 *
 * @return UDS_STATUS_OK once transmission of this frame is CONFIRMED —
 *         accepted by the CAN controller / physically placed on the bus —
 *         not merely enqueued into a driver queue or TX mailbox.
 * @return UDS_STATUS_ERR_CAN_TX_FAILED if transmission failed or the
 *         confirmation wait exceeded the timeout below.
 * @return UDS_STATUS_ERR_CAN_BUS_OFF if controller is in bus-off state.
 *
 * @note CONTRACT: This is the CONFIRMED contract, not a "queued" one.
 *       transport/isotp.c arms ISO 15765-2 N_As/N_Ar (the sender/receiver
 *       "transmission confirmation" timers, ISO 15765-2:2016 Table 5)
 *       immediately before calling this function and disarms them the
 *       instant it returns — so a return of UDS_STATUS_OK IS, at this
 *       layer, the transmission-confirmation event the ISO-TP timers exist
 *       to bound. An implementation that returns as soon as the frame is
 *       merely queued (e.g. handed to a TX mailbox without waiting for the
 *       controller to actually place it on the bus) silently defeats N_As/
 *       N_Ar: the timer is armed and disarmed before the real transmission
 *       has happened or failed, so it can never catch a stuck or lost frame.
 *       See transport/isotp.h's N_As/N_Ar notes on isotp_tick_1ms() for how
 *       the timers consume this return.
 * @note TIMING: Must not block indefinitely — bound the confirmation wait
 *       to (at most) the ISO_TP N_As/N_Ar budget (ISOTP_TIMEOUT_AS_MS /
 *       ISOTP_TIMEOUT_AR_MS in transport/isotp.h, 25 ms by default) and
 *       return UDS_STATUS_ERR_CAN_TX_FAILED on expiry. Bounded blocking is
 *       expected and is how the reference Zephyr port satisfies this
 *       contract (platform/zephyr/zephyr_can.c calls can_send() with a
 *       K_MSEC(25) timeout and no async callback, which blocks the caller
 *       until the controller confirms the frame or the timeout elapses).
 *       A platform that instead reports success on enqueue only (e.g. a
 *       bare "add to TX mailbox" HAL call with no wait for the completion
 *       event) does NOT satisfy this contract even though it never blocks
 *       indefinitely — see docs/INTEGRATION_GUIDE.md's FreeRTOS CAN send
 *       example for the corrected pattern.
 * @note SAFETY: ASIL-B relevant — transmission failure must be reported.
 */
typedef uds_status_t (*can_transmit_fn)(
    can_transport_t       *self,
    const uds_can_frame_t *frame
);

/**
 * @brief Prototype for CAN frame reception poll function.
 *
 * @param[in]  self       Pointer to the CAN transport instance.
 * @param[out] out_frame  Buffer to receive the CAN frame.
 * @param[out] out_ready  Set to true if a frame is available.
 *
 * @return UDS_STATUS_OK (regardless of whether a frame is available).
 * @return UDS_STATUS_ERR_CAN_RX_FAILED on hardware error.
 */
typedef uds_status_t (*can_receive_fn)(
    can_transport_t  *self,
    uds_can_frame_t  *out_frame,
    bool             *out_ready
);

/**
 * @brief Prototype for CAN controller status query function.
 *
 * @param[in]  self       Pointer to the CAN transport instance.
 * @param[out] bus_off    Set to true if controller is bus-off.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_CAN_NOT_READY if controller state unavailable.
 */
typedef uds_status_t (*can_get_status_fn)(
    can_transport_t *self,
    bool            *bus_off
);

/**
 * @brief CAN transport interface descriptor (vtable).
 *
 * Platform implementations populate this structure and pass it to
 * the ISO-TP layer at initialization time.
 */
typedef struct can_transport_ops {
    can_transmit_fn    transmit;    /**< Required: frame transmission function.
                                      *   CONFIRMED contract — must not return
                                      *   UDS_STATUS_OK until the frame is
                                      *   confirmed transmitted, not merely
                                      *   queued. See can_transmit_fn's doc
                                      *   comment above. */
    can_receive_fn     receive;     /**< Required: frame reception poll function. */
    can_get_status_fn  get_status;  /**< Required: controller status query. */
} can_transport_ops_t;

/* --------------------------------------------------------------------------
 * CAN transport instance
 * -------------------------------------------------------------------------- */

/**
 * @brief CAN transport instance, binding ops to platform-specific state.
 *
 * Platform layer allocates and populates this structure. The ISO-TP layer
 * calls through ops without knowledge of the underlying driver.
 */
struct can_transport {
    const can_transport_ops_t *ops;       /**< Pointer to operations vtable. */
    void                      *platform;  /**< Opaque pointer to platform-specific context. */
    bool                       ready;     /**< True if controller is initialized and operational. */
};

/* --------------------------------------------------------------------------
 * Public API — convenience wrappers
 * -------------------------------------------------------------------------- */

/**
 * @brief Transmit a CAN frame via the transport interface.
 *
 * @param[in] can    Initialized CAN transport instance.
 * @param[in] frame  CAN frame to transmit.
 *
 * @return UDS_STATUS_OK once the frame's transmission is CONFIRMED (not
 *         merely queued) — this call forwards directly to ops->transmit(),
 *         which defines and must satisfy the CONFIRMED contract documented
 *         on the can_transmit_fn typedef above. Callers (transport/isotp.c)
 *         rely on this: ISO-TP's N_As/N_Ar timers are armed immediately
 *         before this call and disarmed the instant it returns, treating
 *         that return as the transmission-confirmation event.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_CAN_NOT_READY if transport not ready.
 * @return Return value of ops->transmit() otherwise (e.g.
 *         UDS_STATUS_ERR_CAN_TX_FAILED, UDS_STATUS_ERR_CAN_BUS_OFF).
 */
uds_status_t can_transport_transmit(
    can_transport_t       *can,
    const uds_can_frame_t *frame
);

/**
 * @brief Poll for a received CAN frame.
 *
 * @param[in]  can        Initialized CAN transport instance.
 * @param[out] out_frame  Buffer to receive the frame.
 * @param[out] out_ready  True if a frame was available and copied.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_CAN_NOT_READY if transport not ready.
 */
uds_status_t can_transport_receive(
    can_transport_t *can,
    uds_can_frame_t *out_frame,
    bool            *out_ready
);

/**
 * @brief Query CAN controller operational status.
 *
 * @param[in]  can      Initialized CAN transport instance.
 * @param[out] bus_off  True if the controller is in bus-off state.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 */
uds_status_t can_transport_get_status(
    can_transport_t *can,
    bool            *bus_off
);

#ifdef __cplusplus
}
#endif

#endif /* CAN_TRANSPORT_H */
