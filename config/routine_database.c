/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/routine_database.c
 *
 * PURPOSE: Routine Identifier (RID) database — fixed-capacity static storage
 *          with runtime registration API.
 *
 *          Architecture:
 *            - s_routine_table[UDS_MAX_ROUTINE_COUNT] is the backing store.
 *            - routine_database_init() clears the table and arms the guard.
 *            - routine_database_register() copies entries in at init time,
 *              called from the generated routine_handlers_register_all().
 *            - routine_database_find() performs read-only linear lookup.
 *
 *          This design allows generated/routine_handlers.c to own routine
 *          content (derived from YAML) while this file owns storage and
 *          lookup. No modification to this file is required when routines
 *          change — only re-running tools/codegen.py is needed.
 *
 * SAFETY  : No dynamic memory. Static array bounded at compile time.
 *           All public functions guard NULL inputs explicitly.
 *           Register path is init-only; no concurrent access after startup.
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "routine_database.h"
#include "uds_types.h"

#include <string.h>
#include <stddef.h>

/* =============================================================================
 * Internal state
 * ============================================================================= */

/** Fixed-capacity routine registration table. */
static routine_entry_t s_routine_table[UDS_MAX_ROUTINE_COUNT];

/** Number of entries currently registered. */
static uint16_t s_routine_count = (uint16_t)0U;

/** Initialization guard. */
static bool s_initialized = false;

/* =============================================================================
 * Public API implementations
 * ============================================================================= */

uds_status_t routine_database_init(void)
{
    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    (void)memset(s_routine_table, 0, sizeof(s_routine_table));
    s_routine_count = (uint16_t)0U;
    s_initialized   = true;

    return UDS_STATUS_OK;
}

uds_status_t routine_database_register(const routine_entry_t *entry)
{
    uint16_t i;

    if (entry == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* start_cb is mandatory — every routine must support startRoutine. */
    if (entry->start_cb == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (s_routine_count >= (uint16_t)UDS_MAX_ROUTINE_COUNT) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* Reject duplicate RIDs. */
    for (i = (uint16_t)0U; i < s_routine_count; i++) {
        if (s_routine_table[i].rid == entry->rid) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
    }

    /* Validate support_flags: at minimum ROUTINE_SUPPORT_START must be set. */
    if ((entry->support_flags & (uint8_t)ROUTINE_SUPPORT_START) == (uint8_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    s_routine_table[s_routine_count] = *entry;
    s_routine_count++;

    return UDS_STATUS_OK;
}

const routine_entry_t *routine_database_find(uint16_t rid)
{
    uint16_t i;

    if (!s_initialized) {
        return NULL;
    }

    for (i = (uint16_t)0U; i < s_routine_count; i++) {
        if (s_routine_table[i].rid == rid) {
            return &s_routine_table[i];
        }
    }

    return NULL;
}

uds_status_t routine_database_get_count(uint16_t *out_count)
{
    if (out_count == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    *out_count = s_routine_count;

    return UDS_STATUS_OK;
}

// End of routine_database.c
