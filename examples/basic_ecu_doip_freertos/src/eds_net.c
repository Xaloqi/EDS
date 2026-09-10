// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip_freertos/src/eds_net.c
 *
 * PURPOSE: Bring up lwIP over the QEMU mps2-an386 LAN9118 MAC, so the DoIP
 *          server task has a real TCP/IP stack to listen on.
 *
 *          ADDRESSING. QEMU's user-mode ("slirp") network is fixed:
 *            guest    10.0.2.15
 *            gateway  10.0.2.2
 *            netmask  255.255.255.0
 *          and `-net user,hostfwd=tcp::13400-:13400` forwards the host port
 *          to 10.0.2.15:13400 unless told otherwise. The address is therefore
 *          configured statically — DHCP would negotiate for several seconds
 *          only to hand back the value already hard-coded here, and would add
 *          a timing-dependent step to every CI run.
 *
 *          TASK STRUCTURE.
 *            eds_net_start()  — called from main() before the scheduler;
 *                               only creates the bring-up task.
 *            net_setup_task   — runs tcpip_init(), adds the netif, brings it
 *                               up, then becomes the RX poll loop.
 *
 *          The setup task deletes nothing and never returns: after
 *          initialisation it *is* the RX poll task, which avoids a second
 *          task control block and a second stack.
 *
 * SAFETY  : Example application code — not safety-assessed.
 * STANDARD: No recursion. Static netif storage, no malloc.
 * =============================================================================
 */

#include "eds_net.h"
#include "board_uart.h"
#include "ethernetif_lan9118.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/init.h"
#include "netif/ethernet.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------ */

/* QEMU slirp fixed addressing — see file header. */
#define EDS_NET_IP_0        (10U)
#define EDS_NET_IP_1        (0U)
#define EDS_NET_IP_2        (2U)
#define EDS_NET_IP_3        (15U)

#define EDS_NET_GW_3        (2U)

#define EDS_NET_MASK_0      (255U)
#define EDS_NET_MASK_1      (255U)
#define EDS_NET_MASK_2      (255U)
#define EDS_NET_MASK_3      (0U)

/* Locally administered MAC (bit 1 of the first octet set, multicast bit
 * clear). slirp learns the guest address from ARP, so the exact value only
 * needs to be stable and unicast. */
#define EDS_NET_MAC_0       (0x02U)
#define EDS_NET_MAC_1       (0x00U)
#define EDS_NET_MAC_2       (0x00U)
#define EDS_NET_MAC_3       (0xEDU)
#define EDS_NET_MAC_4       (0x50U)
#define EDS_NET_MAC_5       (0x01U)

#define NET_TASK_STACK_WORDS  (1024U)
/* Above the DoIP server task (6) and below tcpip (7): inbound frames must be
 * lifted out of the FIFO promptly, but must not starve the stack thread that
 * consumes them. */
#define NET_TASK_PRIORITY     (6U)

/** RX poll period. 1 tick = 1 ms at configTICK_RATE_HZ = 1000. */
#define NET_POLL_PERIOD_TICKS (1U)

/* ---------------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------------ */

static struct netif   s_netif;
static volatile bool  s_net_ready = false;

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------ */

static void net_log_ipv4(const char *label, const ip4_addr_t *addr)
{
    uint32_t host_order = lwip_ntohl(ip4_addr_get_u32(addr));

    board_uart_puts(label);
    board_uart_put_u32((host_order >> 24U) & 0xFFU);
    board_uart_puts(".");
    board_uart_put_u32((host_order >> 16U) & 0xFFU);
    board_uart_puts(".");
    board_uart_put_u32((host_order >> 8U) & 0xFFU);
    board_uart_puts(".");
    board_uart_put_u32(host_order & 0xFFU);
    board_uart_puts("\r\n");
}

static void net_setup_task(void *pvParameters)
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gateway;

    (void)pvParameters;

    board_uart_puts("[net] tcpip_init...\r\n");

    /* Blocks until lwIP's own thread is running — legal here (scheduler is
     * up), fatal if it had been called from main(). */
    tcpip_init(NULL, NULL);

    board_uart_puts("[net] tcpip up, lwip ");
    board_uart_puts(LWIP_VERSION_STRING);
    board_uart_puts("\r\n");

    IP4_ADDR(&ipaddr,  EDS_NET_IP_0,   EDS_NET_IP_1,   EDS_NET_IP_2,   EDS_NET_IP_3);
    IP4_ADDR(&gateway, EDS_NET_IP_0,   EDS_NET_IP_1,   EDS_NET_IP_2,   EDS_NET_GW_3);
    IP4_ADDR(&netmask, EDS_NET_MASK_0, EDS_NET_MASK_1, EDS_NET_MASK_2, EDS_NET_MASK_3);

    s_netif.hwaddr[0] = EDS_NET_MAC_0;
    s_netif.hwaddr[1] = EDS_NET_MAC_1;
    s_netif.hwaddr[2] = EDS_NET_MAC_2;
    s_netif.hwaddr[3] = EDS_NET_MAC_3;
    s_netif.hwaddr[4] = EDS_NET_MAC_4;
    s_netif.hwaddr[5] = EDS_NET_MAC_5;

    /* tcpip_input (not ethernet_input): the RX poll loop below runs on this
     * task, not on lwIP's thread, so frames must be handed over through the
     * tcpip mailbox rather than processed in place. */
    if (netif_add(&s_netif, &ipaddr, &netmask, &gateway, NULL,
                  lan9118_netif_init, tcpip_input) == NULL) {
        board_uart_puts("[net] FATAL: netif_add failed (no LAN9118?)\r\n");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    netif_set_link_up(&s_netif);

    board_uart_puts("[net] mac  02:00:00:ed:50:01\r\n");
    board_uart_puts("[net] chip id_rev ");
    board_uart_put_hex32(lan9118_id_rev());
    board_uart_puts("\r\n");
    net_log_ipv4("[net] ip   ", &ipaddr);
    net_log_ipv4("[net] mask ", &netmask);
    net_log_ipv4("[net] gw   ", &gateway);

    s_net_ready = true;
    board_uart_puts("[net] netif up — DoIP reachable on port 13400\r\n");

    /* Become the RX poll loop. */
    for (;;) {
        (void)lan9118_poll(&s_netif);
        vTaskDelay((TickType_t)NET_POLL_PERIOD_TICKS);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

void eds_net_start(void)
{
    (void)xTaskCreate(
        net_setup_task,
        "net",
        (configSTACK_DEPTH_TYPE)NET_TASK_STACK_WORDS,
        NULL,
        (UBaseType_t)NET_TASK_PRIORITY,
        NULL);
}

bool eds_net_is_ready(void)
{
    return s_net_ready;
}
