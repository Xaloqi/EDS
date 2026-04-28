// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_can.h
 *
 * PURPOSE: Zephyr CAN backend — public initialisation interface.
 *
 *          Declares zephyr_can_platform_init(), the only function that
 *          application code (main.c) needs to call directly.
 *
 * [P2-3] Physical addressing support added.
 *        zephyr_can_platform_init() now accepts physical_rx_id (0 = disabled).
 *        When non-zero, a second can_add_rx_filter_msgq() call installs a
 *        point-to-point RX filter alongside the 0x7DF functional broadcast
 *        filter.
 *
 * SAFETY  : ASIL-B candidate.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#ifndef ZEPHYR_CAN_H
#define ZEPHYR_CAN_H

#include <stdint.h>
#include <zephyr/device.h>
#include "can_transport.h"
#include "uds_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the Zephyr CAN backend.
 *
 * Installs one or two RX filters:
 *   Filter 1 (always):   0x7DF  — ISO 15765-4 functional broadcast.
 *   Filter 2 (optional): physical_rx_id — point-to-point ECU address.
 *
 * @param[out] out_transport  Receives initialised can_transport_t pointer.
 * @param[in]  can_dev        Zephyr CAN device (DEVICE_DT_GET).
 * @param[in]  physical_rx_id Physical RX CAN ID. Pass 0U to disable.
 *                            Typical: 0x7E0–0x7E7 (ISO 15765-4).
 *
 * @return UDS_STATUS_OK on success.
 * @return UDS_STATUS_ERR_NULL_PTR if any required pointer is NULL.
 * @return UDS_STATUS_ERR_CAN_NOT_READY if CAN device not ready.
 * @return UDS_STATUS_ERR_PLATFORM if mode/start/functional-filter fails.
 *
 * @code
 *   // Functional only:
 *   zephyr_can_platform_init(&t, DEVICE_DT_GET(DT_ALIAS(can0)), 0U);
 *
 *   // Functional + physical 0x7E0:
 *   zephyr_can_platform_init(&t, DEVICE_DT_GET(DT_ALIAS(can0)), 0x7E0U);
 *
 *   // From generated config:
 *   zephyr_can_platform_init(&t, DEVICE_DT_GET(DT_ALIAS(can0)),
 *                             GEN_CAN_PHYSICAL_RX_ID);
 * @endcode
 */
uds_status_t zephyr_can_platform_init(
    can_transport_t    **out_transport,
    const struct device *can_dev,
    uint32_t             physical_rx_id);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_CAN_H */
