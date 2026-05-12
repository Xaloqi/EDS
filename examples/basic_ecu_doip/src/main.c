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
 *   main()        — init → uds_generated_init → eds_doip_platform_start → return
 *   doip_thread   — K_THREAD_DEFINE in zephyr_lwip.c; runs eds_doip_server_run()
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

/* --------------------------------------------------------------------------
 * DoIP platform headers
 * -------------------------------------------------------------------------- */
#include "platform_doip.h"   /* eds_doip_platform_start() */
#include "doip_server.h"     /* DOIP_PORT */

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

    LOG_INF("UDS stack ready: %u DIDs  %u DTCs",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT);

    /*
     * Start DoIP server.
     * This registers the Zephyr BSD-socket platform ops with doip_server.c
     * and activates the pre-defined doip_thread (K_THREAD_DEFINE in
     * zephyr_lwip.c). The thread starts accepting TCP connections on
     * DOIP_ECU_LOGICAL_ADDR / DOIP_PORT after a 500 ms startup delay.
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
