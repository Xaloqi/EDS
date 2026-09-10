// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/common/boards/qemu_cortex_m4/board_log.c
 *
 * PURPOSE: Polled CMSDK APB UART0 boot log for QEMU mps2-an386.
 *
 *          Register layout verified against QEMU's own device model
 *          (hw/char/cmsdk-apb-uart.c) rather than from datasheet memory,
 *          since QEMU is the only target this driver ever runs on:
 *            DATA     @ 0x00
 *            STATE    @ 0x04  bit 0 = TXFULL
 *            CTRL     @ 0x08  bit 0 = TX_EN, bit 1 = RX_EN
 *            BAUDDIV  @ 0x10  (QEMU warns below 16; the divisor is otherwise
 *                              ignored because the backend is a host chardev)
 *
 *          UART0 base 0x40004000 is from QEMU hw/arm/mps2.c uartbase[0].
 *
 *          This is the same driver basic_ecu_doip_freertos carries locally as
 *          board_uart.c (EDS #267), narrowed to the two entry points the
 *          shared board_log.h declares.
 *
 * SAFETY  : Example diagnostics output — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. No malloc, no recursion.
 * =============================================================================
 */

#include "board_log.h"

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Register map
 * ------------------------------------------------------------------------ */

#define UART0_BASE          (0x40004000UL)

#define UART_DATA_OFFSET    (0x00UL)
#define UART_STATE_OFFSET   (0x04UL)
#define UART_CTRL_OFFSET    (0x08UL)
#define UART_BAUDDIV_OFFSET (0x10UL)

#define UART_STATE_TXFULL   (0x01UL)
#define UART_CTRL_TX_EN     (0x01UL)
#define UART_CTRL_RX_EN     (0x02UL)

/* QEMU's model logs a guest error for BAUDDIV < 16; the value is otherwise
 * unused because the backend is a host character device. */
#define UART_BAUDDIV_MIN    (16UL)

#define UART_REG(off)  (*(volatile uint32_t *)(UART0_BASE + (off)))

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

void board_log_init(void)
{
    UART_REG(UART_BAUDDIV_OFFSET) = UART_BAUDDIV_MIN;
    UART_REG(UART_CTRL_OFFSET)    = UART_CTRL_TX_EN | UART_CTRL_RX_EN;
}

static void board_log_putc(char c)
{
    while ((UART_REG(UART_STATE_OFFSET) & UART_STATE_TXFULL) != 0UL) {
        /* Busy-wait for TX FIFO space. */
    }
    UART_REG(UART_DATA_OFFSET) = (uint32_t)(uint8_t)c;
}

void board_log_puts(const char *s)
{
    const char *p = s;

    if (p == NULL) {
        return;
    }

    while (*p != '\0') {
        board_log_putc(*p);
        p++;
    }
}
