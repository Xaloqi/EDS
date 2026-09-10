// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: .../boards/qemu_cortex_m4/lwip_port/arch/cc.h
 *
 * PURPOSE: lwIP compiler/architecture abstraction for arm-none-eabi-gcc on
 *          Cortex-M4 (little-endian, 32-bit).
 *
 * SAFETY  : Example port layer — not safety-assessed.
 * =============================================================================
 */

#ifndef EDS_LWIP_ARCH_CC_H
#define EDS_LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* Cortex-M is little-endian in every configuration this example supports. */
#define BYTE_ORDER          LITTLE_ENDIAN

/* lwIP uses the C99 <inttypes.h> format macros for its own printf output. */
#define LWIP_NO_INTTYPES_H  0

#define LWIP_CHKSUM_ALGORITHM  2

/* Structure packing — GCC syntax. */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT  __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

/* Diagnostics.
 *
 * LWIP_PLATFORM_DIAG is routed to the board UART so lwIP's own messages land
 * in the same boot log as everything else. LWIP_PLATFORM_ASSERT halts rather
 * than continuing: an assertion failure inside the IP stack leaves it in an
 * undefined state, and a halted CPU is far easier to diagnose from a CI log
 * than a stack that limps on and fails later somewhere unrelated.
 */
void eds_lwip_platform_diag(const char *msg);
void eds_lwip_platform_assert(const char *msg, const char *file, int line);

#define LWIP_PLATFORM_DIAG(x)   do { eds_lwip_platform_diag("lwip"); } while (0)
#define LWIP_PLATFORM_ASSERT(x) \
    do { eds_lwip_platform_assert((x), __FILE__, __LINE__); } while (0)

#endif /* EDS_LWIP_ARCH_CC_H */
