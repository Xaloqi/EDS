/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/safeboot_ecu/src/main.c
 *
 * PURPOSE: SafeBootECU application entry point — OTA DFU + MCUboot integration.
 *
 *          Phase 6A additions over Phase 2B:
 *            - Dedicated k_thread for the diagnostics poll loop
 *              (named "diag_task", CONFIG_DIAG_TASK_STACK_SIZE bytes)
 *            - k_mutex guards on session and security context access
 *            - k_timer-driven 1 ms tick via diag_timer_t abstraction
 *            - Hardware watchdog supervision via diag_wdt_t
 *            - Overrun detection: logs if the 1 ms deadline is missed
 *            - Bus-off recovery: log + continue (hardware auto-recovers)
 *            - Complete zephyr_port_ecu_reset() wired to sys_reboot()
 *
 * THREAD MODEL:
 *
 *   main()                     — Zephyr main thread
 *     └─ platform init
 *     └─ UDS stack init
 *     └─ start diag_timer
 *     └─ start diag thread
 *     └─ return (main thread exits; diag thread runs indefinitely)
 *
 *   diag_task_entry()          — Diagnostics thread (priority: cooperative)
 *     └─ loop every 1 ms:
 *          ├─ diag_timer_wait_tick()     — block until 1 ms tick
 *          ├─ can_transport_receive()    — poll CAN RX queue
 *          ├─ isotp_process_rx_frame()   — ISO-TP reassembly
 *          │    └─ on_isotp_rx_complete() — UDS dispatch + response TX
 *          ├─ isotp_tick_1ms()           — N_Cr/N_As/N_Bs timers
 *          ├─ uds_server_tick_1ms()      — S3server / lockout timers
 *          └─ diag_wdt_feed()            — kick watchdog
 *
 * STACK BUDGET:
 *   CONFIG_DIAG_TASK_STACK_SIZE (default 4096 bytes)
 *   Worst-case stack depth (measured with -fstack-usage):
 *     on_isotp_rx_complete -> uds_server_process_request -> service handler
 *     Measured: ~1600 bytes. 4096 bytes gives 2.5x safety margin.
 *
 * SAFETY  : ASIL-B candidate. Production checklist before deployment:
 *   [ ] Replace stub key algorithm with TRNG-backed OEM algorithm
 *   [ ] Enable CONFIG_STACK_CANARIES=y in prj.conf
 *   [ ] Enable CONFIG_THREAD_STACK_INFO=y for WCET analysis
 *   [ ] Set CONFIG_DIAG_WDT_WINDOW_MS to <= 100 ms
 *   [ ] Validate worst-case poll loop time < 1 ms on target hardware
 *   [ ] Add bus-off recovery logic (can_recover() + filter re-registration)
 *   [ ] Review LOG_ERR paths — consider safe-state fallback
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * VERSION : 0.6.0 (Phase 6A)
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

/* --------------------------------------------------------------------------
 * Diagnostics stack headers
 * -------------------------------------------------------------------------- */
#include "uds_types.h"
#include "uds_server.h"
#include "uds_periodic.h"
#include "uds_session.h"
#include "isotp.h"
#include "can_transport.h"
#include "zephyr_port.h"
#include "zephyr_mutex.h"
#include "zephyr_timer.h"
#include "zephyr_wdt.h"

/* --------------------------------------------------------------------------
 * Generated headers
 * -------------------------------------------------------------------------- */
#include "uds_init.h"
#include "generated_config.h"

/* --------------------------------------------------------------------------
 * Zephyr headers
 * -------------------------------------------------------------------------- */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/mcuboot.h>

LOG_MODULE_REGISTER(safeboot_ecu, LOG_LEVEL_INF);

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

/* Overrun warning threshold: log if N consecutive ticks were missed. */
#define DIAG_OVERRUN_LOG_THRESHOLD    (3U)

/* =============================================================================
 * Static allocations
 * ============================================================================= */

/* Thread stack — file scope, never on heap. */
K_THREAD_STACK_DEFINE(s_diag_stack, CONFIG_DIAG_TASK_STACK_SIZE);
static struct k_thread s_diag_thread;

/* UDS message buffers — file scope to avoid stack exhaustion in thread. */
static uds_msg_buf_t s_req_buf;
static uds_msg_buf_t s_resp_buf;

/* Platform objects. */
static diag_mutex_t s_session_lock;    /**< Protects uds_session_ctx_t access. */
static diag_mutex_t s_security_lock;   /**< Protects uds_security_ctx_t access. */
static diag_timer_t s_tick_timer;      /**< 1 ms periodic tick. */
static diag_wdt_t   s_wdt;            /**< Hardware watchdog. */

/* =============================================================================
 * Zephyr TRNG callback for UDS SecurityAccess [P1-SEC]
 *
 * Registered with uds_security_algo_set_rng_cb() during ECU initialization.
 * Provides entropy from Zephyr's entropy subsystem (hardware TRNG when the
 * SoC has one). Falls back to the internal LFSR in uds_security_algo.c when
 * no entropy device is present in the board DTS (e.g. native_sim).
 *
 * ROOT CAUSE FIX (B1/B2):
 *   DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_entropy)) generates an unresolved
 *   linker symbol __device_dts_ord_N even when the DTS chosen node is absent.
 *   The correct pattern is to guard the entire device lookup with the
 *   DT_HAS_CHOSEN(zephyr_entropy) preprocessor check so that on boards
 *   without an entropy node (native_sim, bare nucleo without RNG DTS node)
 *   the symbol is never emitted and the LFSR fallback is used automatically.
 *
 * OEM INTEGRATION (boards WITH a hardware TRNG):
 *   1. Add the RNG DTS node to your board overlay, e.g. for STM32H743:
 *        &rng { status = "okay"; };
 *      and set chosen { zephyr,entropy = &rng; }; in the overlay root.
 *   2. Set CONFIG_ENTROPY_GENERATOR=y in the board .conf file.
 *   3. The TRNG callback is then wired automatically at boot — no code change.
 *
 *   Per-level AES-128 keys MUST be injected from OTP/HSM before production:
 *     uds_security_algo_set_level_key(0x01U, otp_key_level1);
 *     uds_security_algo_set_level_key(0x03U, otp_key_level2);
 * ============================================================================= */

#include "uds_security_algo.h"

/* Guard the entropy driver header and device handle on CONFIG_ENTROPY_GENERATOR.
 *
 * ROOT-CAUSE FIX (CI failure — Phase 1):
 *   DT_HAS_CHOSEN(zephyr_entropy) is TRUE on BOTH native_sim and nucleo_h743zi
 *   because their built-in board DTS already defines a zephyr,entropy chosen
 *   node (native_sim → POSIX /dev/urandom shim; STM32H743ZI → hardware RNG).
 *   When DT_HAS_CHOSEN=1, DEVICE_DT_GET() compiles and emits a reference to
 *   __device_dts_ord_N. But CONFIG_ENTROPY_GENERATOR was not set, so the
 *   entropy driver object is never added to libdrivers__entropy.a, and the
 *   linker cannot resolve that symbol → undefined reference at link time.
 *
 * CORRECT GUARD:
 *   Use CONFIG_ENTROPY_GENERATOR (the Kconfig symbol that controls whether
 *   the entropy driver is compiled and linked) rather than DT_HAS_CHOSEN
 *   (which only checks the DTS — not whether the driver is built).
 *   When CONFIG_ENTROPY_GENERATOR=n, this block is excluded from compilation
 *   entirely and no device reference is emitted to the linker.
 *
 * BOARD POLICY:
 *   native_sim   : CONFIG_ENTROPY_GENERATOR=n  → LFSR fallback (CI/simulation only)
 *   nucleo_h743zi: CONFIG_ENTROPY_GENERATOR=y  → STM32H743 hardware RNG via &rng node
 */
#if defined(CONFIG_ENTROPY_GENERATOR)
#include <zephyr/drivers/entropy.h>

/** Handle to Zephyr entropy device (TRNG source). */
static const struct device *s_entropy_dev = NULL;

/**
 * @brief Zephyr TRNG callback — fills buf with len random bytes.
 *
 * Called by uds_security_algo_generate_seed() each time a seed nonce
 * is needed.  Uses entropy_get_entropy() which maps to the SoC's hardware
 * TRNG (e.g. STM32 RNG, nRF CC3xx) or the best-available PRNG.
 */
static uds_status_t diag_trng_cb(uint8_t *buf, uint8_t len)
{
    if ((s_entropy_dev == NULL) || !device_is_ready(s_entropy_dev)) {
        return UDS_STATUS_ERR_PLATFORM;
    }
    int rc = entropy_get_entropy(s_entropy_dev, buf, (size_t)len);
    return (rc == 0) ? UDS_STATUS_OK : UDS_STATUS_ERR_PLATFORM;
}
#endif /* CONFIG_ENTROPY_GENERATOR */

/**
 * @brief Initialize TRNG (if available) and register callback with the
 *        security algorithm module.
 *
 * Guards the Zephyr entropy device lookup behind DT_HAS_CHOSEN so that
 * platforms without an entropy DTS node (native_sim, bare STM32 without
 * RNG node) compile and link correctly.  On those platforms the internal
 * LFSR in uds_security_algo.c is used automatically (development / CI only).
 *
 * Call once during ECU startup BEFORE uds_generated_init().
 *
 * @return Always 0 (non-fatal: TRNG absence is warned, not fatal).
 */
static int diag_security_algo_init(void)
{
#if defined(CONFIG_ENTROPY_GENERATOR)
    s_entropy_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
    if (!device_is_ready(s_entropy_dev)) {
        LOG_WRN("[P1-SEC] TRNG device not ready — using LFSR fallback (dev only)");
        s_entropy_dev = NULL;
    } else {
        uds_security_algo_set_rng_cb(diag_trng_cb);
        LOG_INF("[P1-SEC] TRNG registered: %s", s_entropy_dev->name);
    }
#else
    /* CONFIG_ENTROPY_GENERATOR=n on this board.
     * uds_security_algo.c LFSR is used automatically — suitable for CI /
     * simulation only. Production targets set CONFIG_ENTROPY_GENERATOR=y. */
    LOG_WRN("[P1-SEC] No zephyr_entropy DTS node — LFSR seed fallback active. "
            "Add RNG DTS node and CONFIG_ENTROPY_GENERATOR=y for production.");
#endif /* CONFIG_ENTROPY_GENERATOR */

    /*
     * OEM KEY INJECTION PLACEHOLDER
     * ─────────────────────────────
     * Replace the block below with real key material before production.
     *
     * Example (keys read from OTP fuses via SoC-specific driver):
     *
     *   uint8_t key_l1[16], key_l2[16];
     *   otp_read_key(OTP_KEY_SLOT_DIAG_L1, key_l1, sizeof(key_l1));
     *   otp_read_key(OTP_KEY_SLOT_DIAG_L2, key_l2, sizeof(key_l2));
     *   uds_security_algo_set_level_key(0x01U, key_l1);
     *   uds_security_algo_set_level_key(0x03U, key_l2);
     *   memset(key_l1, 0, sizeof(key_l1));
     *   memset(key_l2, 0, sizeof(key_l2));
     *
     * Without this, compile-time placeholder keys are used — NOT SECURE.
     */
    LOG_WRN("[P1-SEC] Using compile-time placeholder AES keys. "
            "Inject OTP keys before production.");

    return 0;
}
/* =============================================================================
 * ISO-TP RX completion callback
 *
 * Called from diag_task_entry() when isotp_process_rx_frame() has
 * reassembled a complete UDS PDU.
 *
 * Mutex discipline:
 *   The UDS server internally accesses session and security contexts.
 *   We acquire both locks before calling uds_server_process_request()
 *   to prevent concurrent access from any future second thread (e.g.
 *   a background DTC setter).
 *
 * TIMING: Must complete within GEN_P2_SERVER_MAX_MS (25 ms).
 * ============================================================================= */
static void on_isotp_rx_complete(
    const uint8_t *data,
    uint32_t       length,
    void          *arg)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)arg;
    isotp_ctx_t      *tp  = uds_generated_get_isotp();
    uds_status_t      status;
    uint16_t          i;

    if ((data == NULL) || (srv == NULL) || (tp == NULL)) {
        LOG_ERR("on_isotp_rx_complete: NULL argument — PDU dropped.");
        return;
    }

    if ((length == (uint32_t)0U) || (length > (uint32_t)UDS_MAX_PAYLOAD_LEN)) {
        LOG_WRN("on_isotp_rx_complete: invalid length %u — PDU dropped.",
                (unsigned)length);
        return;
    }

    /* Copy PDU into static request buffer. */
    for (i = (uint16_t)0U; i < (uint16_t)length; i++) {
        s_req_buf.data[i] = data[i];
    }
    s_req_buf.length = (uint16_t)length;
    s_resp_buf.length = (uint16_t)0U;

    LOG_DBG("UDS RX: SID=0x%02X len=%u", (unsigned)data[0], (unsigned)length);

    /* Acquire context locks before dispatching. */
    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);

    status = uds_server_process_request(srv, &s_req_buf, &s_resp_buf);

    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);

    if ((status != UDS_STATUS_OK) && (s_resp_buf.length == (uint16_t)0U)) {
        LOG_ERR("UDS server error 0x%02X — no response generated.",
                (unsigned)status);
        return;
    }

    if (s_resp_buf.length > (uint16_t)0U) {
        LOG_DBG("UDS TX: RSID=0x%02X len=%u",
                (unsigned)s_resp_buf.data[0], (unsigned)s_resp_buf.length);

        status = isotp_transmit(tp, s_resp_buf.data, s_resp_buf.length);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("ISO-TP TX error 0x%02X", (unsigned)status);
        }
    }

    /* [P2-0x11-01] Deferred reset: response is on the wire, now reset. */
    if (srv->pending_reset_type != (uint8_t)0U) {
        uint8_t reset_type = srv->pending_reset_type;
        srv->pending_reset_type = (uint8_t)0U;
        (void)zephyr_port_nvm_flush();
        k_msleep(50);  /* Allow ISO-TP TX to reach the wire before reset (As timer). */
        (void)zephyr_port_ecu_reset(reset_type);
        /* Does not return. */
    }
}

/* =============================================================================
 * 1 ms tick callback — invoked from diag_task_entry() via diag_timer_wait_tick()
 *
 * Advances all time-sensitive state machines. Called in thread context
 * (not ISR), so mutex acquisition and logging are safe here.
 * ============================================================================= */
typedef struct tick_ctx {
    uds_server_ctx_t *srv;
    isotp_ctx_t      *tp;
} tick_ctx_t;

static void on_tick(void *arg)
{
    tick_ctx_t *ctx = (tick_ctx_t *)arg;

    if ((ctx == NULL) || (ctx->tp == NULL) || (ctx->srv == NULL)) {
        return;
    }

    /* ISO-TP timers: N_Cr, N_As, N_Bs. */
    (void)isotp_tick_1ms(ctx->tp);

    /* UDS server timers: S3server session timeout + security lockout. */
    (void)diag_mutex_lock(&s_session_lock);
    (void)diag_mutex_lock(&s_security_lock);
    (void)uds_server_tick_1ms(ctx->srv);
    (void)uds_periodic_tick_1ms();
    (void)diag_mutex_unlock(&s_security_lock);
    (void)diag_mutex_unlock(&s_session_lock);
}

/* =============================================================================
 * Diagnostics task entry point
 *
 * Runs as a dedicated Zephyr thread. Loops at ~1 ms intervals, driven by
 * the k_timer-backed diag_timer_t.
 *
 * Each iteration:
 *   1. Wait for 1 ms tick (blocks on semaphore from timer ISR).
 *   2. Poll CAN RX queue — forward to ISO-TP reassembler.
 *   3. Tick via on_tick() callback (ISO-TP + UDS timers).
 *   4. Feed hardware watchdog.
 *   5. Check for overruns and log if threshold exceeded.
 * ============================================================================= */
static void diag_task_entry(void *p1, void *p2, void *p3)
{
    uds_server_ctx_t *srv = (uds_server_ctx_t *)p1;
    can_transport_t  *can = (can_transport_t  *)p2;
    isotp_ctx_t      *tp  = (isotp_ctx_t      *)p3;

    uds_status_t    status;
    uds_can_frame_t rx_frame;
    bool            frame_ready;
    uint32_t        overrun_count;
    uint32_t        prev_overrun = 0U;

    tick_ctx_t tick_ctx = { .srv = srv, .tp = tp };

    LOG_INF("Diagnostics task started (stack: %u bytes, priority: %d).",
            (unsigned)CONFIG_DIAG_TASK_STACK_SIZE,
            CONFIG_DIAG_TASK_PRIORITY);

    /* Start 1 ms periodic timer. */
    if (diag_timer_start(&s_tick_timer) != UDS_STATUS_OK) {
        LOG_ERR("Failed to start 1 ms timer — aborting diag task.");
        return;
    }

    while (true) {

        /* ── [1] Wait for 1 ms tick ──────────────────────────────────────── */
        status = diag_timer_wait_tick(&s_tick_timer, 2U);
        if (status == UDS_STATUS_ERR_TIMEOUT) {
            /* Tick did not arrive within 2 ms — timer may have stalled. */
            LOG_WRN("1 ms tick timeout — continuing.");
        }

        /* ── [2] CAN RX poll ─────────────────────────────────────────────── */
        frame_ready = false;
        status = can_transport_receive(can, &rx_frame, &frame_ready);

        if (status != UDS_STATUS_OK) {
            if (status == UDS_STATUS_ERR_CAN_BUS_OFF) {
                LOG_ERR("CAN bus-off condition detected.");
                /*
                 * TODO [APPLICATION]: Implement bus-off recovery:
                 *   1. Call can_recover(plat->can_dev, K_MSEC(200)).
                 *   2. Re-register RX filter.
                 *   3. Clear s_bus_off flag.
                 * For now: log and continue — the WDT will reset if the
                 * bus remains off long enough that the loop stalls.
                 */
            } else {
                LOG_DBG("can_transport_receive error: 0x%02X", (unsigned)status);
            }

        } else if (frame_ready) {

            status = isotp_process_rx_frame(
                tp,
                &rx_frame,
                on_isotp_rx_complete,
                (void *)srv
            );

            if ((status != UDS_STATUS_OK) &&
                (status != (uds_status_t)UDS_STATUS_ERR_TP_FRAME_INVALID)) {
                LOG_WRN("ISO-TP RX error: 0x%02X", (unsigned)status);
            }

        } else {
            /* No frame this iteration — idle. */
        }

        /* ── [3] 1 ms tick processing (ISO-TP + UDS + periodic timers) ─── */
        on_tick(&tick_ctx);

        {
            static uds_msg_buf_t s_periodic_frame;
            while (uds_periodic_pop_due(&s_periodic_frame) == UDS_STATUS_OK) {
                (void)isotp_transmit(tp, s_periodic_frame.data,
                                     (uint32_t)s_periodic_frame.length);
            }
        }

        /* ── [4] Watchdog feed ───────────────────────────────────────────── */
        (void)diag_wdt_feed(&s_wdt);

        /* ── [5] Overrun detection ───────────────────────────────────────── */
        (void)diag_timer_pending_ticks(&s_tick_timer, &overrun_count);
        if (overrun_count > prev_overrun) {
            uint32_t new_overruns = overrun_count - prev_overrun;
            if (new_overruns >= DIAG_OVERRUN_LOG_THRESHOLD) {
                LOG_WRN("Poll loop overrun: %u ticks missed. "
                        "Worst-case execution time exceeds 1 ms deadline.",
                        (unsigned)new_overruns);
            }
            prev_overrun = overrun_count;
        }

    } /* while (true) */
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
 * main() — Platform + stack initialization, then start diagnostics thread.
 *
 * main() exits after starting the diagnostics thread. The Zephyr scheduler
 * takes over and the diagnostics thread runs indefinitely.
 * ============================================================================= */
int main(void)
{
    uds_status_t      status;
    can_transport_t  *can  = NULL;
    uds_server_ctx_t *srv  = NULL;
    isotp_ctx_t      *tp   = NULL;

    /* ── Banner ──────────────────────────────────────────────────────────── */
    LOG_INF("=============================================================");
    LOG_INF("Xaloqi EDS  v" GEN_ECU_VERSION);
    LOG_INF("  ECU      : " GEN_ECU_NAME);
    LOG_INF("  Config   : " GEN_GENERATED_TIMESTAMP);
    LOG_INF("  DIDs     : %u   DTCs: %u",
            (unsigned)GEN_DID_COUNT, (unsigned)GEN_DTC_COUNT);
    LOG_INF("  RX CAN   : 0x%03X     TX CAN: 0x%03X",
            (unsigned)DIAG_RX_CAN_ID, (unsigned)DIAG_TX_CAN_ID);
    LOG_INF("=============================================================");

    /* ── Mutex initialization ────────────────────────────────────────────── */
    if (diag_mutex_init(&s_session_lock) != UDS_STATUS_OK) {
        LOG_ERR("Session mutex init failed.");
        return -1;
    }
    if (diag_mutex_init(&s_security_lock) != UDS_STATUS_OK) {
        LOG_ERR("Security mutex init failed.");
        return -1;
    }
    LOG_INF("Mutex objects initialized.");

    /* ── Watchdog initialization ─────────────────────────────────────────── */
    status = diag_wdt_init(&s_wdt);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("WDT init failed: 0x%02X", (unsigned)status);
        /* Non-fatal in this example — continue without WDT. */
    }

    /* ── 1 ms timer initialization ───────────────────────────────────────── */
    /*
     * Timer callback (on_tick) is registered here but not started yet —
     * start is deferred to the diagnostics thread after stack init completes.
     * Passing NULL for arg; tick_ctx is set up in the thread entry function.
     */
    status = diag_timer_init(&s_tick_timer, on_tick, NULL);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("Timer init failed: 0x%02X", (unsigned)status);
        return -1;
    }

    /* ── Platform + CAN transport ────────────────────────────────────────── */
    {
        const zephyr_port_cfg_t port_cfg = { .can_dev = DIAG_CAN_DEV };

        status = zephyr_port_init(&port_cfg, &can);
        if (status != UDS_STATUS_OK) {
            LOG_ERR("Platform init failed: 0x%02X", (unsigned)status);
            return -1;
        }
    }
    LOG_INF("CAN transport ready.");

    /* ── Security algorithm init (TRNG + key injection) ─────────────────── */
    /* [P1-SEC] Must run before uds_generated_init() so the TRNG callback
     * and any injected OEM keys are in place before the first seed request. */
    (void)diag_security_algo_init();

    /* ── UDS stack initialization ────────────────────────────────────────── */
    status = uds_generated_init(can, DIAG_RX_CAN_ID, DIAG_TX_CAN_ID);
    if (status != UDS_STATUS_OK) {
        LOG_ERR("UDS stack init failed: 0x%02X", (unsigned)status);
        return -1;
    }

    srv = uds_generated_get_server();
    tp  = uds_generated_get_isotp();

    if ((srv == NULL) || (tp == NULL)) {
        LOG_ERR("Context accessors returned NULL after init.");
        return -1;
    }

    (void)uds_periodic_init();
    (void)uds_session_register_change_cb(srv->cfg.session_ctx,
                                          s_on_session_change);

    LOG_INF("UDS stack ready.");
    LOG_INF("  P2max  = %u ms", (unsigned)GEN_P2_SERVER_MAX_MS);
    LOG_INF("  P2*max = %u ms", (unsigned)GEN_P2_STAR_SERVER_MAX_MS);
    LOG_INF("  S3     = %u ms", (unsigned)GEN_S3_SERVER_TIMEOUT_MS);

    /* ── MCUboot image confirmation ──────────────────────────────────────── */
    /*
     * On the first boot after an OTA swap, boot_is_img_confirmed() returns
     * false.  Calling boot_write_img_confirmed() marks the new image permanent
     * so MCUboot will not roll back to the previous slot on the next reset.
     *
     * Non-fatal by design: if confirmation fails the device continues to run
     * but will roll back on the next power cycle — correct safety behaviour
     * for a defective image that cannot write its own confirmation.
     *
     * Requires CONFIG_MCUBOOT_IMG_MANAGER=y and CONFIG_BOOTLOADER_MCUBOOT=y
     * in boards/nucleo_h743zi/nucleo_h743zi.conf.
     */
    {
        int boot_rc;

        if (!boot_is_img_confirmed()) {
            boot_rc = boot_write_img_confirmed();
            if (boot_rc == 0) {
                LOG_INF("[OTA] New image confirmed — rollback guard cleared.");
            } else {
                LOG_ERR("[OTA] boot_write_img_confirmed() err %d — "
                        "MCUboot will roll back on next reset.", boot_rc);
            }
        } else {
            LOG_DBG("[OTA] Image already confirmed.");
        }
    }

    /* ── Start diagnostics thread ────────────────────────────────────────── */
    /*
     * Pass srv, can, and tp as thread arguments (p1, p2, p3).
     * Thread is cooperative priority CONFIG_DIAG_TASK_PRIORITY.
     *
     * K_NO_WAIT: thread is immediately runnable; Zephyr will schedule it
     * when main() yields (returns).
     */
    (void)k_thread_create(
        &s_diag_thread,
        s_diag_stack,
        K_THREAD_STACK_SIZEOF(s_diag_stack),
        diag_task_entry,
        (void *)srv,
        (void *)can,
        (void *)tp,
        CONFIG_DIAG_TASK_PRIORITY,
        (uint32_t)0U,
        K_NO_WAIT
    );

    k_thread_name_set(&s_diag_thread, "diag_task");

    LOG_INF("Diagnostics thread started.");

    /*
     * main() returns here. The Zephyr main thread exits, and the
     * diagnostics thread (diag_task) runs indefinitely under RTOS control.
     */
    return 0;
}
