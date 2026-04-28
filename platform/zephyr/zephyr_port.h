// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_port.h
 *
 * PURPOSE: Platform abstraction layer (PAL) public API for Zephyr RTOS.
 *          Provides hardware-independent services required by the diagnostics
 *          stack: millisecond timestamps, CAN initialization, and ECU reset.
 *
 *          All platform-specific dependencies are contained within
 *          zephyr_port.c. No Zephyr headers are included here.
 *
 * SAFETY  : Platform services include safety-relevant functions (reset).
 *           ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 *
 * [PLATFORM REFACTOR — INCLUDE GUARD]
 *
 *   This file was originally at transport/zephyr_port.h. It was moved to
 *   platform/zephyr/zephyr_port.h during the platform layer refactor
 *   (phase-housekeeping). The include guard was renamed from ZEPHYR_PORT_H
 *   to ZEPHYR_PLATFORM_PORT_H at the same time.
 *
 *   transport/zephyr_port.h no longer exists. This file in platform/zephyr/
 *   is the sole canonical copy.
 *
 * [NEW-H1 FIX — zephyr_can_platform_init SIGNATURE]
 *
 *   Updated from stale 2-parameter to correct 3-parameter signature to match
 *   zephyr_can.h (authoritative) and zephyr_can.c (definition).
 * =============================================================================
 */

#ifndef ZEPHYR_PLATFORM_PORT_H   /* [NEW-H1 FIX] Renamed from ZEPHYR_PORT_H */
#define ZEPHYR_PLATFORM_PORT_H

#include "uds_types.h"
#include "can_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Forward declaration for Zephyr device (avoids including zephyr/device.h here)
 * -------------------------------------------------------------------------- */
struct device;

/* --------------------------------------------------------------------------
 * Platform configuration
 * -------------------------------------------------------------------------- */

/**
 * @brief Platform initialization configuration block.
 */
typedef struct zephyr_port_cfg {
    const struct device *can_dev;         /**< Zephyr CAN device (from DEVICE_DT_GET). */
    uint32_t             physical_rx_id;  /**< Physical RX CAN ID (0 = functional only).
                                           *   [P2-3] Passed to zephyr_can_platform_init.
                                           *   Typical: 0x7E0U for ECU 0. */
} zephyr_port_cfg_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialize the Zephyr platform abstraction layer.
 *
 * Must be called before any other platform or diagnostics API.
 * Initializes CAN controller and acquires the transport interface.
 *
 * @param[in]  cfg            Platform configuration block.
 * @param[out] out_transport  Set to point to the initialized CAN transport.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_PLATFORM if CAN driver initialization failed.
 */
uds_status_t zephyr_port_init(
    const zephyr_port_cfg_t *cfg,
    can_transport_t        **out_transport
);

/**
 * @brief Return the current system time in milliseconds.
 *
 * @return Monotonically increasing millisecond timestamp.
 *
 * @note TIMING: Source is Zephyr k_uptime_get_32(). Wraps at ~49.7 days.
 * @note SAFETY: Must not be called from ISR context.
 */
uds_timestamp_ms_t zephyr_port_get_time_ms(void);

/**
 * @brief Delay execution for the specified number of milliseconds.
 *
 * Uses Zephyr k_msleep(). Must not be called from ISR context.
 *
 * @param[in] ms  Duration to sleep in milliseconds.
 */
void zephyr_port_delay_ms(uint32_t ms);

/**
 * @brief Trigger a controlled ECU reset.
 *
 * Invoked by service 0x11 (ECUReset) after transmitting the positive response.
 *
 * @param[in] reset_type  Reset type sub-function value (0x01/0x02/0x03).
 *
 * @return Does not return on success (executes hardware reset).
 * @return UDS_STATUS_ERR_PLATFORM if reset could not be executed.
 *
 * @note SAFETY: ASIL-B relevant. Reset must coordinate with safety monitors.
 *               This function must not return if reset_type is valid.
 */
uds_status_t zephyr_port_ecu_reset(uint8_t reset_type);

/**
 * @brief Flush NVM / persist diagnostic counters before reset.
 *
 * [P2-0x11-02] Must be called after positive response for SID 0x11 has been
 * transmitted and before invoking zephyr_port_ecu_reset().
 *
 * Persists: DTC status, security attempt counters, session counters, and any
 * other ECU-lifecycle data that must survive the reset.
 *
 * @return UDS_STATUS_OK if flush completed successfully.
 * @return UDS_STATUS_ERR_PLATFORM if NVM write failed (non-fatal to reset sequence).
 *
 * @note SAFETY: Best-effort — reset proceeds even if flush returns an error.
 *               Loss of diagnostic counters is acceptable; loss of safety data
 *               is not. Safety-critical data should be written eagerly, not here.
 */
uds_status_t zephyr_port_nvm_flush(void);

/**
 * @brief Enter a critical section (disable relevant interrupts).
 *
 * Used to protect shared state in the single-threaded diagnostics task
 * from concurrent modification by ISR-context code (e.g. CAN RX ISR).
 *
 * @return Opaque key value to pass to zephyr_port_exit_critical().
 *
 * @note SAFETY: Must be paired with zephyr_port_exit_critical().
 */
uint32_t zephyr_port_enter_critical(void);

/**
 * @brief Exit a critical section (restore interrupt mask).
 *
 * @param[in] key  Key value returned by zephyr_port_enter_critical().
 */
void zephyr_port_exit_critical(uint32_t key);

/**
 * @brief Initialize the Zephyr CAN platform adapter.
 *
 * Called internally by zephyr_port_init(). Exposed for direct use
 * in test harnesses and application code that needs direct CAN control.
 *
 * Installs one or two RX filters:
 *   Filter 1 (always):   0x7DF  — ISO 15765-4 functional broadcast.
 *   Filter 2 (optional): physical_rx_id — point-to-point ECU address.
 *
 * @param[out] out_transport  Receives pointer to initialized transport.
 * @param[in]  can_dev        Zephyr CAN device pointer.
 * @param[in]  physical_rx_id Physical RX CAN ID. Pass 0U to disable.
 *                            Typical: 0x7E0–0x7E7 (ISO 15765-4).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 * @return UDS_STATUS_ERR_CAN_NOT_READY if CAN device not ready.
 * @return UDS_STATUS_ERR_PLATFORM if mode/start/functional-filter fails.
 *
 * [NEW-H1 FIX] Updated from 2-parameter to 3-parameter signature to match
 * the definition in zephyr_can.c and the declaration in zephyr_can.h.
 */
uds_status_t zephyr_can_platform_init(
    can_transport_t    **out_transport,
    const struct device *can_dev,
    uint32_t             physical_rx_id   /* [P2-3] 0 = functional-only */
);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_PLATFORM_PORT_H */
