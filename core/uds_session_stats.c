// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_session_stats.c
 *
 * PURPOSE: Session statistics persistence implementation.
 * =============================================================================
 */

#include "uds_session_stats.h"
#include "nvm_store.h"
#include "uds_types.h"

#include <string.h>

static bool                s_initialized = false;
static nvm_session_stats_t s_stats;
static bool                s_dirty = false;  /* true = RAM ahead of NVM */

uds_status_t uds_session_stats_init(void)
{
    size_t       bytes_read;
    uds_status_t rc;

    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    (void)memset(&s_stats, 0, sizeof(s_stats));

    /*
     * MISRA C:2012 Rule 14.4: the controlling expression shall be essentially
     * Boolean. nvm_store_is_ready() returns bool; compare explicitly to true
     * to satisfy Rule 14.4 without a cast.
     */
    if (nvm_store_is_ready() == true) {
        rc = nvm_store_read(
            (uint16_t)NVM_KEY_SESSION_STATS,
            &s_stats, sizeof(s_stats), &bytes_read);

        if (rc == UDS_STATUS_ERR_DID_NOT_FOUND) {
            /* First boot — start from zero. */
            (void)memset(&s_stats, 0, sizeof(s_stats));
        } else if (rc != UDS_STATUS_OK) {
            /* NVM error — proceed with zero counters. */
            (void)memset(&s_stats, 0, sizeof(s_stats));
        } else if (bytes_read < sizeof(s_stats)) {
            /*
             * Partial read: older schema with fewer fields.
             * Zero-fill the remainder (forward-compatible migration).
             */
            (void)memset(
                (uint8_t *)(&s_stats) + bytes_read,
                0,
                sizeof(s_stats) - bytes_read);
        }
    }

    /* Increment reset counter on every power-on/init. */
    s_stats.total_resets++;
    s_dirty = true;

    s_initialized = true;
    return UDS_STATUS_OK;
}

void uds_session_stats_on_change(
    uds_session_type_t old_session,
    uds_session_type_t new_session)
{
    (void)old_session;

    if (!s_initialized) {
        return;
    }

    switch (new_session) {
        case UDS_SESSION_PROGRAMMING:
            s_stats.programming_session_count++;
            s_dirty = true;
            break;
        case UDS_SESSION_EXTENDED:
            s_stats.extended_session_count++;
            s_dirty = true;
            break;
        default:
            break; /* DEFAULT and SAFETY_SYSTEM not separately counted. */
    }
}

void uds_session_stats_record_security(bool was_unlock)
{
    if (!s_initialized) {
        return;
    }

    if (was_unlock) {
        s_stats.security_unlock_count++;
    } else {
        s_stats.security_lockout_count++;
    }
    s_dirty = true;
}

uds_status_t uds_session_stats_flush(void)
{
    uds_status_t rc;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (!nvm_store_is_ready()) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (!s_dirty) {
        return UDS_STATUS_OK; /* Nothing to write. */
    }

    rc = nvm_store_write(
        (uint16_t)NVM_KEY_SESSION_STATS,
        &s_stats, sizeof(s_stats));

    if (rc == UDS_STATUS_OK) {
        s_dirty = false;
    }

    return rc;
}

uds_status_t uds_session_stats_get(nvm_session_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    *out_stats = s_stats;
    return UDS_STATUS_OK;
}

/* Test-only reset function */
void uds_session_stats_test_reset(void)
{
    (void)memset(&s_stats, 0, sizeof(s_stats));
    s_initialized = false;
    s_dirty       = false;
}
