#include "socket.h"

#include <uapi/syscalls.h>

#include "string.h"
#include "syscall.h"

int socket(int domain, int type, int protocol)
{
    return (int)syscall3(SYS_SOCKET, (uint64_t)domain, (uint64_t)type, (uint64_t)protocol);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    if (!addr || addrlen < sizeof(struct sockaddr_in))
        return -1;
    struct sockaddr_in *in = (struct sockaddr_in *)addr;
    return (int)syscall2(SYS_BIND, (uint64_t)sockfd, (uint64_t)ntohs(in->sin_port));
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    if (!addr || addrlen < sizeof(struct sockaddr_in))
        return -1;
    struct sockaddr_in *in = (struct sockaddr_in *)addr;
    return (int)syscall3(SYS_CONNECT, (uint64_t)sockfd, (uint64_t)in->sin_addr.s_addr, (uint64_t)ntohs(in->sin_port));
}

int send(int sockfd, const void *buf, size_t len, int flags)
{
    (void)flags;
    return (int)syscall3(SYS_SEND, (uint64_t)sockfd, (uint64_t)buf, (uint64_t)len);
}

int recv(int sockfd, void *buf, size_t len, int flags)
{
    (void)flags;
    return (int)syscall3(SYS_RECV, (uint64_t)sockfd, (uint64_t)buf, (uint64_t)len);
}

int sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
    if (!dest_addr || addrlen < sizeof(struct sockaddr_in))
        return -1;
    (void)flags;
    struct sockaddr_in *in = (struct sockaddr_in *)dest_addr;
    return (int)syscall6(SYS_SENDTO, (uint64_t)sockfd, (uint64_t)buf, (uint64_t)len, (uint64_t)in->sin_addr.s_addr,
                         (uint64_t)ntohs(in->sin_port), 0);
}

int recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    (void)flags;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;

    int r = (int)syscall6(SYS_RECVFROM, (uint64_t)sockfd, (uint64_t)buf, (uint64_t)len, (uint64_t)&src_ip,
                          (uint64_t)&src_port, 0);

    if (r >= 0 && src_addr) {
        // Build the address in a zeroed local (clears sin_zero), store the port
        // in network order to match bind/connect/sendto, then copy only as much
        // as the caller's buffer allows (POSIX value-result addrlen).
        struct sockaddr_in filled;
        memset(&filled, 0, sizeof(filled));
        filled.sin_family = AF_INET;
        filled.sin_addr.s_addr = src_ip;
        filled.sin_port = htons(src_port);
        socklen_t room = addrlen ? *addrlen : (socklen_t)sizeof(filled);
        socklen_t copy = room < (socklen_t)sizeof(filled) ? room : (socklen_t)sizeof(filled);
        memcpy(src_addr, &filled, copy);
        if (addrlen)
            *addrlen = (socklen_t)sizeof(filled);
    }

    return r;
}

int closesocket(int sockfd)
{
    return (int)syscall1(SYS_CLOSESOCKET, (uint64_t)sockfd);
}

int resolve_host(const char *hostname, struct in_addr *out_addr)
{
    if (!hostname || !out_addr)
        return -1;
    return (int)syscall2(SYS_RESOLVE, (uint64_t)hostname, (uint64_t)out_addr);
}

uint16_t htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}
uint16_t ntohs(uint16_t v)
{
    return htons(v);
}

uint32_t htonl(uint32_t v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
uint32_t ntohl(uint32_t v)
{
    return htonl(v);
}
