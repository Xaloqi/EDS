// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_exec.h
 *
 * PURPOSE: [EDS#197] The diagnostic executor: one thread owns the UDS server,
 *          and each transport (a producer) hands it requests instead of
 *          calling uds_server_process_request() from its own thread.
 *
 *          This file is RTOS-free. It holds the slots and the executor's
 *          logic; the platform layer supplies waking, completion and a short
 *          critical section as function pointers (platform/zephyr and
 *          platform/freertos bindings, EDS#197 J4/J5).
 *
 *  Slots, not a queue. Each producer has one slot: a request it posted, or
 *  a call (uds_exec_call()). The slot points at the producer's own request
 *  and response buffers; nothing is copied. A producer has at most one
 *  request in flight.
 *
 *  Producer side (any thread):
 *    uds_exec_submit()    post a request. POSTED, or the producer answers
 *                         itself: REJECT_BUSY = send 7F sid 21
 *                         (uds_server_build_negative_response()), DROPPED =
 *                         send nothing. Physical requests are never dropped.
 *                         Busy means its own slot is still in use
 *                         (backpressure), or another producer's job is
 *                         running a step right now (the busy_src fast
 *                         reject, so the answer comes within P2).
 *    uds_exec_call()      post a function to run on the executor (end of a
 *                         session on disconnect, the discovery VIN read).
 *    uds_exec_done()      true once the slot's request or call has run; the
 *                         response is in the producer's buffer (length 0:
 *                         nothing to send).
 *    uds_exec_release()   the producer is finished with the slot: on every
 *                         exit path, once it has posted. After sending the
 *                         response, after a send error, on a disconnect or a
 *                         timeout. A reset the request left pending is
 *                         performed after this. Released before it ran, a
 *                         request is withdrawn; released while it runs, the
 *                         slot is freed when the executor is done with it.
 *
 *  Buffers: once posted, a slot's req and resp are the executor's until
 *  uds_exec_done(), or, after a release, until the producer can post again
 *  (uds_exec_submit() returns POSTED). Meanwhile the producer receives into
 *  a buffer of its own, not into req.
 *    uds_exec_push_frame() / uds_exec_push_done()
 *                         a frame the executor routes to this producer: a
 *                         pending job's 7F sid 78 keepalive or final
 *                         response, or a periodic message it subscribed.
 *    uds_exec_job_pending()
 *                         true while a job this producer's request started
 *                         may still send it a frame.
 *    uds_exec_disconnect() the producer's peer is gone (a DoIP
 *                         disconnect): the executor aborts its job and
 *                         discards a frame routed to it, so no frame waits
 *                         for a producer that will not take it.
 *
 *  Executor side (one thread): uds_exec_run_once(), in this order:
 *    1. elapsed 1 ms ticks: uds_server_tick_1ms(), uds_periodic_tick_1ms();
 *       then a reported pushed frame, and producers that disconnected;
 *    2. posted slots, round-robin, one per slot per pass;
 *    3. uds_server_poll(), its frame routed to the job's source;
 *    4. uds_periodic_pop_due_src(), routed to the subscriber's source;
 *    5. a pending ECUReset, once its source released the response.
 *
 * SAFETY  : No malloc. No recursion. No RTOS call. The critical section
 *           guards only this module's own small state, never a dispatch.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef UDS_EXEC_H
#define UDS_EXEC_H

#include "uds_types.h"
#include "uds_server.h"
#include "eds_elapsed.h"

#include <stdbool.h>
#include <stdint.h>

/** Producers (transports) one executor serves. Default 2, at most 4. */
#ifndef EDS_EXEC_MAX_PRODUCERS
#define EDS_EXEC_MAX_PRODUCERS  (2U)
#endif

#if (EDS_EXEC_MAX_PRODUCERS < 1U) || (EDS_EXEC_MAX_PRODUCERS > 4U)
#error "EDS_EXEC_MAX_PRODUCERS must be 1..4"
#endif

/** Most 1 ms ticks one pass runs; the rest carries over (eds_elapsed). */
#ifndef EDS_EXEC_TICK_CAP_MS
#define EDS_EXEC_TICK_CAP_MS    (100U)
#endif

/** Result of uds_exec_submit(). */
typedef enum uds_exec_submit_result {
    UDS_EXEC_POSTED      = 0U, /**< Wait for uds_exec_done().                      */
    UDS_EXEC_REJECT_BUSY = 1U, /**< Physical: the producer sends 7F sid 21 itself. */
    UDS_EXEC_DROPPED     = 2U, /**< Functional while busy: nothing is sent.        */
    UDS_EXEC_INVALID     = 3U  /**< Unknown source or a NULL argument.             */
} uds_exec_submit_result_t;

/** A function uds_exec_call() runs on the executor, with the server. */
typedef void (*uds_exec_fn_t)(uds_server_ctx_t *srv, void *arg);

/** One producer: a transport, as the executor sees it. */
typedef struct uds_exec_producer_cfg {
    uint8_t src;          /**< Request source, 0..7 (uds_server_process_request_src()). */
    bool    periodic_tx;  /**< It can send periodic messages (decision ⬥E).             */
    /** Executor thread: a response or a pushed frame is ready for it. Optional. */
    void  (*notify)(void *arg);
    void   *notify_arg;
} uds_exec_producer_cfg_t;

/** The executor's bindings, supplied by the platform layer (or a test). */
typedef struct uds_exec_cfg {
    uds_server_ctx_t *srv;                    /**< Initialized server. Required.          */
    uint32_t        (*now_ms)(void);          /**< Millisecond clock. Required.           */
    void            (*enter)(void);           /**< Short critical section. Required.      */
    void            (*exit)(void);            /**< Ends it. Required.                     */
    void            (*wake)(void *arg);       /**< Wake the executor. Optional.           */
    void             *wake_arg;
    void            (*reset)(uint8_t reset_type); /**< Perform an ECUReset. Optional.     */
    const uds_exec_producer_cfg_t *producers; /**< 1..EDS_EXEC_MAX_PRODUCERS entries.     */
    uint8_t           producer_count;
} uds_exec_cfg_t;

/** Counters, for supervision and the concurrency tests. */
typedef struct uds_exec_stats {
    uint32_t served;            /**< Requests dispatched to the server.               */
    uint32_t calls;             /**< uds_exec_call() functions run.                    */
    uint32_t reject_busy;       /**< REJECT_BUSY answers (backpressure + fast reject). */
    uint32_t dropped;           /**< DROPPED requests.                                 */
    uint32_t periodic_dropped;  /**< Periodic frames for a source with no producer.   */
    uint32_t max_queue_wait_ms; /**< Longest submit-to-dispatch wait.                  */
} uds_exec_stats_t;

/** @cond INTERNAL */
typedef enum uds_exec_slot_state {
    UDS_EXEC_SLOT_FREE    = 0U,
    UDS_EXEC_SLOT_POSTED  = 1U,
    UDS_EXEC_SLOT_RUNNING = 2U,
    UDS_EXEC_SLOT_DONE    = 3U
} uds_exec_slot_state_t;

typedef struct uds_exec_slot {
    uds_exec_slot_state_t state;
    bool                  is_call;
    bool                  released;   /* released while RUNNING: free, not DONE */
    uds_addressing_t      addressing;
    uint32_t              rx_ms;
    const uds_msg_buf_t  *req;
    uds_msg_buf_t        *resp;
    uds_exec_fn_t         fn;
    void                 *fn_arg;
} uds_exec_slot_t;

typedef enum uds_exec_push_kind {
    UDS_EXEC_PUSH_NONE     = 0U,
    UDS_EXEC_PUSH_JOB      = 1U,
    UDS_EXEC_PUSH_PERIODIC = 2U
} uds_exec_push_kind_t;
/** @endcond */

/** Executor state. Caller-allocated (static); members are internal. */
typedef struct uds_exec {
    uds_exec_cfg_t       cfg;
    uds_exec_slot_t      slots[EDS_EXEC_MAX_PRODUCERS];
    eds_elapsed_t        clock;
    uint8_t              rr_next;     /* round-robin start for the next pass   */
    uint8_t              busy_src;    /* source whose job step runs, or NONE   */
    uint8_t              job_src;     /* source of the pending job, or NONE    */
    uint8_t              reset_type;  /* ECUReset owed, 0 if none              */
    uint8_t              reset_src;   /* the source that asked for it          */
    bool                 reset_released;
    uds_exec_push_kind_t push_kind;   /* the one pushed frame in flight        */
    uint8_t              push_src;
    uint8_t              push_state;  /* idle, ready, sent or failed           */
    uds_msg_buf_t        push_buf;
    uds_exec_stats_t     stats;
    bool                 initialized;
    uint8_t              gone_mask;   /* sources that disconnected, by bit     */
} uds_exec_t;

/**
 * @brief Initialize the executor. Call before any producer runs.
 *
 * Also sets uds_periodic_set_tx_src_mask() from the producers' periodic_tx.
 *
 * @return UDS_STATUS_OK; UDS_STATUS_ERR_NULL_PTR; UDS_STATUS_ERR_INVALID_PARAM
 *         for a missing required binding, a producer count outside
 *         1..EDS_EXEC_MAX_PRODUCERS, a source above 7 or a repeated source.
 */
uds_status_t uds_exec_init(uds_exec_t *ex, const uds_exec_cfg_t *cfg);

/**
 * @brief Producer: post a request. req and resp stay the producer's and must
 *        not be touched until uds_exec_done() is true.
 */
uds_exec_submit_result_t uds_exec_submit(uds_exec_t          *ex,
                                         uint8_t              src,
                                         const uds_msg_buf_t *req,
                                         uds_msg_buf_t       *resp,
                                         uds_addressing_t     addressing);

/**
 * @brief Producer: run fn(srv, arg) on the executor. Same slot as a request.
 * @return UDS_STATUS_OK (wait for uds_exec_done(), then uds_exec_release());
 *         UDS_STATUS_ERR_BUSY if the slot is in use; ERR_NULL_PTR /
 *         ERR_INVALID_PARAM for bad arguments.
 */
uds_status_t uds_exec_call(uds_exec_t *ex, uint8_t src, uds_exec_fn_t fn, void *arg);

/** @brief Producer: true once its request or call has run. */
bool uds_exec_done(uds_exec_t *ex, uint8_t src);

/**
 * @brief Producer: it is finished with its slot. Call on every exit path
 *        after posting: response sent, send error, disconnect, timeout.
 *        DONE: the slot is free. Not yet run: the request is withdrawn.
 *        Running: the slot is freed, with no DONE, once the run ends; the
 *        buffers stay the executor's until then.
 */
void uds_exec_release(uds_exec_t *ex, uint8_t src);

/**
 * @brief Producer: the frame the executor routed to it, or NULL. Send it,
 *        then call uds_exec_push_done(). The same frame is returned until then.
 */
const uds_msg_buf_t *uds_exec_push_frame(uds_exec_t *ex, uint8_t src);

/**
 * @brief Producer: the routed frame was sent (sent = true) or could not be
 *        (false: a job frame is offered again on the next pass; a periodic
 *        frame waits for its next period).
 */
void uds_exec_push_done(uds_exec_t *ex, uint8_t src, bool sent);

/**
 * @brief Producer: true while a job its request started can still route it a
 *        frame (a 7F sid 78 or the final response). False once the job is
 *        over, with or without a final frame (aborted with its session).
 */
bool uds_exec_job_pending(uds_exec_t *ex, uint8_t src);

/**
 * @brief Producer: its peer is gone. On its next pass the executor aborts the
 *        producer's pending job (uds_server_pending_abort()) and discards a
 *        frame still routed to it. The slot is not touched: release it as
 *        usual (contract 1).
 */
void uds_exec_disconnect(uds_exec_t *ex, uint8_t src);

/**
 * @brief Executor thread: one pass (see the file header for the order).
 * @return true if work is left that the next pass can do without waiting.
 */
bool uds_exec_run_once(uds_exec_t *ex);

/** @brief A copy of the counters (taken in the critical section). */
uds_status_t uds_exec_get_stats(uds_exec_t *ex, uds_exec_stats_t *out);

#endif /* UDS_EXEC_H */
