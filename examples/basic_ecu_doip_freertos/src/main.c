// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip_freertos/src/main.c
 *
 * PURPOSE: BasicECU_DoIP_FreeRTOS — the same 5 DIDs / 2 DTCs / 3 routines
 *          as basic_ecu, served over DoIP (ISO 13400-2) on FreeRTOS + LwIP.
 *
 *          This is the FreeRTOS counterpart to basic_ecu_doip (Zephyr).
 *          The UDS stack, DoIP server logic, and platform abstraction layer
 *          are identical. Only the OS primitives and IP stack differ.
 *
 *          INTEGRATION SEQUENCE (4 steps):
 *
 *            1. eds_platform_init()                     — CAN transport + NVM
 *            2. uds_generated_init(NULL, 0, 0)          — UDS stack (DoIP-only,
 *                                                         no CAN transport)
 *            3. eds_doip_platform_start_freertos(...)   — DoIP server task
 *            4. vTaskStartScheduler()
 *
 *          Note: can = NULL in step 2 because transport: doip is set in
 *          diagnostics_config.yaml. The EDS_DOIP_ONLY_BUILD guard in
 *          generated/uds_init.c skips isotp_init() when can is NULL.
 *
 *          For production: replace lwip_netif_add() / dhcp stubs below with
 *          your MCU's Ethernet driver initialisation. The DoIP server task
 *          will start listening on port 13400 once the network is up.
 *
 * TARGET : Any FreeRTOS + LwIP Ethernet-capable MCU (STM32H7, i.MX RT, etc.)
 *          CI target: QEMU ARM Cortex-M4 (compile-only; no LwIP in CI)
 *
 * SAFETY  : ASIL-B candidate.
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* EDS stack */
#include "platform_api.h"
#include "uds_init.h"
#include "generated_config.h"
#include "uds_security_algo.h"

/* DoIP platform */
#include "platform_doip.h"
#include "doip_server.h"

#include <stdint.h>
#include <stdbool.h>

/* =============================================================================
 * DoIP ECU configuration
 * ============================================================================= */

#define DOIP_ECU_LOGICAL_ADDR   (0xE400U)  /**< Standard xaloqi-tester ECU addr */
#define DOIP_TASK_STACK_BYTES   (4096U)
#define DOIP_TASK_PRIORITY      (6U)

/* =============================================================================
 * Application state — same stub DIDs as basic_ecu_doip
 * ============================================================================= */

static const uint8_t s_vin[17]            = "DOIPFRTOSEDS0001";
static const uint8_t s_ecu_serial[4]      = { 0x03U, 0x00U, 0x00U, 0x01U };
static       uint8_t s_spare_part_num[11] = "EDS-FRT-001";
static uint16_t      s_engine_speed_rpm   = 800U;
static int8_t        s_coolant_temp_degc  = 85;

/* =============================================================================
 * DID handlers — identical contract to basic_ecu_doip
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
 * main() — four-step integration
 * ============================================================================= */

int main(void)
{
    uds_status_t      status;
    uds_server_ctx_t *srv = NULL;

    /*
     * Step 0: Security algorithm placeholder.
     * Inject TRNG callback and OEM keys before production.
     */
    (void)uds_security_algo_set_rng_cb(NULL);

    /*
     * Step 1: Platform init.
     * DoIP-only build — pass NULL for CAN transport. No CAN driver needed.
     * The EDS platform layer is still initialised for NVM and session state.
     */
    status = eds_platform_init(&(eds_platform_cfg_t){
        .can_send            = NULL,   /* no CAN in DoIP-only build */
        .nvm                 = { NULL, NULL, NULL },
        .uds_task_stack_size = 0U,     /* no UDS CAN poll task */
        .uds_task_priority   = 0U,
    });
    if (status != UDS_STATUS_OK) {
        /* Platform init failure is fatal — no logging available yet */
        for (;;) { vTaskDelay(pdMS_TO_TICKS(1000U)); }
    }

    /*
     * Step 2: UDS stack init.
     * NULL CAN transport — EDS_DOIP_ONLY_BUILD guard in uds_init.c
     * skips isotp_init(). UDS server, session, security, DIDs, DTCs
     * and routines are all initialised normally.
     */
    status = uds_generated_init(NULL, 0U, 0U);
    if (status != UDS_STATUS_OK) {
        for (;;) { vTaskDelay(pdMS_TO_TICKS(1000U)); }
    }

    srv = uds_generated_get_server();
    if (srv == NULL) {
        for (;;) { vTaskDelay(pdMS_TO_TICKS(1000U)); }
    }

    /*
     * Step 3: Start DoIP server task.
     * The task is created here but blocks in vTaskDelay(500ms) before
     * calling lwip_listen — giving LwIP time to come up after the
     * scheduler starts.
     *
     * For production: call lwip_netif_add() / lwip_dhcp_start() before
     * this point so the netif is ready when the DoIP task unblocks.
     */
    status = eds_doip_platform_start_freertos(
        DOIP_ECU_LOGICAL_ADDR,
        DOIP_PORT,
        srv,
        DOIP_TASK_STACK_BYTES,
        DOIP_TASK_PRIORITY
    );
    if (status != UDS_STATUS_OK) {
        for (;;) { vTaskDelay(pdMS_TO_TICKS(1000U)); }
    }

    /*
     * Step 4: Hand control to the FreeRTOS scheduler.
     * The DoIP server task, UDS session timer task (if any), and any
     * application tasks all run from here.
     */
    vTaskStartScheduler();

    /* Should never reach here */
    for (;;) {}
    return 0;
}
