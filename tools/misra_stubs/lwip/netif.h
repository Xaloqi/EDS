/* MISRA analysis stub for lwIP <lwip/netif.h> (EDS#583). Declarations only. */
#ifndef MISRA_STUB_LWIP_NETIF_H
#define MISRA_STUB_LWIP_NETIF_H

#include <stdint.h>

struct netif {
    uint8_t hwaddr_len;
    uint8_t hwaddr[6];
};
extern struct netif *netif_default;

#endif
