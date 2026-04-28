// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu/src/main.c
 *
 * PURPOSE: BasicECU Zephyr application entry point.
 *
 *          The simplest possible EDS integration — 5 DIDs, 2 DTCs, 3 routines.
 *          Use this as a starting point for a new Zephyr ECU.
 *
 * TARGET:  native_sim (CI) and nucleo_h743zi (hardware).
 *          Portable to any Zephyr-supported board with CAN peripheral.
 *
 * THREAD MODEL:
 *   main()       — platform init → UDS init → launch diag_task → return 0
 *   diag_task    — 1 ms poll loop: CAN RX → ISO-TP → UDS dispatch → CAN TX
 *
 * DID HANDLERS:
 *   Stub implementations for all 5 DIDs. Each stub returns a deterministic
 *   value suitable for testing. Replace with real sensor reads before
 *   integration testing.
 *
 * BUILDING:
 *   west build -b native_sim examples/basic_ecu
 *   west build -b nucleo_h743zi examples/basic_ecu
 *
 * SAFETY   : ASIL-B candidate.
 * STANDARD : MISRA C:2012 alignment intended.
 * LICENSE  : Apache-2.0
 * =============================================================================
 */

/* --------------------------------------------------------------------------
 * Diagnostics stack headers
 * -------------------------------------------------------------------------- */
#include "uds_types.h"
#include "uds_server.h"
#include "uds_security_algo.h"
#include "isotp.h"
#include "can_transport.h"
#include "zephyr_port.h"
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"

/* --------------------------------------------------------------------------
 * Generated headers (from diagnostics_config.yaml via codegen.py)
 * -------------------------------------------------------------------------- */
#include "uds_init.h"
#include "generated_config.h"
#include "did_handlers.h"

/* --------------------------------------------------------------------------
 * Zephyr headers
 * -------------------------------------------------------------------------- */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(basic_ecu, LOG_LEVEL_INF);

/* =============================================================================
 * Configuration
 * ============================================================================= */

#define DIAG_CAN_DEV         DEVICE_DT_GET(DT_ALIAS(can0))
#define DIAG_RX_CAN_ID       ((uint32_t)GEN_CAN_RX_ID)   /* 0x7DF */
#define DIAG_TX_CAN_ID       ((uint32_t)GEN_CAN_TX_ID)   /* 0x7E8 */

#ifndef CONFIG_DIAG_TASK_STACK_SIZE
#define CONFIG_DIAG_TASK_STACK_SIZE   (4096U)
#endif

#ifndef CONFIG_DIAG_TASK_PRIORITY
#define CONFIG_DIAG_TASK_PRIORITY     (5)
#endif

/* =============================================================================
 * Application state — BasicECU
 *
 * Stub values for the 5 DIDs. Replace with real sensor/NVM reads in production.
 * =============================================================================*/

static const uint8_t s_vin[17]            = "BASICECUEDS00001";
static const uint8_t s_ecu_serial[4]      = { 0x01U, 0x00U, 0x00U, 0x01U };
static       uint8_t s_spare_part_num[11] = "EDS-BAS-001";
static uint16_t      s_engine_speed_rpm   = 800U;   /* idle RPM */
static int8_t        s_coolant_temp_degc  = 85;     /* normal operating temp */

/* =============================================================================
 * Static allocations
 * ============================================================================= */

K_THREAD_STACK_DEFINE(s_diag_stack, CONFIG_DIAG_TASK_STACK_SIZE);
static struct k_thread s_diag_thread;

static diag_mutex_t s_session_lock;
static diag_mutex_t s_security_lock;
static diag_timer_t s_tick_timer;
static diag_wdt_t   s_wdt;

/* =============================================================================
 * ISO-TP RX completion callback
 *
 * Called by isotp_process_rx_frame() when a complete UDS PDU is reassembled.
 * Holds the session lock for the duration of dispatch + response transmission.
 * ============================================================================= */

static void on_isotp_rx_complete(
    const uint8_t *data,
    uint16_t       length,
    void          *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();

    /* Static buffers — no stack allocation of uds_msg_buf_t. */
    static uds_msg_buf_t s_req;
    static uds_msg_buf_t s_resp;

    uint16_t i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) { return; }
    if ((length == 0U) || (length > (uint16_t)UDS_MAX_PAYLOAD_LEN)) { return; }

    (void)diag_mutex_lock(&s_session_lock);

    for (i = 0U; i < length; i++) {
        s_req.data[i] = data[i];
    }
    s_req.length  = length;
    s_resp.length = 0U;

    (void)uds_server_process_request(srv, &s_req, &s_resp);

    if (s_resp.length > 0U) {
        (void)isotp_transmit(tp, s_resp.data, s_resp.length);
    }

    (void)diag_mutex_unlock(&s_session_lock);
}

/* =============================================================================
 * 1 ms tick callback — drives ISO-TP and UDS session timers
 * ============================================================================= */

typedef struct { uds_server_ctx_t *srv; isotp_ctx_t *tp; } tick_ctx_t;

static void on_tick(void *arg)
{
    tick_ctx_t *ctx = (tick_ctx_t *)arg;
    if (ctx == NULL) { return; }
    (void)isotp_tick_1ms(ctx->tp);
    (void)uds_server_tick_1ms(ctx->srv);
}

/* =============================================================================
 * Diagnostics task
 * ============================================================================= */

static void diag_task_entry(void *p1, void *p2, void *p3)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)p1;
    can_transport_t  *can = (can_transport_t  *)p2;
    isotp_ctx_t      *tp  = (isotp_ctx_t      *)p3;

    uds_status_t    status;
    uds_can_frame_t rx_frame;
    bool            frame_ready;
    tick_ctx_t      tick_ctx = { .srv = srv, .tp = tp };

    LOG_INF("BasicECU diag task started");

    if (diag_timer_start(&s_tick_timer) != UDS_STATUS_OK) {
        LOG_ERR("Timer start failed."); return;
    }

    while (true) {
        (void)diag_timer_wait_tick(&s_tick_timer, 2U);

        frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);
        if ((status == UDS_STATUS_OK) && frame_ready) {
            (void)isotp_process_rx_frame(tp, &rx_frame,
                                          on_isotp_rx_complete, srv);
        }

        on_tick(&tick_ctx);
        (void)diag_wdt_feed(&s_wdt);
    }
}

/* =============================================================================
 * DID handler implementations (stubs — replace with real reads)
 * ============================================================================= */

uds_status_t did_read_VehicleIdentificationNumber(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)17U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    /* TODO [APPLICATION]: Read VIN from NVM. */
    for (uint16_t i = 0U; i < 17U; i++) { buf[i] = s_vin[i]; }
    *out_len = 17U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_ECUSerialNumber(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)4U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    /* TODO [APPLICATION]: Read ECU serial from NVM/one-time-programmable area. */
    for (uint16_t i = 0U; i < 4U; i++) { buf[i] = s_ecu_serial[i]; }
    *out_len = 4U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_VehicleManufacturerSparePartNumber(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)11U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    /* TODO [APPLICATION]: Read part number from NVM. */
    for (uint16_t i = 0U; i < 11U; i++) { buf[i] = (uint8_t)s_spare_part_num[i]; }
    *out_len = 11U;
    return UDS_STATUS_OK;
}

uds_status_t did_write_VehicleManufacturerSparePartNumber(
    const uint8_t *data, uint16_t length)
{
    if (data == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    if (length != (uint16_t)11U) { return UDS_STATUS_ERR_INVALID_PARAM; }
    /* TODO [APPLICATION]: Validate and write to NVM. */
    for (uint16_t i = 0U; i < 11U; i++) { s_spare_part_num[i] = data[i]; }
    return UDS_STATUS_OK;
}

uds_status_t did_read_EngineSpeed(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)2U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    /* TODO [APPLICATION]: Read engine speed from CAN signal or sensor. */
    buf[0] = (uint8_t)((s_engine_speed_rpm >> 8U) & 0xFFU);
    buf[1] = (uint8_t)(s_engine_speed_rpm & 0xFFU);
    *out_len = 2U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_CoolantTemperature(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)1U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    /* TODO [APPLICATION]: Read coolant temperature from ADC/sensor. */
    buf[0] = (uint8_t)((int16_t)s_coolant_temp_degc + 40);  /* offset encoding */
    *out_len = 1U;
    return UDS_STATUS_OK;
}

/* =============================================================================
 * main()
 * ============================================================================= */

int main(void)
{
    uds_status_t      status;
    can_transport_t  *can = NULL;
    uds_server_ctx_t *srv = NULL;
    isotp_ctx_t      *tp  = NULL;

    LOG_INF("Xaloqi EDS BasicECU starting");

    if (diag_mutex_init(&s_session_lock)  != UDS_STATUS_OK) { return -1; }
    if (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK) { return -1; }

    (void)diag_wdt_init(&s_wdt);

    status = diag_timer_init(&s_tick_timer, on_tick, NULL);
    if (status != UDS_STATUS_OK) { LOG_ERR("Timer init failed."); return -1; }

    {
        const zephyr_port_cfg_t port_cfg = {
            .can_dev = DIAG_CAN_DEV,
        };
        status = zephyr_port_init(&port_cfg, &can);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("Platform init failed: 0x%02X", (unsigned)status);
            return -1;
        }
    }

    /*
     * Security algorithm init — no TRNG on basic_ecu (CI / development build).
     * For production: call uds_security_algo_set_rng_cb() with a real TRNG
     * callback and uds_security_algo_set_level_key() with OEM keys from OTP.
     * See docs/SECURITY_NOTICE.md.
     */
    LOG_WRN("[SEC] Using placeholder AES keys — inject OEM keys before production.");
    (void)uds_security_algo_set_rng_cb(NULL);

    status = uds_generated_init(can, DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS stack init failed: 0x%02X", (unsigned)status);
        return -1;
    }

    srv = uds_generated_get_server();
    tp  = uds_generated_get_isotp();
    if ((srv == NULL) || (tp == NULL)) {
        LOG_ERR("Stack context NULL after init."); return -1;
    }

    LOG_INF("UDS stack ready: %u DIDs  %u DTCs  RX=0x%03X TX=0x%03X",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT,
            (unsigned)DIAG_RX_CAN_ID, (unsigned)DIAG_TX_CAN_ID);

    (void)k_thread_create(
        &s_diag_thread, s_diag_stack,
        K_THREAD_STACK_SIZEOF(s_diag_stack),
        diag_task_entry,
        (void *)srv, (void *)can, (void *)tp,
        CONFIG_DIAG_TASK_PRIORITY, 0U, K_NO_WAIT
    );
    k_thread_name_set(&s_diag_thread, "diag_task");

    return 0;
}
