// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip_freertos/src/eds_net.h
 *
 * PURPOSE: IP stack bring-up for the FreeRTOS + DoIP example on QEMU.
 *
 *          Only compiled in the real-lwIP build (-DLWIP_DIR=...).
 *
 * SAFETY  : Example application code — not safety-assessed.
 * =============================================================================
 */

#ifndef EDS_NET_H
#define EDS_NET_H

#include <stdbool.h>

/**
 * Create the network bring-up task.
 *
 * Only creates the task; it must not do the work inline. lwIP's tcpip_init()
 * blocks on a semaphore until its own thread signals back, which cannot
 * happen before vTaskStartScheduler(). Called from main() before the
 * scheduler starts.
 */
void eds_net_start(void);

/** True once the netif is up and sockets can be opened. */
bool eds_net_is_ready(void);

#endif /* EDS_NET_H */
