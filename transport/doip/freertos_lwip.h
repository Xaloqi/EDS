// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/doip/freertos_lwip.h
 *
 * PURPOSE: FreeRTOS + LwIP platform binding for the DoIP TCP transport.
 *
 *          Implements eds_doip_platform_ops_t using LwIP BSD-socket API
 *          (lwip_socket, lwip_bind, lwip_listen, lwip_accept, lwip_send,
 *          lwip_recv, lwip_close). Works with any LwIP 2.x port on any MCU
 *          that has an Ethernet peripheral — STM32H7, NXP i.MX RT, etc.
 *
 *          The DoIP server task is created via xTaskCreate() inside
 *          freertos_doip_platform_init(). It starts after vTaskStartScheduler().
 *          Stack size and priority are configurable via the cfg struct or
 *          compile-time defaults.
 *
 * USAGE (from platform/freertos/platform_doip.c or application main.c):
 *
 *   // After eds_platform_init() and uds_generated_init():
 *   freertos_doip_platform_init(&(freertos_doip_cfg_t){
 *       .logical_address = 0xE400U,
 *       .port            = DOIP_PORT,
 *       .uds_ctx         = &g_uds_ctx,
 *       .task_stack_size = 4096U,
 *       .task_priority   = 6U,
 *   });
 *   // Then: vTaskStartScheduler();
 *
 * REQUIRED: LwIP configured with TCP support. The application must call
 *           lwip_init() and bring up the netif before the DoIP task starts.
 *
 * SAFETY  : Platform binding only. All safety logic in doip_server.c.
 *           ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef FREERTOS_DOIP_LWIP_H
#define FREERTOS_DOIP_LWIP_H

#include "doip_server.h"
#include "uds_server.h"
#include <stdint.h>

/* FreeRTOS headers — provided by the customer's FreeRTOS port */
#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration defaults (override via -D or FreeRTOSConfig.h)
 * ========================================================================== */

#ifndef FREERTOS_DOIP_TASK_STACK_WORDS
/** DoIP server task stack size in words (4 bytes each on Cortex-M). */
#define FREERTOS_DOIP_TASK_STACK_WORDS   (1024U)
#endif

#ifndef FREERTOS_DOIP_TASK_PRIORITY
/** DoIP server task priority (one below the UDS poll task at priority 5). */
#define FREERTOS_DOIP_TASK_PRIORITY      (6U)
#endif

/* ============================================================================
 * Configuration struct
 * ========================================================================== */

typedef struct freertos_doip_cfg {
    uint16_t          logical_address;  /**< This ECU's DoIP logical address. */
    uint16_t          port;             /**< TCP port (DOIP_PORT = 13400). */
    uds_server_ctx_t *uds_ctx;          /**< Initialised UDS server context. */
    uint32_t          task_stack_size;  /**< Stack in bytes (0 = use default). */
    UBaseType_t       task_priority;    /**< FreeRTOS task priority (0 = use default). */
} freertos_doip_cfg_t;

/* ============================================================================
 * Public API
 * ========================================================================== */

/**
 * @brief Initialise the FreeRTOS+LwIP DoIP platform binding and create the
 *        DoIP server task.
 *
 * Registers the LwIP BSD-socket platform ops with the DoIP server core
 * (via eds_doip_register_platform()) and creates a dedicated FreeRTOS task
 * that runs eds_doip_server_run(). The task starts once vTaskStartScheduler()
 * is called by the application.
 *
 * Must be called AFTER eds_platform_init() and uds_generated_init(),
 * and BEFORE vTaskStartScheduler().
 *
 * @param[in] cfg  Non-NULL pointer to configuration. uds_ctx must be non-NULL.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if cfg or cfg->uds_ctx is NULL.
 * @return UDS_STATUS_ERR_PLATFORM if xTaskCreate() fails.
 */
uds_status_t freertos_doip_platform_init(const freertos_doip_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_DOIP_LWIP_H */
