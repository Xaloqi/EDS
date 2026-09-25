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
 *   FIXED_PARTITION_ID(image_1) — MCUboot secondary slot.
 *   [#200/#291] Was FLASH_AREA_ID(image_1) — that macro does not exist in
 *   Zephyr v3.7.0 (this project's pinned revision, west.yml). It was
 *   renamed to FIXED_PARTITION_ID some releases back; FLASH_AREA_ID
 *   compiled to an implicit-function-declaration error citing "'image_1'
 *   undeclared", not a missing-macro error, which is what made this take
 *   two wrong guesses to root-cause: this file was never compiled by any
 *   CI job before #291, the first real Zephyr build of safeboot_ecu.
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
#include "zephyr_wdt.h"
#include "uds_flash_ops.h"
#include "uds_transfer_ctx.h"
#include "uds_types.h"

#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

#include <string.h>
#include <stdint.h>

/* [FIX] Was LOG_MODULE_DECLARE(basic_ecu) — see transport/zephyr_can.c for rationale. */
LOG_MODULE_REGISTER(zephyr_flash_ops, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Secondary slot region (derived from flash map at compile time)
 * -------------------------------------------------------------------------- */

#ifndef FIXED_PARTITION_ID
#  error "FIXED_PARTITION_ID not available — ensure CONFIG_FLASH_MAP=y in prj.conf"
#endif

/** @brief ID of the MCUboot secondary slot (image_1). */
#define ZEPHYR_FLASH_SECONDARY_SLOT  FIXED_PARTITION_ID(image_1)

/* --------------------------------------------------------------------------
 * Static memory map — single readable + writable region (secondary slot)
 * -------------------------------------------------------------------------- */

static uds_flash_region_t s_flash_region;   /* populated at init */

static const uds_flash_ops_t *sp_registered_ops = NULL;

/* --------------------------------------------------------------------------
 * [#312] Chunked erase + bounded watchdog bridge
 *
 * STM32H7's own devicetree binding (stm32h743.dtsi etc.) declares this
 * board's flash: erase-block-size = 128 KB, max-erase-time = 4000 ms —
 * both manufacturer-documented, not measured guesses. A single sector
 * erase can legitimately run for up to 4 seconds; this ECU's ASIL-B
 * watchdog window is 100 ms (CONFIG_DIAG_WDT_WINDOW_MS). z_flash_erase()
 * below (whole-region, one call) starves that window on any region larger
 * than a small fraction of one sector — see issue #312 for the full
 * evidence trail (RequestDownload never answering, a clean unprompted
 * MCUboot/Zephyr reboot appearing mid-transfer).
 *
 * z_flash_erase_step() erases exactly one bounded increment (<= one
 * sector) per call, so service_0x34.c can spread a large erase across
 * multiple poll-loop iterations with NRC 0x78 between them (UDS timing
 * contract) and a watchdog feed between them (poll-loop liveness). That
 * still leaves ONE increment's own worst-case duration (up to 4000 ms)
 * unfed from the poll loop's normal per-iteration feed — s_erase_feed_timer
 * bridges exactly that one bounded call: started immediately before
 * flash_area_erase(), stopped immediately after. It is not a standing
 * bypass of watchdog supervision — outside this one call (including if the
 * poll loop itself later hangs for an unrelated reason) the watchdog
 * behaves exactly as before.
 * -------------------------------------------------------------------------- */

/** One STM32H7 flash sector — see stm32h743.dtsi's erase-block-size. */
#define ZEPHYR_FLASH_ERASE_UNIT_SIZE       (128UL * 1024UL)

/** Feed period while bridging one erase_step_cb() call. Comfortably under
 *  the 100 ms window with margin for jitter. */
#define ZEPHYR_FLASH_ERASE_FEED_PERIOD_MS  (20U)

static diag_wdt_t *sp_erase_wdt = NULL;

static void s_erase_feed_timer_expiry(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    if (sp_erase_wdt != NULL) {
        (void)diag_wdt_feed(sp_erase_wdt);
    }
}

K_TIMER_DEFINE(s_erase_feed_timer, s_erase_feed_timer_expiry, NULL);

void zephyr_flash_ops_set_wdt(diag_wdt_t *wdt)
{
    sp_erase_wdt = wdt;
}

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

static uds_status_t z_flash_erase_step(uint32_t   address,
                                        uint32_t   max_size,
                                        uint32_t  *out_erased_bytes)
{
    const struct flash_area *fa = NULL;
    uint32_t                 chunk;
    int                       rc;

    if ((out_erased_bytes == NULL) || (max_size == (uint32_t)0U)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    chunk = (max_size < (uint32_t)ZEPHYR_FLASH_ERASE_UNIT_SIZE)
                ? max_size
                : (uint32_t)ZEPHYR_FLASH_ERASE_UNIT_SIZE;

    rc = flash_area_open(ZEPHYR_FLASH_SECONDARY_SLOT, &fa);
    if (rc != 0) {
        LOG_ERR("z_flash_erase_step: flash_area_open(image_1) failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* Bridge the watchdog across this one bounded call — see the [#312]
     * block comment above s_erase_feed_timer. */
    if (sp_erase_wdt != NULL) {
        k_timer_start(&s_erase_feed_timer,
                      K_MSEC(ZEPHYR_FLASH_ERASE_FEED_PERIOD_MS),
                      K_MSEC(ZEPHYR_FLASH_ERASE_FEED_PERIOD_MS));
    }

    rc = flash_area_erase(fa, (off_t)(address - s_flash_region.base_address),
                          (size_t)chunk);

    if (sp_erase_wdt != NULL) {
        k_timer_stop(&s_erase_feed_timer);
    }

    flash_area_close(fa);

    if (rc != 0) {
        LOG_ERR("z_flash_erase_step failed: %d (addr=0x%08X chunk=%u)",
                rc, address, (unsigned)chunk);
        return UDS_STATUS_ERR_PLATFORM;
    }

    *out_erased_bytes = chunk;
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
    .erase_step_cb   = z_flash_erase_step,  /* [#312] chunked erase, preferred by service_0x34.c */
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
    s_flash_region.readable     = true;

    flash_area_close(fa);

    /* Register with the global flash ops singleton. */
    return uds_flash_ops_register(&s_zephyr_flash_ops);
}
