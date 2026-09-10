// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip_freertos/boards/qemu_cortex_m4/board_uart.c
 *
 * PURPOSE: Polled CMSDK APB UART0 driver for QEMU mps2-an386 boot logging.
 *
 *          Register layout verified against QEMU's own device model
 *          (hw/char/cmsdk-apb-uart.c, v7.0.0) rather than from datasheet
 *          memory, since QEMU is the only target this driver ever runs on:
 *            DATA     @ 0x00
 *            STATE    @ 0x04  bit 0 = TXFULL
 *            CTRL     @ 0x08  bit 0 = TX_EN, bit 1 = RX_EN
 *            BAUDDIV  @ 0x10  (QEMU warns below 16; the divisor is otherwise
 *                              ignored because the backend is a host chardev)
 *
 *          UART0 base 0x40004000 is from hw/arm/mps2.c uartbase[0].
 *
 * SAFETY  : Example diagnostics output — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. No malloc, no recursion.
 * =============================================================================
 */

#include "board_uart.h"

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

#define DECIMAL_BASE   (10U)
#define HEX_DIGITS     (8U)
#define NIBBLE_BITS    (4U)
#define NIBBLE_MASK    (0x0FU)

/* uint32_t max is 4294967295 — 10 digits plus terminator. */
#define U32_DEC_DIGITS_MAX (10U)

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

void board_uart_init(void)
{
    UART_REG(UART_BAUDDIV_OFFSET) = UART_BAUDDIV_MIN;
    UART_REG(UART_CTRL_OFFSET)    = UART_CTRL_TX_EN | UART_CTRL_RX_EN;
}

static void board_uart_putc(char c)
{
    while ((UART_REG(UART_STATE_OFFSET) & UART_STATE_TXFULL) != 0UL) {
        /* Busy-wait for TX FIFO space. */
    }
    UART_REG(UART_DATA_OFFSET) = (uint32_t)(uint8_t)c;
}

void board_uart_puts(const char *s)
{
    const char *p = s;

    if (p == NULL) {
        return;
    }

    while (*p != '\0') {
        board_uart_putc(*p);
        p++;
    }
}

void board_uart_put_hex32(uint32_t val)
{
    static const char digits[] = "0123456789abcdef";
    uint32_t          shift    = HEX_DIGITS * NIBBLE_BITS;

    while (shift > 0U) {
        shift -= NIBBLE_BITS;
        board_uart_putc(digits[(val >> shift) & NIBBLE_MASK]);
    }
}

void board_uart_put_u32(uint32_t val)
{
    char     buf[U32_DEC_DIGITS_MAX];
    uint32_t v   = val;
    uint32_t len = 0U;

    if (v == 0U) {
        board_uart_putc('0');
        return;
    }

    while ((v > 0U) && (len < U32_DEC_DIGITS_MAX)) {
        buf[len] = (char)('0' + (char)(v % DECIMAL_BASE));
        v       /= DECIMAL_BASE;
        len++;
    }

    while (len > 0U) {
        len--;
        board_uart_putc(buf[len]);
    }
}
