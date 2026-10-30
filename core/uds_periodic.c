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
    bool     active;        /* [EDS#466] slot in use (pDID 0x00 is valid) */
    uint8_t  periodic_id;
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

/* [EDS#194] periodic_id of the subscription most recently returned by
 * uds_periodic_pop_due(), or 0 if none is currently outstanding (never
 * popped yet, or already consumed by a prior uds_periodic_requeue_last()
 * call). Single-slot by design: uds_periodic_pop_due() returns at most
 * one frame per call, so there is never more than one "in flight". */
static uint8_t s_last_popped_id;
static bool    s_last_popped_valid;

/* [SEC-PDID-ACCESS-01] Contexts the subscribing 0x2A request ran under. Every
 * push re-checks the DID's session and security against them, so a
 * subscription stops when the tester can no longer read the DID (security
 * re-locked on a session change). NULL = never bound: no re-check. */
static const uds_session_ctx_t  *s_access_sess;
static const uds_security_ctx_t *s_access_sec;

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
        if (s_subs[i].active && (s_subs[i].periodic_id == periodic_id)) {
            return &s_subs[i];
        }
    }
    return NULL;
}

static uds_periodic_sub_t *s_find_free(void)
{
    uint8_t i;
    for (i = (uint8_t)0U; i < (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        if (!s_subs[i].active) {
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
    s_last_popped_valid = false;
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
        slot->counter_ms  = (uint16_t)1U;   /* first message at the next tick */
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

    slot->active      = true;
    slot->periodic_id = periodic_id;
    slot->mode        = mode;
    slot->interval_ms = interval;
    /* ISO 14229-1:2026 §10.5.5 (Figures 25/26): the first periodic message
     * follows the initial positive response at the scheduler's next run, not
     * a full period later; the interval applies from then on [EDS#546]. */
    slot->counter_ms  = (uint16_t)1U;
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
    s_last_popped_valid = false;
    return UDS_STATUS_OK;
}

uds_status_t uds_periodic_tick_1ms(void)
{
    uint8_t i;

    for (i = (uint8_t)0U; i < (uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS; i++) {
        if (!s_subs[i].active) {
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
        if (!s_subs[i].active || !s_subs[i].due) {
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

        /* [SEC-PDID-ACCESS-01] Re-check session and security; drop the
         * subscription if the DID is no longer readable. */
        if ((s_access_sess != NULL) && (s_access_sec != NULL) &&
            (uds_safety_validate_did_access(entry, s_access_sess, s_access_sec,
                                            UDS_SAFETY_ACCESS_READ) != UDS_STATUS_OK)) {
            (void)uds_periodic_unsubscribe(s_subs[i].periodic_id);
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

        /* Build [periodicDataIdentifier, dataRecord...]: ISO 14229-1:2026
         * Table 221 / §10.5.1 — the periodic messages carry the pDID and the
         * data "but not the positive response message service identifier"
         * [EDS#546]. */
        out_frame->data[0U] = s_subs[i].periodic_id;

        {
            uint16_t j;
            for (j = (uint16_t)0U; j < out_len; j++) {
                out_frame->data[(uint16_t)1U + j] = data_buf[j];
            }
        }

        out_frame->length = (uint16_t)1U + out_len;

        /* [EDS#194] Record what was just popped so a failed transmit can
         * ask for it back via uds_periodic_requeue_last(). */
        s_last_popped_id    = s_subs[i].periodic_id;
        s_last_popped_valid = true;

        return UDS_STATUS_OK;
    }

    return UDS_STATUS_ERR_NOT_FOUND;
}

static uint32_t s_tx_can_id;   /* [EDS#546] deliberately not cleared by uds_periodic_init() */

void uds_periodic_set_tx_can_id(uint32_t can_id)
{
    s_tx_can_id = can_id;
}

bool uds_periodic_tx_can_id_is_set(void)
{
    return (s_tx_can_id != (uint32_t)0U);
}

uint8_t uds_periodic_free_slots(void)
{
    return (uint8_t)((uint8_t)UDS_PERIODIC_MAX_SUBSCRIPTIONS - s_count);
}

bool uds_periodic_is_subscribed(uint8_t periodic_id)
{
    return (s_find_slot(periodic_id) != NULL);
}

void uds_periodic_set_access_ctx(const uds_session_ctx_t  *session_ctx,
                                 const uds_security_ctx_t *security_ctx)
{
    s_access_sess = session_ctx;
    s_access_sec  = security_ctx;
}

uds_status_t uds_periodic_requeue_last(void)
{
    uds_periodic_sub_t *slot;
    uint8_t              id;

    id = s_last_popped_id;
    if (!s_last_popped_valid) {
        return UDS_STATUS_ERR_NOT_FOUND;
    }
    s_last_popped_valid = false;     /* consumed — one requeue per pop */

    slot = s_find_slot(id);
    if (slot == NULL) {
        /* Subscription was cancelled between the pop and this call —
         * nothing to re-arm, not an error the caller needs to react to. */
        return UDS_STATUS_ERR_NOT_FOUND;
    }

    slot->due = true;
    return UDS_STATUS_OK;
}
