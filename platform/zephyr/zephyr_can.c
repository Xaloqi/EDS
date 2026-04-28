// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_can.c
 *
 * PURPOSE: Zephyr RTOS CAN driver adapter — full Phase 6A implementation.
 *
 *          Implements the can_transport_ops_t interface against the Zephyr
 *          CAN controller API (zephyr/drivers/can.h).
 *
 *          Architecture:
 *
 *            CAN ISR (Zephyr driver)
 *              │  rx_callback()   — matches our diagnostic RX filter
 *              ▼
 *            k_msgq  s_can_rx_msgq   (8 frames, ISR-safe ring buffer)
 *              │
 *              ▼
 *            zephyr_can_receive()  <- poll loop calls can_transport_receive()
 *              │  k_msgq_get(K_NO_WAIT)
 *              ▼
 *            uds_can_frame_t  ->  isotp_process_rx_frame()
 *
 *          TX path:
 *            isotp_transmit() -> can_transport_transmit() -> can_send()
 *
 *          Bus-off path:
 *            state_change_cb() -> set s_bus_off flag (irq_lock protected)
 *            can_transport_get_status() -> reads s_bus_off
 *
 * SAFETY  : ASIL-B candidate. CAN errors propagated faithfully.
 *           s_bus_off written atomically via irq_lock/unlock.
 * STANDARD: MISRA C:2012 alignment intended.
 *
 * [NEW-H1 FIX] zephyr_can.c previously included "zephyr_port.h" as its first
 * header. Both transport/zephyr_port.h and platform/zephyr_port.h carry a
 * stale 2-parameter forward declaration of zephyr_can_platform_init(), while
 * the definition below has 3 parameters (physical_rx_id added in P2-3).
 * GCC C99/C11 §6.7.6.3 treats this as a conflicting-types error and aborts
 * compilation of this translation unit.
 *
 * Fix: include "zephyr_can.h" first. zephyr_can.h carries the authoritative
 * 3-parameter declaration that matches the definition below exactly.
 * "zephyr_port.h" is retained for zephyr_port_cfg_t and other platform types
 * but is now included after the owning header, so the conflicting forward
 * declaration is suppressed by the include guard ZEPHYR_PLATFORM_PORT_H
 * (renamed from ZEPHYR_PORT_H to eliminate the guard collision between the
 * two zephyr_port.h files — see fix notes in both zephyr_port.h files).
 * =============================================================================
 */

#include "zephyr_can.h"    /* [NEW-H1 FIX] Own header first — carries authoritative
                             * 3-arg declaration of zephyr_can_platform_init().
                             * This MUST precede zephyr_port.h so the correct
                             * prototype is seen before any stale forward decl. */
#include "zephyr_port.h"   /* Platform types: zephyr_port_cfg_t, timestamps, etc. */
#include "can_transport.h"
#include "uds_types.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <string.h>

/* [FIX] Was LOG_MODULE_DECLARE(basic_ecu, LOG_LEVEL_INF). That symbol is only
 * instantiated when basic_ecu/src/main.c is compiled.  BMS and MC ECU builds
 * include this file but not basic_ecu/src/main.c, so log_const_basic_ecu was
 * undefined at link time.  Each shared transport/platform file now owns its
 * own log module so it can be compiled into any ECU variant. */
LOG_MODULE_REGISTER(zephyr_can, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * RX message queue: 8 classic CAN frames, ISR-safe.
 * -------------------------------------------------------------------------- */
#define CAN_RX_MSGQ_DEPTH   (8U)

K_MSGQ_DEFINE(s_can_rx_msgq,
              sizeof(struct can_frame),
              CAN_RX_MSGQ_DEPTH,
              4U);

/* --------------------------------------------------------------------------
 * Bus-off flag: written from ISR, read from thread.
 * -------------------------------------------------------------------------- */
static volatile bool s_bus_off = false;

/* --------------------------------------------------------------------------
 * Platform context
 * -------------------------------------------------------------------------- */
typedef struct zephyr_can_platform {
    const struct device *can_dev;
    int                  rx_filter_id;
    int                  phys_filter_id;  /**< Physical address filter slot; -1 if unused. */
    bool                 initialized;
} zephyr_can_platform_t;

static zephyr_can_platform_t s_zephyr_can_platform;
static can_transport_t       s_zephyr_can_transport;

/* --------------------------------------------------------------------------
 * State change callback (ISR / workqueue context)
 * -------------------------------------------------------------------------- */
static void state_change_cb(const struct device   *dev,
                             enum can_state         state,
                             struct can_bus_err_cnt err_cnt,
                             void                  *user_data)
{
    unsigned int key;

    (void)dev;
    (void)err_cnt;
    (void)user_data;

    key = irq_lock();
    s_bus_off = (state == CAN_STATE_BUS_OFF);
    irq_unlock(key);

    if (state == CAN_STATE_BUS_OFF) {
        LOG_ERR("CAN: Bus-off state entered.");
    } else if (state == CAN_STATE_ERROR_PASSIVE) {
        LOG_WRN("CAN: Error-passive state (TEC or REC >= 128).");
    } else {
        /* Active or stopped — no action. */
    }
}

/* --------------------------------------------------------------------------
 * can_transport_ops_t — transmit
 * -------------------------------------------------------------------------- */
static uds_status_t zephyr_can_transmit(
    can_transport_t       *self,
    const uds_can_frame_t *frame)
{
    zephyr_can_platform_t *plat;
    struct can_frame       zf;
    int                    rc;

    if ((self == NULL) || (frame == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    plat = (zephyr_can_platform_t *)self->platform;

    if ((plat == NULL) || !plat->initialized || (plat->can_dev == NULL)) {
        return UDS_STATUS_ERR_CAN_NOT_READY;
    }

    if (s_bus_off) {
        return UDS_STATUS_ERR_CAN_BUS_OFF;
    }

    if (frame->dlc > (uint8_t)CAN_MAX_DLC) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    (void)memset(&zf, 0, sizeof(zf));
    zf.id    = frame->id;
    zf.dlc   = frame->dlc;
    zf.flags = (uint8_t)0U;  /* can_frame_flags_t removed in Zephyr 3.x; flags is uint8_t */
    (void)memcpy(zf.data, frame->data, (size_t)frame->dlc);

    /* Timeout = 25 ms = ISO 15765-2 N_As. */
    rc = can_send(plat->can_dev, &zf, K_MSEC(25), NULL, NULL);

    if (rc == -ENETDOWN) {
        return UDS_STATUS_ERR_CAN_BUS_OFF;
    }
    if (rc != 0) {
        LOG_WRN("CAN TX failed: %d (ID 0x%03X)", rc, (unsigned)frame->id);
        return UDS_STATUS_ERR_CAN_TX_FAILED;
    }

    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * can_transport_ops_t — receive (non-blocking poll)
 * -------------------------------------------------------------------------- */
static uds_status_t zephyr_can_receive(
    can_transport_t *self,
    uds_can_frame_t *out_frame,
    bool            *out_ready)
{
    zephyr_can_platform_t *plat;
    struct can_frame       zf;
    int                    rc;

    if ((self == NULL) || (out_frame == NULL) || (out_ready == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    plat = (zephyr_can_platform_t *)self->platform;

    if ((plat == NULL) || !plat->initialized) {
        *out_ready = false;
        return UDS_STATUS_ERR_CAN_NOT_READY;
    }

    *out_ready = false;

    rc = k_msgq_get(&s_can_rx_msgq, &zf, K_NO_WAIT);
    if (rc == -EAGAIN) {
        return UDS_STATUS_OK;   /* No frame available — normal idle. */
    }
    if (rc != 0) {
        return UDS_STATUS_ERR_CAN_RX_FAILED;
    }

    (void)memset(out_frame, 0, sizeof(*out_frame));
    out_frame->id  = zf.id;
    out_frame->dlc = (uint8_t)MIN(zf.dlc, (uint8_t)CAN_MAX_DLC);
    (void)memcpy(out_frame->data, zf.data, (size_t)out_frame->dlc);

    *out_ready = true;
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * can_transport_ops_t — get_status
 * -------------------------------------------------------------------------- */
static uds_status_t zephyr_can_get_status(
    can_transport_t *self,
    bool            *bus_off)
{
    zephyr_can_platform_t *plat;
    unsigned int           key;
    bool                   local;

    if ((self == NULL) || (bus_off == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    plat = (zephyr_can_platform_t *)self->platform;

    if ((plat == NULL) || !plat->initialized) {
        *bus_off = true;
        return UDS_STATUS_ERR_CAN_NOT_READY;
    }

    key   = irq_lock();
    local = s_bus_off;
    irq_unlock(key);

    *bus_off = local;
    return UDS_STATUS_OK;
}

/* --------------------------------------------------------------------------
 * Static vtable
 * -------------------------------------------------------------------------- */
static const can_transport_ops_t s_zephyr_can_ops = {
    .transmit   = zephyr_can_transmit,
    .receive    = zephyr_can_receive,
    .get_status = zephyr_can_get_status,
};

/* [P2-3] Physical addressing support.
 *
 * zephyr_can_platform_init() now accepts an optional physical_rx_id parameter.
 * When non-zero, a second RX filter is installed for that CAN ID alongside the
 * ISO 15765-4 functional broadcast address (0x7DF).
 *
 * Typical usage (basic_ecu/src/main.c):
 *
 *   // Functional only (0x7DF):
 *   zephyr_can_platform_init(&transport, can_dev, 0U);
 *
 *   // Functional + physical 0x7E0:
 *   zephyr_can_platform_init(&transport, can_dev, 0x7E0U);
 *
 * The physical_rx_id value comes from diagnostics_config.yaml can.physical_rx_id.
 *
 * [NEW-H1 FIX] The 3-parameter signature here must match the declaration in
 * zephyr_can.h exactly. The former forward declaration in zephyr_port.h (both
 * transport/ and platform/ copies) listed only 2 parameters — those stale
 * declarations are updated in this patch set.
 */
uds_status_t zephyr_can_platform_init(
    can_transport_t    **out_transport,
    const struct device *can_dev,
    uint32_t             physical_rx_id)
{
    struct can_filter  diag_filter;
    int                filter_id;
    int                phys_filter_id = -1;
    int                rc;

    if ((out_transport == NULL) || (can_dev == NULL)) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (!device_is_ready(can_dev)) {
        LOG_ERR("CAN device not ready.");
        return UDS_STATUS_ERR_CAN_NOT_READY;
    }

    /* Set to normal (non-loopback) operating mode. */
    rc = can_set_mode(can_dev, CAN_MODE_NORMAL);
    if (rc != 0) {
        LOG_ERR("CAN: can_set_mode(NORMAL) failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* Start the CAN controller. */
    rc = can_start(can_dev);
    if ((rc != 0) && (rc != -EALREADY)) {
        LOG_ERR("CAN: can_start() failed: %d", rc);
        return UDS_STATUS_ERR_PLATFORM;
    }

    /* Register state-change (bus-off) callback. */
    can_set_state_change_callback(can_dev, state_change_cb, NULL);

    /* ── Filter 1: ISO 15765-4 functional address 0x7DF ─────────────────────
     *
     * Functional addressing broadcasts to all ECUs simultaneously.
     * Required for: session control, tester present, global DTC clear.
     */
    (void)memset(&diag_filter, 0, sizeof(diag_filter));
    diag_filter.id    = (uint32_t)0x7DFU;
    diag_filter.mask  = (uint32_t)0x7FFU;   /* Exact match. */
    diag_filter.flags = (uint8_t)0U;         /* can_filter_flags_t removed in Zephyr 3.x */

    filter_id = can_add_rx_filter_msgq(can_dev, &s_can_rx_msgq, &diag_filter);
    if (filter_id < 0) {
        LOG_ERR("CAN: functional RX filter (0x7DF) failed: %d", filter_id);
        return UDS_STATUS_ERR_PLATFORM;
    }
    LOG_INF("CAN: functional filter installed (ID=0x7DF, slot %d).", filter_id);

    /* ── Filter 2: Physical address (optional) ───────────────────────────────
     *
     * Physical addressing enables point-to-point sessions between this ECU
     * and a single tester. Required for:
     *   - Concurrent multi-ECU diagnostic sessions (each ECU has a unique ID)
     *   - Firmware download (0x34/0x36/0x37): physical addressing mandatory
     *   - OEM tester tools that skip 0x7DF after initial session handshake
     *
     * ISO 15765-4 default physical RX IDs: 0x7E0..0x7E7.
     * Set physical_rx_id = 0 to disable (functional-only mode).
     */
    if (physical_rx_id != (uint32_t)0U) {
        (void)memset(&diag_filter, 0, sizeof(diag_filter));
        diag_filter.id    = physical_rx_id;
        diag_filter.mask  = (uint32_t)0x7FFU;   /* Exact match. */
        diag_filter.flags = (uint8_t)0U;

        phys_filter_id = can_add_rx_filter_msgq(
            can_dev, &s_can_rx_msgq, &diag_filter);
        if (phys_filter_id < 0) {
            /* Non-fatal: functional filter still active — log warning only. */
            LOG_WRN("CAN: physical filter (0x%03X) failed: %d — "
                    "functional (0x7DF) remains active.",
                    (unsigned)physical_rx_id, phys_filter_id);
            phys_filter_id = -1;
        } else {
            LOG_INF("CAN: physical filter installed (ID=0x%03X, slot %d).",
                    (unsigned)physical_rx_id, phys_filter_id);
        }
    } else {
        LOG_DBG("CAN: physical addressing disabled (physical_rx_id=0).");
    }

    /* Populate platform and transport contexts. */
    (void)memset(&s_zephyr_can_platform, 0, sizeof(s_zephyr_can_platform));
    s_zephyr_can_platform.can_dev        = can_dev;
    s_zephyr_can_platform.rx_filter_id   = filter_id;
    s_zephyr_can_platform.phys_filter_id = phys_filter_id;
    s_zephyr_can_platform.initialized    = true;
    s_bus_off = false;

    (void)memset(&s_zephyr_can_transport, 0, sizeof(s_zephyr_can_transport));
    s_zephyr_can_transport.ops      = &s_zephyr_can_ops;
    s_zephyr_can_transport.platform = &s_zephyr_can_platform;
    s_zephyr_can_transport.ready    = true;

    *out_transport = &s_zephyr_can_transport;

    LOG_INF("CAN: Transport initialized (device: %s).", can_dev->name);
    return UDS_STATUS_OK;
}
