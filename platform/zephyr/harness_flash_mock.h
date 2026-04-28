/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/harness_flash_mock.h
 *
 * PURPOSE: RAM-backed flash mock for the integration test harness.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 * =============================================================================
 */

#ifndef HARNESS_FLASH_MOCK_H
#define HARNESS_FLASH_MOCK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Total size of the mock flash region (4 KB — suitable for 512-byte test images). */
#define HARNESS_FLASH_MOCK_SIZE       (4096U)

/**
 * @brief Maximum block length advertised to the tester in the 0x34 response.
 *
 * Value of 256 bytes (payload) means maxNumberOfBlockLength = 257
 * (including the 1-byte block counter in the 0x36 request).
 * Chosen to exercise multi-block transfers within the 4 KB mock flash.
 */
#define HARNESS_FLASH_MOCK_BLOCK_LEN  (256U)

/**
 * @brief Register the mock flash ops table and reset the flash buffer.
 *
 * Must be called from harness_ecu_start() (or equivalent) before any
 * 0x34 request can be processed.
 */
void harness_flash_mock_register(void);

/**
 * @brief De-register and reset the flash mock (used between tests).
 *
 * Clears the flash buffer, resets state flags, and de-registers the
 * ops table (sets global ops pointer to NULL).
 */
void harness_flash_mock_reset(void);

/** @brief Return a pointer to the raw flash buffer (for test inspection). */
const uint8_t *harness_flash_mock_buf(void);

/** @brief Return the base address configured in the memory map. */
uint32_t harness_flash_mock_base(void);

/** @brief Return the size of the mock flash region in bytes. */
uint32_t harness_flash_mock_size(void);

/** @brief Return true if flash_mock_erase() has been called at least once. */
bool harness_flash_mock_was_erased(void);

#ifdef __cplusplus
}
#endif

#endif /* HARNESS_FLASH_MOCK_H */
