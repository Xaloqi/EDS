/* MISRA analysis stub for <zephyr/net/net_mgmt.h> (EDS#583). */
#ifndef MISRA_STUB_ZEPHYR_NET_MGMT_H
#define MISRA_STUB_ZEPHYR_NET_MGMT_H

#include <stdint.h>

struct net_if;
struct net_mgmt_event_callback { uint32_t event_mask; };

void net_mgmt_init_event_callback(struct net_mgmt_event_callback *cb,
                                  void (*handler)(struct net_mgmt_event_callback *, uint64_t,
                                                  struct net_if *),
                                  uint64_t mgmt_event_mask);
void net_mgmt_add_event_callback(struct net_mgmt_event_callback *cb);

#endif
