/* MISRA analysis stub for lwIP <lwip/sockets.h> (EDS#583). Declarations only. */
#ifndef MISRA_STUB_LWIP_SOCKETS_H
#define MISRA_STUB_LWIP_SOCKETS_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t socklen_t;

struct sockaddr;
struct in_addr { uint32_t s_addr; };
struct sockaddr_in {
    uint8_t         sin_len;
    uint8_t         sin_family;
    uint16_t        sin_port;
    struct in_addr  sin_addr;
    char            sin_zero[8];
};
struct timeval { long tv_sec; long tv_usec; };
typedef struct { uint32_t fds_bits[2]; } fd_set;

void misra_stub_fd_zero(fd_set *set);
void misra_stub_fd_set(unsigned int fd, fd_set *set);
int  misra_stub_fd_isset(unsigned int fd, const fd_set *set);

#define FD_ZERO(set)        misra_stub_fd_zero(set)
#define FD_SET(fd, set)     misra_stub_fd_set((fd), (set))
#define FD_ISSET(fd, set)   misra_stub_fd_isset((fd), (set))

int      lwip_socket(int domain, int type, int protocol);
int      lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int      lwip_bind(int s, const struct sockaddr *name, socklen_t namelen);
int      lwip_listen(int s, int backlog);
int      lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen);
int      lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset,
                     struct timeval *timeout);
int      lwip_send(int s, const void *dataptr, size_t size, int flags);
int      lwip_recv(int s, void *mem, size_t len, int flags);
int      lwip_sendto(int s, const void *dataptr, size_t size, int flags,
                     const struct sockaddr *to, socklen_t tolen);
int      lwip_recvfrom(int s, void *mem, size_t len, int flags,
                       struct sockaddr *from, socklen_t *fromlen);
int      lwip_close(int s);
uint16_t lwip_htons(uint16_t x);
uint32_t lwip_htonl(uint32_t x);
uint16_t lwip_ntohs(uint16_t x);
uint32_t lwip_ntohl(uint32_t x);

#endif
