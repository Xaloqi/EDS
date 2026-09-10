// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip_freertos/boards/qemu_cortex_m4/board_uart.h
 *
 * PURPOSE: Boot-log output over the CMSDK APB UART0 of QEMU's mps2-an386.
 *
 *          Exists so the CI leg can produce real, checkable evidence that the
 *          firmware booted, that the Ethernet MAC was identified, and that the
 *          DoIP server reached its listen state — rather than inferring all of
 *          that from whether a TCP connect happened to succeed.
 *
 * SAFETY  : Example diagnostics output — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. No malloc, no recursion.
 * =============================================================================
 */

#ifndef EDS_BOARD_UART_H
#define EDS_BOARD_UART_H

#include <stdint.h>

/** Enable UART0 transmit. Safe to call before the scheduler starts. */
void board_uart_init(void);

/** Write a NUL-terminated string. Blocking, polled; no buffering. */
void board_uart_puts(const char *s);

/** Write @p val as 8 lowercase hex digits, no prefix. */
void board_uart_put_hex32(uint32_t val);

/** Write @p val as unsigned decimal. */
void board_uart_put_u32(uint32_t val);

#endif /* EDS_BOARD_UART_H */
