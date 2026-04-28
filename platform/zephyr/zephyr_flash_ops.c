/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr_flash_ops.c
 *
 * PURPOSE: Zephyr platform implementation of uds_flash_ops_t callbacks.
 *
 * Targets the MCUboot secondary slot (image_1) using Zephyr's flash_map API.
 * This is the production implementation for the ARDEP board and any other
 * Zephyr target using MCUboot-based firmware update.
 *
 * USAGE:
 *   Call zephyr_flash_ops_init() from the application's main() before the
 *   UDS stack is started.  This registers the flash ops table so that
 *   service_0x34.c can accept RequestDownload requests.
 *
 *   Example in main.c:
 *     zephyr_flash_ops_init();
 *     uds_generated_init(can_transport, rx_id, tx_id);
 *
 * FLASH AREA:
 *   FLASH_AREA_ID(image_1) — MCUboot secondary slot.
 *   The primary slot (image_0) is never written directly; MCUboot performs
 *   the swap on next boot after the secondary slot is validated.
 *
 * CRC VERIFICATION:
 *   The verify callback reads back the written bytes from flash and computes
 *   the same CRC-32 that service_0x37.c accumulated over the received data.
 *   A mismatch returns UDS_STATUS_ERR_GENERIC which maps to NRC 0x72.
 *
 * SAFETY:
 *   REQ-FLASH-001: zephyr_flash_ops_init() must be called before stack init.
 *   REQ-FLASH-002: Memory map enforces writes only to the secondary slot.
 *   REQ-FLASH-003: Verify callback confirms CRC integrity post-transfer.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "zephyr_flash_ops.h"
#include "uds_flash_ops.h"
#include "uds_transfer_ctx.h"
#include "uds_types.h"

#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <stdint.h>

/* [FIX] Was LOG_MODULE_DECLARE(basic_ecu) — see transport/zephyr_can.c for rationale. */
LOG_MODULE_REGISTER(zephyr_flash_ops, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Secondary slot region (derived from flash map at compile time)
 * -------------------------------------------------------------------------- */

#ifndef FLASH_AREA_ID
#  error "FLASH_AREA_ID not available — ensure CONFIG_FLASH_MAP=y in prj.conf"
#endif

/** @brief ID of the MCUboot secondary slot (image_1). */
#define ZEPHYR_FLASH_SECONDARY_SLOT  FLASH_AREA_ID(image_1)

/* --------------------------------------------------------------------------
 * Static memory map — single writable region (secondary slot)
 * -------------------------------------------------------------------------- */

static uds_flash_region_t s_flash_region;   /* populated at init */

static const uds_flash_ops_t *sp_registered_ops = NULL;

/* --------------------------------------------------------------------------
 * Callback implementations
 * -------------------------------------------------------------------------- */

static uds_status_t z_flash_erase(uint32_t address, uint32_t size_bytes)
{
    const struct flash_area *fa = NULL;
    int rc;

    rc = flash_area_open(ZEPHYR_FLASH_SECONDARY_SLOT, &fa);
    if (rc != 0) {
        LOG_ERR("flash_area_open(image_1) failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    rc = flash_area_erase(fa, (off_t)(address - s_flash_region.base_address),
                          (size_t)size_bytes);
    flash_area_close(fa);

    if (rc != 0) {
        LOG_ERR("flash_area_erase failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    LOG_INF("Flash erased: addr=0x%08X size=%u", address, size_bytes);
    return UDS_STATUS_OK;
}

static uds_status_t z_flash_write(uint32_t       address,
                                   const uint8_t *data,
                                   uint32_t       length)
{
    const struct flash_area *fa = NULL;
    int rc;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    rc = flash_area_open(ZEPHYR_FLASH_SECONDARY_SLOT, &fa);
    if (rc != 0) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    rc = flash_area_write(fa, (off_t)(address - s_flash_region.base_address),
                          data, (size_t)length);
    flash_area_close(fa);

    if (rc != 0) {
        LOG_ERR("flash_area_write failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    return UDS_STATUS_OK;
}

static uds_status_t z_flash_verify(uint32_t address,
                                    uint32_t size_bytes,
                                    uint32_t expected_crc)
{
    const struct flash_area *fa   = NULL;
    uint8_t                  buf[128U];
    uint32_t                 crc  = (uint32_t)0xFFFFFFFFUL;
    uint32_t                 remaining;
    uint32_t                 offset;
    uint32_t                 chunk;
    int                      rc;
    uds_status_t             status = UDS_STATUS_OK;

    rc = flash_area_open(ZEPHYR_FLASH_SECONDARY_SLOT, &fa);
    if (rc != 0) {
        return UDS_STATUS_ERR_PLATFORM;
    }

    remaining = size_bytes;
    offset    = (uint32_t)0U;

    while (remaining > (uint32_t)0U) {
        chunk = (remaining < (uint32_t)sizeof(buf)) ? remaining
                                                     : (uint32_t)sizeof(buf);

        rc = flash_area_read(fa,
                             (off_t)((address - s_flash_region.base_address) + offset),
                             buf,
                             (size_t)chunk);
        if (rc != 0) {
            status = UDS_STATUS_ERR_PLATFORM;
            break;
        }

        crc = uds_transfer_crc32_update(crc, buf, chunk);

        offset    += chunk;
        remaining -= chunk;
    }

    flash_area_close(fa);

    if (status != UDS_STATUS_OK) {
        return status;
    }

    crc = uds_transfer_crc32_finalise(crc);

    if (crc != expected_crc) {
        LOG_ERR("Flash verify CRC mismatch: computed=0x%08X expected=0x%08X",
                crc, expected_crc);
        return UDS_STATUS_ERR_GENERIC;
    }

    LOG_INF("Flash verify OK: CRC=0x%08X", crc);
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Flash ops table
 * -------------------------------------------------------------------------- */

static uds_flash_ops_t s_zephyr_flash_ops = {
    .erase_cb        = z_flash_erase,
    .write_cb        = z_flash_write,
    .verify_cb       = z_flash_verify,
    .memory_map      = &s_flash_region,
    .region_count    = (uint8_t)1U,
    .max_block_length = (uint16_t)256U,   /* 256 bytes payload per TransferData block */
};

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

uds_status_t zephyr_flash_ops_init(void)
{
    const struct flash_area *fa = NULL;
    int rc;

    rc = flash_area_open(ZEPHYR_FLASH_SECONDARY_SLOT, &fa);
    if (rc != 0) {
        LOG_ERR("zephyr_flash_ops_init: flash_area_open(image_1) failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* Populate the memory region descriptor from the flash map. */
    s_flash_region.base_address = (uint32_t)fa->fa_off;
    s_flash_region.size_bytes   = (uint32_t)fa->fa_size;
    s_flash_region.writable     = true;

    flash_area_close(fa);

    /* Register with the global flash ops singleton. */
    return uds_flash_ops_register(&s_zephyr_flash_ops);
}
