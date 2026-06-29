/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/harness_flash_mock.c
 *
 * PURPOSE: RAM-backed flash operations mock for the integration test harness.
 *
 * Provides a concrete uds_flash_ops_t implementation backed by a static
 * RAM buffer.  Follows the same pattern as nvm_store_mock.c.
 *
 * Compiled when HARNESS_FLASH_MOCK=1 is defined (set by build_harness.sh).
 *
 * DESIGN:
 *   - Single 4 KB RAM buffer covers the entire "flash" region.
 *   - flash_mock_erase()  — fills region with 0xFF (erased state).
 *   - flash_mock_write()  — copies data to the buffer at the given offset.
 *   - flash_mock_verify() — computes CRC-32 over written data and compares.
 *   - flash_mock_reset()  — clears the buffer and state flags (for test setup).
 *
 * MEMORY MAP (single writable region):
 *   Base address : 0x08020000  (typical MCUboot secondary slot for STM32H7)
 *   Size         : HARNESS_FLASH_MOCK_SIZE (4096 by default)
 *
 * The base address matches the nucleo_h743zi board configuration and is used
 * in all harness test sequences for consistency with the Zephyr port.
 *
 * SAFETY: Test-only. NOT for production firmware.
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#include "harness_flash_mock.h"
#include "uds_flash_ops.h"
#include "uds_transfer_ctx.h"   /* for CRC helper */
#include "uds_types.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

/** Base address reported to the flash ops memory map. */
#define FLASH_MOCK_BASE_ADDR  (0x08020000UL)

/** Total size of the mock flash region in bytes. */
#define FLASH_MOCK_SIZE       (HARNESS_FLASH_MOCK_SIZE)

/** Content of an erased byte (matches real NOR flash). */
#define FLASH_MOCK_ERASED     (0xFFU)

/* --------------------------------------------------------------------------
 * Static state
 * -------------------------------------------------------------------------- */

static uint8_t  s_flash_buf[FLASH_MOCK_SIZE];
static bool     s_erased   = false;
static uint32_t s_write_cursor = (uint32_t)0U;   /* tracks last written end */

/* --------------------------------------------------------------------------
 * Registered memory map (single writable region)
 * -------------------------------------------------------------------------- */

static const uds_flash_region_t k_mock_regions[1U] = {
    {
        .base_address = FLASH_MOCK_BASE_ADDR,
        .size_bytes   = (uint32_t)FLASH_MOCK_SIZE,
        .writable     = true,
        .readable     = true,
    }
};

/* --------------------------------------------------------------------------
 * Callback implementations
 * -------------------------------------------------------------------------- */

static uds_status_t flash_mock_erase(uint32_t address, uint32_t size_bytes)
{
    uint32_t offset;

    if (address < FLASH_MOCK_BASE_ADDR) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    offset = (uint32_t)(address - (uint32_t)FLASH_MOCK_BASE_ADDR);

    if ((offset + size_bytes) > (uint32_t)FLASH_MOCK_SIZE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    (void)memset(&s_flash_buf[offset], (int)FLASH_MOCK_ERASED, (size_t)size_bytes);
    s_erased       = true;
    s_write_cursor = (uint32_t)0U;

    return UDS_STATUS_OK;
}

static uds_status_t flash_mock_write(uint32_t       address,
                                      const uint8_t *data,
                                      uint32_t       length)
{
    uint32_t offset;

    if (data == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (address < FLASH_MOCK_BASE_ADDR) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    offset = (uint32_t)(address - (uint32_t)FLASH_MOCK_BASE_ADDR);

    if ((offset + length) > (uint32_t)FLASH_MOCK_SIZE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    (void)memcpy(&s_flash_buf[offset], data, (size_t)length);

    if ((offset + length) > s_write_cursor) {
        s_write_cursor = offset + length;
    }

    return UDS_STATUS_OK;
}

static uds_status_t flash_mock_verify(uint32_t address,
                                       uint32_t size_bytes,
                                       uint32_t expected_crc)
{
    uint32_t offset;
    uint32_t computed;

    if (address < FLASH_MOCK_BASE_ADDR) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    offset = (uint32_t)(address - (uint32_t)FLASH_MOCK_BASE_ADDR);

    if ((offset + size_bytes) > (uint32_t)FLASH_MOCK_SIZE) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    /* Compute CRC-32 over the stored flash content. */
    computed = uds_transfer_crc32_update(
        (uint32_t)0xFFFFFFFFUL,
        &s_flash_buf[offset],
        size_bytes);
    computed = uds_transfer_crc32_finalise(computed);

    if (computed != expected_crc) {
        return UDS_STATUS_ERR_GENERIC;
    }

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Registered flash ops table
 * -------------------------------------------------------------------------- */

static const uds_flash_ops_t k_mock_flash_ops = {
    .erase_cb        = flash_mock_erase,
    .write_cb        = flash_mock_write,
    .verify_cb       = flash_mock_verify,
    .memory_map      = k_mock_regions,
    .region_count    = (uint8_t)1U,
    .max_block_length = (uint16_t)HARNESS_FLASH_MOCK_BLOCK_LEN,
};

/* --------------------------------------------------------------------------
 * Public harness API
 * -------------------------------------------------------------------------- */

void harness_flash_mock_register(void)
{
    /* Reset state and register the ops table. */
    (void)memset(s_flash_buf, (int)FLASH_MOCK_ERASED, sizeof(s_flash_buf));
    s_erased       = false;
    s_write_cursor = (uint32_t)0U;

    (void)uds_flash_ops_register(&k_mock_flash_ops);
}

void harness_flash_mock_reset(void)
{
    (void)memset(s_flash_buf, (int)FLASH_MOCK_ERASED, sizeof(s_flash_buf));
    s_erased       = false;
    s_write_cursor = (uint32_t)0U;
    /* De-register to ensure clean state between tests that need it. */
    (void)uds_flash_ops_register(NULL);
}

const uint8_t *harness_flash_mock_buf(void)
{
    return s_flash_buf;
}

uint32_t harness_flash_mock_base(void)
{
    return (uint32_t)FLASH_MOCK_BASE_ADDR;
}

uint32_t harness_flash_mock_size(void)
{
    return (uint32_t)FLASH_MOCK_SIZE;
}

bool harness_flash_mock_was_erased(void)
{
    return s_erased;
}
