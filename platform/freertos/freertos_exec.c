// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_exec.c
 *
 * PURPOSE: [EDS#197] FreeRTOS binding of the diagnostic executor. See
 *          freertos_exec.h.
 *
 * SAFETY  : No malloc. No recursion. One executor per image.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "freertos_exec.h"
#include "platform_api.h"   /* eds_platform_uptime_ms() */

#include "FreeRTOS.h"
#include "task.h"

#include <stdint.h>

/* Notification bits (freertos_exec.h). */
#define EXEC_BIT_WAKE    (1UL << 0)   /* executor: work posted             */
#define EXEC_BIT_SLOT(i) (1UL << (8U + (uint32_t)(i)))   /* producer: slot i done */

static StaticTask_t            s_exec_tcb;
static StackType_t             s_exec_stack[EDS_EXEC_STACK_WORDS];
static TaskHandle_t            s_exec_task;

static uds_exec_t              s_ex;
static freertos_exec_cfg_t     s_cfg;
static uds_exec_producer_cfg_t s_producers[EDS_EXEC_MAX_PRODUCERS];
static TaskHandle_t volatile   s_waiter[EDS_EXEC_MAX_PRODUCERS];
static bool                    s_started;

/* --------------------------------------------------------------------------
 * uds_exec bindings
 * -------------------------------------------------------------------------- */

static void cs_enter(void)
{
    taskENTER_CRITICAL();
}

static void cs_exit(void)
{
    taskEXIT_CRITICAL();
}

static void wake(void *arg)
{
    (void)arg;
    (void)xTaskNotify(s_exec_task, EXEC_BIT_WAKE, eSetBits);
}

static void slot_notify(void *arg)
{
    uint32_t     idx  = (uint32_t)(uintptr_t)arg;
    TaskHandle_t task = s_waiter[idx];

    if (task != NULL) {
        (void)xTaskNotify(task, EXEC_BIT_SLOT(idx), eSetBits);
    }
}

/* --------------------------------------------------------------------------
 * The executor task
 * -------------------------------------------------------------------------- */

static void exec_task(void *arg)
{
    bool more;

    (void)arg;
    for (;;) {
        /* A producer's wake, or one tick (1 ms at configTICK_RATE_HZ 1000). */
        (void)xTaskNotifyWait(0UL, EXEC_BIT_WAKE, NULL, (TickType_t)1);
        do {
            more = uds_exec_run_once(&s_ex);
            if (s_cfg.after_pass != NULL) {
                s_cfg.after_pass();
            }
        } while (more);
    }
}

static uint8_t slot_of(uint8_t src)
{
    uint8_t i;
    uint8_t idx = EDS_EXEC_MAX_PRODUCERS;

    for (i = 0U; i < s_cfg.producer_count; i++) {
        if (s_producers[i].src == src) {
            idx = i;
        }
    }
    return idx;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

uds_status_t freertos_exec_start(const freertos_exec_cfg_t *cfg)
{
    uds_exec_cfg_t ec = { 0 };
    uds_status_t   status;
    uint8_t        i;

    if ((cfg == NULL) || (cfg->srv == NULL) || (cfg->producers == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (s_started) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }
    if ((cfg->producer_count == 0U) || (cfg->producer_count > EDS_EXEC_MAX_PRODUCERS)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    s_cfg = *cfg;
    for (i = 0U; i < cfg->producer_count; i++) {
        s_producers[i]            = cfg->producers[i];
        s_producers[i].notify     = slot_notify;
        s_producers[i].notify_arg = (void *)(uintptr_t)i;
        s_waiter[i]               = NULL;
    }

    /* The task exists before uds_exec can call wake(). */
    s_exec_task = xTaskCreateStatic(exec_task, "eds_exec", (uint32_t)EDS_EXEC_STACK_WORDS,
                                    NULL, (UBaseType_t)EDS_EXEC_PRIORITY,
                                    s_exec_stack, &s_exec_tcb);

    ec.srv            = cfg->srv;
    ec.now_ms         = eds_platform_uptime_ms;
    ec.enter          = cs_enter;
    ec.exit           = cs_exit;
    ec.wake           = wake;
    ec.wake_arg       = NULL;
    ec.reset          = cfg->reset;
    ec.producers      = s_producers;
    ec.producer_count = cfg->producer_count;
    status = uds_exec_init(&s_ex, &ec);
    if (status == UDS_STATUS_OK) {
        s_started = true;
    } else {
        vTaskDelete(s_exec_task);
    }
    return status;
}

uds_exec_t *freertos_exec_get(void)
{
    return s_started ? &s_ex : NULL;
}

bool freertos_exec_wait(uint8_t src, TickType_t timeout)
{
    uint8_t    idx   = slot_of(src);
    TickType_t start = xTaskGetTickCount();
    TickType_t spent;
    bool       done  = false;
    bool       late  = false;

    if (s_started && (idx < s_cfg.producer_count)) {
        s_waiter[idx] = xTaskGetCurrentTaskHandle();
        done = uds_exec_done(&s_ex, src);
        while (!done && !late) {
            spent = xTaskGetTickCount() - start;
            if (timeout == portMAX_DELAY) {
                (void)xTaskNotifyWait(0UL, EXEC_BIT_SLOT(idx), NULL, portMAX_DELAY);
            } else if (spent < timeout) {
                (void)xTaskNotifyWait(0UL, EXEC_BIT_SLOT(idx), NULL, timeout - spent);
            } else {
                late = true;
            }
            done = uds_exec_done(&s_ex, src);
        }
    }
    return done;
}

bool freertos_exec_wait_event(uint8_t src, uint32_t timeout_ms)
{
    uint8_t  idx  = slot_of(src);
    uint32_t bits = 0UL;
    bool     got  = false;

    if (s_started && (idx < s_cfg.producer_count)) {
        /* Registered before the check: a notify after it is not lost. */
        s_waiter[idx] = xTaskGetCurrentTaskHandle();
        got = uds_exec_done(&s_ex, src) || (uds_exec_push_frame(&s_ex, src) != NULL);
        if (!got) {
            (void)xTaskNotifyWait(0UL, EXEC_BIT_SLOT(idx), &bits, pdMS_TO_TICKS(timeout_ms));
            got = ((bits & EXEC_BIT_SLOT(idx)) != 0UL);
        }
    }
    return got;
}
