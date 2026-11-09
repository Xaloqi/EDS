// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/bench_permanent_dtc.c
 *
 * PURPOSE: [EDS#580] See bench_permanent_dtc.h.
 * =============================================================================
 */

#include "bench_permanent_dtc.h"
#include "dtc_database.h"

#include <zephyr/logging/log.h>

#if defined(CONFIG_BOARD_NATIVE_SIM)
#include <stdlib.h>
#endif

LOG_MODULE_REGISTER(bench_perm_dtc, LOG_LEVEL_INF);

void bench_permanent_dtc_step(uint32_t dtc_code)
{
    const dtc_entry_t *entry = dtc_database_find(dtc_code);

#if defined(CONFIG_BOARD_NATIVE_SIM)
    /* Control run: EDS_BENCH_NO_PERMANENT=1 leaves the DTC non-permanent, so a
     * capture can show 14 FF FF FF clearing it. */
    if (getenv("EDS_BENCH_NO_PERMANENT") != NULL) {
        return;
    }
#endif
    if ((entry == NULL) || entry->is_permanent ||
        ((entry->status_byte & (uint8_t)DTC_STATUS_CONFIRMED_DTC) == 0U)) {
        return;
    }
    if (dtc_database_set_permanent(dtc_code, true) == UDS_STATUS_OK) {
        LOG_WRN("[BENCH] DTC 0x%06X marked permanent (status 0x%02X)",
                (unsigned)dtc_code, (unsigned)entry->status_byte);
    }
}
