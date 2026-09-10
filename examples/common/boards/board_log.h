// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/common/boards/board_log.h
 *
 * PURPOSE: Minimal board-provided boot log, used by the FreeRTOS examples to
 *          emit checkable evidence that the firmware actually booted and
 *          reached vTaskStartScheduler().
 *
 *          It exists because "the build succeeded" was, for a long time, the
 *          only signal these examples produced — and an empty binary builds
 *          perfectly well (EDS issue #268). A boot banner on the console turns
 *          a compile-only CI leg into one that can assert on real output.
 *
 *          Every board that a FreeRTOS example targets provides an
 *          implementation of this header. Where a board has no console the
 *          implementation is an explicit no-op, so example code never needs a
 *          conditional around a log call.
 *
 * SAFETY  : Example diagnostics output — not safety-assessed.
 * STANDARD: MISRA C:2012 alignment intended. No malloc, no recursion.
 * =============================================================================
 */

#ifndef EDS_BOARD_LOG_H
#define EDS_BOARD_LOG_H

/**
 * Bring the board console up. Safe to call before the scheduler starts, and
 * safe to call on a board with no console (the call is then a no-op).
 */
void board_log_init(void);

/**
 * Write a NUL-terminated string to the board console. Blocking and polled;
 * no buffering, no interrupts, no dynamic memory. A NULL argument is ignored.
 */
void board_log_puts(const char *s);

#endif /* EDS_BOARD_LOG_H */
