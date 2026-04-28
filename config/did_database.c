// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/did_database.c
 *
 * PURPOSE: DID database implementation — fixed-capacity static storage with
 *          runtime registration API.
 *
 *          Architecture:
 *            - s_did_table[UDS_MAX_DID_COUNT] is the backing store.
 *            - did_database_init() clears the table and arms the guard.
 *            - did_database_register() copies entries in at init time,
 *              called from the generated did_handlers_register_all().
 *            - did_database_find() performs read-only linear lookup.
 *
 *          This design allows generated/did_handlers.c to own DID content
 *          (derived from YAML) while this file owns storage and lookup.
 *          No modification to this file is required when DIDs change —
 *          only re-running tools/codegen.py is needed.
 *
 * SAFETY  : No dynamic memory. Static array bounded at compile time.
 *           All public functions guard NULL inputs explicitly.
 *           Register path is init-only; no concurrent access after startup.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "did_database.h"
#include "uds_types.h"

#include <string.h>
#include <stddef.h>

/* =============================================================================
 * Internal state
 *
 * MISRA C:2012 Rule 8.7: File-scope static — not visible outside this TU.
 * MISRA C:2012 Rule 21.3: No heap. Static array with compile-time bound.
 * ============================================================================= */

/**
 * @brief Fixed-capacity DID registration table.
 *
 * Entries are copied in by did_database_register() during initialisation.
 * Not const — entries are written once during init, then treated as read-only.
 *
 * SAFETY: Sized to UDS_MAX_DID_COUNT (64). Any YAML config requesting more
 *         than 64 DIDs must be rejected by the codegen validation step.
 */
static did_entry_t s_did_table[UDS_MAX_DID_COUNT];

/** Number of entries currently registered. */
static uint16_t s_did_count = (uint16_t)0U;

/** Initialization guard. */
static bool s_initialized = false;

/* =============================================================================
 * Public API implementations
 * ============================================================================= */

uds_status_t did_database_init(void)
{
    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    /*
     * Zero the table so unregistered slots are deterministically empty.
     * MISRA C:2012 Rule 21.6: memset on a plain-old-data struct is safe.
     */
    (void)memset(s_did_table, 0, sizeof(s_did_table));
    s_did_count    = (uint16_t)0U;
    s_initialized  = true;

    return UDS_STATUS_OK;
}

uds_status_t did_database_register(const did_entry_t *entry)
{
    uint16_t i;

    /* [REQ-SAFE-004] NULL guard */
    if (entry == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* Capacity guard */
    if (s_did_count >= (uint16_t)UDS_MAX_DID_COUNT) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* Validate data_length — must be 1..DID_MAX_DATA_LEN */
    if ((entry->data_length == (uint16_t)0U) ||
        (entry->data_length > (uint16_t)DID_MAX_DATA_LEN)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Reject duplicate DID IDs */
    for (i = (uint16_t)0U; i < s_did_count; i++) {
        if (s_did_table[i].did_id == entry->did_id) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
    }

    /*
     * Copy entry into the table.
     * The caller's did_entry_t may be stack-local or const-static; either
     * is safe because we copy by value here.
     */
    s_did_table[s_did_count] = *entry;
    s_did_count++;

    return UDS_STATUS_OK;
}

const did_entry_t *did_database_find(uint16_t did_id)
{
    uint16_t i;

    if (!s_initialized) {
        return NULL;
    }

    for (i = (uint16_t)0U; i < s_did_count; i++) {
        if (s_did_table[i].did_id == did_id) {
            return &s_did_table[i];
        }
    }

    return NULL;
}

uds_status_t did_database_get_count(uint16_t *out_count)
{
    if (out_count == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    *out_count = s_did_count;

    return UDS_STATUS_OK;
}

// ✅ End of did_database.c
