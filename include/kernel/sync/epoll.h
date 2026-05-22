#pragma once
#include <stdint.h>
#include <stddef.h>
#include <uapi/syscalls_ext.h>

struct SyscallFrame;

int64_t sys_epoll_create(int size);
int64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int64_t sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
