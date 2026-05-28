#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <uapi/syscalls.h>

int epoll_create(int size);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif
