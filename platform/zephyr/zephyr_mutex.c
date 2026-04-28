// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_mutex.c
 *
 * PURPOSE: Implements diag_mutex_t over Zephyr k_mutex.
 *
 * SAFETY  : ASIL-B candidate — mutex correctness is prerequisite to
 *           data integrity of session and security contexts.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "zephyr_mutex.h"
#include "uds_types.h"

#include <zephyr/kernel.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Compile-time size guard
 *
 * If Zephyr ever grows struct k_mutex beyond DIAG_MUTEX_OPAQUE_SIZE bytes,
 * this assertion will catch it at compile time rather than producing silent
 * memory corruption.
 * -------------------------------------------------------------------------- */
BUILD_ASSERT(sizeof(struct k_mutex) <= DIAG_MUTEX_OPAQUE_SIZE,
             "DIAG_MUTEX_OPAQUE_SIZE is too small for struct k_mutex — "
             "increase DIAG_MUTEX_OPAQUE_SIZE in zephyr_mutex.h");

/* --------------------------------------------------------------------------
 * Helper: extract embedded k_mutex pointer from opaque handle
 * -------------------------------------------------------------------------- */
static struct k_mutex *to_kmutex(diag_mutex_t *m)
{
    /* MISRA-11.3: intentional cast — opaque storage proven compatible above. */
    return (struct k_mutex *)(void *)m->_opaque;  /* NOLINT */
}

/* --------------------------------------------------------------------------
 * Public API implementations
 * -------------------------------------------------------------------------- */

uds_status_t diag_mutex_init(diag_mutex_t *m)
{
    int rc;

    if (m == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    (void)memset(m->_opaque, 0, sizeof(m->_opaque));

    rc = k_mutex_init(to_kmutex(m));
    if (rc != 0) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}

uds_status_t diag_mutex_lock(diag_mutex_t *m)
{
    int rc;

    if (m == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    rc = k_mutex_lock(to_kmutex(m), K_FOREVER);
    if (rc != 0) {
        /*
         * K_FOREVER should not time out under normal conditions.
         * A non-zero return here indicates a kernel-level invariant violation
         * (e.g. calling from ISR context) — treat as fatal platform error.
         */
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}

uds_status_t diag_mutex_unlock(diag_mutex_t *m)
{
    int rc;

    if (m == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    rc = k_mutex_unlock(to_kmutex(m));
    if (rc != 0) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}

uds_status_t diag_mutex_trylock(diag_mutex_t *m, bool *acquired)
{
    int rc;

    if ((m == NULL) || (acquired == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    rc = k_mutex_lock(to_kmutex(m), K_NO_WAIT);
    *acquired = (rc == 0);

    return UDS_STATUS_OK;
}
