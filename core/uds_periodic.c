// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_periodic.c
 *
 * PURPOSE: SID 0x2A — ReadDataByPeriodicIdentifier scheduler.
 *
 *          Tick-driven subscription engine for periodic DID push.
 *          Subscriptions are registered by service_0x2A.c and consumed
 *          by the main poll loop via uds_periodic_pop_due().
 *
 * SAFETY  : No malloc/free. Static table s_subs[UDS_PERIODIC_MAX_SUBSCRIPTIONS].
 *           uds_periodic_pop_due() intentionally skips session/security
 *           re-validation — access was checked at subscription time (0x2A
 *           request). Subscriptions are bulk-cancelled on return to Default
 *           session via uds_periodic_cancel_all().
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_periodic.h"
#include "uds_safety.h"
#include "did_database.h"
#include "uds_types.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Internal types
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  periodic_id;   /* 0x00 = slot free */
    uint8_t  mode;          /* UDS_PERIODIC_MODE_* */
    uint16_t counter_ms;    /* countdown; fires when 0 */
    uint16_t interval_ms;   /* reload value after firing */
    bool     due;           /* true when counter hit 0 this tick */
} uds_periodic_sub_t;

/* --------------------------------------------------------------------------
 * Module state — no heap, static file scope
 * -------------------------------------------------------------------------- */

static uds_periodic_sub_t s_subs[UDS_PERIODIC_MAX_SUBSCRIPTIONS];
static uint8_t            s_count;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static uint16_t s_mode_to_interval_ms(uint8_t mode)
{
    switch (mode) {
        case UDS_PERIODIC_MODE_SLOW:   return (uint16_t)UDS_PERIODIC_SLOW_MS;
        case UDS_PERIODIC_MODE_MEDIUM: return (uint16_t)UDS_PERIODIC_MEDIUM_MS;
        case UDS_PERIODIC_MODE_FAST:   return (uint16_t)UDS_PERIODIC_FAST_MS;
        default:                       return (uint16_t)UDS_PERIODIC_SLOW_MS;
    }
}

static uds_periodic_sub_t *s_find_slot(uint8_t periodic_id)
{
    uint8_t i;
    for (i = (uint8_t)0U; i < (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].periodic_id == periodic_id) {
            return &s_subs[i];
        }
    }
    return NULL;
}

static uds_periodic_sub_t *s_find_free(void)
{
    uint8_t i;
    for (i = (uint8_t)0U; i < (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].periodic_id == (uint8_t)0U) {
            return &s_subs[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

uds_status_t uds_periodic_init(void)
{
    (void)memset(s_subs, 0, sizeof(s_subs));
    s_count = (uint8_t)0U;
    return UDS_STATUS_OK;
}

uds_status_t uds_periodic_subscribe(uint8_t periodic_id, uint8_t mode)
{
    uds_periodic_sub_t *slot;
    uint16_t            interval;

    interval = s_mode_to_interval_ms(mode);

    /* Update existing slot if already subscribed. */
    slot = s_find_slot(periodic_id);
    if (slot != NULL) {
        slot->mode        = mode;
        slot->interval_ms = interval;
        slot->counter_ms  = interval;
        slot->due         = false;
        return UDS_STATUS_OK;
    }

    /* Allocate a new slot. */
    if (s_count >= (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    slot = s_find_free();
    if (slot == NULL) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    slot->periodic_id = periodic_id;
    slot->mode        = mode;
    slot->interval_ms = interval;
    slot->counter_ms  = interval;
    slot->due         = false;
    s_count++;

    return UDS_STATUS_OK;
}

uds_status_t uds_periodic_unsubscribe(uint8_t periodic_id)
{
    uds_periodic_sub_t *slot = s_find_slot(periodic_id);

    if (slot == NULL) {
        return UDS_STATUS_OK;  /* no-op */
    }

    (void)memset(slot, 0, sizeof(*slot));
    if (s_count > (uint8_t)0U) {
        s_count--;
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_periodic_cancel_all(void)
{
    (void)memset(s_subs, 0, sizeof(s_subs));
    s_count = (uint8_t)0U;
    return UDS_STATUS_OK;
}

uds_status_t uds_periodic_tick_1ms(void)
{
    uint8_t i;

    for (i = (uint8_t)0U; i < (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].periodic_id == (uint8_t)0U) {
            continue;
        }

        if (s_subs[i].counter_ms > (uint16_t)0U) {
            s_subs[i].counter_ms--;
        }

        if (s_subs[i].counter_ms == (uint16_t)0U) {
            s_subs[i].due        = true;
            s_subs[i].counter_ms = s_subs[i].interval_ms;
        }
    }

    return UDS_STATUS_OK;
}

uds_status_t uds_periodic_pop_due(uds_msg_buf_t *out_frame)
{
    uint8_t            i;
    uds_status_t       status;
    const did_entry_t *entry;
    uint8_t            data_buf[DID_MAX_DATA_LEN];
    uint16_t           full_did;
    uint16_t           out_len;

    if (out_frame == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    for (i = (uint8_t)0U; i < (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        if ((s_subs[i].periodic_id == (uint8_t)0U) || !s_subs[i].due) {
            continue;
        }

        /* Clear due flag before any further work to prevent re-entry. */
        s_subs[i].due = false;

        full_did = (uint16_t)((uint16_t)0xF200U | (uint16_t)s_subs[i].periodic_id);

        /* Resolve DID — skip if no longer registered (app may have removed it). */
        entry  = NULL;
        status = uds_safety_find_did(full_did, &entry);
        if ((status != UDS_STATUS_OK) || (entry == NULL)) {
            continue;
        }

        /* Skip DIDs with no read callback. */
        if (entry->read_cb == NULL) {
            continue;
        }

        out_len = (uint16_t)0U;
        status  = entry->read_cb(data_buf, (uint16_t)DID_MAX_DATA_LEN, &out_len);
        if (status != UDS_STATUS_OK) {
            continue;
        }

        /* Build [0x6A, periodicDataIdentifier, dataRecord...] */
        out_frame->data[0U] = (uint8_t)0x6AU;
        out_frame->data[1U] = s_subs[i].periodic_id;

        {
            uint16_t j;
            for (j = (uint16_t)0U; j < out_len; j++) {
                out_frame->data[(uint16_t)2U + j] = data_buf[j];
            }
        }

        out_frame->length = (uint16_t)2U + out_len;

        return UDS_STATUS_OK;
    }

    return UDS_STATUS_ERR_NOT_FOUND;
}
