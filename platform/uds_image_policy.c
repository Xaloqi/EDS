/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/uds_image_policy.c
 *
 * PURPOSE: Image verification policy table global registration — shared
 *          singleton (issue #232, Phase 1).
 *
 * This translation unit holds the single global pointer to the registered
 * uds_image_policy_t.  It is the only place where the pointer is modified,
 * so any future thread-safety requirement can be addressed here without
 * touching service code.  Same shape as platform/uds_flash_ops.c.
 *
 * SAFETY:
 *   REQ-IMGPOL-001: Registration must occur before any 0x34 request on a
 *                   production build.
 *   REQ-IMGPOL-004: NULL is refused by uds_image_policy_register(); the
 *                   only way to reach the unregistered state again is the
 *                   explicitly-named, test-only uds_image_policy_clear().
 *   No dynamic allocation.  No recursion.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "uds_image_policy.h"
#include "uds_types.h"

#include <stddef.h>

/* --------------------------------------------------------------------------
 * Static singleton
 * -------------------------------------------------------------------------- */

/** @brief Currently registered image policy. NULL until registered. */
static const uds_image_policy_t *s_image_policy = NULL;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

uds_status_t uds_image_policy_register(const uds_image_policy_t *policy)
{
    /* REQ-IMGPOL-004: NULL is not a de-registration here — see the header
     * for why this deliberately differs from uds_flash_ops_register(). */
    if (policy == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Validate that all mandatory callbacks are populated.  A policy with a
     * NULL verification hook is a gate that verifies nothing. */
    if (policy->begin_cb == NULL) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (policy->update_cb == NULL) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (policy->finalise_cb == NULL) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    /* commit_cb and abort_cb are optional — NULL is a valid, documented
     * configuration and must not be rejected here. */

    s_image_policy = policy;
    return UDS_STATUS_OK;
}

const uds_image_policy_t *uds_image_policy_get(void)
{
    return s_image_policy;
}

void uds_image_policy_clear(void)
{
    s_image_policy = NULL;
}

void uds_image_policy_notify_abort(void)
{
    const uds_image_policy_t *policy = s_image_policy;

    if (policy != NULL) {
        if (policy->abort_cb != NULL) {
            policy->abort_cb();
        }
    }
}
