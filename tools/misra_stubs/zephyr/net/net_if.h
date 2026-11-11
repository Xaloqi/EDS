/* MISRA analysis stub for <zephyr/net/net_if.h> (EDS#583). */
#ifndef MISRA_STUB_ZEPHYR_NET_IF_H
#define MISRA_STUB_ZEPHYR_NET_IF_H

#include <stdint.h>

struct net_if;
struct net_linkaddr { uint8_t *addr; uint8_t len; };

struct net_if       *net_if_get_default(void);
struct net_linkaddr *net_if_get_link_addr(struct net_if *iface);

#endif
