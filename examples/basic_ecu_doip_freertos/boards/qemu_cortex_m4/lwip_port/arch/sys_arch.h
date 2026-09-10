// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: .../boards/qemu_cortex_m4/lwip_port/arch/sys_arch.h
 *
 * PURPOSE: lwIP OS abstraction types, mapped onto FreeRTOS primitives.
 *
 *          lwIP's contrib tree ships a FreeRTOS port, but contrib is a
 *          separate repository from the lwIP release this build pins. Rather
 *          than pull a second dependency for ~250 lines, the port lives here
 *          alongside the driver it is used with.
 *
 * SAFETY  : Example port layer — not safety-assessed.
 * =============================================================================
 */

#ifndef EDS_LWIP_ARCH_SYS_ARCH_H
#define EDS_LWIP_ARCH_SYS_ARCH_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "queue.h"

/* lwIP checks against these sentinels, so an "empty" handle must compare
 * equal to NULL — which FreeRTOS handles do when uninitialised. */
#define SYS_MBOX_NULL   NULL
#define SYS_SEM_NULL    NULL

typedef SemaphoreHandle_t sys_sem_t;
typedef SemaphoreHandle_t sys_mutex_t;
typedef QueueHandle_t     sys_mbox_t;
typedef TaskHandle_t      sys_thread_t;

/* SYS_ARCH_PROTECT nesting level. Unused value on FreeRTOS (the critical
 * section itself nests), but lwIP requires the type to exist. */
typedef UBaseType_t sys_prot_t;

#endif /* EDS_LWIP_ARCH_SYS_ARCH_H */
