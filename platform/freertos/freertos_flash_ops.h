// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_flash_ops.h
 *
 * PURPOSE: STM32H743ZI dual-bank flash ops for UDS download services.
 *
 *          FreeRTOS counterpart of platform/zephyr/zephyr_flash_ops.h.
 *          Provides freertos_flash_ops_init() which registers the
 *          uds_flash_ops_t table so that services 0x34 / 0x36 / 0x37
 *          can accept firmware download requests.
 *
 * TARGET HARDWARE:
 *   STM32H743ZI — 2 MB dual-bank internal flash.
 *
 *   Flash layout (set in linker script / CMakeLists):
 *     Bank 1  0x08000000 – 0x080FFFFF  1 MB  running application
 *     Bank 2  0x08100000 – 0x081DFFFF  896 KB  OTA staging area
 *     NVM     0x081E0000 – 0x081FFFFF  128 KB  last sector (reserved)
 *
 *   The OTA staging area (Bank 2, excluding NVM sector) is the writable
 *   region accepted by the UDS download services.  The running application
 *   in Bank 1 is never written directly by the UDS stack.
 *
 * BACKEND SELECTION (compile-time):
 *   Defined STM32H7xx  → HAL_FLASH_* (real hardware, CubeIDE / Makefile)
 *   Otherwise          → RAM stub     (CI / QEMU — no flash hardware)
 *
 *   To enable the HAL backend in your project:
 *     #define STM32H7xx                 (or STM32H743xx)
 *     #include "stm32h7xx_hal.h"
 *   and add stm32h7xx_hal_flash.c + stm32h7xx_hal_flash_ex.c to your build.
 *
 * USAGE:
 *   Call freertos_flash_ops_init() from main() before uds_generated_init().
 *   The generated uds_init.c calls this automatically at Step 5.7 when
 *   safeboot.platform: freertos is set in diagnostics_config.yaml.
 *
 * SAFETY:
 *   REQ-FLASH-001: Register before any 0x34 request is accepted.
 *   REQ-FLASH-002: Address range validated to OTA staging area only.
 *   REQ-FLASH-003: CRC-32 verified by verify_cb after transfer exit.
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef FREERTOS_FLASH_OPS_H
#define FREERTOS_FLASH_OPS_H

#include "uds_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * STM32H743ZI flash layout constants
 *
 * These match the linker script (boards/nucleo_h743zi/linker.ld).
 * Adjust if your board uses a different partition scheme.
 * -------------------------------------------------------------------------- */

/** Base address of Bank 2 (OTA staging area start). */
#define FREERTOS_FLASH_OTA_BASE    ((uint32_t)0x08100000UL)

/** Size of OTA staging area: Bank 2 minus the NVM sector (896 KB). */
#define FREERTOS_FLASH_OTA_SIZE    ((uint32_t)(896U * 1024U))

/** Sector size on STM32H743ZI: 128 KB per sector. */
#define FREERTOS_FLASH_SECTOR_SIZE ((uint32_t)(128U * 1024U))

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialise FreeRTOS/STM32H743 flash ops and register with UDS.
 *
 * Populates the uds_flash_ops_t table for the OTA staging region (Bank 2)
 * and calls uds_flash_ops_register().
 *
 * Must be called once at startup before uds_generated_init().
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_PLATFORM if flash driver self-check fails.
 */
uds_status_t freertos_flash_ops_init(void);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_FLASH_OPS_H */
