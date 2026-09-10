// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: .../boards/qemu_cortex_m4/lwip_port/sys_arch.c
 *
 * PURPOSE: lwIP OS abstraction layer implemented on FreeRTOS primitives.
 *
 *          Implements the NO_SYS=0 contract from lwIP's sys.h: mutexes,
 *          binary/counting semaphores, mailboxes, thread creation, a
 *          millisecond clock, and the SYS_ARCH_PROTECT critical section.
 *
 *          Timeout semantics follow lwIP's contract exactly:
 *            - timeout == 0 means "wait forever"
 *            - a timed-out wait returns SYS_ARCH_TIMEOUT
 *            - a successful wait returns elapsed milliseconds
 *          Getting the timeout==0 case backwards is the classic failure mode
 *          here: it turns every blocking lwIP wait into a busy spin.
 *
 * SAFETY  : Example port layer — not safety-assessed.
 * STANDARD: No recursion. Allocation is FreeRTOS-pool only (heap_4's static
 *           array), never libc malloc.
 * =============================================================================
 */

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "lwip/stats.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "board_uart.h"

#include <stddef.h>

/** Upper bound for counting semaphores handed to lwIP. lwIP never uses a
 *  semaphore as a deep counter — this only has to exceed the number of
 *  signals that can pile up before the waiter is scheduled. */
#define EDS_LWIP_SEM_MAX_COUNT  (16U)

/** Milliseconds per second, for tick <-> ms conversion. */
#define EDS_MS_PER_SEC          (1000ULL)

/* ---------------------------------------------------------------------------
 * Init / time
 * ------------------------------------------------------------------------ */

void sys_init(void)
{
    /* Nothing to do: FreeRTOS is already initialised by the time lwIP starts
     * (eds_net.c runs tcpip_init() from a task, after the scheduler). */
}

u32_t sys_now(void)
{
    return (u32_t)(((uint64_t)xTaskGetTickCount() * 1000ULL) /
                   (uint64_t)configTICK_RATE_HZ);
}

u32_t sys_jiffies(void)
{
    return (u32_t)xTaskGetTickCount();
}

/* ---------------------------------------------------------------------------
 * Critical section (SYS_LIGHTWEIGHT_PROT)
 * ------------------------------------------------------------------------ */

sys_prot_t sys_arch_protect(void)
{
    taskENTER_CRITICAL();
    return (sys_prot_t)1;
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
    taskEXIT_CRITICAL();
}

/* ---------------------------------------------------------------------------
 * Mutexes
 * ------------------------------------------------------------------------ */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    if (mutex == NULL) {
        return ERR_ARG;
    }
    *mutex = xSemaphoreCreateMutex();
    if (*mutex == NULL) {
        return ERR_MEM;
    }
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    (void)xSemaphoreTake(*mutex, portMAX_DELAY);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    (void)xSemaphoreGive(*mutex);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    vSemaphoreDelete(*mutex);
    *mutex = NULL;
}

int sys_mutex_valid(sys_mutex_t *mutex)
{
    return ((mutex != NULL) && (*mutex != NULL)) ? 1 : 0;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
    if (mutex != NULL) {
        *mutex = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Semaphores
 * ------------------------------------------------------------------------ */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    if (sem == NULL) {
        return ERR_ARG;
    }

    /* Counting, not binary: lwIP's sem usage relies on a signal given before
     * a matching wait being remembered. xSemaphoreCreateCounting takes
     * (maxCount, initialCount) in that order — passing the initial count as
     * the maximum caps the semaphore at 0 and deadlocks every waiter. */
    *sem = xSemaphoreCreateCounting((UBaseType_t)EDS_LWIP_SEM_MAX_COUNT,
                                    (UBaseType_t)count);
    if (*sem == NULL) {
        return ERR_MEM;
    }
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
    (void)xSemaphoreGive(*sem);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    TickType_t started = xTaskGetTickCount();

    if (timeout == 0U) {
        /* lwIP contract: 0 means block indefinitely. */
        (void)xSemaphoreTake(*sem, portMAX_DELAY);
    } else {
        if (xSemaphoreTake(*sem, pdMS_TO_TICKS(timeout)) != pdTRUE) {
            return SYS_ARCH_TIMEOUT;
        }
    }

    return (u32_t)(((uint64_t)(xTaskGetTickCount() - started) * 1000ULL) /
                   (uint64_t)configTICK_RATE_HZ);
}

void sys_sem_free(sys_sem_t *sem)
{
    vSemaphoreDelete(*sem);
    *sem = NULL;
}

int sys_sem_valid(sys_sem_t *sem)
{
    return ((sem != NULL) && (*sem != NULL)) ? 1 : 0;
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
    if (sem != NULL) {
        *sem = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Mailboxes
 * ------------------------------------------------------------------------ */

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    if (mbox == NULL) {
        return ERR_ARG;
    }
    *mbox = xQueueCreate((UBaseType_t)size, (UBaseType_t)sizeof(void *));
    if (*mbox == NULL) {
        return ERR_MEM;
    }
    return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    (void)xQueueSendToBack(*mbox, &msg, portMAX_DELAY);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if (xQueueSendToBack(*mbox, &msg, (TickType_t)0) != pdTRUE) {
        return ERR_MEM;
    }
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    BaseType_t higher_woken = pdFALSE;

    if (xQueueSendToBackFromISR(*mbox, &msg, &higher_woken) != pdTRUE) {
        return ERR_MEM;
    }

    /* lwIP 2.2.1 has no ERR_NEED_SCHED, so the "a higher-priority task was
     * woken" signal cannot be returned to the caller — request the context
     * switch here instead. Unused in practice: this driver is polled and
     * never posts from an ISR (see ethernetif_lan9118.c). */
    portYIELD_FROM_ISR(higher_woken);
    return ERR_OK;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    TickType_t started = xTaskGetTickCount();
    void      *received = NULL;

    if (timeout == 0U) {
        (void)xQueueReceive(*mbox, &received, portMAX_DELAY);
    } else {
        if (xQueueReceive(*mbox, &received, pdMS_TO_TICKS(timeout)) != pdTRUE) {
            if (msg != NULL) {
                *msg = NULL;
            }
            return SYS_ARCH_TIMEOUT;
        }
    }

    if (msg != NULL) {
        *msg = received;
    }

    return (u32_t)(((uint64_t)(xTaskGetTickCount() - started) * 1000ULL) /
                   (uint64_t)configTICK_RATE_HZ);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    void *received = NULL;

    if (xQueueReceive(*mbox, &received, (TickType_t)0) != pdTRUE) {
        if (msg != NULL) {
            *msg = NULL;
        }
        return SYS_MBOX_EMPTY;
    }

    if (msg != NULL) {
        *msg = received;
    }
    return 0U;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    vQueueDelete(*mbox);
    *mbox = NULL;
}

int sys_mbox_valid(sys_mbox_t *mbox)
{
    return ((mbox != NULL) && (*mbox != NULL)) ? 1 : 0;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
    if (mbox != NULL) {
        *mbox = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------------ */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread,
                            void *arg, int stacksize, int prio)
{
    TaskHandle_t handle = NULL;

    /* lwIP passes stacksize in bytes; FreeRTOS wants words. */
    BaseType_t rc = xTaskCreate(
        thread,
        name,
        (configSTACK_DEPTH_TYPE)((uint32_t)stacksize / sizeof(StackType_t)),
        arg,
        (UBaseType_t)prio,
        &handle);

    if (rc != pdPASS) {
        return NULL;
    }
    return handle;
}

/* ---------------------------------------------------------------------------
 * Diagnostics hooks referenced by arch/cc.h
 * ------------------------------------------------------------------------ */

void eds_lwip_platform_diag(const char *msg)
{
    board_uart_puts("[lwip] ");
    board_uart_puts(msg);
    board_uart_puts("\r\n");
}

void eds_lwip_platform_assert(const char *msg, const char *file, int line)
{
    (void)file;
    board_uart_puts("[lwip] ASSERT: ");
    board_uart_puts(msg);
    board_uart_puts(" line ");
    board_uart_put_u32((uint32_t)line);
    board_uart_puts("\r\n");

    taskDISABLE_INTERRUPTS();
    for (;;) {
        /* Halt: the stack is in an undefined state. */
    }
}
