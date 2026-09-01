// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/freertos/freertos_rng_example.c
 *
 * PURPOSE: Reference TRNG callback for uds_security_algo_set_rng_cb() on
 *          FreeRTOS targets. Wires an on-chip hardware RNG peripheral to the
 *          uds_algo_rng_cb_t contract EDS expects.
 *
 * *** THIS FILE IS AN EXAMPLE, NOT A SHIPPED COMPONENT. ***
 *   - It is NOT added to any CMakeLists.txt / build script. Copy it into
 *     your own application source tree and adapt it to your MCU's actual
 *     TRNG peripheral before use — the STM32 HAL calls below are a worked
 *     example, not a portable driver.
 *   - Every shipped FreeRTOS example (examples/*_freertos/src/main.c) passes
 *     NULL to uds_security_algo_set_rng_cb() and logs a "no TRNG — CI/dev
 *     build" warning. That NULL is intentional for CI/simulation and is NOT
 *     something to leave in a production build — see docs/SECURITY_NOTICE.md.
 *   - This file's Zephyr equivalent does not exist yet either: every shipped
 *     Zephyr example (examples/basic_ecu/src/main.c and siblings) also
 *     passes NULL. If you're integrating on Zephyr, the same requirement
 *     applies — this file's shape (guard against a stub backend, real HAL
 *     calls behind a target-detection macro) is the pattern to follow there
 *     too, just via Zephyr's CONFIG_ENTROPY_* / entropy_get_entropy() API
 *     instead of the STM32 HAL calls shown here.
 *
 * CONTRACT: uds_algo_rng_cb_t (core/uds_security_algo.h)
 *     typedef uds_status_t (*uds_algo_rng_cb_t)(uint8_t *buf, uint8_t len);
 *   Fill buf[0..len) with len bytes of hardware entropy. Return
 *   UDS_STATUS_OK on success, any error status on failure — the caller
 *   (uds_security_algo_generate_seed()) fails the seed request closed on a
 *   non-OK return in a production build; it does not fall back to a weaker
 *   source. See core/uds_security_algo.h's SEC-TRNG-FAILCLOSED-01.
 *
 * REQUIREMENTS (see docs/SECURITY_NOTICE.md before adapting this):
 *   - Hardware TRNG peripheral, not a software PRNG/LFSR/counter.
 *   - At least 64 bits of min-entropy for the 8-byte seed EDS requests
 *     (UDS_SECURITY_SEED_LEN). A 32-bit peripheral output needs 2 draws.
 *   - Fail closed: a peripheral error must return a non-OK status, never
 *     silently substitute a predictable value.
 *
 * WIRING (in your application's init, before uds_generated_init()):
 *     uds_security_algo_set_rng_cb(freertos_rng_example_generate);
 *
 * STANDARD: MISRA C:2012 alignment intended.
 * =============================================================================
 */

#include "uds_security_algo.h"
#include "uds_types.h"

#include <string.h>
#include <stdint.h>

#if defined(STM32H7xx) || defined(STM32H743xx)

/* =============================================================================
 * STM32H7 HAL backend — RNG peripheral (AHB2, on-chip hardware TRNG)
 *
 * Requires: RNG clock enabled and hrng initialised (MX_RNG_Init() or
 * equivalent) before this callback is ever invoked — i.e. before
 * uds_generated_init() runs. This file does not perform that init itself;
 * it assumes your application's platform bring-up already did it, same as
 * freertos_flash_ops.c assumes flash is ready before UDS init.
 * =============================================================================
 */

#include "stm32h7xx_hal.h"

extern RNG_HandleTypeDef hrng; /* Defined by your STM32CubeMX-generated init. */

uds_status_t freertos_rng_example_generate(uint8_t *buf, uint8_t len)
{
    uint8_t  i;
    uint32_t rnd;

    if (buf == NULL) {
        return UDS_STATUS_ERR_NULL_PTR;
    }

    /* Draw 4 bytes per HAL call; UDS_SECURITY_SEED_LEN (8) needs 2 draws. */
    for (i = 0U; i < len; i += 4U) {
        if (HAL_RNG_GenerateRandomNumber(&hrng, &rnd) != HAL_OK) {
            /* Fail closed — never substitute a predictable value here. */
            return UDS_STATUS_ERR_SEC_SEED_UNAVAILABLE;
        }

        {
            uint8_t copy = (uint8_t)((len - i) < 4U ? (len - i) : 4U);
            (void)memcpy(&buf[i], &rnd, copy);
        }
    }

    return UDS_STATUS_OK;
}

#else /* !STM32H7xx */

/* =============================================================================
 * No recognised target backend selected.
 *
 * Deliberately fails to compile rather than silently linking a weak stub —
 * a security-critical entropy source degrading silently is exactly the
 * failure mode docs/SECURITY_NOTICE.md warns about. If you're adapting
 * this file for a different MCU (NXP TRNG, Renesas RSIP-E, etc.), add your
 * own #elif branch above with your peripheral's real API — see the NXP
 * example in docs/SECURITY_NOTICE.md's "Concrete MCU examples" section —
 * and delete this #error once you have.
 * =============================================================================
 */
#error "freertos_rng_example.c: no target TRNG backend selected. This file " \
       "is a worked example (STM32H7 shown) — add your MCU's real TRNG " \
       "peripheral API here, or don't compile this file until you have."

#endif
