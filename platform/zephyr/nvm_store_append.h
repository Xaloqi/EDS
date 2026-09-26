// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/nvm_store_append.h
 *
 * PURPOSE: Watchdog wiring for platform/zephyr/nvm_store_append.c — see that
 * file's header comment for the full design (issue #304). The rest of this
 * backend's API is nvm_store.h's ordinary nvm_store_* functions; this
 * header exists only for the one Zephyr-specific extra call site needs.
 * =============================================================================
 */

#ifndef NVM_STORE_APPEND_H
#define NVM_STORE_APPEND_H

#include "zephyr_wdt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the watchdog instance to feed across this backend's one
 *        bounded bank-erase call (compaction, or first-boot formatting).
 *
 * OPTIONAL. Without it, compaction still works but a bank erase (this
 * hardware's own worst case: up to 4000 ms, STM32H7's documented
 * max-erase-time per 128 KB sector) is not bridged and may exceed a short
 * poll-loop watchdog window. Call once from main(), after diag_wdt_init()
 * and before nvm_store_init(), exactly like
 * platform/zephyr/zephyr_flash_ops.c's zephyr_flash_ops_set_wdt() (issue
 * #312) — the same pattern, applied to this file's own one erase call site.
 * Pass NULL to stop feeding.
 *
 * @param[in] wdt  Watchdog instance to feed, or NULL.
 */
void nvm_store_append_set_wdt(diag_wdt_t *wdt);

#ifdef __cplusplus
}
#endif

#endif /* NVM_STORE_APPEND_H */
