// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/safeboot_freertos_ecu/boards/nucleo_h743zi/board_log.c
 *
 * PURPOSE: board_log.h implementation for the NUCLEO-H743ZI2 — deliberately
 *          a no-op.
 *
 *          src/main.c is shared between this board and qemu_cortex_m4, and
 *          calls board_log_init() / board_log_puts() unconditionally so the
 *          QEMU CI leg can assert on real boot output. This board therefore
 *          has to provide the symbols. It provides them as no-ops rather than
 *          as a USART3 driver, on purpose:
 *
 *          A working console here means bringing up USART3 (PD8/PD9, the
 *          ST-Link virtual COM port — see boards/nucleo_h743zi/
 *          nucleo_h743zi.overlay), which in turn means committing to a
 *          specific RCC clock tree to get the baud divisor right. The H743
 *          comes out of reset on HSI at 64 MHz, but this example's
 *          FreeRTOSConfig.h declares configCPU_CLOCK_HZ = 400 MHz, i.e. it
 *          assumes a customer-supplied PLL setup this file cannot see. Baud
 *          rate written against a guessed kernel clock produces garbage on
 *          the wire, and there was no NUCLEO-H743ZI2 available to test it
 *          against. Shipping an unverified UART driver that looks correct is
 *          worse than shipping nothing: it would be the same class of defect
 *          as the empty binaries this change exists to fix (EDS issue #268) —
 *          something that builds clean and does not work.
 *
 *          TO ENABLE: implement these two functions against USART3 using your
 *          project's clock configuration, or route them to your existing
 *          logging backend. No other file needs to change.
 *
 * SAFETY  : Example diagnostics output — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. No malloc, no recursion.
 * =============================================================================
 */

#include "board_log.h"

void board_log_init(void)
{
    /* Intentionally empty — see the file header. */
}

void board_log_puts(const char *s)
{
    (void)s;   /* Intentionally discarded — see the file header. */
}
