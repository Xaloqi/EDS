// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/safeboot_freertos_ecu/boards/nucleo_h743zi/freertos_hooks.c
 *
 * PURPOSE: Application-supplied FreeRTOS callbacks for the STM32H743ZI board.
 *
 *          This board needs three hooks, not the one the QEMU boards need,
 *          because its FreeRTOSConfig.h is stricter:
 *
 *            configSUPPORT_STATIC_ALLOCATION = 1
 *                -> vApplicationGetIdleTaskMemory(), called by
 *                   vTaskStartScheduler() to obtain the idle task's TCB and
 *                   stack. The kernel deliberately does not provide it.
 *
 *            configCHECK_FOR_STACK_OVERFLOW = 2
 *                -> vApplicationStackOverflowHook()
 *
 *            configUSE_MALLOC_FAILED_HOOK = 1
 *                -> vApplicationMallocFailedHook()
 *
 *          None of the three existed anywhere in the repository, because this
 *          example had never linked a non-empty image: with no vector table
 *          the linker discarded every caller before the references could be
 *          resolved (EDS issue #268, and startup.c in this directory).
 *
 *          configUSE_TIMERS is 0, so vApplicationGetTimerTaskMemory() is not
 *          required and is intentionally absent.
 *
 * FAILURE POLICY: both failure hooks halt with interrupts disabled rather than
 *          returning or resetting. A stack overflow or a failed allocation in
 *          a diagnostic ECU is a defect, not a runtime condition to paper
 *          over, and stopping at the point of detection is what leaves the
 *          state a debugger can read. A product integrating this example is
 *          expected to replace these with its own safe-state handling —
 *          typically logging to NVM and letting the watchdog reset the part.
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

#if (configCHECK_FOR_STACK_OVERFLOW > 0)

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;

    taskDISABLE_INTERRUPTS();
    for (;;) {
        /* Halt: the overflowing task's name is in pcTaskName for a debugger. */
    }
}

#endif /* configCHECK_FOR_STACK_OVERFLOW */

#if (configUSE_MALLOC_FAILED_HOOK == 1)

void vApplicationMallocFailedHook(void);

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;) {
        /* Halt: configTOTAL_HEAP_SIZE is exhausted. */
    }
}

#endif /* configUSE_MALLOC_FAILED_HOOK */
