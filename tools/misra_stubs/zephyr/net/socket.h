/* MISRA analysis stub for <zephyr/net/socket.h> (EDS#583). Declarations only. */
#ifndef MISRA_STUB_ZEPHYR_SOCKET_H
#define MISRA_STUB_ZEPHYR_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef uint32_t socklen_t;

struct sockaddr;
struct in_addr { uint32_t s_addr; };
struct sockaddr_in {
    uint16_t        sin_family;
    uint16_t        sin_port;
    struct in_addr  sin_addr;
};
struct zsock_pollfd { int fd; short events; short revents; };

int     zsock_socket(int family, int type, int proto);
int     zsock_setsockopt(int sock, int level, int optname, const void *optval, socklen_t optlen);
int     zsock_bind(int sock, const struct sockaddr *addr, socklen_t addrlen);
int     zsock_listen(int sock, int backlog);
int     zsock_accept(int sock, struct sockaddr *addr, socklen_t *addrlen);
int     zsock_poll(struct zsock_pollfd *fds, int nfds, int timeout);
ssize_t zsock_send(int sock, const void *buf, size_t len, int flags);
ssize_t zsock_recv(int sock, void *buf, size_t max_len, int flags);
ssize_t zsock_sendto(int sock, const void *buf, size_t len, int flags,
                     const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t zsock_recvfrom(int sock, void *buf, size_t max_len, int flags,
                       struct sockaddr *src_addr, socklen_t *addrlen);
int     zsock_close(int sock);
uint16_t htons(uint16_t x);

#endif
