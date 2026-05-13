// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: transport/doip/zephyr_lwip.h
 *
 * PURPOSE: Zephyr platform binding for the DoIP TCP transport.
 *
 *          Implements eds_doip_platform_ops_t using the Zephyr BSD-socket
 *          API (zsock_*) backed by the native IP stack (LwIP or native_sim
 *          loopback). Platform ops are registered by calling
 *          zephyr_doip_platform_init() before eds_doip_server_run().
 *
 *          The DoIP server thread is defined and started here via
 *          K_THREAD_DEFINE — the application does not need to create it.
 *
 * USAGE (from platform/zephyr/platform_doip.c or application main):
 *
 *   // 1. Call after eds_platform_init() and uds_generated_init():
 *   zephyr_doip_platform_init(0xE400U, DOIP_PORT, &g_uds_server_ctx);
 *
 *   // 2. That's it — the DoIP thread runs automatically.
 *
 * REQUIRED Kconfig (add to boards/native_sim.conf or prj.conf):
 *   CONFIG_NETWORKING=y
 *   CONFIG_NET_IPV4=y
 *   CONFIG_NET_TCP=y
 *   CONFIG_NET_SOCKETS=y
 *   CONFIG_NET_SOCKETS_POSIX_NAMES=y
 *   CONFIG_NET_IF_MAX_IPV4_COUNT=1
 *   CONFIG_NET_MAX_CONN=8
 *   CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE=4096
 *   CONFIG_POSIX_MAX_FDS=16
 *   CONFIG_NET_LOOPBACK=y          # for native_sim
 *   CONFIG_NET_NATIVE=y            # for native_sim
 *
 * SAFETY  : ASIL-B candidate. Platform binding only — all safety logic is
 *           in doip_server.c (transport-agnostic).
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef ZEPHYR_DOIP_LWIP_H
#define ZEPHYR_DOIP_LWIP_H

#include "doip_server.h"
#include "uds_server.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the Zephyr DoIP platform binding.
 *
 * Registers the Zephyr BSD-socket platform ops with the DoIP server
 * (via eds_doip_register_platform()) and stores the ECU logical address,
 * TCP port, and UDS server context for use by the DoIP thread.
 *
 * Must be called from the application after:
 *   1. eds_platform_init()
 *   2. uds_generated_init()
 *
 * The DoIP server thread (K_THREAD_DEFINE) starts automatically once
 * this function returns. No explicit thread-start call is needed.
 *
 * @param[in] logical_address  This ECU's DoIP logical address (e.g. 0xE400).
 * @param[in] port             TCP port to listen on (pass DOIP_PORT = 13400).
 * @param[in] uds_ctx          Pointer to the initialised UDS server context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if uds_ctx is NULL.
 */
uds_status_t zephyr_doip_platform_init(uint16_t          logical_address,
                                        uint16_t          port,
                                        uds_server_ctx_t *uds_ctx);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DOIP_LWIP_H */
