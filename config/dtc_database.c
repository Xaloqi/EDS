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
 *            - dtc_database_set_status() is the runtime status-update path
 *              (refused while ControlDTCSetting is off, EDS#448);
 *              dtc_database_restore_status() (boot-time mirror load) and the
 *              clear functions bypass that gate.
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
#include "uds_comm_control.h"   /* uds_comm_control_dtc_setting_is_on() [EDS#448] */

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

/** [EDS#602] ECU-wide FunctionalGroupIdentifier; valid only if s_functional_group_set. */
static uint8_t s_functional_group = 0U;
static bool    s_functional_group_set = false;

/** [EDS#145] First / most recent records, indexed by dtc_record_t; 0 = empty. */
static uint32_t s_records[DTC_RECORD_COUNT];

/* [EDS#354] Optional lock callbacks — see dtc_database_set_lock_callbacks(). */
/* [EDS#632] A status change that dtc_mirror must write (DTC_PERSIST_TRIGGER_MASK
 * changed on the monitor path) and has not written yet. Under the lock. */
static bool s_unsaved = false;

static dtc_database_lock_cb_t   s_dtc_lock_cb   = NULL;
static dtc_database_unlock_cb_t s_dtc_unlock_cb = NULL;

static void dtc_db_lock(void)
{
    if (s_dtc_lock_cb != NULL) {
        s_dtc_lock_cb();
    }
}

static void dtc_db_unlock(void)
{
    if (s_dtc_unlock_cb != NULL) {
        s_dtc_unlock_cb();
    }
}

/**
 * @brief Linear scan for dtc_code. Caller must hold the lock (if any).
 *
 * Internal, unlocked counterpart to the public dtc_database_find() —
 * every public function that needs to find-then-mutate an entry calls
 * this directly so the whole find+mutate sequence is one critical
 * section, not two (dtc_database_find() locks only its own scan).
 */
static dtc_entry_t *dtc_find_internal(uint32_t dtc_code)
{
    uint16_t i;

    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        if (s_dtc_table[i].dtc_code == dtc_code) {
            return &s_dtc_table[i];
        }
    }

    return NULL;
}

/* =============================================================================
 * Public API implementations
 * ============================================================================= */

void dtc_database_set_lock_callbacks(
    dtc_database_lock_cb_t   lock_cb,
    dtc_database_unlock_cb_t unlock_cb)
{
    s_dtc_lock_cb   = lock_cb;
    s_dtc_unlock_cb = unlock_cb;
}

uds_status_t dtc_database_init(void)
{
    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    (void)memset(s_dtc_table, 0, sizeof(s_dtc_table));
    (void)memset(s_records, 0, sizeof(s_records));
    s_dtc_count   = (uint16_t)0U;
    s_functional_group     = 0U;
    s_functional_group_set = false;
    s_unsaved     = false;
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
    s_dtc_table[s_dtc_count].status_byte             = (uint8_t)DTC_STATUS_INIT_AND_CLEARED; /* [EDS#448] */
    s_dtc_table[s_dtc_count].severity                = severity;
    s_dtc_table[s_dtc_count].fault_detection_counter = (uint8_t)0x00U;
    s_dtc_table[s_dtc_count].is_permanent            = false;
    s_dtc_table[s_dtc_count].description             = description; /* pointer copy — string must be static */
    s_dtc_count++;

    return UDS_STATUS_OK;
}

dtc_entry_t *dtc_database_find(uint32_t dtc_code)
{
    dtc_entry_t *entry;

    if (!s_initialized) {
        return NULL;
    }

    dtc_db_lock();
    entry = dtc_find_internal(dtc_code);
    dtc_db_unlock();

    return entry;
}

/* [EDS#145] Empty every record that names dtc_code. Caller holds the lock. */
static void records_forget(uint32_t dtc_code)
{
    uint32_t r;

    for (r = 0U; r < (uint32_t)DTC_RECORD_COUNT; r++) {
        if (s_records[r] == dtc_code) {
            s_records[r] = 0U;
        }
    }
}

/* [EDS#145] Record a monitor transition (ISO 14229-1:2013 §11.3.1.12/13):
 * a 0->1 edge of testFailed or confirmedDTC sets "most recent" always and
 * "first" only while it is empty. Caller holds the lock. */
static void records_note_transition(uint32_t dtc_code, uint8_t old_status,
                                    uint8_t new_status)
{
    uint8_t rising = (uint8_t)(new_status & (uint8_t)(~old_status));

    if ((rising & (uint8_t)DTC_STATUS_TEST_FAILED) != (uint8_t)0U) {
        if (s_records[DTC_RECORD_FIRST_TEST_FAILED] == 0U) {
            s_records[DTC_RECORD_FIRST_TEST_FAILED] = dtc_code;
        }
        s_records[DTC_RECORD_MOST_RECENT_TEST_FAILED] = dtc_code;
    }
    if ((rising & (uint8_t)DTC_STATUS_CONFIRMED_DTC) != (uint8_t)0U) {
        if (s_records[DTC_RECORD_FIRST_CONFIRMED] == 0U) {
            s_records[DTC_RECORD_FIRST_CONFIRMED] = dtc_code;
        }
        s_records[DTC_RECORD_MOST_RECENT_CONFIRMED] = dtc_code;
    }
}

/* Set a status byte without the ControlDTCSetting gate (restore/clear).
 * [EDS#145] note_transition: true only for the monitor path, the one
 * producer of first/most recent records; clear_fdc: true only for a clear,
 * which also forgets the records naming this DTC. */
static uds_status_t dtc_set_status_internal(uint32_t dtc_code, uint8_t status_byte,
                                            bool clear_fdc, bool note_transition)
{
    dtc_entry_t *entry;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    dtc_db_lock();
    entry = dtc_find_internal(dtc_code);
    if (entry != NULL) {
        if (note_transition) {
            records_note_transition(dtc_code, entry->status_byte, status_byte);
            /* [EDS#632] Only the monitor path: restore is NVM itself, and a
             * clear (0x14) writes the mirror at once. */
            if (((uint8_t)(entry->status_byte ^ status_byte) &
                 (uint8_t)DTC_PERSIST_TRIGGER_MASK) != (uint8_t)0U) {
                s_unsaved = true;
            }
        }
        entry->status_byte = status_byte;
        if (clear_fdc) {
            entry->fault_detection_counter = (uint8_t)0U;
            records_forget(dtc_code);
        }
    }
    dtc_db_unlock();

    return (entry != NULL) ? UDS_STATUS_OK : UDS_STATUS_ERR_DID_NOT_FOUND;
}

uds_status_t dtc_database_set_status(uint32_t dtc_code, uint8_t status_byte)
{
    /* [EDS#448] ISO 14229-1:2026 Annex D.2.1: no status transition while
     * ControlDTCSetting is off. (Not initialised = on, see
     * uds_comm_control_dtc_setting_is_on().) */
    if (!uds_comm_control_dtc_setting_is_on()) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }
    return dtc_set_status_internal(dtc_code, status_byte, false, true);
}

uds_status_t dtc_database_restore_status(uint32_t dtc_code, uint8_t status_byte)
{
    return dtc_set_status_internal(dtc_code, status_byte, false, false);
}

uds_status_t dtc_database_clear_one(uint32_t dtc_code)
{
    return dtc_set_status_internal(dtc_code, (uint8_t)DTC_STATUS_INIT_AND_CLEARED, true, false);
}

uds_status_t dtc_database_clear_all(void)
{
    uint16_t i;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    dtc_db_lock();
    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        s_dtc_table[i].status_byte             = (uint8_t)DTC_STATUS_INIT_AND_CLEARED; /* [EDS#448] */
        s_dtc_table[i].fault_detection_counter = (uint8_t)0U;                          /* [EDS#451] */
    }
    (void)memset(s_records, 0, sizeof(s_records));   /* [EDS#145] §11.3.1.12/13 */
    dtc_db_unlock();

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

    dtc_db_lock();
    count = (uint16_t)0U;
    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        if ((s_dtc_table[i].status_byte & status_mask) != (uint8_t)0U) {
            count++;
        }
    }
    dtc_db_unlock();

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

    dtc_db_lock();
    if (index >= s_dtc_count) {
        dtc_db_unlock();
        return UDS_STATUS_ERR_DID_NOT_FOUND;
    }
    *out_dtc_code = s_dtc_table[index].dtc_code;
    *out_status   = s_dtc_table[index].status_byte;
    dtc_db_unlock();

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_set_fault_counter(uint32_t dtc_code, uint8_t counter)
{
    dtc_entry_t *entry;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    dtc_db_lock();
    entry = dtc_find_internal(dtc_code);
    if (entry != NULL) {
        entry->fault_detection_counter = counter;
    }
    dtc_db_unlock();

    return (entry != NULL) ? UDS_STATUS_OK : UDS_STATUS_ERR_DID_NOT_FOUND;
}

uds_status_t dtc_database_set_permanent(uint32_t dtc_code, bool permanent)
{
    dtc_entry_t *entry;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    dtc_db_lock();
    entry = dtc_find_internal(dtc_code);
    if (entry != NULL) {
        entry->is_permanent = permanent;
    }
    dtc_db_unlock();

    return (entry != NULL) ? UDS_STATUS_OK : UDS_STATUS_ERR_DID_NOT_FOUND;
}

uds_status_t dtc_database_clear_non_permanent(void)
{
    uint16_t i;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    dtc_db_lock();
    for (i = (uint16_t)0U; i < s_dtc_count; i++) {
        if (!s_dtc_table[i].is_permanent) {
            s_dtc_table[i].status_byte             = (uint8_t)DTC_STATUS_INIT_AND_CLEARED; /* [EDS#448] */
            s_dtc_table[i].fault_detection_counter = (uint8_t)0U;                          /* [EDS#451] */
        }
    }
    /* [EDS#145] A successful ClearDiagnosticInformation empties all four
     * records (ISO 14229-1:2013 §11.3.1.12/13), including one that names a
     * permanent DTC this clear left standing. */
    (void)memset(s_records, 0, sizeof(s_records));
    dtc_db_unlock();

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_get_record(dtc_record_t which, uint32_t *out_dtc_code)
{
    if (out_dtc_code == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if ((uint32_t)which >= (uint32_t)DTC_RECORD_COUNT) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    dtc_db_lock();
    *out_dtc_code = s_records[which];
    dtc_db_unlock();

    return UDS_STATUS_OK;
}

uds_status_t dtc_database_restore_record(dtc_record_t which, uint32_t dtc_code)
{
    uds_status_t rc = UDS_STATUS_OK;

    if ((uint32_t)which >= (uint32_t)DTC_RECORD_COUNT) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    dtc_db_lock();
    s_records[which] = 0U;
    if (dtc_code != 0U) {
        if (dtc_find_internal(dtc_code) != NULL) {
            s_records[which] = dtc_code;
        } else {
            rc = UDS_STATUS_ERR_DID_NOT_FOUND;   /* Removed by a firmware update. */
        }
    }
    dtc_db_unlock();

    return rc;
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

uds_status_t dtc_database_set_functional_group(uint8_t group)
{
    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }
    if (group == 0xFFU) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    s_functional_group     = group;
    s_functional_group_set = true;
    return UDS_STATUS_OK;
}

uds_status_t dtc_database_get_functional_group(uint8_t *out_group)
{
    if (out_group == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (!s_initialized || !s_functional_group_set) {
        return UDS_STATUS_ERR_NOT_FOUND;
    }
    *out_group = s_functional_group;
    return UDS_STATUS_OK;
}

/* [EDS#632] dtc_mirror's dirty flag: peek, take (read and clear), mark. */
bool dtc_database_has_unsaved(void)
{
    bool unsaved;

    dtc_db_lock();
    unsaved = s_unsaved;
    dtc_db_unlock();
    return unsaved;
}

bool dtc_database_take_unsaved(void)
{
    bool unsaved;

    dtc_db_lock();
    unsaved   = s_unsaved;
    s_unsaved = false;
    dtc_db_unlock();
    return unsaved;
}

void dtc_database_mark_unsaved(void)
{
    dtc_db_lock();
    s_unsaved = true;
    dtc_db_unlock();
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
    (void)memset(s_records, 0, sizeof(s_records));
    s_dtc_count   = (uint16_t)0U;
    s_functional_group     = 0U;
    s_functional_group_set = false;
    s_unsaved     = false;
    s_initialized = false;
}
#endif /* UNIT_TEST */

