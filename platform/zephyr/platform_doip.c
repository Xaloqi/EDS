// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/platform_doip.c
 *
 * PURPOSE: Zephyr DoIP platform registration shim.
 *
 *          This file is the glue between the platform abstraction layer
 *          (platform/platform_api.h) and the DoIP transport binding
 *          (transport/doip/zephyr_lwip.c).  It re-exports
 *          zephyr_doip_platform_init() under the platform-neutral name
 *          eds_doip_platform_start() for use by application main.c.
 *
 *          CAN-only builds simply do not include this file in target_sources.
 *          The rest of the stack is completely unaffected — eds_doip_register_
 *          platform() is never called, so s_ops in doip_server.c remains NULL
 *          and eds_doip_server_run() returns UDS_STATUS_ERR_NOT_INITIALIZED
 *          (which is never called in a CAN-only build either).
 *
 * CALL SEQUENCE (DoIP-enabled Zephyr application):
 *
 *   // After eds_platform_init() and uds_generated_init():
 *   uds_server_ctx_t *srv = uds_generated_get_server();
 *   eds_doip_platform_start(0xE400U, DOIP_PORT, srv);
 *   // DoIP thread is now running automatically.
 *
 * SAFETY  : Platform binding only. ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "platform_doip.h"
#include "zephyr_lwip.h"   /* transport/doip/zephyr_lwip.h — included via -I path */

uds_status_t eds_doip_platform_start(uint16_t          logical_address,
                                      uint16_t          port,
                                      uds_server_ctx_t *uds_ctx)
{
    return zephyr_doip_platform_init(logical_address, port, uds_ctx);
}
