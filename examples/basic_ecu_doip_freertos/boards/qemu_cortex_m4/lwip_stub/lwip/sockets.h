/*
 * lwip/sockets.h — minimal compile-only stub for CI.
 * Real LwIP headers replace this on hardware targets.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LWIP_SOCKETS_STUB_H
#define LWIP_SOCKETS_STUB_H

#include <stdint.h>
#include <stddef.h>

/* Types */
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
typedef uint32_t socklen_t;

struct in_addr  { in_addr_t s_addr; };
struct sockaddr { uint8_t sa_data[14]; };
struct sockaddr_in {
    uint8_t        sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

typedef struct { long tv_sec; long tv_usec; } struct_timeval;
#define timeval struct_timeval

typedef struct { unsigned long fds_bits[1]; } fd_set;
#define FD_ZERO(s)       do { (s)->fds_bits[0] = 0UL; } while (0)
#define FD_SET(fd, s)    do { (s)->fds_bits[0] |= (1UL << (unsigned)(fd)); } while (0)

/* Constants */
#define AF_INET        2
#define SOCK_STREAM    1
#define IPPROTO_TCP    6
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define INADDR_ANY     0U

/* Stubs — never called in compile-only build */
static inline uint16_t lwip_htons(uint16_t x) { return x; }
static inline int lwip_socket(int d, int t, int p)          { (void)d;(void)t;(void)p; return -1; }
static inline int lwip_setsockopt(int s, int l, int o, const void *v, socklen_t sl)
                                                             { (void)s;(void)l;(void)o;(void)v;(void)sl; return -1; }
static inline int lwip_bind(int s, const struct sockaddr *a, socklen_t sl)
                                                             { (void)s;(void)a;(void)sl; return -1; }
static inline int lwip_listen(int s, int b)                  { (void)s;(void)b; return -1; }
static inline int lwip_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t)
                                                             { (void)n;(void)r;(void)w;(void)e;(void)t; return -1; }
static inline int lwip_accept(int s, struct sockaddr *a, socklen_t *sl)
                                                             { (void)s;(void)a;(void)sl; return -1; }
static inline int lwip_send(int s, const void *d, size_t l, int f)
                                                             { (void)s;(void)d;(void)l;(void)f; return -1; }
static inline int lwip_recv(int s, void *d, size_t l, int f)
                                                             { (void)s;(void)d;(void)l;(void)f; return -1; }
static inline int lwip_close(int s)                          { (void)s; return -1; }

#endif /* LWIP_SOCKETS_STUB_H */
