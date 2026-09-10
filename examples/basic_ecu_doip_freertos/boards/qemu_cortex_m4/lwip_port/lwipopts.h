// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/basic_ecu_doip_freertos/boards/qemu_cortex_m4/lwip_port/
 *       lwipopts.h
 *
 * PURPOSE: lwIP configuration for the FreeRTOS + DoIP example on QEMU
 *          mps2-an386.
 *
 *          Only used by the real-lwIP build (-DLWIP_DIR=/path/to/lwip). The
 *          default build still compiles against ../lwip_stub and never sees
 *          this file.
 *
 *          Sizing rationale: DoIP carries one diagnostic client over a single
 *          TCP connection at a time (DOIP_MAX_CONNECTIONS), so the pools are
 *          deliberately small. They are sized for the UDS worst case — a
 *          0x36 TransferData block — not for throughput.
 *
 * SAFETY  : Example configuration — not safety-assessed.
 * =============================================================================
 */

#ifndef EDS_LWIPOPTS_H
#define EDS_LWIPOPTS_H

/* ---------------------------------------------------------------------------
 * OS mode
 *
 * NO_SYS = 0 is mandatory here, not a preference: transport/doip/
 * freertos_lwip.c is written against the BSD socket API (lwip_socket,
 * lwip_select, lwip_accept...), which only exists in the OS-ful build.
 * ------------------------------------------------------------------------ */

#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1

/* ---------------------------------------------------------------------------
 * Memory
 *
 * MEM_LIBC_MALLOC = 0: lwIP uses its own static heap array rather than
 * newlib's malloc. newlib's _sbrk under --specs=nosys.specs fails, and the
 * project's allocation rules rule out a libc heap regardless.
 * ------------------------------------------------------------------------ */

#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (24 * 1024)

#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_RAW_PCB                0
#define MEMP_NUM_UDP_PCB                4
#define MEMP_NUM_TCP_PCB                6
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                16
#define MEMP_NUM_NETBUF                 8
#define MEMP_NUM_NETCONN                8
#define MEMP_NUM_TCPIP_MSG_API          8
#define MEMP_NUM_TCPIP_MSG_INPKT        16
#define MEMP_NUM_SYS_TIMEOUT            8

#define PBUF_POOL_SIZE                  16
#define PBUF_POOL_BUFSIZE               1536   /* one full Ethernet frame */

/* ---------------------------------------------------------------------------
 * Protocols
 * ------------------------------------------------------------------------ */

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1      /* ping = cheap liveness proof */
#define LWIP_RAW                        0
#define LWIP_UDP                        1      /* DoIP vehicle identification */
#define LWIP_TCP                        1      /* DoIP diagnostic channel     */
#define LWIP_DNS                        0
#define LWIP_IGMP                       0

/* Static addressing — see eds_net.c. QEMU's user-mode (slirp) network hands
 * 10.0.2.15 to the guest and forwards -net user,hostfwd there, so DHCP would
 * only ever hand back the address we already hard-code. Skipping it removes
 * a multi-second, timing-dependent step from every CI run. */
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0

#define LWIP_TCP_KEEPALIVE              1

/* ---------------------------------------------------------------------------
 * TCP tuning
 * ------------------------------------------------------------------------ */

#define TCP_MSS                         1460
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_WND                         (4 * TCP_MSS)

/* ---------------------------------------------------------------------------
 * API layers
 * ------------------------------------------------------------------------ */

#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_SOCKET_SELECT              1      /* freertos_lwip.c uses select */
#define LWIP_SOCKET_POLL                0
#define LWIP_COMPAT_SOCKETS             0      /* keep the lwip_* prefixes    */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0
#define LWIP_NETIF_API                  0
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_TCP_KEEPALIVE              1

/* lwIP declares `struct timeval` itself. newlib-nano's <sys/time.h> does not
 * reliably provide it for this bare-metal multilib — with
 * LWIP_TIMEVAL_PRIVATE = 0 the DoIP binding fails to compile with
 * "storage size of 'tv' isn't known" at transport/doip/freertos_lwip.c:120.
 * Nothing in this build includes <sys/time.h>, so there is no competing
 * definition to collide with. */
#define LWIP_TIMEVAL_PRIVATE            1
#define LWIP_PROVIDE_ERRNO              0

/* ---------------------------------------------------------------------------
 * Threading
 *
 * TCPIP_THREAD_PRIO sits above the DoIP server task (priority 6) so inbound
 * segments are processed promptly even while the DoIP task is runnable.
 * FreeRTOSConfig.h caps priorities at configMAX_PRIORITIES = 8.
 * ------------------------------------------------------------------------ */

#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          2048
#define TCPIP_THREAD_PRIO               7
#define TCPIP_MBOX_SIZE                 16
#define DEFAULT_THREAD_STACKSIZE        1024
#define DEFAULT_THREAD_PRIO             3
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         8

/* ---------------------------------------------------------------------------
 * Callbacks / diagnostics
 * ------------------------------------------------------------------------ */

#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HOSTNAME             1

#define LWIP_STATS                      0
#define LWIP_DEBUG                      0
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_UDP              1

#endif /* EDS_LWIPOPTS_H */
