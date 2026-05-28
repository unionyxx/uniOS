#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void exit(int status);
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int open(const char *pathname, int flags);
void close(int fd);
int fork(void);
int exec(const char *path);
int waitpid(int pid, int *status);
int waitpid_nohang(int pid, int *status);
int mkdir(const char *pathname);
int unlink(const char *pathname);
int rmdir(const char *pathname);
int rename(const char *oldpath, const char *newpath);
void yield(void);
int dup2(int oldfd, int newfd);
int pipe(int pipefd[2]);
#include <uapi/signal.h>
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

#include <uapi/display.h>
#include <uapi/fs.h>
int stat(const char *path, struct VNodeStat *buf);
int get_volumes(struct VolumeInfo *out, size_t max_count);
int get_storage_mode(void);
int set_storage_mode(int mode);
#include <uapi/event.h>
#include <uapi/sysinfo.h>
int get_procs(struct ProcessInfo *out, size_t max_count);
int get_time(struct SysTime *time);
uint64_t get_uptime(void);
uint64_t get_ticks(void);
uint64_t get_tsc_freq(void);
int getrandom(void *buf, size_t len);
void sleep_ms(uint32_t ms);
int get_meminfo(struct MemInfo *info);
int get_sysinfo(struct SystemProfile *info);

int fb_info(uint32_t *info);
void *fb_mmap(void);
void fb_flush(void);
void fb_blit(const void *buffer, uint32_t size);
void fb_blit_rect(const void *buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride);
int display_get_caps(struct DisplayCaps *caps);
int display_get_status(struct DisplayStatus *status);
int display_query_connectors(struct DisplayConnectorInfo *out, size_t max_count);
int display_get_modes(uint32_t connector_id, struct DisplayMode *out, size_t max_count);
int display_set_mode(const struct DisplayMode *mode);
int display_atomic_commit(const struct DisplayAtomicRequest *request);
int display_buffer_create(struct DisplayBufferCreate *request);
int display_buffer_map(struct DisplayBufferMap *request);
int display_buffer_destroy(DisplayBufferHandle handle);
uint32_t display_present(const struct DisplayPresentRequest *req);
uint32_t display_compose_submit(const struct DisplayComposeRequest *req);
uint32_t display_wait(void);
int display_wait_event(struct DisplayEvent *event);
int display_poll_event(struct DisplayEvent *event);
uint64_t get_tsc_freq(void);
int get_event(struct Event *ev);
int wait_event(struct Event *ev);
int poll_event(struct Event *ev);
void sound_play(const char *path);
void sound_write(const void *data, uint32_t size);
void sound_config(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);

int futex(volatile uint32_t *uaddr, int op, uint32_t val);
int thread_create(void (*fn)(void), void *arg, void *stack_addr, void *frame);
int ftruncate(int fd, uint64_t size);
int fd_transfer(uint64_t target_pid, int fd);

#ifdef __cplusplus
}
#endif

