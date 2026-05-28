#include "unistd.h"

#include <stddef.h>
#include <stdint.h>
#include <uapi/fs.h>
#include <uapi/syscalls.h>
#include <sys/epoll.h>
#include <sys/mman.h>

#include "syscall.h"


void exit(int status)
{
    syscall1(SYS_EXIT, (uint64_t)status);
    while (1)
        ; // Should never reach here
}

int write(int fd, const void *buf, size_t count)
{
    return (int)syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, count);
}

int read(int fd, void *buf, size_t count)
{
    return (int)syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, count);
}

int open(const char *pathname, int flags)
{
    return (int)syscall2(SYS_OPEN, (uint64_t)pathname, (uint64_t)flags);
}

void close(int fd)
{
    syscall1(SYS_CLOSE, (uint64_t)fd);
}

int fork(void)
{
    return (int)syscall1(SYS_FORK, 0);
}

int exec(const char *path)
{
    return (int)syscall1(SYS_EXEC, (uint64_t)path);
}

int waitpid(int pid, int *status)
{
    return (int)syscall3(SYS_WAIT4, (uint64_t)pid, (uint64_t)status, 0);
}

int waitpid_nohang(int pid, int *status)
{
    return (int)syscall3(SYS_WAIT4, (uint64_t)pid, (uint64_t)status, WNOHANG);
}

int mkdir(const char *pathname)
{
    return (int)syscall1(SYS_MKDIR, (uint64_t)pathname);
}

int unlink(const char *pathname)
{
    return (int)syscall1(SYS_UNLINK, (uint64_t)pathname);
}

int rmdir(const char *pathname)
{
    return (int)syscall1(SYS_RMDIR, (uint64_t)pathname);
}

int rename(const char *oldpath, const char *newpath)
{
    return (int)syscall2(SYS_RENAME, (uint64_t)oldpath, (uint64_t)newpath);
}

void yield(void)
{
    syscall1(SYS_YIELD, 0);
}

int dup2(int oldfd, int newfd)
{
    return (int)syscall2(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd);
}

int pipe(int pipefd[2])
{
    return (int)syscall1(SYS_PIPE, (uint64_t)pipefd);
}

int get_procs(struct ProcessInfo *out, size_t max_count)
{
    return (int)syscall2(SYS_GETPROCS, (uint64_t)out, (uint64_t)max_count);
}

int stat(const char *path, struct VNodeStat *buf)
{
    return (int)syscall2(SYS_STAT, (uint64_t)path, (uint64_t)buf);
}

int get_volumes(struct VolumeInfo *out, size_t max_count)
{
    return (int)syscall2(SYS_GETVOLUMES, (uint64_t)out, (uint64_t)max_count);
}

int get_storage_mode(void)
{
    return (int)syscall1(SYS_STORAGE_GET_MODE, 0);
}

int set_storage_mode(int mode)
{
    return (int)syscall1(SYS_STORAGE_SET_MODE, (uint64_t)mode);
}

int get_time(struct SysTime *time)
{
    return (int)syscall1(SYS_GETTIME, (uint64_t)time);
}

uint64_t get_uptime(void)
{
    return syscall1(SYS_GETUPTIME, 0);
}

uint64_t get_ticks(void)
{
    return syscall1(SYS_GET_TICKS, 0);
}

void sleep_ms(uint32_t ms)
{
    uint64_t start = get_ticks();
    while (get_ticks() < start + ms) {
        yield();
    }
}

int get_meminfo(struct MemInfo *info)
{
    return (int)syscall1(SYS_GETMEMINFO, (uint64_t)info);
}

int get_sysinfo(struct SystemProfile *info)
{
    return (int)syscall1(SYS_GETSYSINFO, (uint64_t)info);
}

int fb_info(uint32_t *info)
{
    return (int)syscall1(SYS_FB_INFO, (uint64_t)info);
}

void *fb_mmap(void)
{
    return (void *)syscall1(SYS_FB_MMAP, 0);
}

void fb_flush(void)
{
    syscall1(SYS_FB_FLUSH, 0);
}

void fb_blit(const void *buffer, uint32_t size)
{
    syscall2(SYS_FB_BLIT, (uint64_t)buffer, (uint64_t)size);
}

void fb_blit_rect(const void *buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride)
{
    syscall6(SYS_FB_BLIT_RECT, (uint64_t)buffer, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, (uint64_t)stride);
}

int display_get_caps(struct DisplayCaps *caps)
{
    return (int)syscall1(SYS_DISPLAY_GET_CAPS, (uint64_t)caps);
}

int display_get_status(struct DisplayStatus *status)
{
    return (int)syscall1(SYS_DISPLAY_GET_STATUS, (uint64_t)status);
}

int display_query_connectors(struct DisplayConnectorInfo *out, size_t max_count)
{
    return (int)syscall2(SYS_DISPLAY_QUERY_CONNECTORS, (uint64_t)out, (uint64_t)max_count);
}

int display_get_modes(uint32_t connector_id, struct DisplayMode *out, size_t max_count)
{
    return (int)syscall3(SYS_DISPLAY_GET_MODES, (uint64_t)connector_id, (uint64_t)out, (uint64_t)max_count);
}

int display_set_mode(const struct DisplayMode *mode)
{
    return (int)syscall1(SYS_DISPLAY_SET_MODE, (uint64_t)mode);
}

int display_atomic_commit(const struct DisplayAtomicRequest *request)
{
    return (int)syscall1(SYS_DISPLAY_ATOMIC_COMMIT, (uint64_t)request);
}

int display_buffer_create(struct DisplayBufferCreate *request)
{
    return (int)syscall1(SYS_DISPLAY_BUFFER_CREATE, (uint64_t)request);
}

int display_buffer_map(struct DisplayBufferMap *request)
{
    return (int)syscall1(SYS_DISPLAY_BUFFER_MAP, (uint64_t)request);
}

int display_buffer_destroy(DisplayBufferHandle handle)
{
    return (int)syscall1(SYS_DISPLAY_BUFFER_DESTROY, (uint64_t)handle);
}

uint32_t display_present(const struct DisplayPresentRequest *req)
{
    return (uint32_t)syscall1(SYS_DISPLAY_PRESENT, (uint64_t)req);
}

uint32_t display_compose_submit(const struct DisplayComposeRequest *req)
{
    return (uint32_t)syscall1(SYS_DISPLAY_COMPOSE_SUBMIT, (uint64_t)req);
}

uint32_t display_wait(void)
{
    return (uint32_t)syscall1(SYS_DISPLAY_WAIT, 0);
}

int display_wait_event(struct DisplayEvent *event)
{
    return (int)syscall2(SYS_DISPLAY_EVENT_WAIT, (uint64_t)event, 1);
}

int display_poll_event(struct DisplayEvent *event)
{
    return (int)syscall2(SYS_DISPLAY_EVENT_WAIT, (uint64_t)event, 0);
}

uint64_t get_tsc_freq(void)
{
    return syscall1(SYS_GET_TSC_FREQ, 0);
}

int getrandom(void *buf, size_t len)
{
    return (int)syscall2(SYS_GETRANDOM, (uint64_t)buf, (uint64_t)len);
}

int get_event(struct Event *ev)
{
    return poll_event(ev);
}

int wait_event(struct Event *ev)
{
    return (int)syscall2(SYS_GET_EVENT, (uint64_t)ev, 1);
}

int poll_event(struct Event *ev)
{
    return (int)syscall2(SYS_GET_EVENT, (uint64_t)ev, 0);
}

void sound_play(const char *path)
{
    syscall1(SYS_SOUND_PLAY, (uint64_t)path);
}

void sound_write(const void *data, uint32_t size)
{
    syscall2(SYS_SOUND_WRITE, (uint64_t)data, (uint64_t)size);
}

void sound_config(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample)
{
    syscall3(SYS_SOUND_CONFIG, (uint64_t)sample_rate, (uint64_t)channels, (uint64_t)bits_per_sample);
}

extern void __sigret(void);

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    if (act) {
        struct sigaction k_act = *act;
        k_act.sa_restorer = __sigret;
        return (int)syscall3(SYS_SIGACTION, (uint64_t)signum, (uint64_t)&k_act, (uint64_t)oldact);
    }
    return (int)syscall3(SYS_SIGACTION, (uint64_t)signum, 0, (uint64_t)oldact);
}

int epoll_create(int size)
{
    return (int)syscall1(SYS_EPOLL_CREATE, (uint64_t)size);
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    return (int)syscall6(SYS_EPOLL_CTL, (uint64_t)epfd, (uint64_t)op, (uint64_t)fd, (uint64_t)event, 0, 0);
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    return (int)syscall6(SYS_EPOLL_WAIT, (uint64_t)epfd, (uint64_t)events, (uint64_t)maxevents, (uint64_t)timeout, 0, 0);
}

int mprotect(void *addr, size_t len, int prot)
{
    return (int)syscall3(SYS_MPROTECT, (uint64_t)addr, (uint64_t)len, (uint64_t)prot);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)
{
    return (void *)syscall6(SYS_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)prot, (uint64_t)flags, (uint64_t)fd, (uint64_t)offset);
}

int munmap(void *addr, size_t length)
{
    return (int)syscall2(SYS_MUNMAP, (uint64_t)addr, (uint64_t)length);
}

int memfd_create(const char *name, unsigned int flags)
{
    return (int)syscall2(SYS_MEMFD_CREATE, (uint64_t)name, (uint64_t)flags);
}

int futex(volatile uint32_t *uaddr, int op, uint32_t val)
{
    return (int)syscall3(SYS_FUTEX, (uint64_t)uaddr, (uint64_t)op, (uint64_t)val);
}

int thread_create(void (*fn)(void), void *arg, void *stack_addr, void *frame)
{
    return (int)syscall4(SYS_THREAD_CREATE, (uint64_t)fn, (uint64_t)arg, (uint64_t)stack_addr, (uint64_t)frame);
}

