// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/bench_slow_routine.c
 *
 * PURPOSE: [EDS#282] See bench_slow_routine.h. Deterministic: each step is
 *          one k_busy_wait() of the requested length, so the measured gaps
 *          are the server's, not the workload's.
 * =============================================================================
 */

#include "bench_slow_routine.h"
#include "routine_database.h"
#include "uds_session.h"

#include <zephyr/kernel.h>
#include <string.h>

static uint8_t  s_steps_total;
static uint8_t  s_steps_left;
static uint32_t s_step_ms;

static uds_status_t s_start(const uint8_t *opt_buf, uint8_t opt_len,
                            uint8_t *result_buf, uint8_t result_buf_len,
                            uint8_t *result_len)
{
    uint32_t step_ms;

    (void)result_buf;
    (void)result_buf_len;
    *result_len = 0U;

    if ((opt_buf == NULL) || (opt_len != 2U) || (opt_buf[0] == 0U)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }
    step_ms = (uint32_t)opt_buf[1] * 10U;
    if ((step_ms == 0U) || (step_ms > (uint32_t)BENCH_SLOW_ROUTINE_STEP_MAX_MS)) {
        return UDS_STATUS_ERR_REQUEST_OUT_OF_RANGE;
    }

    s_steps_total = opt_buf[0];
    s_steps_left  = opt_buf[0];
    s_step_ms     = step_ms;
    return UDS_STATUS_OK;
}

static uds_step_result_t s_step(uint8_t *result_buf, uint8_t result_buf_len,
                                uint8_t *result_len, uds_nrc_t *nrc)
{
    if ((s_steps_left == 0U) || (result_buf_len < 1U)) {
        *nrc = UDS_NRC_CONDITIONS_NOT_CORRECT;
        return UDS_STEP_FAILED;
    }

    k_busy_wait(s_step_ms * 1000U);
    s_steps_left--;
    if (s_steps_left > 0U) {
        return UDS_STEP_IN_PROGRESS;
    }

    result_buf[0] = s_steps_total;
    *result_len   = 1U;
    return UDS_STEP_DONE;
}

static void s_abort(void)
{
    s_steps_left = 0U;
}

uds_status_t bench_slow_routine_register(void)
{
    static const routine_deferral_t deferral = {
        .step_cb     = s_step,
        .abort_cb    = s_abort,
        .step_max_ms = (uint32_t)BENCH_SLOW_ROUTINE_STEP_MAX_MS,
    };
    routine_entry_t entry;
    uds_status_t    status;

    (void)memset(&entry, 0, sizeof(entry));
    entry.rid            = (uint16_t)BENCH_SLOW_ROUTINE_RID;
    entry.support_flags  = (uint8_t)ROUTINE_SUPPORT_START;
    entry.min_session    = (uint8_t)UDS_SESSION_EXTENDED;
    entry.security_level = (uint8_t)0U;
    entry.start_cb       = s_start;
    entry.description    = "BenchSlowRoutine";

    status = routine_database_register(&entry);
    if (status == UDS_STATUS_OK) {
        status = routine_database_set_deferral((uint16_t)BENCH_SLOW_ROUTINE_RID, &deferral);
    }
    return status;
}
