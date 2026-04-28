// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/platform_api.h
 *
 * PURPOSE: Platform Abstraction Interface — the contract both HALs implement.
 *
 *          This header declares every function that core/ and the integration
 *          layer (main.c / application) call that must be implemented
 *          differently per RTOS.
 *
 *          IMPLEMENTATIONS:
 *            platform/zephyr/zephyr_platform_api.c  — Zephyr k_*, sys_reboot
 *            platform/freertos/freertos_platform_api.c — FreeRTOS + callbacks
 *
 *          DESIGN RULES:
 *            1. No RTOS types in signatures.
 *               No k_mutex_t, no SemaphoreHandle_t, no struct device.
 *            2. No dynamic allocation. All storage is caller-provided or
 *               statically allocated inside the implementation.
 *            3. All functions return uds_status_t or void.
 *               No RTOS-specific error codes leak into the interface.
 *            4. Existing stable names (diag_mutex_t, nvm_store_*) are kept
 *               as-is — they were already platform-neutral in signature.
 *               New functions use the eds_platform_* prefix.
 *
 *          WHAT IS NOT IN THIS FILE:
 *            - diag_timer_t  — application-layer concern (main.c only).
 *              FreeRTOS main.c uses FreeRTOS timers directly; Zephyr main.c
 *              uses diag_timer_t from platform/zephyr/zephyr_timer.h.
 *            - diag_wdt_t    — same reasoning as diag_timer_t.
 *            - zephyr_port_init / zephyr_can_platform_init — Zephyr-specific
 *              startup, replaced by eds_platform_init() below.
 *
 * SAFETY  : ASIL-B candidate. Reset and NVM flush paths are safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef EDS_PLATFORM_API_H
#define EDS_PLATFORM_API_H

#include "uds_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CAN frame type (platform-independent)
 *
 * Used by eds_platform_init() callbacks and freertos_can.c.
 * Zephyr transport converts between this and struct can_frame internally.
 * ========================================================================== */

typedef struct {
    uint32_t id;           /**< CAN frame ID (11-bit or 29-bit). */
    uint8_t  data[8];      /**< CAN data bytes (classic CAN, max 8). */
    uint8_t  dlc;          /**< Data length code (0–8). */
    bool     is_extended;  /**< True if 29-bit extended ID. */
} eds_can_frame_t;

/* ============================================================================
 * Mutex
 *
 * WHY THE MUTEX INTERFACE IS NOT DECLARED HERE — DESIGN RATIONALE
 * ---------------------------------------------------------------
 * The mutex type and its lock/unlock functions are deliberately NOT declared
 * in this file, even though they are part of the platform abstraction.
 * This is an intentional design decision, not an oversight.
 *
 * The interface is split for one concrete reason: including both
 * platform/zephyr/zephyr_mutex.h and platform_api.h in the same translation
 * unit (which happens in zephyr_port_mock.c and the integration layer
 * main.c on Zephyr) would cause duplicate-declaration errors because
 * zephyr_mutex.h is already included transitively via zephyr_port.h.
 *
 * The mutex declarations ARE platform-neutral in signature — diag_mutex_t
 * is an opaque handle, and diag_mutex_lock/unlock/init take only that handle
 * and return uds_status_t. They satisfy Design Rules 1 and 3 above. They
 * are simply declared in the platform-specific header rather than here to
 * avoid the include-guard collision.
 *
 * WHERE TO FIND THE DECLARATIONS:
 *   Zephyr   : platform/zephyr/zephyr_mutex.h   — diag_mutex_t, diag_mutex_*
 *   FreeRTOS : platform/freertos/freertos_can.h  — mutex is internal to the
 *              platform layer; the integration layer (main.c) does not call
 *              mutex functions directly on FreeRTOS.
 *
 * CALLERS:
 *   main.c and the ISO-TP integration layer on Zephyr use diag_mutex_t to
 *   protect on_isotp_rx_complete() against concurrent calls from the CAN
 *   ISR and the diagnostics thread. On FreeRTOS this protection is provided
 *   by the task model (the UDS poll task is the sole accessor) and the
 *   ISR-safe RX queue in freertos_can.c — no explicit mutex is needed in
 *   the integration layer.
 *
 * SUMMARY: The interface is deliberately split. platform_api.h is the
 * primary contract. zephyr_mutex.h is an addendum for Zephyr callers only.
 * ========================================================================== */

/* (Mutex declarations are in platform/zephyr/zephyr_mutex.h — see above.) */

/* ============================================================================
 * System — reset and time
 *
 * Caller: the integration layer (diagnostics task / main.c) reads
 *         ctx->pending_reset_type after 0x11 dispatch and calls these.
 *         service_0x11.c itself does NOT call these — it only sets the flag.
 *
 * Zephyr impl  : platform/zephyr/zephyr_platform_api.c
 * FreeRTOS impl: platform/freertos/freertos_platform_api.c
 * Host test    : tests/mocks/zephyr_port_mock.c
 * ========================================================================== */

/**
 * @brief Trigger a controlled ECU reset.
 *
 * @param reset_type  0x01 hardReset, 0x02 keyOffOnReset, 0x03 softReset
 *                    (ISO 14229-1 Table 186 sub-function values).
 *
 * @return Does not return on success.
 * @return UDS_STATUS_ERR_PLATFORM if reset_type is unsupported or reset fails.
 *
 * @note SAFETY: Caller must transmit the positive response and flush NVM
 *               via eds_platform_nvm_flush() BEFORE calling this function.
 *               This function must not return if reset_type is valid.
 */
uds_status_t eds_platform_ecu_reset(uint8_t reset_type);

/**
 * @brief Persist diagnostic counters before ECU reset.
 *
 * Flushes: DTC status-byte mirror, security attempt counter (already eager),
 *          session statistics, ECU lifecycle counter.
 *
 * Called by the integration layer immediately before eds_platform_ecu_reset().
 * Best-effort — reset proceeds even if this returns an error.
 *
 * @return UDS_STATUS_OK if all flushes succeeded.
 * @return UDS_STATUS_ERR_PLATFORM if any NVM write failed (non-fatal to reset).
 */
uds_status_t eds_platform_nvm_flush(void);

/**
 * @brief Return system uptime in milliseconds.
 *
 * Monotonically increasing. Wraps at UINT32_MAX (~49.7 days).
 * Used by ISO-TP and UDS session timers.
 *
 * Zephyr: k_uptime_get_32()
 * FreeRTOS: xTaskGetTickCount() * portTICK_PERIOD_MS
 *
 * @return Uptime in milliseconds.
 */
uint32_t eds_platform_uptime_ms(void);

/* ============================================================================
 * Platform init
 *
 * Called once at startup before uds_generated_init().
 *
 * Zephyr:   cfg fields are ignored — DTS + Kconfig wire CAN and NVM.
 *           Pass NULL or a zeroed struct.
 * FreeRTOS: Customer provides can_send callback and optional NVM ops.
 *           The platform layer creates the UDS task internally.
 * ========================================================================== */

/**
 * @brief Customer-provided CAN send function (FreeRTOS only).
 *
 * Called by eds_can_send() when the UDS stack needs to transmit a frame.
 * Must be callable from task context (not necessarily ISR-safe).
 *
 * @param[in] frame  Frame to transmit.
 * @return UDS_STATUS_OK on success, UDS_STATUS_ERR_PLATFORM on failure.
 */
typedef uds_status_t (*eds_can_send_fn_t)(const eds_can_frame_t *frame);

/**
 * @brief NVM operations table (optional FreeRTOS override).
 *
 * If all three function pointers are NULL, the platform uses a built-in
 * RAM-backed stub (development/testing only, not persistent).
 * For production FreeRTOS targets, populate all three with flash driver calls.
 *
 * Matches the nvm_store_* API contract: key-value store, uint16_t keys.
 */
typedef struct {
    uds_status_t (*read) (uint16_t key, uint8_t *buf, size_t len,
                          size_t *out_len);
    uds_status_t (*write)(uint16_t key, const uint8_t *buf, size_t len);
    bool         (*is_ready)(void);
} eds_nvm_ops_t;

/**
 * @brief Platform initialisation configuration.
 *
 * Zephyr: all fields ignored — pass NULL or a zero-initialised struct.
 * FreeRTOS: populate can_send at minimum.
 */
typedef struct {
    /** CAN send function. Required for FreeRTOS. Ignored on Zephyr. */
    eds_can_send_fn_t  can_send;

    /** NVM ops. Optional for FreeRTOS (RAM stub used if NULL). Ignored on Zephyr. */
    eds_nvm_ops_t      nvm;

    /** UDS task stack size in bytes. 0 = use default (2048). FreeRTOS only. */
    uint32_t           uds_task_stack_size;

    /** UDS task RTOS priority. 0 = use default (5). FreeRTOS only. */
    uint32_t           uds_task_priority;
} eds_platform_cfg_t;

/**
 * @brief Initialise the platform abstraction layer.
 *
 * Must be called before nvm_store_init() and uds_generated_init().
 *
 * Zephyr: no-op — DTS + Kconfig handle everything. Returns UDS_STATUS_OK.
 * FreeRTOS: registers the customer's can_send callback and NVM ops.
 *            Does NOT start the scheduler — customer calls vTaskStartScheduler().
 *
 * @param[in] cfg  Configuration block. May be NULL on Zephyr.
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if cfg is NULL on FreeRTOS (can_send required).
 * @return UDS_STATUS_ERR_INVALID_PARAM if can_send is NULL on FreeRTOS.
 */
uds_status_t eds_platform_init(const eds_platform_cfg_t *cfg);

/**
 * @brief Feed a CAN frame into the platform RX path.
 *
 * FreeRTOS only. Call this from the customer's CAN RX interrupt or callback.
 * Thread-safe and ISR-safe — posts to an internal queue, does not call
 * the UDS stack directly.
 *
 * Zephyr: not used — Zephyr CAN driver callbacks handle this internally.
 *
 * @param[in] frame  Received CAN frame.
 */
void eds_platform_can_input(const eds_can_frame_t *frame);

/**
 * @brief Create and start the EDS UDS poll task (FreeRTOS only).
 *
 * This function encapsulates everything a FreeRTOS integrator must do after
 * eds_platform_init() and uds_generated_init() to make the UDS stack run:
 *   1. Creates the UDS poll task using static allocation (no heap).
 *   2. The task polls the CAN RX queue, drives ISO-TP timers, and drives
 *      the UDS server session/security timers at 1ms resolution.
 *   3. Returns to the caller — the application must call vTaskStartScheduler()
 *      after this function returns (or after creating its own tasks).
 *
 * CALL SEQUENCE (FreeRTOS integration):
 * @code
 *   eds_platform_init(&cfg);            // wire CAN send + NVM ops
 *   uds_generated_init(transport, rx_id, tx_id);  // start UDS stack
 *   eds_freertos_start();               // create poll task
 *   vTaskStartScheduler();             // hand control to FreeRTOS
 * @endcode
 *
 * Stack size and priority are taken from the values passed to
 * eds_platform_init() via eds_platform_cfg_t.uds_task_stack_size and
 * eds_platform_cfg_t.uds_task_priority. Defaults (2048 bytes / priority 5)
 * are used if those fields were zero.
 *
 * The ISO-TP RX completion callback (on_isotp_rx_complete) is wired
 * internally — the application does not need to provide it.
 *
 * Zephyr: this function does not exist. Zephyr wires the poll thread
 * internally via zephyr_port.c. Do not call this on Zephyr.
 *
 * @pre eds_platform_init() must have been called and returned UDS_STATUS_OK.
 * @pre uds_generated_init() must have been called and returned UDS_STATUS_OK.
 *
 * @return UDS_STATUS_OK            Task created successfully.
 * @return UDS_STATUS_ERR_NOT_INITIALIZED  eds_platform_init() was not called first.
 * @return UDS_STATUS_ERR_OS_RESOURCE     xTaskCreateStatic() returned NULL
 *                                        (should not happen with valid static storage).
 */
uds_status_t eds_freertos_start(void);

#ifdef __cplusplus
}
#endif

#endif /* EDS_PLATFORM_API_H */
