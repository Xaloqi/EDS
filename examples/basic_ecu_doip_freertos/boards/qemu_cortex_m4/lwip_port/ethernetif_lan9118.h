// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: .../boards/qemu_cortex_m4/lwip_port/ethernetif_lan9118.h
 *
 * PURPOSE: lwIP netif driver for the SMSC LAN9118 Ethernet controller
 *          emulated by QEMU's mps2-an386 machine.
 *
 * SAFETY  : Example driver — not safety-assessed.
 * =============================================================================
 */

#ifndef EDS_ETHERNETIF_LAN9118_H
#define EDS_ETHERNETIF_LAN9118_H

#include "lwip/err.h"
#include "lwip/netif.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * netif init callback — pass to netif_add().
 *
 * Resets the controller, verifies it is actually a LAN9118, programs the MAC
 * address from @p netif->hwaddr and enables TX/RX.
 *
 * @return ERR_OK on success, ERR_IF if no LAN9118 responds at the expected
 *         address.
 */
err_t lan9118_netif_init(struct netif *netif);

/**
 * Drain every complete frame currently in the RX FIFO into lwIP.
 *
 * Must be called from a task context (it calls netif->input, which posts to
 * the tcpip mailbox). The driver is deliberately polled rather than
 * interrupt-driven — see the .c file for why.
 *
 * @return number of frames handed to lwIP.
 */
uint32_t lan9118_poll(struct netif *netif);

/** Chip ID/revision read during init, for boot logging. 0 if init failed. */
uint32_t lan9118_id_rev(void);

#endif /* EDS_ETHERNETIF_LAN9118_H */
