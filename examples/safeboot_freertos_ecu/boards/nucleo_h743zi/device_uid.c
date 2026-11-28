// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/safeboot_freertos_ecu/boards/nucleo_h743zi/device_uid.c
 *
 * PURPOSE: eds_platform_get_device_uid() (EDS#57) for the STM32H743: reads the
 *          96-bit factory UID. FreeRTOS has no hwinfo, so this board file
 *          supplies the reader; the CMake for this board defines
 *          EDS_PLATFORM_BOARD_DEVICE_UID so freertos_platform_api.c leaves
 *          the function to this file.
 *
 *          The UID is three words at UID_BASE (0x1FF1E800, RM0433 "Device
 *          electronic signature"). The bytes returned are the same as
 *          Zephyr's hwinfo_stm32 returns on the same chip: word 2, word 1,
 *          word 0, each big-endian, so a Zephyr and a FreeRTOS build of one
 *          board report the same 12 bytes. To compare with a debug probe:
 *            pyocd cmd -t stm32h743zitx -c "read32 0x1FF1E800 3"
 *          prints word0 word1 word2; the UID is word2 word1 word0 as hex.
 *
 *          No STM32 HAL is needed: the registers are read directly.
 *
 * SAFETY  : Not safety-relevant: the UID identifies the silicon.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "platform_api.h"
#include "eds_device_uid.h"

#include <stdint.h>

#define STM32H7_UID_BASE 0x1FF1E800UL

static int32_t stm32h7_uid_source(uint8_t *buf, size_t len)
{
    volatile const uint32_t *const uid = (volatile const uint32_t *)STM32H7_UID_BASE;
    size_t i;
    size_t n = 12U;

    if (len < n) {
        n = len;
    }
    for (i = 0U; i < n; i++) {
        /* word 2 first, word 0 last, each most-significant byte first */
        const uint32_t word = uid[2U - (i / 4U)];
        buf[i] = (uint8_t)(word >> (24U - (8U * (i % 4U))));
    }
    return (int32_t)n;
}

uds_status_t eds_platform_get_device_uid(uint8_t *buf, size_t len)
{
    return eds_device_uid_read(stm32h7_uid_source, buf, len);
}
