/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/dual_transport_ecu/src/can_producer.h
 *
 * PURPOSE: [EDS#197] The CAN side of dual_transport_ecu: ISO-TP as a producer
 *          of the diagnostic executor (core/uds_exec.h). RTOS-free: the
 *          example's CAN thread calls can_producer_step() every 1 ms, and
 *          tests/concurrency/tsan_c2_dual_transport.c runs the same code on
 *          the host.
 *
 *          The producer never blocks. Each step drains every queued CAN
 *          frame (EDS#558), ticks ISO-TP, sends a finished response and
 *          releases its slot, then sends a frame the executor routed to it.
 *          A request is posted as soon as ISO-TP has reassembled it.
 *
 *  Producer contracts (uds_exec.h):
 *    1. uds_exec_release() on every exit path: the response handed to
 *       ISO-TP, nothing to send, or a send error.
 *    2. While its request is posted, the producer does not write that
 *       request's buffers. It keeps receiving: ISO-TP reassembles the next
 *       request into its own buffer, and a second complete physical request
 *       is answered 7F sid 21 from a three-byte buffer of the producer's
 *       own (a functional one is dropped), without touching the posted pair.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#ifndef CAN_PRODUCER_H
#define CAN_PRODUCER_H

#include "uds_exec.h"
#include "isotp.h"
#include "can_transport.h"

#include <stdbool.h>
#include <stdint.h>

/** Counters, for the example's log and the concurrency test. */
typedef struct can_producer_stats {
    uint32_t posted;        /**< Requests posted to the executor.                       */
    uint32_t answered;      /**< Responses (or 0x21 answers) handed to ISO-TP.          */
    uint32_t local_busy;    /**< 7F sid 21 answered by the producer itself.             */
    uint32_t dropped;       /**< Functional requests dropped while busy, and 0x21
                                 answers beyond CAN_PRODUCER_NRC_QUEUE.                 */
    uint32_t pushed;        /**< Routed frames sent: job frames and periodic messages.  */
    uint32_t bus_off;       /**< can_transport_receive() reported bus-off.              */
} can_producer_stats_t;

/** 7F sid 21 answers held while ISO-TP is busy sending; more are dropped (counted). */
#define CAN_PRODUCER_NRC_QUEUE  (4U)

typedef struct can_producer {
    uds_exec_t           *ex;
    uint8_t               src;
    isotp_ctx_t          *tp;
    can_transport_t      *can;
    uds_msg_buf_t         req;        /* the posted request: the executor's until done */
    uds_msg_buf_t         resp;       /* its response                                  */
    bool                  in_flight;  /* posted, not yet answered and released          */
    uint8_t               nrc[CAN_PRODUCER_NRC_QUEUE][3]; /* 7F sid 21 not yet sent   */
    uint8_t               nrc_head;
    uint8_t               nrc_count;
    can_producer_stats_t  stats;
} can_producer_t;

/** @brief Bind the producer to the executor, its source and the ISO-TP channel. */
void can_producer_init(can_producer_t *p, uds_exec_t *ex, uint8_t src,
                       isotp_ctx_t *tp, can_transport_t *can);

/** @brief One 1 ms step on the CAN thread (see the file header). Never blocks. */
void can_producer_step(can_producer_t *p);

#endif /* CAN_PRODUCER_H */
