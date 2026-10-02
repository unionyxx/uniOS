#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <uapi/syscalls.h>

#define THREAD_DETACHED (1u << 0)

typedef struct thread_attr
{
    uint32_t flags;
    uint32_t priority;
    size_t stack_size;
    void *stack_addr;
} thread_attr_t;

#define MFD_CLOEXEC 0x0001u
#define MFD_ALLOW_SEALING 0x0002u

#define MAP_SHARED 0x01u
#define MAP_PRIVATE 0x02u
#define MAP_ANONYMOUS 0x20u

/* Active NIC kind for NetStatus.nic. */
#define NET_NIC_NONE 0
#define NET_NIC_E1000 1
#define NET_NIC_RTL8139 2

/* Socket states for SYS_SOCKET_STATE. NET_TCP_* values mirror the kernel
 * TcpState order so the dispatcher maps 1:1. */
#define NET_TCP_CLOSED 0
#define NET_TCP_LISTEN 1
#define NET_TCP_SYN_SENT 2
#define NET_TCP_SYN_RECEIVED 3
#define NET_TCP_ESTABLISHED 4
#define NET_TCP_FIN_WAIT_1 5
#define NET_TCP_FIN_WAIT_2 6
#define NET_TCP_CLOSE_WAIT 7
#define NET_TCP_CLOSING 8
#define NET_TCP_LAST_ACK 9
#define NET_TCP_TIME_WAIT 10
#define NET_SOCK_UDP_OPEN 11

typedef struct NetStatus
{
    uint32_t ip; /* host order, LSB = first octet; 0 when unconfigured */
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint8_t link_up;
    uint8_t configured; /* DHCP completed at least once since boot */
    uint8_t nic;        /* NET_NIC_* */
    uint8_t reserved[5];
} NetStatus;

#ifdef __cplusplus
}
#endif
