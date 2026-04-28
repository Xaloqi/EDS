// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_platform_api.c
 *
 * PURPOSE: Implements platform/platform_api.h using FreeRTOS APIs.
 *
 *          This file provides the eds_platform_* functions for FreeRTOS builds.
 *          Unlike the Zephyr implementation, which is wired via DTS + Kconfig,
 *          the FreeRTOS implementation requires the customer to provide:
 *            1. A CAN send function pointer (required)
 *            2. NVM read/write callbacks (optional — RAM stub used if absent)
 *
 *          CUSTOMER INTEGRATION SEQUENCE:
 *            1. Implement a CAN send function:
 *                 static uds_status_t my_can_send(const eds_can_frame_t *f) { ... }
 *            2. Call eds_platform_init() before uds_generated_init():
 *                 eds_platform_init(&(eds_platform_cfg_t){
 *                     .can_send            = my_can_send,
 *                     .nvm                 = { my_read, my_write, my_is_ready },
 *                     .uds_task_stack_size = 2048U,
 *                     .uds_task_priority   = 5U,
 *                 });
 *            3. Call uds_generated_init() to start the UDS stack.
 *            4. Call vTaskStartScheduler() — do NOT start it before step 3.
 *            5. In your CAN RX interrupt/callback:
 *                 eds_platform_can_input(&frame);
 *
 *          RESET:
 *            eds_platform_ecu_reset() calls the customer-registered reset
 *            callback. If none is registered, it calls portNVIC_SystemReset
 *            (Cortex-M) or equivalent. Customer provides this via
 *            eds_platform_set_reset_cb() if platform-specific sequencing
 *            (e.g. PMIC power-off) is required.
 *
 *          NVM FLUSH:
 *            eds_platform_nvm_flush() flushes DTC mirror, session stats, and
 *            lifecycle counter through the nvm_store_* API. If the customer
 *            registered NVM ops, those are called; otherwise the RAM stub
 *            is used (data is lost on reset — only for development).
 *
 * SAFETY  : ASIL-B candidate. Reset path is safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "platform_api.h"
#include "freertos_can.h"
#include "nvm_store.h"
#include "dtc_mirror.h"
#include "uds_session_stats.h"
#include "uds_types.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration — implemented in freertos_nvm.c */
void freertos_nvm_register_ops(const eds_nvm_ops_t *ops);

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

/** True after eds_platform_init() has completed successfully. */
static bool s_initialized = false;

/** Customer-provided CAN send function. */
static eds_can_send_fn_t s_can_send = NULL;

/** Customer-provided NVM operations (all three pointers, or all NULL). */
static eds_nvm_ops_t s_nvm_ops;

/** Customer-provided reset callback (optional). */
static void (*s_reset_cb)(uint8_t reset_type) = NULL;

/* --------------------------------------------------------------------------
 * Reset sub-function values (ISO 14229-1 Table 186)
 * -------------------------------------------------------------------------- */
#define ECU_RESET_HARD_RESET   (0x01U)
#define ECU_RESET_KEY_OFF_ON   (0x02U)
#define ECU_RESET_SOFT_RESET   (0x03U)

/* --------------------------------------------------------------------------
 * RAM-backed NVM stub
 *
 * Used when the customer does not provide NVM ops. Non-persistent — data
 * is lost on reset. Suitable for development and CI only.
 * Same slot layout as nvm_store_mock.c.
 * -------------------------------------------------------------------------- */

#define FREERTOS_NVM_MAX_RECORDS  (16U)

typedef struct {
    uint16_t key;
    uint8_t  data[NVM_MAX_RECORD_BYTES];
    size_t   len;
    bool     used;
} freertos_nvm_record_t;

static freertos_nvm_record_t s_nvm_records[FREERTOS_NVM_MAX_RECORDS];
static bool                  s_nvm_stub_ready = false;

static freertos_nvm_record_t *nvm_stub_find(uint16_t key)
{
    uint8_t i;
    for (i = 0U; i < (uint8_t)FREERTOS_NVM_MAX_RECORDS; i++) {
        if (s_nvm_records[i].used && (s_nvm_records[i].key == key)) {
            return &s_nvm_records[i];
        }
    }
    return NULL;
}

static freertos_nvm_record_t *nvm_stub_alloc(uint16_t key)
{
    uint8_t i;
    for (i = 0U; i < (uint8_t)FREERTOS_NVM_MAX_RECORDS; i++) {
        if (!s_nvm_records[i].used) {
            s_nvm_records[i].key  = key;
            s_nvm_records[i].used = true;
            s_nvm_records[i].len  = 0U;
            return &s_nvm_records[i];
        }
    }
    return NULL;
}

static uds_status_t nvm_stub_read(uint16_t key, uint8_t *buf,
                                   size_t len, size_t *out_len)
{
    const freertos_nvm_record_t *rec;
    size_t copy_len;

    if (buf == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    if (len == 0U)   { return UDS_STATUS_ERR_INVALID_PARAM; }
    if (!s_nvm_stub_ready) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    rec = nvm_stub_find(key);
    if (rec == NULL) { return UDS_STATUS_ERR_DID_NOT_FOUND; }

    copy_len = (rec->len < len) ? rec->len : len;
    (void)memcpy(buf, rec->data, copy_len);
    if (out_len != NULL) { *out_len = copy_len; }

    return UDS_STATUS_OK;
}

static uds_status_t nvm_stub_write(uint16_t key, const uint8_t *buf,
                                    size_t len)
{
    freertos_nvm_record_t *rec;

    if (buf == NULL) { return UDS_STATUS_ERR_NULL_PTR; }
    if ((len == 0U) || (len > (size_t)NVM_MAX_RECORD_BYTES)) {
        return UDS_STATUS_ERR_INVALID_PARAM;
    }
    if (!s_nvm_stub_ready) { return UDS_STATUS_ERR_NOT_INITIALIZED; }

    rec = nvm_stub_find(key);
    if (rec == NULL) { rec = nvm_stub_alloc(key); }
    if (rec == NULL) { return UDS_STATUS_ERR_PLATFORM; }

    (void)memcpy(rec->data, buf, len);
    rec->len = len;

    return UDS_STATUS_OK;
}

static bool nvm_stub_is_ready(void)
{
    return s_nvm_stub_ready;
}

/* Default task parameters — overridden by eds_platform_init() values. */
#define EDS_POLL_TASK_DEFAULT_STACK_BYTES   (2048U)
#define EDS_POLL_TASK_DEFAULT_PRIORITY      (5U)

#ifndef EDS_POLL_TASK_MAX_STACK_WORDS
#define EDS_POLL_TASK_MAX_STACK_WORDS  \
    (EDS_POLL_TASK_DEFAULT_STACK_BYTES / sizeof(StackType_t))
#endif

static StackType_t  s_poll_stack[EDS_POLL_TASK_MAX_STACK_WORDS];
static StaticTask_t s_poll_task_tcb;

/* Task stack size (words) and priority — populated by eds_platform_init(). */
static uint32_t s_task_stack_size = EDS_POLL_TASK_DEFAULT_STACK_BYTES;
static uint32_t s_task_priority   = EDS_POLL_TASK_DEFAULT_PRIORITY;

/* ============================================================================
 * eds_platform_init
 * ========================================================================== */

uds_status_t eds_platform_init(const eds_platform_cfg_t *cfg)
{
    nvm_store_cfg_t nvm_cfg;

    if (cfg == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    if (cfg->can_send == NULL) {
        /*
         * can_send is required. Without it the UDS stack cannot transmit
         * any response. Fail fast here rather than silently at first TX.
         */
        return UDS_STATUS_ERR_INVALID_PARAM;
    }

    if (s_initialized) {
        return UDS_STATUS_ERR_ALREADY_INITIALIZED;
    }

    /* Store customer callbacks. */
    s_can_send = cfg->can_send;
    s_nvm_ops  = cfg->nvm;

    /* Store task parameters for eds_freertos_start(). */
    if (cfg->uds_task_stack_size > 0U) {
        s_task_stack_size = cfg->uds_task_stack_size;
    }
    if (cfg->uds_task_priority > 0U) {
        s_task_priority = cfg->uds_task_priority;
    }

    /*
     * Initialise the CAN shim. This wires s_can_send into the
     * can_transport_ops_t vtable used by the ISO-TP layer.
     */
    freertos_can_init(cfg->can_send);

    /*
     * Initialise NVM.
     *
     * If the customer provided all three NVM ops, register them with
     * freertos_nvm.c so that nvm_store_* calls route through the
     * customer's flash driver (persistent across resets).
     *
     * Otherwise, activate the built-in RAM stub. Non-persistent —
     * suitable for development and CI only.
     */
    if ((cfg->nvm.read    != NULL) &&
        (cfg->nvm.write   != NULL) &&
        (cfg->nvm.is_ready != NULL))
    {
        /* Customer-provided persistent NVM. */
        freertos_nvm_register_ops(&cfg->nvm);
        s_nvm_ops = cfg->nvm;
    }
    else
    {
        /*
         * No customer NVM ops — activate the built-in RAM stub.
         * Clear state and register stub ops with freertos_nvm.c.
         */
        (void)memset(s_nvm_records, 0, sizeof(s_nvm_records));
        s_nvm_stub_ready = false;

        s_nvm_ops.read     = nvm_stub_read;
        s_nvm_ops.write    = nvm_stub_write;
        s_nvm_ops.is_ready = nvm_stub_is_ready;

        freertos_nvm_register_ops(&s_nvm_ops);
    }

    /* Activate the nvm_store layer (writes schema version record). */
    (void)memset(&nvm_cfg, 0, sizeof(nvm_cfg));
    s_nvm_stub_ready = true;   /* Must be true before nvm_store_init() reads. */
    (void)nvm_store_init(&nvm_cfg);

    s_initialized = true;

    return UDS_STATUS_OK;
}

/* ============================================================================
 * eds_platform_set_reset_cb
 *
 * Optional. Call before eds_platform_init() if platform-specific reset
 * sequencing is required (e.g. PMIC power-off, watchdog kick pattern).
 * If not registered, eds_platform_ecu_reset() calls portNVIC_SystemReset().
 * ========================================================================== */

void eds_platform_set_reset_cb(void (*cb)(uint8_t reset_type))
{
    s_reset_cb = cb;
}

/* ============================================================================
 * eds_platform_ecu_reset
 * ========================================================================== */

uds_status_t eds_platform_ecu_reset(uint8_t reset_type)
{
    /*
     * SAFETY (ISO 14229-1 §11.3.3):
     * Caller MUST have already:
     *   1. Transmitted the positive 0x51 response via ISO-TP.
     *   2. Waited for TX confirmation (N_As timeout).
     *   3. Called eds_platform_nvm_flush().
     *
     * This function must NOT return on a valid reset type.
     */

    switch (reset_type) {
        case ECU_RESET_HARD_RESET:
        case ECU_RESET_KEY_OFF_ON:
        case ECU_RESET_SOFT_RESET:
            break;
        default:
            return UDS_STATUS_ERR_PLATFORM;
    }

    if (s_reset_cb != NULL) {
        /*
         * Customer-provided reset — handles PMIC, power sequencer, etc.
         * Must not return on success.
         */
        s_reset_cb(reset_type);
    }
    else
    {
        /*
         * Default: ARM Cortex-M NVIC system reset via SCB->AIRCR.
         * This is the CMSIS-standard reset sequence, equivalent to
         * NVIC_SystemReset() but without requiring the CMSIS headers.
         * Works on Cortex-M0/M3/M4/M7/M33 without any FreeRTOS dependency.
         *
         * If the customer's BSP provides a higher-level reset function
         * (e.g. HAL_NVIC_SystemReset() on STM32), register it via
         * eds_platform_set_reset_cb() instead.
         *
         * TX CONFIRMATION CONTRACT:
         * The caller (service_0x11 integration layer) is responsible for
         * ensuring the positive 0x51 response has been fully transmitted
         * before calling this function. The correct sequence is:
         *
         *   1. Transmit 0x51 response via isotp_transmit().
         *   2. Poll isotp_tick_1ms() until the TX state machine returns to
         *      IDLE — this confirms the last CF has been sent and the N_As
         *      timer has not expired.
         *   3. Call eds_platform_nvm_flush().
         *   4. Call eds_platform_ecu_reset().
         *
         * DO NOT use vTaskDelay() or any fixed time delay as a substitute
         * for TX confirmation. A fixed delay is unreliable: it may expire
         * before the CAN peripheral completes the frame on a loaded bus
         * (N_As = 25 ms per ISO 15765-2 Table 5), and it wastes time when
         * the bus is idle. The ISO-TP TX pump is the correct confirmation
         * mechanism.
         */

        /* SCB Application Interrupt and Reset Control Register */
        *((volatile uint32_t *)0xE000ED0CUL) =
            (uint32_t)0x05FA0000UL |    /* VECTKEY write key */
            (uint32_t)0x00000004UL;     /* SYSRESETREQ bit */

        /* Ensure the write completes before we exit */
        __asm volatile ("dsb 0xF" ::: "memory");

        /* Should not reach here — loop defensively */
        for (;;) { }
    }

    /* Unreachable — defensive return. */
    return UDS_STATUS_ERR_PLATFORM;
}

/* ============================================================================
 * eds_platform_nvm_flush
 * ========================================================================== */

uds_status_t eds_platform_nvm_flush(void)
{
    uds_status_t rc;
    uds_status_t final_rc = UDS_STATUS_OK;

    /*
     * [FLUSH-01] DTC status-byte mirror.
     * Serialises all registered DTC status bytes into a single NVM record.
     * Non-fatal: DTC history is lost on reset if this fails.
     */
    rc = dtc_mirror_flush_all();
    if (rc != UDS_STATUS_OK) {
        final_rc = rc;
    }

    /*
     * [FLUSH-02] ECU lifecycle counter.
     * Incremented on every ECU reset. Informational — not safety-relevant.
     */
    if ((s_nvm_ops.is_ready != NULL) && s_nvm_ops.is_ready()) {
        uint32_t lifecycle_cnt = 0U;
        size_t   read_len      = 0U;

        rc = nvm_store_read(
            (uint16_t)NVM_KEY_LIFECYCLE_CNT,
            &lifecycle_cnt, sizeof(lifecycle_cnt), &read_len);

        if ((rc == UDS_STATUS_OK) || (rc == UDS_STATUS_ERR_DID_NOT_FOUND)) {
            lifecycle_cnt++;
            (void)nvm_store_write(
                (uint16_t)NVM_KEY_LIFECYCLE_CNT,
                &lifecycle_cnt, sizeof(lifecycle_cnt));
        }
    }

    /*
     * [FLUSH-03] Security attempt counter — written eagerly on each failed
     * attempt. Already current in NVM. No action needed here.
     */

    /*
     * [FLUSH-04] Session statistics.
     * Dirty-flagged internally — only writes if data has changed.
     */
    rc = uds_session_stats_flush();
    if (rc != UDS_STATUS_OK) {
        /* Non-fatal — informational counters. */
    }

    return final_rc;
}

/* ============================================================================
 * eds_platform_uptime_ms
 * ========================================================================== */

uint32_t eds_platform_uptime_ms(void)
{
    /*
     * xTaskGetTickCount() returns ticks since scheduler start.
     * portTICK_PERIOD_MS = 1000 / configTICK_RATE_HZ.
     *
     * For configTICK_RATE_HZ = 1000 (1 ms ticks), this returns ms directly.
     * For other tick rates the multiplication gives the correct ms value.
     *
     * Cast to uint32_t — TickType_t is uint32_t on 32-bit targets, but
     * may be uint16_t on 16-bit targets. The cast is intentional.
     */
    return (uint32_t)((uint32_t)xTaskGetTickCount() *
                      (uint32_t)portTICK_PERIOD_MS);
}

/* ============================================================================
 * eds_freertos_start
 *
 * Creates the UDS poll task and returns. The caller must call
 * vTaskStartScheduler() after this function (and after creating any
 * application tasks).
 *
 * The poll task:
 *   - Runs every 1ms (vTaskDelay(1)).
 *   - Dequeues CAN frames from the FreeRTOS RX queue and feeds them to
 *     isotp_process_rx_frame().
 *   - Calls isotp_tick_1ms() and uds_server_tick_1ms() each iteration.
 *   - On ISO-TP RX completion, dispatches to uds_server_process_request()
 *     and transmits the response via isotp_transmit().
 *
 * Uses static allocation (xTaskCreateStatic) — no heap required.
 * Stack size and priority are taken from the values stored in
 * s_task_stack_size / s_task_priority by eds_platform_init().
 * ========================================================================== */

/* Static request / response buffers for the poll task.
 *
 * [STACK-FIX] uds_msg_buf_t is 4097 bytes. Declaring two instances on the
 * poll-task stack would require at minimum 8194 bytes of stack, which exceeds
 * the configurable 2048-byte default. These are module-level statics accessed
 * only from the single UDS poll task — no concurrent access is possible.
 */
#include "uds_server.h"
#include "isotp.h"
#include "uds_init.h"        /* uds_generated_get_server(), uds_generated_get_isotp() */
#include "can_transport.h"

static uds_msg_buf_t s_poll_req_buf;
static uds_msg_buf_t s_poll_resp_buf;

/* Maximum stack words (StackType_t units). Sized for the default 2048-byte
 * stack. If the customer passes a larger stack in eds_platform_cfg_t, the
 * extra bytes are unused but harmless. For a custom size, rebuild with a
 * different EDS_POLL_TASK_MAX_STACK_WORDS definition. */

/*
 * ISO-TP RX completion callback — called by isotp_process_rx_frame() when a
 * full UDS PDU has been reassembled. Dispatches to the UDS server and
 * transmits the response via ISO-TP.
 *
 * Runs in the context of the UDS poll task. s_poll_req_buf and s_poll_resp_buf
 * are module-level statics — no concurrent access from other tasks or ISRs.
 */
static void eds_on_isotp_rx_complete(
    const uint8_t *data,
    uint16_t       length,
    void          *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();
    uds_status_t      status;
    uint16_t          i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) {
        return;
    }

    if ((length == (uint16_t)0U) || (length > (uint16_t)UDS_MAX_PAYLOAD_LEN)) {
        return;
    }

    for (i = (uint16_t)0U; i < length; i++) {
        s_poll_req_buf.data[i] = data[i];
    }
    s_poll_req_buf.length  = length;
    s_poll_resp_buf.length = (uint16_t)0U;

    status = uds_server_process_request(srv, &s_poll_req_buf, &s_poll_resp_buf);

    if ((status != UDS_STATUS_OK) && (s_poll_resp_buf.length == (uint16_t)0U)) {
        return;
    }

    if (s_poll_resp_buf.length > (uint16_t)0U) {
        (void)isotp_transmit(tp, s_poll_resp_buf.data, s_poll_resp_buf.length);
    }
}

/*
 * UDS poll task body.
 *
 * Runs at 1ms resolution. Each iteration:
 *   1. vTaskDelay(1) — yield for 1 tick (assumes configTICK_RATE_HZ >= 1000).
 *   2. Drain CAN RX queue — forward frames to ISO-TP reassembler.
 *   3. Advance ISO-TP timers (N_Cr, N_As, N_Bs).
 *   4. Advance UDS server timers (S3server session timeout, security lockout).
 */
static void eds_uds_poll_task(void *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    can_transport_t  *can = freertos_can_get_transport();
    isotp_ctx_t      *tp  = uds_generated_get_isotp();
    uds_can_frame_t   rx_frame;
    bool              frame_ready;
    uds_status_t      status;

    for (;;) {
        vTaskDelay((TickType_t)1U);

        frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);

        if ((status == UDS_STATUS_OK) && frame_ready) {
            (void)isotp_process_rx_frame(
                tp, &rx_frame, eds_on_isotp_rx_complete, (void *)srv);
        }

        (void)isotp_tick_1ms(tp);
        (void)uds_server_tick_1ms(srv);
    }
}

uds_status_t eds_freertos_start(void)
{
    uds_server_ctx_t *srv;
    TaskHandle_t      handle;
    uint32_t          stack_words;

    if (!s_initialized) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    srv = uds_generated_get_server();
    if (srv == NULL) {
        return UDS_STATUS_ERR_NOT_INITIALIZED;
    }

    /*
     * Convert bytes → StackType_t words, clamped to the static array.
     * If the customer specified more bytes than EDS_POLL_TASK_MAX_STACK_WORDS
     * allows, clamp silently. The static buffer size is the real ceiling.
     */
    stack_words = s_task_stack_size / (uint32_t)sizeof(StackType_t);
    if ((stack_words == 0U) ||
        (stack_words > (uint32_t)EDS_POLL_TASK_MAX_STACK_WORDS)) {
        stack_words = (uint32_t)EDS_POLL_TASK_MAX_STACK_WORDS;
    }

    handle = xTaskCreateStatic(
        eds_uds_poll_task,
        "eds_uds_poll",
        (uint32_t)stack_words,
        (void *)srv,
        (UBaseType_t)s_task_priority,
        s_poll_stack,
        &s_poll_task_tcb
    );

    if (handle == NULL) {
        /*
         * xTaskCreateStatic() only returns NULL if the stack or TCB pointer
         * is NULL. With valid static storage this should never happen.
         */
        return UDS_STATUS_ERR_OS_RESOURCE;
    }

    return UDS_STATUS_OK;
}

/* ============================================================================
 * eds_platform_can_input
 *
 * Called from the customer's CAN RX interrupt or callback.
 * Thread-safe and ISR-safe — delegates to freertos_can_input() which
 * posts to the internal RX queue without calling the UDS stack directly.
 * ========================================================================== */

void eds_platform_can_input(const eds_can_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    freertos_can_input(frame);
}
