// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/eds_nvm_diag_nvs.c
 *
 * PURPOSE: eds_nvm_init_diag_nvs(), see the header. The partition geometry is
 *          read at run time because the boards differ: 128 KB pages (STM32H7),
 *          8 KB pages (MCX N947), 8 KB pages (S32K344).
 *
 *          The partition is named by its DTS node label (eds_nvs_slot), not by
 *          its "label" string ("diag_nvs"); see basic_ecu/src/main.c.
 * =============================================================================
 */

#include "eds_nvm_diag_nvs.h"
#include "nvm_store.h"

#if defined(DIAG_NVM_BACKEND_APPEND)
#include "nvm_store_append.h"
#endif

#if defined(CONFIG_BOARD_NATIVE_SIM) || defined(EDS_NVM_MOCK) || \
    (defined(CONFIG_BOARD_MR_CANHUBK3) && !defined(CONFIG_SOC_FLASH_MCUX_C40))

uds_status_t eds_nvm_init_diag_nvs(diag_wdt_t *wdt)
{
    (void)wdt;
    /* nvm_store_mock.c ignores the config (native_sim, or a bring-up build
     * that asked for the mock, EDS_NVM_MOCK). Zephyr v3.7.0 has no
     * driver for the S32K344 internal flash, so there is no partition device
     * to resolve there; v4.4.0 adds one (CONFIG_SOC_FLASH_MCUX_C40). */
    return nvm_store_init(NULL);
}

#else

#include "eds_partition.h"
#include <zephyr/drivers/flash.h>

uds_status_t eds_nvm_init_diag_nvs(diag_wdt_t *wdt)
{
    const struct device    *nvs_dev = EDS_PARTITION_DEVICE(eds_nvs_slot);
    const off_t              nvs_off = EDS_PARTITION_OFFSET(eds_nvs_slot);
    const size_t              nvs_sz = EDS_PARTITION_SIZE(eds_nvs_slot);
    struct flash_pages_info  page_info;
    uds_status_t             status;

#if defined(DIAG_NVM_BACKEND_APPEND)
    /* [EDS#504] The append backend erases one bank during compaction; it
     * feeds the watchdog across that call. Must precede nvm_store_init(). */
    nvm_store_append_set_wdt(wdt);
#else
    (void)wdt;
#endif

    if (flash_get_page_info_by_offs(nvs_dev, nvs_off, &page_info) != 0) {
        status = UDS_STATUS_ERR_PLATFORM;
    } else {
        const nvm_store_cfg_t nvm_cfg = {
            .flash_dev    = (const void *)nvs_dev,
            .flash_offset = (uint32_t)nvs_off,
#if defined(DIAG_NVM_BACKEND_APPEND)
            /* Two banks, each half of diag_nvs, erased whole (EDS#504,
             * EDS#640: the MCX N947's 128-byte write block). */
            .sector_size  = (uint32_t)(nvs_sz / 2U),
            .sector_count = (uint8_t)2U,
#else
            .sector_size  = (uint32_t)page_info.size,
            .sector_count = (uint8_t)(nvs_sz / page_info.size),
#endif
        };

        status = nvm_store_init(&nvm_cfg);
    }

    return status;
}

#endif
