// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip/src/main.c
 *
 * PURPOSE: BasicECU_DoIP — identical DID/DTC set as basic_ecu, served over
 *          DoIP (ISO 13400-2) instead of ISO-TP CAN.
 *
 *          Demonstrates the minimum integration to add DoIP to an EDS ECU:
 *            1. Include zephyr_lwip.h / platform_doip.h
 *            2. Call eds_doip_platform_start() after uds_generated_init()
 *            3. Add Networking Kconfig and the doip: block to diagnostics_config.yaml
 *
 *          The CAN diagnostic task (diag_task) is NOT started here — this
 *          example is DoIP-only. For a production "both" transport ECU,
 *          add the CAN thread from basic_ecu alongside the DoIP init.
 *
 * THREAD MODEL:
 *   main()        — init → uds_generated_init → start tick + DoIP → return
 *   uds_tick_task — 1 ms poll loop: uds_server_tick_1ms() + uds_periodic_tick_1ms().
 *                   [EDS#191] A DoIP-only build has no CAN diag_task, so
 *                   without this thread the S3 session timeout and the
 *                   SecurityAccess lockout countdown never progress — a
 *                   session/unlock, once entered, never expires on its own.
 *   doip_thread   — started via a semaphore (zephyr_lwip.c), not a fixed
 *                   delay [EDS#192]; runs eds_doip_server_run()
 *
 * TARGET: native_sim (CI — loopback networking) and any Ethernet-capable
 *         Zephyr board (e.g. FRDM-K64F, STM32H7 with ETH).
 *
 * BUILDING:
 *   west build -b native_sim examples/basic_ecu_doip
 *
 * TESTING (once built):
 *   # In one terminal:
 *   ./build/zephyr/zephyr.exe
 *   # In another:
 *   python3 -c "
 *   import asyncio
 *   from xaloqi.tester import UdsTester, DoipBus, Session
 *   async def main():
 *       async with UdsTester(DoipBus('127.0.0.1'), rx_id=0xE400, tx_id=0x0E00) as ecu:
 *           vin = await ecu.read_did(0xF190)
 *           print('VIN:', vin)
 *   asyncio.run(main())
 *   "
 *
 * SAFETY   : ASIL-B candidate.
 * STANDARD : MISRA C:2012 alignment intended.
 * LICENSE  : Apache-2.0
 * =============================================================================
 */

/* --------------------------------------------------------------------------
 * EDS stack headers
 * -------------------------------------------------------------------------- */
#include "uds_types.h"
#include "uds_server.h"
#include "uds_security_algo.h"
#include "uds_periodic.h"
#include "uds_session.h"

/* --------------------------------------------------------------------------
 * DoIP platform headers
 * -------------------------------------------------------------------------- */
#include "platform_doip.h"   /* eds_doip_platform_start() */
#include "doip_server.h"     /* DOIP_PORT */
#include "nvm_store.h"

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
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(basic_ecu_doip, LOG_LEVEL_INF);

/* =============================================================================
 * DoIP ECU configuration
 *
 * logical_address: 0xE400 — standard xaloqi-tester default ECU address.
 * source_address:  0x0E00 — standard tester address (accepted during
 *                            routing activation in doip_server.c).
 *
 * These values must match the DoipBus constructor arguments in the
 * xaloqi-tester integration tests (conftest.py).
 * ============================================================================= */

#define DOIP_ECU_LOGICAL_ADDR   (0xE400U)

/* =============================================================================
 * UDS tick task configuration [EDS#191]
 *
 * Drives uds_server_tick_1ms() and uds_periodic_tick_1ms() every 1 ms —
 * the only thing progressing the S3 session timeout and SecurityAccess
 * lockout countdown in a DoIP-only build (no isotp_tick_1ms(): there is
 * no ISO-TP/CAN transport here). Priority matches basic_ecu's diag_task
 * (5) — higher priority than doip_thread's default (7,
 * zephyr_lwip.c) so UDS timer progression is never starved by DoIP
 * request handling, same rationale as the CAN example's own priority split.
 * ============================================================================= */

#ifndef CONFIG_DIAG_TICK_TASK_STACK_SIZE
#define CONFIG_DIAG_TICK_TASK_STACK_SIZE   (1024U)
#endif
#ifndef CONFIG_DIAG_TICK_TASK_PRIORITY
#define CONFIG_DIAG_TICK_TASK_PRIORITY     (5)
#endif

K_THREAD_STACK_DEFINE(s_tick_stack, CONFIG_DIAG_TICK_TASK_STACK_SIZE);
static struct k_thread s_tick_thread;

/* =============================================================================
 * Application state — same stub DIDs as basic_ecu
 * ============================================================================= */

static const uint8_t s_vin[17]            = "DOIPECUEDS00001";  /* 15 chars + \0 padded */
static const uint8_t s_ecu_serial[4]      = { 0x02U, 0x00U, 0x00U, 0x01U };
static       uint8_t s_spare_part_num[11] = "EDS-DIP-001";
static uint16_t      s_engine_speed_rpm   = 800U;
static int8_t        s_coolant_temp_degc  = 85;

/* =============================================================================
 * DID read/write handlers — identical contract to basic_ecu
 * ============================================================================= */

uds_status_t did_read_VehicleIdentificationNumber(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)17U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    for (uint16_t i = 0U; i < 17U; i++) { buf[i] = s_vin[i]; }
    *out_len = 17U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_ECUSerialNumber(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)4U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    for (uint16_t i = 0U; i < 4U; i++) { buf[i] = s_ecu_serial[i]; }
    *out_len = 4U;
    return UDS_STATUS_OK;
}

uds_status_t did_read_VehicleManufacturerSparePartNumber(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)11U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
    for (uint16_t i = 0U; i < 11U; i++) { buf[i] = s_spare_part_num[i]; }
    *out_len = 11U;
    return UDS_STATUS_OK;
}

uds_status_t did_write_VehicleManufacturerSparePartNumber(
    const uint8_t *data, uint16_t length)
{
    if (data == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    if (length != (uint16_t)11U) { return UDS_STATUS_ERR_INVALID_PARAM; }
    for (uint16_t i = 0U; i < 11U; i++) { s_spare_part_num[i] = data[i]; }
    return UDS_STATUS_OK;
}

uds_status_t did_read_EngineSpeed(
    uint8_t *buf, uint16_t buf_len, uint16_t *out_len)
{
    if ((buf == NULL) || (out_len == NULL)) { return UDS_STATUS_ERR_NULL_PTR; }
    if (buf_len < (uint16_t)2U) { return UDS_STATUS_ERR_BUFFER_OVERFLOW; }
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
    buf[0] = (uint8_t)((int16_t)s_coolant_temp_degc + 40);
    *out_len = 1U;
    return UDS_STATUS_OK;
}

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

/* =============================================================================
 * UDS tick task [EDS#191]
 * ============================================================================= */

static void uds_tick_task_entry(void *p1, void *p2, void *p3)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)p1;
    (void)p2;
    (void)p3;

    LOG_INF("UDS tick task started");

    while (true) {
        k_msleep(1);
        (void)uds_server_tick_1ms(srv);
        (void)uds_periodic_tick_1ms();
    }
}

/* =============================================================================
 * main()
 * ============================================================================= */

int main(void)
{
    uds_status_t      status;
    uds_server_ctx_t *srv = NULL;

    LOG_INF("Xaloqi EDS BasicECU_DoIP starting (v1.6.0)");
    LOG_INF("DoIP logical address: 0x%04X  port: %u",
            (unsigned)DOIP_ECU_LOGICAL_ADDR, (unsigned)DOIP_PORT);

    /*
     * Security algorithm init.
     * For production: inject TRNG callback and OEM level keys.
     * See docs/SECURITY_NOTICE.md.
     */
    LOG_WRN("[SEC] Using placeholder AES keys — inject OEM keys before production.");
    (void)uds_security_algo_set_rng_cb(NULL);

    /*
     * [EDS#200] Must run before uds_generated_init() (dtc_mirror_init() at
     * Step 3.5 sits on top of nvm_store and silently no-ops until this
     * succeeds). This example only targets native_sim, where
     * platform/zephyr/nvm_store_mock.c (RAM-backed) is linked instead of
     * the real NVS backend — its nvm_store_init() ignores cfg entirely, so
     * NULL is correct here (see nvm_store.h: "may be NULL on host").
     */
    status = nvm_store_init(NULL);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("NVM store init failed: 0x%02X — DTC mirror will not "
                "survive reset this run.", (unsigned)status);
    }

    /*
     * UDS stack init — DoIP build passes NULL for the CAN transport
     * because the stack will be driven by the DoIP server thread, not
     * a CAN poll loop. uds_generated_init() in a DoIP-only build does
     * not call isotp_init(); the generated code checks the transport
     * field from diagnostics_config.yaml.
     *
     * For a "transport: both" build, pass the real CAN transport here.
     */
    status = uds_generated_init(NULL, 0U, 0U);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS stack init failed: 0x%02X", (unsigned)status);
        return -1;
    }

    srv = uds_generated_get_server();
    if (srv == NULL) {
        LOG_ERR("UDS server context NULL after init.");
        return -1;
    }

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    LOG_INF("UDS stack ready: %u DIDs  %u DTCs",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT);

    /*
     * [EDS#191] Start the 1 ms UDS tick task before the DoIP server so
     * S3/lockout timing is live from the moment a connection can arrive.
     */
    (void)k_thread_create(
        &s_tick_thread, s_tick_stack,
        K_THREAD_STACK_SIZEOF(s_tick_stack),
        uds_tick_task_entry,
        (void *)srv, NULL, NULL,
        CONFIG_DIAG_TICK_TASK_PRIORITY, 0U, K_NO_WAIT
    );
    k_thread_name_set(&s_tick_thread, "uds_tick_task");

    /*
     * Start DoIP server.
     * This registers the Zephyr BSD-socket platform ops with doip_server.c
     * and signals doip_thread (zephyr_lwip.c) to start — a semaphore, not
     * a fixed delay [EDS#192]. The thread then accepts TCP connections on
     * DOIP_ECU_LOGICAL_ADDR / DOIP_PORT.
     */
    status = eds_doip_platform_start(DOIP_ECU_LOGICAL_ADDR, DOIP_PORT, srv);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("DoIP platform start failed: 0x%02X", (unsigned)status);
        return -1;
    }

    LOG_INF("DoIP server activated — awaiting TCP connections on port %u",
            (unsigned)DOIP_PORT);

    /*
     * main() returns here. The Zephyr kernel continues scheduling the
     * doip_thread. In native_sim the process stays alive until killed.
     */
    return 0;
}
