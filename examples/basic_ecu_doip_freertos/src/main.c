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
 *          INTEGRATION SEQUENCE (5 steps):
 *
 *            1. eds_platform_init()                     — CAN transport + NVM
 *            2. uds_generated_init(NULL, 0, 0)          — UDS stack (DoIP-only,
 *                                                         no CAN transport)
 *            2.5. uds_tick_task (xTaskCreate)            — [EDS#191] 1ms
 *                                                         uds_server_tick_1ms()
 *                                                         + uds_periodic_tick_1ms();
 *                                                         without this, S3/lockout
 *                                                         timing never progresses
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
#include "semphr.h"

/* EDS stack */
#include "platform_api.h"
#include "uds_init.h"
#include "uds_server.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "generated_config.h"
#include "uds_security_algo.h"

/* DoIP platform */
#include "platform_doip.h"
#include "doip_server.h"

/* Board support (QEMU mps2-an386) */
#include "board_uart.h"
#if defined(EDS_LWIP_REAL) && (EDS_LWIP_REAL == 1)
#include "eds_net.h"
#endif

#include <stdint.h>
#include <stdbool.h>

/* =============================================================================
 * DoIP ECU configuration
 * ============================================================================= */

#define DOIP_ECU_LOGICAL_ADDR   (0xE400U)  /**< Standard xaloqi-tester ECU addr */
#define DOIP_TASK_STACK_BYTES   (4096U)
#define DOIP_TASK_PRIORITY      (6U)

/*
 * [EDS#191] UDS tick task — drives uds_server_tick_1ms() and
 * uds_periodic_tick_1ms() every 1ms. This example never calls
 * eds_freertos_start() (its poll task is CAN/ISO-TP-specific — it would
 * call can_transport_receive()/isotp_tick_1ms() against the NULL
 * transport this DoIP-only build passes to uds_generated_init(), which
 * this build has no CAN context for), so without a dedicated tick task
 * here, S3 session timeout and SecurityAccess lockout countdown never
 * progress at all.
 */
#define UDS_TICK_TASK_STACK_WORDS  (256U)
#define UDS_TICK_TASK_PRIORITY     (5U)

static void s_on_session_change(uds_session_type_t old_sess,
                                uds_session_type_t new_sess)
{
    (void)old_sess;
    if (new_sess == UDS_SESSION_DEFAULT) {
        (void)uds_periodic_cancel_all();
    }
}

/* =============================================================================
 * [EDS#353] Session/security lock
 *
 * uds_tick_task and the DoIP server task both touch uds_server_ctx_t
 * (session/security state) with no prior synchronization — the same
 * lock discipline the CAN examples already apply to both their tick and
 * dispatch call sites, extended here to DoIP. See
 * docs/threading_guide.md's "Dual-Transport Concurrency" section.
 * ============================================================================= */

static StaticSemaphore_t s_session_lock_buf;
static StaticSemaphore_t s_security_lock_buf;
static SemaphoreHandle_t s_session_lock;
static SemaphoreHandle_t s_security_lock;

static void doip_lock_cb(void)
{
    (void)xSemaphoreTake(s_session_lock, portMAX_DELAY);
    (void)xSemaphoreTake(s_security_lock, portMAX_DELAY);
}

static void doip_unlock_cb(void)
{
    (void)xSemaphoreGive(s_security_lock);
    (void)xSemaphoreGive(s_session_lock);
}

/* =============================================================================
 * UDS tick task [EDS#191]
 * ============================================================================= */

static void uds_tick_task(void *pvParameters)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)pvParameters;

    for (;;) {
        vTaskDelay((TickType_t)1U);
        /* [EDS#353] Same lock doip_lock_cb()/doip_unlock_cb() wrap
         * uds_server_process_request() with below. */
        (void)xSemaphoreTake(s_session_lock, portMAX_DELAY);
        (void)xSemaphoreTake(s_security_lock, portMAX_DELAY);
        (void)uds_server_tick_1ms(srv);
        (void)uds_periodic_tick_1ms();
        (void)xSemaphoreGive(s_security_lock);
        (void)xSemaphoreGive(s_session_lock);
    }
}

/* =============================================================================
 * main() — four-step integration
 * ============================================================================= */

int main(void)
{
    uds_status_t      status;
    uds_server_ctx_t *srv = NULL;

    /*
     * Step 0a: Board bring-up. UART first, so every later step has somewhere
     * to report to. These lines are the CI leg's boot evidence — see
     * compat-tests COVERAGE.md.
     */
    board_uart_init();
    board_uart_puts("\r\n[eds] basic_ecu_doip_freertos boot\r\n");
    board_uart_puts("[eds] board=qemu mps2-an386 cortex-m4f\r\n");

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
        board_uart_puts("[eds] FATAL: eds_platform_init failed\r\n");
        for (;;) { }
    }
    board_uart_puts("[eds] platform init ok\r\n");

    /*
     * Step 2: UDS stack init.
     * NULL CAN transport — EDS_DOIP_ONLY_BUILD guard in uds_init.c
     * skips isotp_init(). UDS server, session, security, DIDs, DTCs
     * and routines are all initialised normally.
     */
    status = uds_generated_init(NULL, 0U, 0U);
    if (status != UDS_STATUS_OK) {
        board_uart_puts("[eds] FATAL: uds_generated_init failed\r\n");
        for (;;) { }
    }

    srv = uds_generated_get_server();
    if (srv == NULL) {
        board_uart_puts("[eds] FATAL: uds_generated_get_server returned NULL\r\n");
        for (;;) { }
    }
    board_uart_puts("[eds] uds stack init ok\r\n");

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    /* [EDS#353] Must be created and registered before the tick task and
     * DoIP server task start below — both race on uds_server_ctx_t from
     * the moment either task runs. */
    s_session_lock  = xSemaphoreCreateMutexStatic(&s_session_lock_buf);
    s_security_lock = xSemaphoreCreateMutexStatic(&s_security_lock_buf);
    configASSERT(s_session_lock != NULL);
    configASSERT(s_security_lock != NULL);
    eds_doip_set_lock_callbacks(doip_lock_cb, doip_unlock_cb);

    /*
     * Step 2.5: Start the 1 ms UDS tick task [EDS#191] — before the DoIP
     * server task so S3/lockout timing is live from the moment a
     * connection can arrive. xTaskCreate (not xTaskCreateStatic) is fine
     * here: this task's own footprint is tiny and one-time at boot,
     * unlike the CAN poll task's stack-budget concerns noted above.
     */
    (void)xTaskCreate(
        uds_tick_task,
        "uds_tick",
        (configSTACK_DEPTH_TYPE)UDS_TICK_TASK_STACK_WORDS,
        (void *)srv,
        (UBaseType_t)UDS_TICK_TASK_PRIORITY,
        NULL
    );

    /*
     * Step 2.75: Bring up the IP stack.
     *
     * Only present in the real-lwIP build (-DLWIP_DIR=...). The default
     * build compiles against boards/qemu_cortex_m4/lwip_stub, whose socket
     * calls all return -1 by design, and has no netif to bring up.
     *
     * eds_net_start() only *creates* the bring-up task; lwIP's own
     * tcpip_init() waits on a semaphore for its thread to come up, which
     * would deadlock if it ran here, before vTaskStartScheduler(). So the
     * real work happens in that task once the scheduler is running. The
     * DoIP server task's existing 500 ms delay covers the ordering.
     */
#if defined(EDS_LWIP_REAL) && (EDS_LWIP_REAL == 1)
    eds_net_start();
#endif

    /*
     * Step 3: Start DoIP server task.
     * The task is created here but blocks in vTaskDelay(500ms) before
     * calling lwip_listen — giving LwIP time to come up after the
     * scheduler starts.
     */
    status = eds_doip_platform_start_freertos(
        DOIP_ECU_LOGICAL_ADDR,
        DOIP_PORT,
        srv,
        DOIP_TASK_STACK_BYTES,
        DOIP_TASK_PRIORITY
    );
    if (status != UDS_STATUS_OK) {
        board_uart_puts("[eds] FATAL: doip platform start failed\r\n");
        for (;;) { }
    }
    board_uart_puts("[eds] doip task created, starting scheduler\r\n");

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
