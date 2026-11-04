/*
 * lwip/netif.h — minimal compile-only stub for CI ([EDS#392] EID source).
 * Real LwIP headers replace this on hardware targets.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LWIP_NETIF_STUB_H
#define LWIP_NETIF_STUB_H

#include <stdint.h>

struct netif {
    uint8_t hwaddr[6];
    uint8_t hwaddr_len;
};

/* No interface in a compile-only build: the EID falls back to zeros. */
#define netif_default ((struct netif *)0)

#endif /* LWIP_NETIF_STUB_H */
