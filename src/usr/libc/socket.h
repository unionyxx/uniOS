#pragma once
#include <stddef.h>
#include <stdint.h>
#include <uapi/syscalls_ext.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2

typedef uint32_t socklen_t;

struct in_addr
{
    uint32_t s_addr;
};

struct sockaddr
{
    uint16_t sa_family;
    char sa_data[14];
};

struct sockaddr_in
{
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

int socket(int domain, int type, int protocol);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int send(int sockfd, const void *buf, size_t len, int flags);
int recv(int sockfd, void *buf, size_t len, int flags);
int sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
int recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
int closesocket(int sockfd);
int resolve_host(const char *hostname, struct in_addr *out_addr);
int net_status(NetStatus *out);
int socket_state(int sockfd);                                      /* NET_TCP_* / NET_SOCK_UDP_OPEN or -errno */
int ping_host(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms); /* 0 or -errno */
int net_renew(void);                                               /* 0 or -errno */

// Helpers
uint16_t htons(uint16_t hostshort);
uint16_t ntohs(uint16_t netshort);
uint32_t htonl(uint32_t hostlong);
uint32_t ntohl(uint32_t netlong);

#ifdef __cplusplus
}
#endif
