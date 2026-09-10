// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/safeboot_freertos_ecu/boards/nucleo_h743zi/hal_timebase.c
 *
 * PURPOSE: Supply HAL_GetTick() to the STM32H7 flash HAL from the FreeRTOS
 *          tick.
 *
 *          The hardware build of this example
 *          (-DBOARD=nucleo_h743zi -DSTM32_HAL_DIR=...) compiles
 *          stm32h7xx_hal_flash.c and stm32h7xx_hal_flash_ex.c. Those call
 *          HAL_GetTick() from FLASH_WaitForLastOperation() to time out a
 *          stalled erase or program cycle. HAL_GetTick() lives in
 *          stm32h7xx_hal.c, which this example deliberately does NOT compile,
 *          so the link failed with:
 *
 *            stm32h7xx_hal_flash.c:(.text.FLASH_WaitForLastOperation+0x8):
 *              undefined reference to `HAL_GetTick'
 *
 *          That failure was invisible until now for the same reason as
 *          everything else in EDS issue #268: with no vector table,
 *          --gc-sections discarded FLASH_WaitForLastOperation along with the
 *          rest of the program before the reference had to be resolved.
 *
 * WHY NOT stm32h7xx_hal.c: pulling it in brings HAL_Init() and
 *          HAL_InitTick(), which program SysTick and own the millisecond
 *          timebase. FreeRTOS also owns SysTick — xPortSysTickHandler is in
 *          vector slot 15 (see startup.c). Two owners of one timer is a real
 *          bug, not a theoretical one. Backing HAL_GetTick() with the
 *          scheduler's own tick counter is the integration ST itself
 *          recommends for FreeRTOS targets, and it keeps a single timebase.
 *
 *          configTICK_RATE_HZ is 1000 in this board's FreeRTOSConfig.h, so
 *          one FreeRTOS tick is exactly one millisecond — the unit
 *          HAL_GetTick() is defined to return. A _Static_assert below keeps
 *          that true if anyone retunes the tick rate.
 *
 * BEFORE THE SCHEDULER RUNS: xTaskGetTickCount() returns 0 and does not
 *          advance, so a HAL flash timeout cannot expire. This example only
 *          touches flash from the UDS 0x34/0x36/0x37 download services, which
 *          run in the poll task long after vTaskStartScheduler(), so the
 *          window does not arise here. Integrators who erase flash during
 *          early boot must supply a pre-scheduler timebase instead.
 *
 * SAFETY  : Example board support — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. No malloc, no recursion.
 * =============================================================================
 */

#if defined(STM32H7xx) || defined(STM32H743xx)

#include "FreeRTOS.h"
#include "task.h"

#include <stdint.h>

_Static_assert(configTICK_RATE_HZ == 1000U,
               "HAL_GetTick() must return milliseconds: this timebase assumes "
               "configTICK_RATE_HZ == 1000. Retune it and the STM32 flash HAL "
               "timeouts silently change scale.");

uint32_t HAL_GetTick(void);

uint32_t HAL_GetTick(void)
{
    return (uint32_t)xTaskGetTickCount();
}

#endif /* STM32H7xx || STM32H743xx */
