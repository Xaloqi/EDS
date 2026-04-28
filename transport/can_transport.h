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
 * @return UDS_STATUS_OK if frame was accepted for transmission.
 * @return UDS_STATUS_ERR_CAN_TX_FAILED if transmission failed.
 * @return UDS_STATUS_ERR_CAN_BUS_OFF if controller is in bus-off state.
 *
 * @note TIMING: Must not block indefinitely. Non-blocking preferred.
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
    can_transmit_fn    transmit;    /**< Required: frame transmission function. */
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
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_CAN_NOT_READY if transport not ready.
 * @return Return value of ops->transmit() otherwise.
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
