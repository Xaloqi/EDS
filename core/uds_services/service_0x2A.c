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
 *          Response : [0x6A]  (positive response, 1 byte only)
 *
 * SAFETY  : DID access validated at subscription time via uds_safety_find_did().
 *           DID_ACCESS_READ flag checked before registering each subscription.
 *           No re-validation at push time — subscriptions are bulk-cancelled
 *           on session return to Default by uds_periodic_cancel_all().
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

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define SVC_0x2A_MIN_REQ_LEN      (3U)   /* [SID, mode, >=1 id] */
#define SVC_0x2A_MODE_OFFSET      (1U)
#define SVC_0x2A_IDS_OFFSET       (2U)
#define SVC_0x2A_PERIODIC_DID_HI  (0xF200U)

/* --------------------------------------------------------------------------
 * SID 0x2A handler
 * -------------------------------------------------------------------------- */

uds_status_t uds_service_0x2A_handler(
    uds_server_ctx_t    *ctx,
    const uds_msg_buf_t *req,
    uds_msg_buf_t       *resp)
{
    uds_status_t       status;
    uint8_t            mode;
    uint16_t           idx;
    uint8_t            periodic_id;
    uint16_t           full_did;
    const did_entry_t *entry;

    if (ctx == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Minimum: [SID, transmissionMode, >=1 periodicDataIdentifier]. */
    status = uds_service_validate_length(req, (uint16_t)SVC_0x2A_MIN_REQ_LEN);
    if (status != UDS_STATUS_OK) {
        return status;
    }

    mode = req->data[SVC_0x2A_MODE_OFFSET];

    /* Validate transmissionMode (0x01–0x04). */
    if ((mode < (uint8_t)UDS_PERIODIC_MODE_SLOW) ||
        (mode > (uint8_t)UDS_PERIODIC_MODE_MAX)) {
        return UDS_STATUS_ERR_SUBFUNCTION_NOT_SUP;
    }

    /* Process each periodicDataIdentifier in the request. */
    for (idx = (uint16_t)SVC_0x2A_IDS_OFFSET; idx < req->length; idx++) {

        periodic_id = req->data[idx];

        if (mode == (uint8_t)UDS_PERIODIC_MODE_STOP) {
            /* stopSending — remove subscription, no error if not found. */
            (void)uds_periodic_unsubscribe(periodic_id);
        } else {
            /* Add or update subscription — validate DID first. */
            full_did = (uint16_t)(SVC_0x2A_PERIODIC_DID_HI | (uint16_t)periodic_id);

            entry  = NULL;
            status = uds_safety_find_did(full_did, &entry);
            if (status != UDS_STATUS_OK) {
                return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
            }

            /* DID must be readable. */
            if ((entry->access_flags & (uint8_t)DID_ACCESS_READ) == (uint8_t)0U) {
                return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
            }

            status = uds_periodic_subscribe(periodic_id, mode);
            if (status != UDS_STATUS_OK) {
                /* Table full maps to NRC 0x31 (requestOutOfRange). */
                return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
            }
        }
    }

    /* Positive response: [0x6A]. */
    status = uds_service_write_pos_sid((uint8_t)UDS_SID_READ_DATA_BY_PERIODIC_ID, resp);
    return status;
}
