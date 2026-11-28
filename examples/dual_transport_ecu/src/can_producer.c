/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/dual_transport_ecu/src/can_producer.c
 *
 * PURPOSE: [EDS#197] ISO-TP as a producer of the diagnostic executor. See
 *          can_producer.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#include "can_producer.h"

#include <stddef.h>
#include <string.h>

void can_producer_init(can_producer_t *p, uds_exec_t *ex, uint8_t src,
                       isotp_ctx_t *tp, can_transport_t *can)
{
    (void)memset(p, 0, sizeof(*p));
    p->ex  = ex;
    p->src = src;
    p->tp  = tp;
    p->can = can;
}

/* The producer's own 0x21 answers, oldest first, while ISO-TP takes them. */
static void send_owed_nrc(can_producer_t *p)
{
    while ((p->nrc_count > 0U) &&
           (isotp_transmit(p->tp, p->nrc[p->nrc_head], 3U) != UDS_STATUS_ERR_BUSY)) {
        p->nrc_head = (uint8_t)((p->nrc_head + 1U) % CAN_PRODUCER_NRC_QUEUE);
        p->nrc_count--;
        p->stats.answered++;
    }
}

/* 7F sid 21 for a request the producer answers itself: sent now, or held
 * until ISO-TP has finished the response it is sending. */
static void answer_busy(can_producer_t *p, uint8_t sid)
{
    uint8_t *n;

    if (p->nrc_count < CAN_PRODUCER_NRC_QUEUE) {
        n = p->nrc[(p->nrc_head + p->nrc_count) % CAN_PRODUCER_NRC_QUEUE];
        n[0] = (uint8_t)UDS_SID_NEGATIVE_RESPONSE;
        n[1] = sid;
        n[2] = (uint8_t)UDS_NRC_BUSY_REPEAT_REQUEST;
        p->nrc_count++;
        p->stats.local_busy++;
        send_owed_nrc(p);
    } else {
        p->stats.dropped++;   /* a tester four requests ahead of its answers */
    }
}

/* ISO-TP has reassembled a request (CAN thread). Post it, or answer it. */
static void on_rx_complete(const uint8_t *data, uint32_t length, void *arg)
{
    can_producer_t          *p = (can_producer_t *)arg;
    uds_addressing_t         addressing = UDS_ADDRESSING_PHYSICAL;
    uds_exec_submit_result_t r = UDS_EXEC_REJECT_BUSY;

    if ((length == 0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) {
        return;
    }
    (void)isotp_get_rx_addressing(p->tp, &addressing);

    if (!p->in_flight) {
        /* Our slot is free: the pair is ours to fill (contract 2). */
        (void)memcpy(p->req.data, data, (size_t)length);
        p->req.length  = (uint16_t)length;
        p->resp.length = 0U;
        r = uds_exec_submit(p->ex, p->src, &p->req, &p->resp, addressing);
        if (r == UDS_EXEC_POSTED) {
            p->in_flight = true;
            p->stats.posted++;
        }
    } else if (addressing == UDS_ADDRESSING_FUNCTIONAL) {
        r = UDS_EXEC_DROPPED;   /* 3E 80 among them: S3 is served by the request in flight */
    } else {
        /* A second request while ours is posted: 0x21 below, the posted
         * pair untouched. */
    }

    if (r == UDS_EXEC_REJECT_BUSY) {
        answer_busy(p, data[0]);   /* ours is in flight, or another producer's job step runs */
    } else if (r == UDS_EXEC_DROPPED) {
        p->stats.dropped++;
    } else {
        /* POSTED; INVALID (no slot for this source) is answered by nobody. */
    }
}

/* [EDS#558] Every queued frame, at most the queue depth: the Flow Control
 * BlockSize keeps a burst within the RX queue only if each step empties it. */
static void drain_rx(can_producer_t *p)
{
    uds_can_frame_t frame;
    bool            ready = true;
    uint32_t        n;
    uds_status_t    rc;

    for (n = 0U; ready && (n < (uint32_t)EDS_CAN_RX_QUEUE_DEPTH); n++) {
        ready = false;
        rc = can_transport_receive(p->can, &frame, &ready);
        if (rc == UDS_STATUS_ERR_CAN_BUS_OFF) {
            p->stats.bus_off++;
        } else if ((rc == UDS_STATUS_OK) && ready) {
            (void)isotp_process_rx_frame(p->tp, &frame, on_rx_complete, p);
        } else {
            /* nothing queued */
        }
    }
}

/* Our request ran: hand its response to ISO-TP, then release the slot
 * (contract 1). A busy channel keeps the slot for the next step; any other
 * result, a send error among them, releases it. An ECUReset the request
 * owes is performed by the executor after the release. */
static void send_response(can_producer_t *p)
{
    uds_status_t rc = UDS_STATUS_OK;

    if (p->in_flight && uds_exec_done(p->ex, p->src)) {
        if (p->resp.length > 0U) {
            rc = isotp_transmit(p->tp, p->resp.data, (uint32_t)p->resp.length);
        }
        if (rc != UDS_STATUS_ERR_BUSY) {
            if (p->resp.length > 0U) {
                p->stats.answered++;
            }
            uds_exec_release(p->ex, p->src);
            p->in_flight = false;
        }
    }
}

/* A frame the executor routed here: a job's 7F sid 78 or final response on
 * the ISO-TP channel, or a periodic message as a UUDT frame (EDS#546). Not
 * sent: offered again (a job frame) or skipped until its next period. */
static void send_pushed(can_producer_t *p)
{
    const uds_msg_buf_t *f;
    bool                 periodic = false;
    uds_status_t         rc;

    f = uds_exec_push_frame(p->ex, p->src, &periodic);
    if (f != NULL) {
        rc = periodic ? isotp_transmit_uudt(p->tp, f->data, (uint32_t)f->length)
                      : isotp_transmit(p->tp, f->data, (uint32_t)f->length);
        if (rc == UDS_STATUS_OK) {
            p->stats.pushed++;
        }
        uds_exec_push_done(p->ex, p->src, rc == UDS_STATUS_OK);
    }
}

void can_producer_step(can_producer_t *p)
{
    drain_rx(p);
    (void)isotp_tick_1ms(p->tp);
    send_owed_nrc(p);
    send_response(p);
    /* A job's frames follow its first answer (7F sid 78) on the channel. */
    if (!p->in_flight) {
        send_pushed(p);
    }
}
