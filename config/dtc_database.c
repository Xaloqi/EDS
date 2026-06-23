// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: config/dtc_database.c
 *
 * PURPOSE: DTC database implementation — fixed-capacity static storage with
 *          runtime registration API.
 *
 *          Architecture:
 *            - s_dtc_table[UDS_MAX_DTC_COUNT] is the backing store.
 *            - dtc_database_init() zeros the table and arms the guard.
 *            - dtc_database_register() is called from generated uds_init.c
 *              during stack initialisation to populate DTC entries derived
 *              from diagnostics_config.yaml.
 *            - dtc_database_find() performs read-only linear lookup.
 *            - dtc_database_set_status() is the sole runtime mutation path.
 *
 *          No modification to this file is required when DTCs change in
 *          the YAML config — only re-running tools/codegen.py is needed.
 *
 * SAFETY  : No dynamic memory. Static array bounded at compile time.
 *           All public functions guard NULL and uninitialised-state inputs.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "dtc_database.h"
#include "uds_types.h"

#include <string.h>
#include <stddef.h>
#include <stdbool.h>

/* =============================================================================
 * Internal state
 * ============================================================================= */

/**
 * @brief Fixed-capacity DTC registration table.
 *
 * Not const — status_byte is updated at runtime by dtc_database_set_status().
 * SAFETY: Sized to UDS_MAX_DTC_COUNT (128). Codegen must reject configs with
 *         more DTCs than this limit.
 */
static dtc_entry_t s_dtc_table[UDS_MAX_DTC_COUNT];

/** Number of DTCs currently registered. */
static uint16_t s_dtc_count = (uint16_t)0U;

/** Initialization guard. */
static bool s_initialized = false;

/* =============================================================================
 * Public API implementations
 * ============================================================================= */

uds_status_t dtc_database_init(void)
{
    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    (void)memset(s_dtc_table, 0, sizeof(s_dtc_table));
    s_dtc_count   = (uint16_t)0U;
    s_initialized = true;

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_register(
    uint32_t    dtc_code,
    uint8_t     severity,
    const char *description)
{
    uint16_t i;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /* DTC code 0 is reserved / invalid */
    if (dtc_code == (uint32_t)0U) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Capacity guard */
    if (s_dtc_count >= (uint16_t)UDS_MAX_DTC_COUNT) {
        return UDS_STATUS_ERR_BUFFER_OVERFLOW;
    }

    /* Reject duplicate DTC codes */
    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        if (s_dtc_table[i].dtc_code == dtc_code) {
            return UDS_STATUS_ERR_INVALID_PARAM;
        }
    }

    s_dtc_table[s_dtc_count].dtc_code                = dtc_code;
    s_dtc_table[s_dtc_count].status_byte             = (uint8_t)0x00U;
    s_dtc_table[s_dtc_count].severity                = severity;
    s_dtc_table[s_dtc_count].fault_detection_counter = (uint8_t)0x00U;
    s_dtc_table[s_dtc_count].is_permanent            = false;
    s_dtc_table[s_dtc_count].description             = description; /* pointer copy — string must be static */
    s_dtc_count++;

    return UDS_STATUS_OK;
}

dtc_entry_t *dtc_database_find(uint32_t dtc_code)
{
    uint16_t i;

    if (!s_initialized) {
        return NULL;
    }

    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        if (s_dtc_table[i].dtc_code == dtc_code) {
            return &s_dtc_table[i];
        }
    }

    return NULL;
}

uds_status_t dtc_database_set_status(uint32_t dtc_code, uint8_t status_byte)
{
    dtc_entry_t *entry;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    entry = dtc_database_find(dtc_code);
    if (entry == NULL) {
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    entry->status_byte = status_byte;

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_clear_all(void)
{
    uint16_t i;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        s_dtc_table[i].status_byte = (uint8_t)0x00U;
    }

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_count_by_status(
    uint8_t   status_mask,
    uint16_t *out_count)
{
    uint16_t i;
    uint16_t count;

    if (out_count == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    count = (uint16_t)0U;
    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        if ((s_dtc_table[i].status_byte & status_mask) != (uint8_t)0U) {
            count++;
        }
    }

    *out_count = count;

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_get_by_index(
    uint16_t  index,
    uint32_t *out_dtc_code,
    uint8_t  *out_status)
{
    if ((out_dtc_code == NULL) || (out_status == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    if (index >= s_dtc_count) {
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    *out_dtc_code = s_dtc_table[index].dtc_code;
    *out_status   = s_dtc_table[index].status_byte;

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_set_fault_counter(uint32_t dtc_code, uint8_t counter)
{
    dtc_entry_t *entry;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    entry = dtc_database_find(dtc_code);
    if (entry == NULL) {
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    entry->fault_detection_counter = counter;

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_set_permanent(uint32_t dtc_code, bool permanent)
{
    dtc_entry_t *entry;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    entry = dtc_database_find(dtc_code);
    if (entry == NULL) {
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }

    entry->is_permanent = permanent;

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_clear_non_permanent(void)
{
    uint16_t i;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        if (!s_dtc_table[i].is_permanent) {
            s_dtc_table[i].status_byte = (uint8_t)0x00U;
        }
    }

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_get_count(uint16_t *out_count)
{
    if (out_count == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    *out_count = s_dtc_count;
    return UDS_STATUS_OK;
}

// ✅ End of dtc_database.c

/* --------------------------------------------------------------------------
 * Test-only reset function.
 *
 * [MISRA 8.7] Guarded by UNIT_TEST so the symbol only exists in test builds.
 * The prototype in dtc_database.h is likewise guarded by UNIT_TEST.
 * -------------------------------------------------------------------------- */
#ifdef UNIT_TEST
void dtc_database_test_reset(void)
{
    uint16_t i;
    for (i = 0U; i < (uint16_t)UDS_MAX_DTC_COUNT; i++) {
        s_dtc_table[i].dtc_code                = 0U;
        s_dtc_table[i].status_byte             = 0U;
        s_dtc_table[i].severity                = 0U;
        s_dtc_table[i].fault_detection_counter = 0U;
        s_dtc_table[i].is_permanent            = false;
        s_dtc_table[i].description             = NULL;
    }
    s_dtc_count   = (uint16_t)0U;
    s_initialized = false;
}
#endif /* UNIT_TEST */

