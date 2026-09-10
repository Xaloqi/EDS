// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/common/boards/qemu_cortex_m4/freertos_hooks.c
 *
 * PURPOSE: Application-supplied FreeRTOS static-allocation callbacks for the
 *          QEMU mps2-an386 board support shared by the FreeRTOS examples.
 *
 *          FreeRTOSConfig.h sets configSUPPORT_STATIC_ALLOCATION = 1, which
 *          makes vTaskStartScheduler() call vApplicationGetIdleTaskMemory()
 *          to obtain the idle task's TCB and stack. The application must
 *          provide it; the kernel deliberately does not.
 *
 *          Nothing in these examples defined it before, because none of them
 *          had ever linked a non-empty image (see startup.c for the
 *          --gc-sections root cause). The undefined reference only surfaces
 *          once the vector table gives the linker something to keep.
 *
 *          All three qemu_cortex_m4 boards set configUSE_TIMERS = 0, so
 *          vApplicationGetTimerTaskMemory() is not required and is
 *          intentionally absent. They also set
 *          configCHECK_FOR_STACK_OVERFLOW = 0 and
 *          configUSE_MALLOC_FAILED_HOOK = 0, so those two hooks are likewise
 *          not required here — the nucleo_h743zi board enables both and
 *          supplies its own.
 *
 * SAFETY  : Example board support — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. Static storage only — no malloc.
 * =============================================================================
 */

#include "FreeRTOS.h"
#include "task.h"

#if (configSUPPORT_STATIC_ALLOCATION == 1)

/* Statically allocated idle-task storage. Deliberately file-scope static
 * rather than heap: the no-malloc rule applies, and these must outlive
 * every caller for the lifetime of the scheduler. */
static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t  **ppxIdleTaskStackBuffer,
                                   uint32_t      *pulIdleTaskStackSize);

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t  **ppxIdleTaskStackBuffer,
                                   uint32_t      *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer   = &s_idle_tcb;
    *ppxIdleTaskStackBuffer = s_idle_stack;
    *pulIdleTaskStackSize   = (uint32_t)configMINIMAL_STACK_SIZE;
}

#endif /* configSUPPORT_STATIC_ALLOCATION */
