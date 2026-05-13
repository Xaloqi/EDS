// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/platform_doip.c
 *
 * PURPOSE: FreeRTOS DoIP platform registration shim.
 *
 *          Thin wrapper that exposes eds_doip_platform_start_freertos()
 *          for use by application main.c, delegating to
 *          freertos_doip_platform_init() in transport/doip/freertos_lwip.c.
 *
 *          CAN-only FreeRTOS builds omit this file from CMakeLists.txt.
 *          No impact on existing FreeRTOS CAN builds.
 *
 * SAFETY  : Platform binding only. ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "platform_doip.h"
#include "freertos_lwip.h"

uds_status_t eds_doip_platform_start_freertos(uint16_t          logical_address,
                                               uint16_t          port,
                                               uds_server_ctx_t *uds_ctx,
                                               uint32_t          task_stack_bytes,
                                               uint32_t          task_priority)
{
    const freertos_doip_cfg_t cfg = {
        .logical_address = logical_address,
        .port            = port,
        .uds_ctx         = uds_ctx,
        .task_stack_size = task_stack_bytes,
        .task_priority   = (UBaseType_t)task_priority,
    };
    return freertos_doip_platform_init(&cfg);
}
