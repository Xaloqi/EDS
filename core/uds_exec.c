// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: core/uds_exec.c
 *
 * PURPOSE: [EDS#197] The diagnostic executor, RTOS-free. See uds_exec.h.
 *
 *  Threads: producer calls (submit, call, done, release, push_frame,
 *  push_done) may run on any thread; uds_exec_run_once() runs on one thread,
 *  the executor. Every field a producer reads or writes is changed only
 *  between cfg.enter() and cfg.exit(). The server, the periodic scheduler
 *  and a slot's buffers while it is RUNNING belong to the executor alone.
 *
 * SAFETY  : No malloc. No recursion. No RTOS call.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_exec.h"
#include "uds_periodic.h"
#include "eds_race_point.h"

#include <stddef.h>
#include <string.h>

/* No producer: busy_src, job_src and push_src when unused. */
#define EXEC_SRC_NONE  ((uint8_t)UDS_SERVER_SRC_NONE)

/* Highest request source the periodic mask can name. */
#define EXEC_SRC_MAX   (7U)

/* Push mailbox states (push_kind says what the frame is). */
#define EXEC_PUSH_IDLE     (0U)  /* nothing in flight                        */
#define EXEC_PUSH_READY    (1U)  /* handed to push_src, not yet reported     */
#define EXEC_PUSH_SENT     (2U)  /* push_src reported it sent                */
#define EXEC_PUSH_FAILED   (3U)  /* push_src reported it could not send it   */

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static uint8_t s_index(const uds_exec_t *ex, uint8_t src);
static void s_notify(const uds_exec_t *ex, uint8_t idx);
static void s_wake(const uds_exec_t *ex);
static void s_run_ticks(uds_exec_t *ex);
static void s_finish_push(uds_exec_t *ex);
static void s_drop_gone(uds_exec_t *ex);
static void s_serve_slots(uds_exec_t *ex);
static void s_serve_one(uds_exec_t *ex, uint8_t idx);
static void s_run_slot(uds_exec_t *ex, uint8_t idx, const uds_exec_slot_t *slot_copy);
static void s_poll_job(uds_exec_t *ex);
static void s_step_job(uds_exec_t *ex);
static void s_pop_periodic(uds_exec_t *ex);
static void s_route_periodic(uds_exec_t *ex, uint8_t psrc);
static void s_do_reset(uds_exec_t *ex);
static bool s_is_response_pending(const uds_msg_buf_t *resp);
static uds_status_t s_check_cfg(const uds_exec_cfg_t *cfg, uint8_t *out_mask);
static bool s_ready(const uds_exec_t *ex);

/* Index of the producer with this source, or producer_count if none. */
static uint8_t s_index(const uds_exec_t *ex, uint8_t src)
{
    uint8_t i;
    uint8_t found = ex->cfg.producer_count;

    for (i = (uint8_t)0U; (i < ex->cfg.producer_count) && (found == ex->cfg.producer_count); i++) {
        if (ex->cfg.producers[i].src == src) {
            found = i;
        }
    }
    return found;
}

static void s_notify(const uds_exec_t *ex, uint8_t idx)
{
    const uds_exec_producer_cfg_t *p = &ex->cfg.producers[idx];

    if (p->notify != NULL) {
        p->notify(p->notify_arg);
    }
}

static void s_wake(const uds_exec_t *ex)
{
    if (ex->cfg.wake != NULL) {
        ex->cfg.wake(ex->cfg.wake_arg);
    }
}

static bool s_is_response_pending(const uds_msg_buf_t *resp)
{
    return (resp->length == (uint16_t)3U) &&
           (resp->data[0] == (uint8_t)UDS_SID_NEGATIVE_RESPONSE) &&
           (resp->data[2] == (uint8_t)UDS_NRC_REQUEST_CORRECTLY_RECEIVED_RESP_PENDING);
}

/* 1. Elapsed 1 ms ticks, at most EDS_EXEC_TICK_CAP_MS per pass. */
static void s_run_ticks(uds_exec_t *ex)
{
    uint32_t n = eds_elapsed_consume(&ex->clock, ex->cfg.now_ms(),
                                     (uint32_t)EDS_EXEC_TICK_CAP_MS);

    while (n > 0U) {
        (void)uds_server_tick_1ms(ex->cfg.srv);
        (void)uds_periodic_tick_1ms();
        n--;
    }
}

/* A pushed frame the producer reported: tell the server, free the mailbox. */
static void s_finish_push(uds_exec_t *ex)
{
    uint8_t              state;
    uds_exec_push_kind_t kind;

    ex->cfg.enter();
    state = ex->push_state;
    kind  = ex->push_kind;
    if ((state == (uint8_t)EXEC_PUSH_SENT) || (state == (uint8_t)EXEC_PUSH_FAILED)) {
        ex->push_state = (uint8_t)EXEC_PUSH_IDLE;
        ex->push_kind  = UDS_EXEC_PUSH_NONE;
        ex->push_src   = EXEC_SRC_NONE;
    }
    ex->cfg.exit();

    /* A job frame not sent is offered again by the next uds_server_poll(). */
    if ((state == (uint8_t)EXEC_PUSH_SENT) && (kind == UDS_EXEC_PUSH_JOB)) {
        uds_server_frame_sent(ex->cfg.srv);
    }
}

/* A producer that disconnected (uds_exec_disconnect()): its job is aborted
 * and a frame still routed to it is discarded, so nothing waits on it. */
static void s_drop_gone(uds_exec_t *ex)
{
    uint8_t gone;
    bool    abort_job = false;

    ex->cfg.enter();
    gone = ex->gone_mask;
    ex->gone_mask = (uint8_t)0U;
    if ((gone != (uint8_t)0U) && (ex->push_state == (uint8_t)EXEC_PUSH_READY) &&
        ((gone & (uint8_t)(1U << ex->push_src)) != (uint8_t)0U)) {
        ex->push_state = (uint8_t)EXEC_PUSH_IDLE;
        ex->push_kind  = UDS_EXEC_PUSH_NONE;
        ex->push_src   = EXEC_SRC_NONE;
    }
    if ((ex->job_src != EXEC_SRC_NONE) &&
        ((gone & (uint8_t)(1U << ex->job_src)) != (uint8_t)0U)) {
        ex->job_src = EXEC_SRC_NONE;
        abort_job   = true;
    }
    ex->cfg.exit();

    if (abort_job) {
        uds_server_pending_abort(ex->cfg.srv);
    }
}

/* 2. Posted slots, round-robin from rr_next, one per slot per pass. Not
 *    while an ECUReset is owed: the ECU is about to restart. */
static void s_serve_slots(uds_exec_t *ex)
{
    uint8_t k;
    uint8_t n = ex->cfg.producer_count;
    bool    reset_owed;

    ex->cfg.enter();
    reset_owed = (ex->reset_type != (uint8_t)0U);
    ex->cfg.exit();

    if (!reset_owed) {
        for (k = (uint8_t)0U; k < n; k++) {
            s_serve_one(ex, (uint8_t)((uint8_t)(ex->rr_next + k) % n));
        }
        ex->rr_next = (uint8_t)((uint8_t)(ex->rr_next + 1U) % n);
    }
}

static void s_serve_one(uds_exec_t *ex, uint8_t idx)
{
    uds_exec_slot_t *slot = &ex->slots[idx];
    uds_exec_slot_t  job;
    bool             posted;

    ex->cfg.enter();
    posted = (slot->state == UDS_EXEC_SLOT_POSTED);
    if (posted) {
        slot->state = UDS_EXEC_SLOT_RUNNING;
        job = *slot;
    }
    ex->cfg.exit();

    if (posted) {
        s_run_slot(ex, idx, &job);
    }
}

/* Run one RUNNING slot's request or call (no lock held), then mark it DONE. */
static void s_run_slot(uds_exec_t *ex, uint8_t idx, const uds_exec_slot_t *slot_copy)
{
    uds_exec_slot_t  job = *slot_copy;
    uint8_t          src = ex->cfg.producers[idx].src;
    uint32_t         wait_ms = 0U;
    uint8_t          reset_type = (uint8_t)0U;
    uds_status_t     status;
    bool             deferred = false;

    if (job.is_call) {
        job.fn(ex->cfg.srv, job.fn_arg);
    } else {
        wait_ms = ex->cfg.now_ms() - job.rx_ms;
        status = uds_server_process_request_src(ex->cfg.srv, job.req, job.resp,
                                                job.addressing, src);
        if (status != UDS_STATUS_OK) {
            job.resp->length = (uint16_t)0U;
        }
        deferred = s_is_response_pending(job.resp);
        (void)uds_server_take_pending_reset(ex->cfg.srv, &reset_type);
    }
    EDS_RACE_POINT(EDS_RACE_EXEC_RAN);

    ex->cfg.enter();
    if (deferred) {
        ex->job_src = src;   /* the request was deferred: its job's frames go here */
    }
    if (job.is_call) {
        ex->stats.calls++;
    } else {
        ex->stats.served++;
        if (wait_ms > ex->stats.max_queue_wait_ms) {
            ex->stats.max_queue_wait_ms = wait_ms;
        }
    }
    if (reset_type != (uint8_t)0U) {
        ex->reset_type     = reset_type;
        ex->reset_src      = src;
        ex->reset_released = ex->slots[idx].released;
    }
    if (ex->slots[idx].released) {
        /* The producer released it while it ran: no one takes a response. */
        ex->slots[idx].released = false;
        ex->slots[idx].state    = UDS_EXEC_SLOT_FREE;
    } else {
        ex->slots[idx].state = UDS_EXEC_SLOT_DONE;
    }
    ex->cfg.exit();

    s_notify(ex, idx);
}

/* 3. The pending job: one step, its frame routed to job_src. busy_src is
 *    published around the call so other producers reject fast (0x21). */
static void s_poll_job(uds_exec_t *ex)
{
    bool mailbox_free;

    ex->cfg.enter();
    mailbox_free = (ex->push_state == (uint8_t)EXEC_PUSH_IDLE);
    if (mailbox_free && (ex->job_src != EXEC_SRC_NONE)) {
        ex->busy_src = ex->job_src;
    }
    ex->cfg.exit();

    if (mailbox_free && (ex->job_src != EXEC_SRC_NONE)) {
        s_step_job(ex);
    }
}

/* Poll the server once while busy_src is published; route its frame. */
static void s_step_job(uds_exec_t *ex)
{
    uds_poll_result_t r;
    uint8_t           idx;

    r = uds_server_poll(ex->cfg.srv, &ex->push_buf);

    ex->cfg.enter();
    ex->busy_src = EXEC_SRC_NONE;
    if (r == UDS_POLL_FRAME) {
        ex->push_kind  = UDS_EXEC_PUSH_JOB;
        ex->push_src   = ex->job_src;
        ex->push_state = (uint8_t)EXEC_PUSH_READY;
    } else if (r == UDS_POLL_IDLE) {
        ex->job_src = EXEC_SRC_NONE;   /* finished, or aborted with its session */
    } else {
        /* UDS_POLL_NOTHING_NOW: the job is still running. */
    }
    ex->cfg.exit();

    if (r == UDS_POLL_FRAME) {
        idx = s_index(ex, ex->job_src);
        if (idx < ex->cfg.producer_count) {
            s_notify(ex, idx);
        }
    }
}

/* 4. One due periodic frame, routed to the source that subscribed it. */
static void s_pop_periodic(uds_exec_t *ex)
{
    uint8_t psrc = EXEC_SRC_NONE;
    bool    mailbox_free;

    ex->cfg.enter();
    mailbox_free = (ex->push_state == (uint8_t)EXEC_PUSH_IDLE);
    ex->cfg.exit();

    if (mailbox_free && (uds_periodic_pop_due_src(&ex->push_buf, &psrc) == UDS_STATUS_OK)) {
        s_route_periodic(ex, psrc);
    }
}

/* Hand a popped periodic frame to its subscriber, or count it dropped. */
static void s_route_periodic(uds_exec_t *ex, uint8_t psrc)
{
    uint8_t idx = s_index(ex, psrc);

    ex->cfg.enter();
    if ((idx < ex->cfg.producer_count) && ex->cfg.producers[idx].periodic_tx) {
        ex->push_kind  = UDS_EXEC_PUSH_PERIODIC;
        ex->push_src   = psrc;
        ex->push_state = (uint8_t)EXEC_PUSH_READY;
    } else {
        ex->stats.periodic_dropped++;
        idx = ex->cfg.producer_count;
    }
    ex->cfg.exit();

    if (idx < ex->cfg.producer_count) {
        s_notify(ex, idx);
    }
}

/* 5. The ECUReset, once its source has released (sent) the response. */
static void s_do_reset(uds_exec_t *ex)
{
    uint8_t type = (uint8_t)0U;

    ex->cfg.enter();
    if ((ex->reset_type != (uint8_t)0U) && ex->reset_released) {
        type           = ex->reset_type;
        ex->reset_type = (uint8_t)0U;
        ex->reset_src  = EXEC_SRC_NONE;
    }
    ex->cfg.exit();

    if ((type != (uint8_t)0U) && (ex->cfg.reset != NULL)) {
        ex->cfg.reset(type);
    }
}

/* --------------------------------------------------------------------------
 * Public API (each with one exit, MISRA C:2012 Rule 15.5)
 * -------------------------------------------------------------------------- */

/* The init checks, one place: UDS_STATUS_OK if cfg can be used. */
static uds_status_t s_check_cfg(const uds_exec_cfg_t *cfg, uint8_t *out_mask)
{
    uds_status_t status = UDS_STATUS_OK;
    uint8_t      i;
    uint8_t      j;

    *out_mask = (uint8_t)0U;
    if ((cfg->srv == NULL) || (cfg->producers == NULL)) {
        status = UDS_STATUS_ERR_NULL_PTR;
    } else if ((cfg->now_ms == NULL) || (cfg->enter == NULL) || (cfg->exit == NULL) ||
               (cfg->producer_count == (uint8_t)0U) ||
               (cfg->producer_count > (uint8_t)EDS_EXEC_MAX_PRODUCERS)) {
        status = UDS_STATUS_ERR_INVALID_PARAM;
    } else {
        for (i = (uint8_t)0U; i < cfg->producer_count; i++) {
            if (cfg->producers[i].src > (uint8_t)EXEC_SRC_MAX) {
                status = UDS_STATUS_ERR_INVALID_PARAM;
            }
            for (j = (uint8_t)(i + 1U); j < cfg->producer_count; j++) {
                if (cfg->producers[i].src == cfg->producers[j].src) {
                    status = UDS_STATUS_ERR_INVALID_PARAM;
                }
            }
            if ((status == UDS_STATUS_OK) && cfg->producers[i].periodic_tx) {
                *out_mask |= (uint8_t)(1U << cfg->producers[i].src);
            }
        }
    }
    return status;
}

/* True for an executor uds_exec_init() accepted. */
static bool s_ready(const uds_exec_t *ex)
{
    return (ex != NULL) && ex->initialized;
}

uds_status_t uds_exec_init(uds_exec_t *ex, const uds_exec_cfg_t *cfg)
{
    uds_status_t status;
    uint8_t      mask = (uint8_t)0U;

    if ((ex == NULL) || (cfg == NULL)) {
        status = UDS_STATUS_ERR_NULL_PTR;
    } else {
        status = s_check_cfg(cfg, &mask);
    }

    if (status == UDS_STATUS_OK) {
        (void)memset(ex, 0, sizeof(*ex));
        ex->cfg       = *cfg;
        ex->busy_src  = EXEC_SRC_NONE;
        ex->job_src   = EXEC_SRC_NONE;
        ex->reset_src = EXEC_SRC_NONE;
        ex->push_src  = EXEC_SRC_NONE;
        eds_elapsed_init(&ex->clock, cfg->now_ms());
        /* Decision ⬥E: 0x2A from a producer with no periodic TX gets NRC 0x22. */
        uds_periodic_set_tx_src_mask(mask);
        ex->initialized = true;
    }
    return status;
}

uds_exec_submit_result_t uds_exec_submit(uds_exec_t          *ex,
                                         uint8_t              src,
                                         const uds_msg_buf_t *req,
                                         uds_msg_buf_t       *resp,
                                         uds_addressing_t     addressing)
{
    uds_exec_submit_result_t result = UDS_EXEC_INVALID;
    uds_exec_slot_t         *slot;
    uint8_t                  idx;
    uint32_t                 now;

    if (s_ready(ex) && (req != NULL) && (resp != NULL)) {
        idx = s_index(ex, src);
        if (idx < ex->cfg.producer_count) {
            slot = &ex->slots[idx];
            now  = ex->cfg.now_ms();

            ex->cfg.enter();
            if ((slot->state != UDS_EXEC_SLOT_FREE) ||
                ((ex->busy_src != EXEC_SRC_NONE) && (ex->busy_src != src))) {
                /* Backpressure (own request still in flight) or the fast
                 * reject (another producer's job step is running).
                 * Functional requests, 3E 80 among them, are dropped: S3 is
                 * served by the request in flight. Physical ones always get
                 * an answer. */
                if (addressing == UDS_ADDRESSING_FUNCTIONAL) {
                    ex->stats.dropped++;
                    result = UDS_EXEC_DROPPED;
                } else {
                    ex->stats.reject_busy++;
                    result = UDS_EXEC_REJECT_BUSY;
                }
            } else {
                slot->is_call    = false;
                slot->addressing = addressing;
                slot->rx_ms      = now;
                slot->req        = req;
                slot->resp       = resp;
                slot->fn         = NULL;
                slot->fn_arg     = NULL;
                slot->released   = false;
                slot->state      = UDS_EXEC_SLOT_POSTED;
                result = UDS_EXEC_POSTED;
            }
            ex->cfg.exit();
        }
    }

    if (result == UDS_EXEC_POSTED) {
        s_wake(ex);
    }
    return result;
}

uds_status_t uds_exec_call(uds_exec_t *ex, uint8_t src, uds_exec_fn_t fn, void *arg)
{
    uds_status_t status;
    uint8_t      idx = (uint8_t)0U;

    if ((ex == NULL) || (fn == NULL)) {
        status = UDS_STATUS_ERR_NULL_PTR;
    } else if (!ex->initialized) {
        status = UDS_STATUS_ERR_NOT_INITIALIZED;
    } else {
        idx = s_index(ex, src);
        status = (idx < ex->cfg.producer_count) ? UDS_STATUS_OK : UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (status == UDS_STATUS_OK) {
        ex->cfg.enter();
        if (ex->slots[idx].state != UDS_EXEC_SLOT_FREE) {
            status = UDS_STATUS_ERR_BUSY;
        } else {
            ex->slots[idx].is_call = true;
            ex->slots[idx].req     = NULL;
            ex->slots[idx].resp    = NULL;
            ex->slots[idx].fn      = fn;
            ex->slots[idx].fn_arg   = arg;
            ex->slots[idx].released = false;
            ex->slots[idx].state    = UDS_EXEC_SLOT_POSTED;
        }
        ex->cfg.exit();
        if (status == UDS_STATUS_OK) {
            s_wake(ex);
        }
    }
    return status;
}

bool uds_exec_done(uds_exec_t *ex, uint8_t src)
{
    bool    done = false;
    uint8_t idx;

    if (s_ready(ex)) {
        idx = s_index(ex, src);
        if (idx < ex->cfg.producer_count) {
            ex->cfg.enter();
            done = (ex->slots[idx].state == UDS_EXEC_SLOT_DONE);
            ex->cfg.exit();
        }
    }
    return done;
}

void uds_exec_release(uds_exec_t *ex, uint8_t src)
{
    uint8_t idx;
    bool    released = false;

    if (s_ready(ex)) {
        idx = s_index(ex, src);
        if (idx < ex->cfg.producer_count) {
            ex->cfg.enter();
            if (ex->slots[idx].state == UDS_EXEC_SLOT_DONE) {
                ex->slots[idx].state = UDS_EXEC_SLOT_FREE;
                if ((ex->reset_type != (uint8_t)0U) && (ex->reset_src == src)) {
                    ex->reset_released = true;
                }
                released = true;
            } else if (ex->slots[idx].state == UDS_EXEC_SLOT_POSTED) {
                /* Given up before it ran: withdrawn, never dispatched. */
                ex->slots[idx].state = UDS_EXEC_SLOT_FREE;
            } else if (ex->slots[idx].state == UDS_EXEC_SLOT_RUNNING) {
                /* Given up while it runs: freed when the executor is done
                 * with its buffers (s_run_slot()). */
                ex->slots[idx].released = true;
            } else {
                /* FREE: nothing to release. */
            }
            ex->cfg.exit();
        }
    }

    if (released) {
        s_wake(ex);
    }
}

const uds_msg_buf_t *uds_exec_push_frame(uds_exec_t *ex, uint8_t src)
{
    const uds_msg_buf_t *frame = NULL;

    if (s_ready(ex)) {
        ex->cfg.enter();
        if ((ex->push_state == (uint8_t)EXEC_PUSH_READY) && (ex->push_src == src)) {
            frame = &ex->push_buf;
        }
        ex->cfg.exit();
    }
    return frame;
}

void uds_exec_push_done(uds_exec_t *ex, uint8_t src, bool sent)
{
    bool reported = false;

    if (s_ready(ex)) {
        ex->cfg.enter();
        if ((ex->push_state == (uint8_t)EXEC_PUSH_READY) && (ex->push_src == src)) {
            ex->push_state = sent ? (uint8_t)EXEC_PUSH_SENT : (uint8_t)EXEC_PUSH_FAILED;
            reported = true;
        }
        ex->cfg.exit();
    }

    if (reported) {
        s_wake(ex);
    }
}

bool uds_exec_job_pending(uds_exec_t *ex, uint8_t src)
{
    bool pending = false;

    if (s_ready(ex)) {
        ex->cfg.enter();
        pending = (ex->job_src == src) ||
                  ((ex->push_state == (uint8_t)EXEC_PUSH_READY) && (ex->push_src == src) &&
                   (ex->push_kind == UDS_EXEC_PUSH_JOB));
        ex->cfg.exit();
    }
    return pending;
}

void uds_exec_disconnect(uds_exec_t *ex, uint8_t src)
{
    if (s_ready(ex) && (s_index(ex, src) < ex->cfg.producer_count)) {
        ex->cfg.enter();
        ex->gone_mask |= (uint8_t)(1U << src);
        ex->cfg.exit();
        s_wake(ex);
    }
}

bool uds_exec_run_once(uds_exec_t *ex)
{
    bool    more = false;
    uint8_t i;

    if (s_ready(ex)) {
        s_run_ticks(ex);
        s_finish_push(ex);
        s_drop_gone(ex);
        s_serve_slots(ex);
        s_poll_job(ex);
        s_pop_periodic(ex);
        s_do_reset(ex);

        ex->cfg.enter();
        for (i = (uint8_t)0U; i < ex->cfg.producer_count; i++) {
            if (ex->slots[i].state == UDS_EXEC_SLOT_POSTED) {
                more = true;
            }
        }
        if ((ex->push_state == (uint8_t)EXEC_PUSH_SENT) ||
            (ex->push_state == (uint8_t)EXEC_PUSH_FAILED) ||
            (ex->gone_mask != (uint8_t)0U) ||
            ((ex->reset_type != (uint8_t)0U) && ex->reset_released)) {
            more = true;
        }
        ex->cfg.exit();
        /* A pending job with nothing to send waits for the next 1 ms tick,
         * as the DoIP poll loop did: a pass right away would spin. */
    }
    return more;
}

uds_status_t uds_exec_get_stats(uds_exec_t *ex, uds_exec_stats_t *out)
{
    uds_status_t status = UDS_STATUS_OK;

    if ((ex == NULL) || (out == NULL)) {
        status = UDS_STATUS_ERR_NULL_PTR;
    } else if (!ex->initialized) {
        status = UDS_STATUS_ERR_NOT_INITIALIZED;
    } else {
        ex->cfg.enter();
        *out = ex->stats;
        ex->cfg.exit();
    }
    return status;
}
