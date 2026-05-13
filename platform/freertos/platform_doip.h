// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/platform_doip.h
 *
 * PURPOSE: Platform-neutral DoIP startup interface for FreeRTOS applications.
 *
 *          Application main.c includes this header and calls
 *          eds_doip_platform_start_freertos() after uds_generated_init()
 *          and before vTaskStartScheduler(). The implementation delegates
 *          to transport/doip/freertos_lwip.c.
 *
 * SAFETY  : Platform binding only. ASIL-B candidate.
 * =============================================================================
 */

#ifndef FREERTOS_PLATFORM_DOIP_H
#define FREERTOS_PLATFORM_DOIP_H

#include "uds_types.h"
#include "uds_server.h"
#include "doip_server.h"   /* for DOIP_PORT */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the DoIP server on this FreeRTOS ECU.
 *
 * Registers the LwIP BSD-socket platform ops, initialises the DoIP
 * server state for the given logical address, and creates the DoIP
 * server FreeRTOS task. The task starts once vTaskStartScheduler()
 * is called.
 *
 * @param[in] logical_address  This ECU's DoIP logical address.
 * @param[in] port             TCP port to listen on (DOIP_PORT = 13400).
 * @param[in] uds_ctx          Initialised UDS server context.
 * @param[in] task_stack_bytes Stack size in bytes (0 = use default 4096).
 * @param[in] task_priority    FreeRTOS task priority (0 = use default 6).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if uds_ctx is NULL.
 * @return UDS_STATUS_ERR_PLATFORM if task creation fails.
 */
uds_status_t eds_doip_platform_start_freertos(uint16_t          logical_address,
                                               uint16_t          port,
                                               uds_server_ctx_t *uds_ctx,
                                               uint32_t          task_stack_bytes,
                                               uint32_t          task_priority);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_PLATFORM_DOIP_H */
