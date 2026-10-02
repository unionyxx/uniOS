#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <uapi/syscalls.h>

#define THREAD_DETACHED (1u << 0)
#define THREAD_INHERIT_FDS (1u << 1)

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
