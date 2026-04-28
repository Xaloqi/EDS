// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_mutex.h
 *
 * PURPOSE: Thin mutex abstraction over Zephyr k_mutex.
 *
 *          The UDS session and security contexts are shared between the
 *          diagnostics thread (reads/writes) and the S3server timeout path
 *          (writes session type on timeout). These must be mutex-protected
 *          whenever preemptive scheduling is used.
 *
 *          This header is intentionally minimal — it exposes only what the
 *          diagnostics stack needs and hides all Zephyr-specific types from
 *          callers. Callers that do not include <zephyr/kernel.h> can still
 *          use these functions by treating diag_mutex_t as an opaque handle.
 *
 * USAGE:
 *          diag_mutex_t lock;
 *          diag_mutex_init(&lock);
 *          diag_mutex_lock(&lock);
 *          // ... critical section ...
 *          diag_mutex_unlock(&lock);
 *
 * SAFETY  : ASIL-B candidate. Mutex ordering must be documented to avoid
 *           priority inversion (use CONFIG_MUTEX_SPIN_ON_OWNER if suitable).
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef ZEPHYR_MUTEX_H
#define ZEPHYR_MUTEX_H

#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Opaque mutex handle
 *
 * The actual storage (struct k_mutex) is embedded inside diag_mutex_t so
 * callers do not need to include <zephyr/kernel.h>.
 *
 * sizeof(struct k_mutex) is guaranteed to fit in DIAG_MUTEX_OPAQUE_SIZE bytes
 * on all Zephyr-supported architectures. The static_assert in the .c file
 * enforces this at compile time.
 * -------------------------------------------------------------------------- */
#define DIAG_MUTEX_OPAQUE_SIZE   (64U)

typedef struct diag_mutex {
    uint8_t _opaque[DIAG_MUTEX_OPAQUE_SIZE]; /**< Storage for struct k_mutex. */
} diag_mutex_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Statically initialise a mutex object.
 *
 * Must be called before any lock/unlock operation.
 * Safe to call from any context (does not block).
 *
 * @param[out] m  Caller-allocated mutex object.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if m is NULL.
 */
uds_status_t diag_mutex_init(diag_mutex_t *m);

/**
 * @brief Acquire the mutex (blocking).
 *
 * Blocks the calling thread until the mutex is available.
 * Must not be called from ISR context.
 *
 * @param[in] m  Initialised mutex.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if m is NULL.
 * @return UDS_STATUS_ERR_TIMEOUT if the kernel reports a timeout (K_FOREVER
 *         should not time out under normal conditions — treated as fatal).
 *
 * @note SAFETY: Acquiring the same mutex twice from the same thread is an
 *               error — Zephyr k_mutex is NOT reentrant by default.
 */
uds_status_t diag_mutex_lock(diag_mutex_t *m);

/**
 * @brief Release the mutex.
 *
 * Must be called from the same thread that acquired it.
 * Must not be called from ISR context.
 *
 * @param[in] m  Initialised and currently-held mutex.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if m is NULL.
 */
uds_status_t diag_mutex_unlock(diag_mutex_t *m);

/**
 * @brief Attempt to acquire the mutex without blocking.
 *
 * Returns immediately regardless of whether the mutex was acquired.
 *
 * @param[in]  m        Initialised mutex.
 * @param[out] acquired Set to true if the mutex was acquired.
 *
 * @return UDS_STATUS_OK on success (check *acquired for result).
 * @return UDS_STATUS_ERR_NULL_PTR if any pointer is NULL.
 */
uds_status_t diag_mutex_trylock(diag_mutex_t *m, bool *acquired);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_MUTEX_H */
