// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_flash_ops.c
 *
 * PURPOSE: FreeRTOS / STM32H743ZI flash ops for UDS download services.
 *
 *          Implements uds_flash_ops_t (erase / write / verify) backed by the
 *          STM32H743ZI dual-bank internal flash via STM32 HAL.
 *
 *          Flash driver design is derived from the STM32H743ZI flash driver
 *          contributed by chenyurong22 (Siemens) in GitHub issue #28:
 *            - Dual-bank layout: Bank 1 (running app) / Bank 2 (OTA staging)
 *            - Cmp_Flash read-before-write optimisation
 *            - 32-byte aligned FLASHWORD programming (H743 requirement)
 *            - Sector-granular erase (128 KB per sector)
 *          Original driver adapted to the uds_flash_ops_t interface and
 *          MISRA C:2012 constraints.
 *
 * BACKENDS:
 *   STM32H7xx defined  → STM32H743 HAL (production hardware)
 *   Otherwise          → RAM stub (CI / QEMU, no flash hardware)
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "freertos_flash_ops.h"
#include "uds_flash_ops.h"
#include "uds_transfer_ctx.h"
#include "uds_types.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* =============================================================================
 * STM32H743ZI HAL backend
 * ============================================================================= */

#if defined(STM32H7xx) || defined(STM32H743xx)

#include "stm32h7xx_hal.h"

/* --------------------------------------------------------------------------
 * Internal helpers — STM32H743 sector lookup
 *
 * STM32H743ZI: each bank has 8 sectors of 128 KB.
 * FLASH_SECTOR_0 through FLASH_SECTOR_7 are the same enum values for
 * both banks; the bank is selected via EraseInitStruct.Banks.
 * -------------------------------------------------------------------------- */

static uint32_t h7_get_sector(uint32_t addr)
{
    uint32_t bank_offset;
    uint32_t sector;

    /* Normalise to within-bank offset. */
    if (addr >= (uint32_t)0x08100000UL) {
        bank_offset = addr - (uint32_t)0x08100000UL;
    } else {
        bank_offset = addr - (uint32_t)0x08000000UL;
    }

    sector = bank_offset / FREERTOS_FLASH_SECTOR_SIZE;
    if (sector > (uint32_t)7U) {
        sector = (uint32_t)7U;
    }
    return sector;
}

/* --------------------------------------------------------------------------
 * erase_cb: erase all sectors in [address, address + size_bytes)
 * -------------------------------------------------------------------------- */

static uds_status_t h7_flash_erase(uint32_t address, uint32_t size_bytes)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t               sector_err = 0U;
    uint32_t               first_sector;
    uint32_t               num_sectors;
    HAL_StatusTypeDef      hal_rc;

    if ((address < FREERTOS_FLASH_OTA_BASE) ||
        ((address + size_bytes) > (FREERTOS_FLASH_OTA_BASE + FREERTOS_FLASH_OTA_SIZE))) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    first_sector = h7_get_sector(address);
    num_sectors  = (size_bytes + FREERTOS_FLASH_SECTOR_SIZE - (uint32_t)1U)
                   / FREERTOS_FLASH_SECTOR_SIZE;

    (void)memset(&erase, 0, sizeof(erase));
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Banks        = FLASH_BANK_2;
    erase.Sector       = first_sector;
    erase.NbSectors    = num_sectors;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_FLASH_Unlock();

    hal_rc = HAL_FLASHEx_Erase(&erase, &sector_err);

    HAL_FLASH_Lock();

    return (hal_rc == HAL_OK) ? UDS_STATUS_OK : UDS_STATUS_ERR_PLATFORM;
}

/* --------------------------------------------------------------------------
 * write_cb: write data to flash at address (32-byte aligned writes)
 * -------------------------------------------------------------------------- */

static uds_status_t h7_flash_write(uint32_t       address,
                                    const uint8_t *data,
                                    uint32_t       length)
{
    uint32_t          offset;
    uint32_t          remaining;
    uint32_t          chunk;
    uint64_t          word[4];      /* 256-bit FLASHWORD = 32 bytes */
    HAL_StatusTypeDef hal_rc;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if ((address < FREERTOS_FLASH_OTA_BASE) ||
        ((address + length) > (FREERTOS_FLASH_OTA_BASE + FREERTOS_FLASH_OTA_SIZE))) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    HAL_FLASH_Unlock();

    offset    = (uint32_t)0U;
    remaining = length;

    while (remaining > (uint32_t)0U) {
        chunk = (remaining >= (uint32_t)32U) ? (uint32_t)32U : remaining;

        (void)memset(word, 0xFF, sizeof(word));
        (void)memcpy(word, &data[offset], (size_t)chunk);

        hal_rc = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                                   (uint32_t)(address + offset),
                                   (uint64_t)((uint32_t)word));
        if (hal_rc != HAL_OK) {
            HAL_FLASH_Lock();
            return UDS_STATUS_ERR_PLATFORM;
        }

        offset    += (uint32_t)32U;
        remaining  = (remaining >= (uint32_t)32U) ? (remaining - (uint32_t)32U) : (uint32_t)0U;
    }

    HAL_FLASH_Lock();
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * verify_cb: read back and CRC-check the written image
 * -------------------------------------------------------------------------- */

static uds_status_t h7_flash_verify(uint32_t address,
                                     uint32_t size_bytes,
                                     uint32_t expected_crc)
{
    uint8_t  buf[128U];
    uint32_t crc       = (uint32_t)0xFFFFFFFFUL;
    uint32_t remaining = size_bytes;
    uint32_t offset    = (uint32_t)0U;
    uint32_t chunk;

    while (remaining > (uint32_t)0U) {
        chunk = (remaining < (uint32_t)sizeof(buf)) ? remaining
                                                     : (uint32_t)sizeof(buf);

        (void)memcpy(buf, (const void *)(address + offset), (size_t)chunk);

        crc = uds_transfer_crc32_update(crc, buf, chunk);

        offset    += chunk;
        remaining -= chunk;
    }

    crc = uds_transfer_crc32_finalise(crc);

    return (crc == expected_crc) ? UDS_STATUS_OK : UDS_STATUS_ERR_GENERIC;
}

/* =============================================================================
 * RAM stub backend — CI / QEMU (no STM32H7xx defined)
 * ============================================================================= */

#else /* !STM32H7xx */

/*
 * RAM-backed stub for CI compile/link testing.
 * Matches the flash layout constants but uses a static buffer.
 * Not suitable for production — flash writes are not persistent.
 */

#define STUB_FLASH_SIZE  (FREERTOS_FLASH_OTA_SIZE)

static uint8_t s_stub_flash[STUB_FLASH_SIZE];
static bool    s_stub_erased = false;

static uds_status_t h7_flash_erase(uint32_t address, uint32_t size_bytes)
{
    uint32_t off;
    uint32_t end;

    if (address < FREERTOS_FLASH_OTA_BASE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    off = address - FREERTOS_FLASH_OTA_BASE;
    end = off + size_bytes;
    if (end > STUB_FLASH_SIZE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    (void)memset(&s_stub_flash[off], 0xFF, (size_t)size_bytes);
    s_stub_erased = true;
    return UDS_STATUS_OK;
}

static uds_status_t h7_flash_write(uint32_t       address,
                                    const uint8_t *data,
                                    uint32_t       length)
{
    uint32_t off;
    uint32_t end;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }
    if (address < FREERTOS_FLASH_OTA_BASE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    off = address - FREERTOS_FLASH_OTA_BASE;
    end = off + length;
    if (end > STUB_FLASH_SIZE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    (void)memcpy(&s_stub_flash[off], data, (size_t)length);
    return UDS_STATUS_OK;
}

static uds_status_t h7_flash_verify(uint32_t address,
                                     uint32_t size_bytes,
                                     uint32_t expected_crc)
{
    uint8_t  buf[128U];
    uint32_t crc       = (uint32_t)0xFFFFFFFFUL;
    uint32_t off;
    uint32_t remaining = size_bytes;
    uint32_t offset    = (uint32_t)0U;
    uint32_t chunk;

    if (address < FREERTOS_FLASH_OTA_BASE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    off = address - FREERTOS_FLASH_OTA_BASE;
    if ((off + size_bytes) > STUB_FLASH_SIZE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    while (remaining > (uint32_t)0U) {
        chunk = (remaining < (uint32_t)sizeof(buf)) ? remaining
                                                     : (uint32_t)sizeof(buf);
        (void)memcpy(buf, &s_stub_flash[off + offset], (size_t)chunk);
        crc       = uds_transfer_crc32_update(crc, buf, chunk);
        offset   += chunk;
        remaining -= chunk;
    }

    crc = uds_transfer_crc32_finalise(crc);

    return (crc == expected_crc) ? UDS_STATUS_OK : UDS_STATUS_ERR_GENERIC;
}

#endif /* STM32H7xx */

/* =============================================================================
 * Flash ops table + public init
 * ============================================================================= */

static const uds_flash_region_t s_ota_region = {
    .base_address = FREERTOS_FLASH_OTA_BASE,
    .size_bytes   = FREERTOS_FLASH_OTA_SIZE,
    .writable     = true,
    .readable     = true,
};

static uds_flash_ops_t s_freertos_flash_ops = {
    .erase_cb         = h7_flash_erase,
    .write_cb         = h7_flash_write,
    .verify_cb        = h7_flash_verify,
    .memory_map       = &s_ota_region,
    .region_count     = (uint8_t)1U,
    .max_block_length = (uint16_t)256U,
};

uds_status_t freertos_flash_ops_init(void)
{
    return uds_flash_ops_register(&s_freertos_flash_ops);
}
