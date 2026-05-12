// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/platform_doip.h
 *
 * PURPOSE: Platform-neutral DoIP startup interface for Zephyr applications.
 *
 *          Application main.c includes this header and calls
 *          eds_doip_platform_start() after uds_generated_init().
 *          The implementation is in platform_doip.c which delegates to
 *          transport/doip/zephyr_lwip.c.
 *
 * SAFETY  : Platform binding only. ASIL-B candidate.
 * =============================================================================
 */

#ifndef PLATFORM_DOIP_H
#define PLATFORM_DOIP_H

#include "uds_types.h"
#include "uds_server.h"
#include "doip_server.h"   /* for DOIP_PORT constant */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the DoIP server on this Zephyr ECU.
 *
 * Registers the Zephyr BSD-socket platform ops, initialises the DoIP
 * server state for the given logical address, and activates the
 * pre-defined DoIP server thread (K_THREAD_DEFINE in zephyr_lwip.c).
 *
 * @param[in] logical_address  This ECU's DoIP logical address.
 * @param[in] port             TCP port to listen on (DOIP_PORT = 13400).
 * @param[in] uds_ctx          Initialised UDS server context.
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if uds_ctx is NULL.
 */
uds_status_t eds_doip_platform_start(uint16_t          logical_address,
                                      uint16_t          port,
                                      uds_server_ctx_t *uds_ctx);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_DOIP_H */
