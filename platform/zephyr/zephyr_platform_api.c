// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/zephyr_platform_api.c
 *
 * PURPOSE: Implements platform/platform_api.h using Zephyr RTOS APIs.
 *
 *          This file provides the eds_platform_* functions for Zephyr builds.
 *          eds_platform_init() is a thin wrapper on Zephyr — DTS and Kconfig
 *          wire CAN, and the built-in NVM backend is the default — but
 *          [#304] Stage 1 it DOES forward cfg->nvm to
 *          platform/zephyr/nvm_store.c's zephyr_nvm_store_register_ops(),
 *          so an application that supplies ops there gets them honoured
 *          instead of the built-in backend, exactly as
 *          platform/freertos/freertos_platform_api.c's eds_platform_init()
 *          already does for FreeRTOS. Passing cfg as NULL (every Zephyr
 *          example as of this Stage's introduction does not call this
 *          function at all) keeps the original Zephyr behaviour unchanged.
 *          The reset and NVM flush implementations are extracted from the
 *          existing zephyr_port.c logic.
 *
 *          RELATIONSHIP TO zephyr_port.c:
 *            zephyr_port.c  — Zephyr-specific startup (CAN init, port init).
 *                             Kept as-is for the Zephyr integration layer.
 *            This file       — eds_platform_* neutral interface implementations.
 *                             Called from platform-neutral code paths.
 *
 * SAFETY  : ASIL-B candidate. Reset and NVM flush are safety-relevant.
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "platform_api.h"
#include "uds_types.h"
#include "nvm_store.h"
#include "dtc_mirror.h"
#include "uds_session_stats.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

/* ECUReset is performed with sys_reboot(). EDS's Kconfig turns REBOOT on for
 * every Zephyr build; refuse to build if it was turned off, rather than fail
 * at link time (EDS#511). */
#if !defined(CONFIG_REBOOT)
#error "EDS on Zephyr needs CONFIG_REBOOT=y: ECUReset is performed with sys_reboot() (EDS#511)."
#endif

#include <string.h>

LOG_MODULE_REGISTER(zephyr_platform_api, LOG_LEVEL_INF);

/* [#498] A conf that asks for newlib marks the request with
 * CONFIG_EDS_REQUIRE_NEWLIB. Kconfig drops CONFIG_NEWLIB_LIBC=y with only a
 * warning when the toolchain has no newlib (Zephyr SDK 1.0.x), and the build
 * silently links picolibc. Every Zephyr EDS build compiles this file, so it
 * is where the swap is refused. */
#if defined(CONFIG_EDS_REQUIRE_NEWLIB) && !defined(CONFIG_NEWLIB_LIBC) && \
    !defined(CONFIG_ARCH_POSIX)
/* CONFIG_ARCH_POSIX (native_sim) is exempt: its host toolchain never has
 * newlib (TOOLCHAIN_HAS_NEWLIB is false), and a simulation binary is never
 * shipped. Its confs state CONFIG_PICOLIBC=y explicitly instead. */
#error "CONFIG_EDS_REQUIRE_NEWLIB=y but the C library is not newlib: this toolchain has no newlib (Zephyr SDK 1.0.x ships none). To accept picolibc, set CONFIG_NEWLIB_LIBC=n and CONFIG_EDS_REQUIRE_NEWLIB=n - see issue #498."
#endif

/* A conf that asks for compiler stack canaries marks the request with
 * CONFIG_EDS_REQUIRE_STACK_CANARIES. Refuse every configuration in which
 * the request is not met for real: dropped by Kconfig (no entropy driver),
 * satisfied only by TEST_RANDOM_GENERATOR, or seeded by an entropy driver
 * that is not usable when the kernel seeds __stack_chk_guard. Zephyr's
 * z_early_rand_get() runs after PRE_KERNEL_2 and needs
 * entropy_get_entropy_isr(). virtio-rng initialises at POST_KERNEL and has
 * no ISR API (-ENOSYS), so its canary would be a k_cycle_get_32()-derived
 * value. native_sim (ARCH_POSIX) is exempt as a simulation target. */
/* [#501] On Zephyr 4.x the canary levels are members of an optional choice
 * (STACK_CANARIES, _STRONG, _ALL, _EXPLICIT); each selects
 * REQUIRES_STACK_CANARIES. On 3.7 only STACK_CANARIES exists. Either means
 * the compiler emits canaries. */
#if defined(CONFIG_STACK_CANARIES) || defined(CONFIG_REQUIRES_STACK_CANARIES)
#define EDS_STACK_CANARIES_ON 1
#endif

#if defined(CONFIG_EDS_REQUIRE_STACK_CANARIES) && !defined(CONFIG_ARCH_POSIX)
#if !defined(EDS_STACK_CANARIES_ON)
#error "CONFIG_EDS_REQUIRE_STACK_CANARIES=y but Kconfig dropped CONFIG_STACK_CANARIES (no entropy driver on this board). Enable an entropy source, or set CONFIG_STACK_CANARIES=n and CONFIG_EDS_REQUIRE_STACK_CANARIES=n explicitly."
#elif !defined(CONFIG_ENTROPY_HAS_DRIVER)
#error "CONFIG_EDS_REQUIRE_STACK_CANARIES=y but the canary is seeded only by TEST_RANDOM_GENERATOR (no entropy driver): it is predictable. Enable a real entropy source, or opt out explicitly."
#elif DT_HAS_CHOSEN(zephyr_entropy) && DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_entropy), virtio_device4)
#error "CONFIG_EDS_REQUIRE_STACK_CANARIES=y but zephyr,entropy is virtio-rng, which initialises at POST_KERNEL with no ISR API: the kernel seeds the canary before it, from k_cycle_get_32(). Use an early entropy source for canaries, or opt out explicitly."
#endif
#endif

/* [#501] Not opt-out-able: canaries that are on while zephyr,entropy is
 * virtio-rng are always seeded by the k_cycle_get_32() fallback, so no
 * configuration makes them meaningful. This also catches the Zephyr 4.x trap
 * where a board opts out with CONFIG_STACK_CANARIES=n after an earlier
 * fragment set =y: the choice stays in y-mode, canaries stay on, and
 * EDS_REQUIRE_STACK_CANARIES=n would otherwise let it through silently. */
#if defined(EDS_STACK_CANARIES_ON) && !defined(CONFIG_ARCH_POSIX) && \
    DT_HAS_CHOSEN(zephyr_entropy) && DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_entropy), virtio_device4)
#error "Stack canaries are enabled but zephyr,entropy is virtio-rng: the canary would be seeded from k_cycle_get_32(), with or without CONFIG_EDS_REQUIRE_STACK_CANARIES. Turn canaries off in the final .config. On Zephyr 4.x, CONFIG_STACK_CANARIES=n does not undo an earlier fragment's =y (choice member): remove the =y instead. See issue #501."
#endif

/* Forward declaration — implemented in platform/zephyr/nvm_store.c. Mirrors
 * platform/freertos/freertos_platform_api.c's identical forward
 * declaration of freertos_nvm_register_ops(). [#304] Stage 1. */
void zephyr_nvm_store_register_ops(const eds_nvm_ops_t *ops);

/* --------------------------------------------------------------------------
 * Reset sub-function values (ISO 14229-1:2026 §9.3.2.2)
 * -------------------------------------------------------------------------- */
#define ECU_RESET_HARD_RESET   (0x01U)
#define ECU_RESET_KEY_OFF_ON   (0x02U)
#define ECU_RESET_SOFT_RESET   (0x03U)

/* ============================================================================
 * eds_platform_init
 * ========================================================================== */

uds_status_t eds_platform_init(const eds_platform_cfg_t *cfg)
{
    /*
     * Zephyr: CAN is configured via DTS (DEVICE_DT_GET / can0 alias),
     * independently of this function — cfg->can_send and the uds_task_*
     * fields remain unused here.
     *
     * [#304] Stage 1: cfg->nvm IS forwarded to
     * platform/zephyr/nvm_store.c's zephyr_nvm_store_register_ops(), so an
     * application that populates it gets those ops honoured by
     * nvm_store_init()/_write()/_read()/_delete()/_erase_all()/_is_ready()
     * in place of the built-in NVS backend — see eds_nvm_ops_t's doc
     * comment, platform_api.h. Passing cfg as NULL here (as every Zephyr
     * example does before this Stage) registers NULL ops, which is exactly
     * this platform's original, unchanged behaviour.
     */
    zephyr_nvm_store_register_ops((cfg != NULL) ? &cfg->nvm : NULL);
    return UDS_STATUS_OK;
}

/* ============================================================================
 * eds_platform_ecu_reset
 * ========================================================================== */

uds_status_t eds_platform_ecu_reset(uint8_t reset_type)
{
    /*
     * SAFETY (ISO 14229-1:2026 §9.3.1):
     *   The caller MUST have already:
     *     1. Transmitted the positive response frame via ISO-TP.
     *     2. Waited for N_As TX confirmation.
     *     3. Called eds_platform_nvm_flush().
     *   This function must NOT return on a valid reset type.
     */
    switch (reset_type) {

        case ECU_RESET_HARD_RESET:
            LOG_INF("ECU reset: hard reset (cold).");
            k_msleep(5);   /* Allow LOG flush before reset. */
            sys_reboot(SYS_REBOOT_COLD);
            break;   /* Unreachable — sys_reboot() does not return. */

        case ECU_RESET_KEY_OFF_ON:
            /*
             * Key-off/on: platform-specific power sequencing.
             * Default: cold reboot. OEM integrators replace sys_reboot with
             * their PMIC/power-sequencer command here.
             * TODO [APPLICATION]: Replace with OEM power sequencer.
             */
            LOG_INF("ECU reset: key-off/on (fallback: cold reboot).");
            k_msleep(5);
            sys_reboot(SYS_REBOOT_COLD);
            break;   /* Unreachable */

        case ECU_RESET_SOFT_RESET:
            /*
             * Warm reset — preserves some peripheral state on supported SoCs.
             * Zephyr falls back to cold reset on platforms without warm reset.
             */
            LOG_INF("ECU reset: soft reset (warm).");
            k_msleep(5);
            sys_reboot(SYS_REBOOT_WARM);
            break;   /* Unreachable */

        default:
            /*
             * service_0x11.c validates reset_type before setting
             * pending_reset_type, so this path should never be reached in
             * production. Return an error so the caller can log it.
             */
            LOG_WRN("ECU reset: unsupported type 0x%02X.", (unsigned)reset_type);
            return UDS_STATUS_ERR_PLATFORM;
    }

    /* Unreachable — sys_reboot() does not return. Defensive return. */
    return UDS_STATUS_ERR_PLATFORM;
}

/* ============================================================================
 * eds_platform_nvm_flush
 * ========================================================================== */

uds_status_t eds_platform_nvm_flush(void)
{
    uds_status_t rc;
    uds_status_t final_rc = UDS_STATUS_OK;

    LOG_INF("NVM flush: persisting diagnostic counters before reset.");

    /*
     * [FLUSH-01] DTC status-byte mirror.
     * Serialises all registered DTC status bytes to a single NVS record.
     * Failure: DTC history is lost on reset — logged but non-fatal.
     */
    rc = dtc_mirror_flush_all();
    if (rc != UDS_STATUS_OK) {
        LOG_ERR("NVM flush: DTC mirror write failed (rc=0x%02X)", (unsigned)rc);
        final_rc = rc;
    } else {
        LOG_DBG("NVM flush: DTC mirror OK");
    }

    /*
     * [FLUSH-02] ECU lifecycle counter.
     * Incremented on every ECU reset. Informational only — not safety-relevant.
     */
    if (nvm_store_is_ready()) {
        uint32_t lifecycle_cnt = 0U;
        size_t   read_len      = 0U;

        rc = nvm_store_read(
            (uint16_t)NVM_KEY_LIFECYCLE_CNT,
            &lifecycle_cnt, sizeof(lifecycle_cnt), &read_len);

        if ((rc == UDS_STATUS_OK) || (rc == UDS_STATUS_ERR_DID_NOT_FOUND)) {
            lifecycle_cnt++;
            rc = nvm_store_write(
                (uint16_t)NVM_KEY_LIFECYCLE_CNT,
                &lifecycle_cnt, sizeof(lifecycle_cnt));

            if (rc == UDS_STATUS_OK) {
                LOG_DBG("NVM flush: lifecycle counter = %u",
                        (unsigned)lifecycle_cnt);
            } else {
                LOG_WRN("NVM flush: lifecycle counter write failed (rc=0x%02X)",
                        (unsigned)rc);
            }
        }
    }

    /*
     * [FLUSH-03] Security attempt counter.
     * Written eagerly on each failed attempt (uds_security_send_key →
     * nvm_save_cb). Already current in NVM — no flush needed here.
     */

    /*
     * [FLUSH-04] Session statistics.
     * Dirty-flagged internally — only writes if data changed since last flush.
     */
    rc = uds_session_stats_flush();
    if (rc != UDS_STATUS_OK) {
        LOG_WRN("NVM flush: session stats write failed (rc=0x%02X)", (unsigned)rc);
        /* Non-fatal — informational counters only. */
    } else {
        LOG_DBG("NVM flush: session stats OK");
    }

    return final_rc;
}

/* ============================================================================
 * eds_platform_uptime_ms
 * ========================================================================== */

uint32_t eds_platform_uptime_ms(void)
{
    return (uint32_t)k_uptime_get_32();
}

/* ============================================================================
 * eds_platform_can_input
 * Not used on Zephyr — CAN RX is handled internally by zephyr_can.c via
 * the Zephyr CAN driver callback and k_msgq. This stub satisfies the linker
 * if platform_api.h is included in a Zephyr translation unit.
 * ========================================================================== */

void eds_platform_can_input(const eds_can_frame_t *frame)
{
    /*
     * Zephyr: no-op. Zephyr CAN driver calls rx_callback() → k_msgq_put()
     * directly. This function exists only to satisfy the platform_api.h
     * contract for FreeRTOS builds. It is never called on Zephyr.
     */
    (void)frame;
}
