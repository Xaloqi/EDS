// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_services/service_0x2A.c
 *
 * PURPOSE: SID 0x2A — ReadDataByPeriodicIdentifier service handler.
 *
 *          Parses the subscription request, validates each 0xF2xx DID, and
 *          registers or removes subscriptions in the periodic scheduler.
 *          Does NOT transmit periodic data — that is the job of the main
 *          loop drain via uds_periodic_pop_due() / isotp_transmit().
 *
 *          Request  : [0x2A, transmissionMode, periodicId_1 {, periodicId_2 ...}]
 *          Response : [0x6A]  (initial positive response, 1 byte only)
 *          Periodic : [periodicId, dataRecord...] per pDID, sent by the main
 *                     loop (ISO 14229-1:2026 Table 221; no 0x6A) [EDS#546]
 *
 * SAFETY  : Each pDID gets the full DID access check (readable, session,
 *           security) at subscription, and uds_periodic_pop_due() re-checks
 *           session and security on every push [SEC-PDID-ACCESS-01].
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "services.h"
#include "uds_types.h"
#include "uds_server.h"
#include "uds_safety.h"
#include "uds_periodic.h"
#include "did_database.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define SVC_0x2A_MIN_REQ_LEN      (2U)   /* [SID, mode] (stopSending, Figure 24) */
#define SVC_0x2A_MIN_START_LEN    (3U)   /* [SID, mode, >=1 pDID] otherwise */
#define SVC_0x2A_MODE_OFFSET      (1U)
#define SVC_0x2A_IDS_OFFSET       (2U)
#define SVC_0x2A_PERIODIC_DID_HI  (0xF200U)
#define SVC_0x2A_BITMAP_BYTES     (32U)  /* one bit per pDID 0x00-0xFF */

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static bool s_bit(const uint8_t *map, uint8_t id)
{
    return ((map[id >> 3U] & (uint8_t)(1U << (id & 7U))) != (uint8_t)0U);
}

static void s_set_bit(uint8_t *map, uint8_t id)
{
    map[id >> 3U] = (uint8_t)(map[id >> 3U] | (uint8_t)(1U << (id & 7U)));
}

/**
 * Figure 24 per-pDID checks. UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE = not
 * supported in the active session (unknown, not readable, or min_session
 * not met): the caller skips it. Any other error (security, 0x33) aborts
 * the request with nothing scheduled. [EDS#466] [SEC-PDID-ACCESS-01]
 */
static uds_status_t s_check_pdid(const uds_server_ctx_t *ctx, uint8_t periodic_id)
{
    const did_entry_t *entry = NULL;
    uint16_t full_did = (uint16_t)(SVC_0x2A_PERIODIC_DID_HI | (uint16_t)periodic_id);

    if (uds_safety_find_did(full_did, &entry) != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
    if ((entry->access_flags & (uint8_t)DID_ACCESS_READ) == (uint8_t)0U) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
#if UDS_SAFETY_ENABLE_SESSION_CHECKS
    if (uds_safety_validate_session(ctx->cfg.session_ctx,
            (uds_session_type_t)entry->min_session) != UDS_STATUS_OK) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
#endif
    /* Readability and session passed: a failure here is security (0x33). */
    return uds_safety_validate_did_access(entry, ctx->cfg.session_ctx,
                                          ctx->cfg.security_ctx,
                                          UDS_SAFETY_ACCESS_READ);
}

/* --------------------------------------------------------------------------
 * SID 0x2A handler
 * -------------------------------------------------------------------------- */

/**
 * ISO 14229-1:2026 §10.5, Table 223, Figure 24 [EDS#466]:
 *   0x13 — length < 2, or < 3 for a mode other than stopSending (checked first)
 *   0x31 — transmissionMode not supported (no SubFunction, §10.5.2.2)
 *   stopSending: stop the listed pDIDs, or all of them if none is listed
 *   per pDID: not supported in the active session → skipped; 0x33 security
 *   0x22 — no periodic CAN ID configured (uds_periodic_set_tx_can_id), nothing scheduled
 *   0x31 — none supported, or the scheduler can't take all supported pDIDs
 *          (then none is scheduled, §10.5.1)
 * A pDID repeated in one request is scheduled once (§10.5.1).
 */
uds_status_t uds_service_0x2A_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t status;
    uint8_t      mode;
    uint16_t     idx;
    uint8_t      periodic_id;
    uint8_t      seen[SVC_0x2A_BITMAP_BYTES];
    uint8_t      supported[SVC_0x2A_BITMAP_BYTES];
    uint16_t     n_supported = (uint16_t)0U;
    uint16_t     n_new       = (uint16_t)0U;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    status = uds_service_validate_length(req, (uint16_t)SVC_0x2A_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    mode = req->data[SVC_0x2A_MODE_OFFSET];

    /* Figure 24: the length check (3 bytes unless stopSending) comes before the
     * transmissionMode check, so `2A 05` and `2A 00` are 0x13, not 0x31 [EDS#546]. */
    if ((mode != (uint8_t)UDS_PERIODIC_MODE_STOP) &&
        (req->length < (uint16_t)SVC_0x2A_MIN_START_LEN)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* transmissionMode (Annex C.4 Table C.10): 0x01-0x04; others reserved. */
    if ((mode < (uint8_t)UDS_PERIODIC_MODE_SLOW) ||
        (mode > (uint8_t)UDS_PERIODIC_MODE_MAX)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    if (mode == (uint8_t)UDS_PERIODIC_MODE_STOP) {
        if (req->length == (uint16_t)SVC_0x2A_MIN_REQ_LEN) {
            (void)uds_periodic_cancel_all();          /* no pDID: stop all */
        } else {
            for (idx = (uint16_t)SVC_0x2A_IDS_OFFSET; idx < req->length; idx++) {
                (void)uds_periodic_unsubscribe(req->data[idx]);
            }
        }
        return uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DATA_BY_PERIODIC_ID, resp);
    }

    /* Pass 1: check every pDID before scheduling any. */
    (void)memset(seen, 0, sizeof(seen));
    (void)memset(supported, 0, sizeof(supported));
    for (idx = (uint16_t)SVC_0x2A_IDS_OFFSET; idx < req->length; idx++) {
        periodic_id = req->data[idx];
        if (s_bit(seen, periodic_id)) {
            continue;                                  /* repeated pDID */
        }
        s_set_bit(seen, periodic_id);

        status = s_check_pdid(ctx, periodic_id);
        if (status == UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE) {
            continue;                                  /* not supported: skip */
        }
        if (status != UDS_STATUS_OK) {
            return status;                             /* 0x33, nothing scheduled */
        }
        s_set_bit(supported, periodic_id);
        n_supported++;
        if (!uds_periodic_is_subscribed(periodic_id)) {
            n_new++;
        }
    }

    if (n_supported == (uint16_t)0U) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
    if (n_new > (uint16_t)uds_periodic_free_slots()) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;    /* none scheduled */
    }

    /* ISO 14229-3 Table 3: periodic messages need a CAN ID of their own, and
     * there is no default. Without one, nothing is scheduled: NRC 0x22
     * (conditionsNotCorrect, Table 223) rather than a periodic message on the
     * diagnostic response ID [EDS#546]. */
    if (!uds_periodic_tx_can_id_is_set()) {
        return UDS_STATUS_ERR_CONDITIONS_NOT_MET;
    }

    /* Pass 2: schedule, in request order. */
    uds_periodic_set_access_ctx(ctx->cfg.session_ctx, ctx->cfg.security_ctx);
    (void)memset(seen, 0, sizeof(seen));
    for (idx = (uint16_t)SVC_0x2A_IDS_OFFSET; idx < req->length; idx++) {
        periodic_id = req->data[idx];
        if (s_bit(supported, periodic_id) && !s_bit(seen, periodic_id)) {
            s_set_bit(seen, periodic_id);
            status = uds_periodic_subscribe(periodic_id, mode);
            if (status != UDS_STATUS_OK) {
                return status;   /* unreachable: capacity checked above */
            }
        }
    }

    return uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DATA_BY_PERIODIC_ID, resp);
}
