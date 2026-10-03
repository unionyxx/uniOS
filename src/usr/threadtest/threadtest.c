// threadtest.elf - boot-launched userspace pthread self-test (debug builds).
// The seven scenarios map 1:1 onto the summary line the smoke suite greps, and
// that summary is the ONLY log line containing '=': its field tokens are the
// unambiguous serial markers. Every scenario is deterministic - the timedwait
// cycles anchor each producer delay to the consumer's announced park instead
// of relying on scheduling latency.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uapi/fs.h>
#include <uapi/tcb.h>

#include "../libmedia/media_audio.h"
#include "log.h"
#include "pthread.h"
#include "unistd.h"

#define LOG_SCOPE "threadtest"

#define MUTEX_THREADS 4
#define MUTEX_ITERS 2500
#define COND_CYCLES 6
#define BCAST_CONSUMERS 3
#define JOIN_WORKERS 4
#define TLS_THREADS 4
#define TLS_INCS 5000
#define TLS_CANARY 0x1CC0FFEEu
#define HANDSHAKE_TIMEOUT_MS 2000

/* ---- create: plain return through the exit shim, join collects the return
 * value, and an fd the thread opened stays usable for the leader (the fd
 * table is shared live, not copied at create). */

static int g_thread_fd;

static void *create_worker(void *arg)
{
    (void)arg;
    g_thread_fd = open("/etc/system.conf", O_RDONLY);
    return (void *)(intptr_t)0x1234321;
}

static bool scenario_create(void)
{
    g_thread_fd = -1;

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, create_worker, NULL);
    if (rc != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario create: pthread_create failed: %d", rc);
        return false;
    }

    void *retval = NULL;
    rc = pthread_join(tid, &retval);
    if (rc != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario create: pthread_join failed: %d", rc);
        return false;
    }
    if ((intptr_t)retval != 0x1234321) {
        LOG_ERROR(LOG_SCOPE, "scenario create: join collected return value %ld", (long)(intptr_t)retval);
        return false;
    }

    if (g_thread_fd < 0) {
        LOG_ERROR(LOG_SCOPE, "scenario create: thread open failed: %d", g_thread_fd);
        return false;
    }
    struct VNodeStat probe = {0};
    int stat_rc = stat("/etc/system.conf", &probe);
    if (stat_rc != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario create: stat of the probe file failed: %d", stat_rc);
        return false;
    }
    if (fsize(g_thread_fd) != (int64_t)probe.size) {
        LOG_ERROR(LOG_SCOPE, "scenario create: leader saw fsize %lld on the thread-opened fd, expected %llu",
                  (long long)fsize(g_thread_fd), (unsigned long long)probe.size);
        return false;
    }

    close(g_thread_fd);
    return true;
}

/* ---- mutex: N threads hammer one counter under the mutex; a lost update
 * or a broken wake handoff shows up as a short count. */

static pthread_mutex_t g_stress_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_stress_counter;

static void *stress_worker(void *arg)
{
    uint64_t iters = (uint64_t)(uintptr_t)arg;
    for (uint64_t i = 0; i < iters; i++) {
        pthread_mutex_lock(&g_stress_mutex);
        g_stress_counter++;
        pthread_mutex_unlock(&g_stress_mutex);
    }
    return NULL;
}

static bool scenario_mutex(void)
{
    g_stress_counter = 0;

    pthread_t tids[MUTEX_THREADS];
    for (int i = 0; i < MUTEX_THREADS; i++) {
        int rc = pthread_create(&tids[i], NULL, stress_worker, (void *)(uintptr_t)MUTEX_ITERS);
        if (rc != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario mutex: pthread_create %d failed: %d", i, rc);
            return false;
        }
    }
    for (int i = 0; i < MUTEX_THREADS; i++) {
        int rc = pthread_join(tids[i], NULL);
        if (rc != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario mutex: pthread_join %d failed: %d", i, rc);
            return false;
        }
    }

    if (g_stress_counter != (uint64_t)MUTEX_THREADS * (uint64_t)MUTEX_ITERS) {
        LOG_ERROR(LOG_SCOPE, "scenario mutex: counter is %llu, expected %llu", (unsigned long long)g_stress_counter,
                  (unsigned long long)(MUTEX_THREADS * MUTEX_ITERS));
        return false;
    }
    return true;
}

/* ---- cond: producer/consumer cycles of pthread_cond_timedwait. The
 * consumer publishes g_wait_cycle while holding the mutex right before it
 * parks; the producer waits for that publication, so its produce delay is
 * anchored to the actual park. Timeout cycles: delay dwarfs the deadline, so
 * the deadline must fire first. Wake cycles: no delay, so the signal (or its
 * EAGAIN re-validation) must win against a deadline it never approaches. A
 * broadcast phase then serves several parked waiters at once. */

static pthread_mutex_t g_cond_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

static const uint64_t g_cond_deadline_ms[COND_CYCLES] = {100, 100, 2000, 100, 2000, 100};
static const uint64_t g_cond_delay_ms[COND_CYCLES] = {300, 300, 0, 300, 0, 300};
static const bool g_cond_expect_timeout[COND_CYCLES] = {true, true, false, true, false, true};

static volatile int g_wait_cycle = -1;
static volatile uint32_t g_cond_items;
static int g_cond_timeouts[COND_CYCLES];
static int g_cond_wakes[COND_CYCLES];
static int g_cond_bad_errs;

static void *cond_consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < COND_CYCLES; i++) {
        pthread_mutex_lock(&g_cond_mutex);
        g_wait_cycle = i;
        while (g_cond_items == 0) {
            int err = pthread_cond_timedwait(&g_cond, &g_cond_mutex, g_cond_deadline_ms[i]);
            if (err == -110)
                g_cond_timeouts[i]++;
            else if (err == 0)
                g_cond_wakes[i]++;
            else
                g_cond_bad_errs++;
        }
        g_cond_items = 0;
        pthread_mutex_unlock(&g_cond_mutex);
    }
    return NULL;
}

static volatile uint32_t g_bcast_servings;
static volatile int g_bcast_served;
static volatile int g_bcast_arrived;

static void *bcast_consumer(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;

    pthread_mutex_lock(&g_cond_mutex);
    g_bcast_arrived++;
    while (g_bcast_servings == 0)
        pthread_cond_wait(&g_cond, &g_cond_mutex);
    g_bcast_servings--;
    g_bcast_served++;
    pthread_mutex_unlock(&g_cond_mutex);

    return (void *)(intptr_t)(100 + (int)idx);
}

static bool wait_for_cond_consumer(int cycle)
{
    for (uint32_t waited = 0; g_wait_cycle != cycle && waited < HANDSHAKE_TIMEOUT_MS; waited += 2)
        sleep_ms(2);
    if (g_wait_cycle != cycle) {
        LOG_ERROR(LOG_SCOPE, "scenario cond: consumer never reached cycle %d, stuck at %d", cycle, g_wait_cycle);
        return false;
    }
    return true;
}

static bool scenario_cond(void)
{
    g_cond_items = 0;
    g_cond_bad_errs = 0;
    for (int i = 0; i < COND_CYCLES; i++) {
        g_cond_timeouts[i] = 0;
        g_cond_wakes[i] = 0;
    }
    g_bcast_servings = 0;
    g_bcast_served = 0;
    g_bcast_arrived = 0;

    pthread_t consumer;
    int rc = pthread_create(&consumer, NULL, cond_consumer, NULL);
    if (rc != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario cond: pthread_create failed: %d", rc);
        return false;
    }

    for (int i = 0; i < COND_CYCLES; i++) {
        if (!wait_for_cond_consumer(i))
            return false;
        sleep_ms((uint32_t)g_cond_delay_ms[i]);
        pthread_mutex_lock(&g_cond_mutex);
        g_cond_items = 1;
        pthread_cond_signal(&g_cond);
        pthread_mutex_unlock(&g_cond_mutex);
    }

    rc = pthread_join(consumer, NULL);
    if (rc != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario cond: consumer join failed: %d", rc);
        return false;
    }

    if (g_cond_bad_errs != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario cond: timedwait reported %d unexpected errors", g_cond_bad_errs);
        return false;
    }
    for (int i = 0; i < COND_CYCLES; i++) {
        if (g_cond_expect_timeout[i] ? g_cond_timeouts[i] == 0 : (g_cond_timeouts[i] != 0 || g_cond_wakes[i] == 0)) {
            LOG_ERROR(LOG_SCOPE, "scenario cond: cycle %d woke with %d timeouts and %d wakes", i, g_cond_timeouts[i],
                      g_cond_wakes[i]);
            return false;
        }
    }

    pthread_t tids[BCAST_CONSUMERS];
    for (int i = 0; i < BCAST_CONSUMERS; i++) {
        rc = pthread_create(&tids[i], NULL, bcast_consumer, (void *)(uintptr_t)i);
        if (rc != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario cond: broadcast pthread_create %d failed: %d", i, rc);
            return false;
        }
    }

    for (uint32_t waited = 0; g_bcast_arrived < BCAST_CONSUMERS && waited < HANDSHAKE_TIMEOUT_MS; waited += 2)
        sleep_ms(2);
    if (g_bcast_arrived < BCAST_CONSUMERS) {
        LOG_ERROR(LOG_SCOPE, "scenario cond: only %d of %d broadcast consumers parked", g_bcast_arrived,
                  BCAST_CONSUMERS);
        return false;
    }
    pthread_mutex_lock(&g_cond_mutex);
    g_bcast_servings = BCAST_CONSUMERS;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_cond_mutex);

    for (int i = 0; i < BCAST_CONSUMERS; i++) {
        void *retval = NULL;
        rc = pthread_join(tids[i], &retval);
        if (rc != 0 || (intptr_t)retval != 100 + i) {
            LOG_ERROR(LOG_SCOPE, "scenario cond: broadcast consumer %d join failed: %d (retval %ld)", i, rc,
                      (long)(intptr_t)retval);
            return false;
        }
    }
    if (g_bcast_served != BCAST_CONSUMERS) {
        LOG_ERROR(LOG_SCOPE, "scenario cond: broadcast served %d of %d consumers", g_bcast_served, BCAST_CONSUMERS);
        return false;
    }
    return true;
}

/* ---- join: workers compute distinct results over once-initialized shared
 * data (pthread_once) and a shared tally under the rwlock, signal completion
 * through a condvar, and every join reaps one thread and reports its value;
 * a second waitpid on a joined tid must be ECHILD. */

static pthread_once_t g_join_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_join_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_join_cond = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t g_join_rwlock = PTHREAD_RWLOCK_INITIALIZER;

static volatile int g_once_runs;
static volatile uint64_t g_join_base;
static volatile uint64_t g_join_tally;
static volatile int g_join_completed;

static void join_once_init(void)
{
    g_once_runs++;
    g_join_base = 7;
}

static void *join_worker(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;

    pthread_once(&g_join_once, join_once_init);

    pthread_rwlock_rdlock(&g_join_rwlock);
    uint64_t base = g_join_base;
    pthread_rwlock_unlock(&g_join_rwlock);

    pthread_rwlock_wrlock(&g_join_rwlock);
    g_join_tally++;
    pthread_rwlock_unlock(&g_join_rwlock);

    pthread_mutex_lock(&g_join_mutex);
    g_join_completed++;
    pthread_cond_signal(&g_join_cond);
    pthread_mutex_unlock(&g_join_mutex);

    return (void *)(intptr_t)(base * 100 + (uint64_t)idx);
}

static bool scenario_join(void)
{
    g_join_completed = 0;

    pthread_t tids[JOIN_WORKERS];
    for (int i = 0; i < JOIN_WORKERS; i++) {
        int rc = pthread_create(&tids[i], NULL, join_worker, (void *)(uintptr_t)i);
        if (rc != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario join: pthread_create %d failed: %d", i, rc);
            return false;
        }
    }

    pthread_mutex_lock(&g_join_mutex);
    while (g_join_completed < JOIN_WORKERS)
        pthread_cond_wait(&g_join_cond, &g_join_mutex);
    pthread_mutex_unlock(&g_join_mutex);

    for (int i = 0; i < JOIN_WORKERS; i++) {
        void *retval = NULL;
        int rc = pthread_join(tids[i], &retval);
        if (rc != 0 || (intptr_t)retval != 700 + i) {
            LOG_ERROR(LOG_SCOPE, "scenario join: worker %d join failed: %d (retval %ld)", i, rc,
                      (long)(intptr_t)retval);
            return false;
        }
    }

    if (g_once_runs != 1) {
        LOG_ERROR(LOG_SCOPE, "scenario join: the once initializer ran %d times", g_once_runs);
        return false;
    }
    if (g_join_tally != JOIN_WORKERS) {
        LOG_ERROR(LOG_SCOPE, "scenario join: rwlock-guarded tally is %llu, expected %d",
                  (unsigned long long)g_join_tally, JOIN_WORKERS);
        return false;
    }

    int status = 0;
    int wait_rc = waitpid((int)tids[0], &status);
    if (wait_rc != -1) {
        LOG_ERROR(LOG_SCOPE, "scenario join: a joined tid is still waitable: %d", wait_rc);
        return false;
    }
    return true;
}

/* ---- detach: the thread exits through pthread_exit after the leader armed
 * it; waitpid on the detached tid must report the thread gone. */

static pthread_mutex_t g_detach_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_detach_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_detach_armed;
static volatile int g_detach_done;

static void *detach_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_detach_mutex);
    while (!g_detach_armed)
        pthread_cond_wait(&g_detach_cond, &g_detach_mutex);
    g_detach_done = 1;
    pthread_cond_signal(&g_detach_cond);
    pthread_mutex_unlock(&g_detach_mutex);

    pthread_exit((void *)(intptr_t)0x5A);
}

static bool scenario_detach(void)
{
    g_detach_armed = 0;
    g_detach_done = 0;

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, detach_worker, NULL);
    if (rc != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario detach: pthread_create failed: %d", rc);
        return false;
    }

    rc = pthread_detach(tid);
    if (rc != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario detach: pthread_detach failed: %d", rc);
        pthread_mutex_lock(&g_detach_mutex);
        g_detach_armed = 1;
        pthread_cond_signal(&g_detach_cond);
        pthread_mutex_unlock(&g_detach_mutex);
        pthread_join(tid, NULL);
        return false;
    }

    pthread_mutex_lock(&g_detach_mutex);
    g_detach_armed = 1;
    pthread_cond_signal(&g_detach_cond);
    while (!g_detach_done)
        pthread_cond_wait(&g_detach_cond, &g_detach_mutex);
    pthread_mutex_unlock(&g_detach_mutex);

    int status = 0;
    int wait_rc = waitpid((int)tid, &status);
    if (wait_rc != -1) {
        LOG_ERROR(LOG_SCOPE, "scenario detach: the detached tid is still waitable: %d", wait_rc);
        return false;
    }
    return true;
}

/* ---- tls: every thread owns its static TLS block. Workers run 5000
 * increments on a __thread counter and return the total through the exit
 * channel: a shared block would split the 20000 increments across the
 * returns instead of giving each thread its own 5000, and the leader's own
 * counter must still read zero. A __thread canary with a nonzero initial
 * value pins the block's placement against the link-time TPOFF - a block
 * shifted even a few bytes corrupts initialized variables. The control
 * block at fs:0 must self-identify, and pthread_self must read this
 * thread's tid out of it. Workers announce completion through the condvar
 * before returning, so a worker stuck on a broken fs base surfaces as a
 * timed-out handshake instead of a hang. */

static __thread uint32_t tls_counter;             /* zero in every thread */
static __thread uint32_t tls_canary = TLS_CANARY; /* nonzero template image */
/* Sizes .tdata to exactly one page: the user linker script aligns .tdata
 * to 4096, so the template ends ON a page boundary and the .tbss tail
 * (tls_counter) starts exactly there - the layout where an uncovered
 * .tbss leaves its VAs unmapped and thread creation fails with -12. The
 * canary and pad both sit in .tdata; the size accounts for the linker's
 * 16-byte alignment of the array after the 4-byte canary. The zero tail
 * reading zero in every worker is the regression pin. */
static __thread uint8_t tls_page_pad[4096 - 16] = {1};

static pthread_mutex_t g_tls_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tls_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_tls_completed;
/* Slot i is written only by worker i before its completion handshake and
 * read by the leader after the join, so no slot is ever shared. */
static pthread_t g_tls_self_seen[TLS_THREADS];
static int g_tls_tcb_bad[TLS_THREADS];
static int g_tls_canary_bad[TLS_THREADS];
static int g_tls_pad_bad[TLS_THREADS];

static void *tls_worker(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;

    UniTcb *tcb;
    __asm__ __volatile__("movq %%fs:0x0, %0" : "=r"(tcb));
    g_tls_self_seen[idx] = pthread_self();
    g_tls_tcb_bad[idx] = (tcb->self != (uint64_t)(uintptr_t)tcb) || (tcb->tid != pthread_self());
    g_tls_canary_bad[idx] = (tls_canary != TLS_CANARY);
    g_tls_pad_bad[idx] = (tls_page_pad[0] != 1);

    for (uint32_t i = 0; i < TLS_INCS; i++)
        tls_counter++;

    pthread_mutex_lock(&g_tls_mutex);
    g_tls_completed++;
    pthread_cond_signal(&g_tls_cond);
    pthread_mutex_unlock(&g_tls_mutex);

    return (void *)(intptr_t)tls_counter;
}

static bool scenario_tls(void)
{
    g_tls_completed = 0;
    for (int i = 0; i < TLS_THREADS; i++) {
        g_tls_self_seen[i] = 0;
        g_tls_tcb_bad[i] = 0;
        g_tls_canary_bad[i] = 0;
        g_tls_pad_bad[i] = 0;
    }

    UniTcb *tcb;
    __asm__ __volatile__("movq %%fs:0x0, %0" : "=r"(tcb));
    if (tcb->self != (uint64_t)(uintptr_t)tcb) {
        LOG_ERROR(LOG_SCOPE, "scenario tls: the control block at fs:0 does not self-identify (self %llu, fs %llu)",
                  (unsigned long long)tcb->self, (unsigned long long)(uintptr_t)tcb);
        return false;
    }
    if (tcb->tid != pthread_self()) {
        LOG_ERROR(LOG_SCOPE, "scenario tls: control block tid %llu disagrees with pthread_self %llu",
                  (unsigned long long)tcb->tid, (unsigned long long)pthread_self());
        return false;
    }

    pthread_t tids[TLS_THREADS];
    for (int i = 0; i < TLS_THREADS; i++) {
        int rc = pthread_create(&tids[i], NULL, tls_worker, (void *)(uintptr_t)i);
        if (rc != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario tls: pthread_create %d failed: %d", i, rc);
            return false;
        }
    }

    pthread_mutex_lock(&g_tls_mutex);
    while (g_tls_completed < TLS_THREADS) {
        int err = pthread_cond_timedwait(&g_tls_cond, &g_tls_mutex, HANDSHAKE_TIMEOUT_MS);
        if (err == -110)
            break; /* a full handshake deadline passed with no completion */
    }
    const bool completed = (g_tls_completed == TLS_THREADS);
    pthread_mutex_unlock(&g_tls_mutex);
    if (!completed) {
        LOG_ERROR(LOG_SCOPE, "scenario tls: only %d of %d workers completed their tls work", g_tls_completed,
                  TLS_THREADS);
        return false;
    }

    for (int i = 0; i < TLS_THREADS; i++) {
        void *retval = NULL;
        int rc = pthread_join(tids[i], &retval);
        if (rc != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario tls: worker %d join failed: %d", i, rc);
            return false;
        }
        if ((intptr_t)retval != TLS_INCS) {
            LOG_ERROR(LOG_SCOPE, "scenario tls: worker %d counted %lu, expected %d (its own 5000 increments)", i,
                      (unsigned long)(intptr_t)retval, TLS_INCS);
            return false;
        }
        if (g_tls_self_seen[i] != tids[i]) {
            LOG_ERROR(LOG_SCOPE, "scenario tls: worker %d saw pthread_self %llu, expected the created tid %llu", i,
                      (unsigned long long)g_tls_self_seen[i], (unsigned long long)tids[i]);
            return false;
        }
        if (g_tls_tcb_bad[i] != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario tls: worker %d's control block failed the self-identity contract", i);
            return false;
        }
        if (g_tls_canary_bad[i] != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario tls: worker %d's tls canary was not its initial value", i);
            return false;
        }
        if (g_tls_pad_bad[i] != 0) {
            LOG_ERROR(LOG_SCOPE, "scenario tls: worker %d's page pad byte was not its initial value", i);
            return false;
        }
    }

    if (tls_canary != TLS_CANARY) {
        LOG_ERROR(LOG_SCOPE, "scenario tls: the main thread's canary is 0x%x, expected 0x%x", tls_canary, TLS_CANARY);
        return false;
    }
    if (tls_counter != 0) {
        LOG_ERROR(LOG_SCOPE, "scenario tls: the main thread's counter moved: %u", tls_counter);
        return false;
    }
    return true;
}

/* ---- wavprobe: media_audio_probe checked against hand-packed WAV byte
 * streams. Headers are assembled with explicit little-endian packing helpers
 * (no struct casts over the buffer), so the scenario pins the byte contract
 * itself: RIFF/WAVE magic, the PCM fmt fields, the data-chunk scan, the
 * payload bounds check against file_size (a header window smaller than the
 * file must still probe), and out untouched on rejection. */

#define WAV_HEADER_BYTES 44
#define WAV_PAYLOAD_BYTES 2048
#define WAV_FILE_BYTES (WAV_HEADER_BYTES + WAV_PAYLOAD_BYTES)
#define WAV_STEREO_RATE 44100
#define WAV_MONO_RATE 22050

static void wav_put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wav_put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void wav_put_magic(uint8_t *p, const char magic[4])
{
    p[0] = (uint8_t)magic[0];
    p[1] = (uint8_t)magic[1];
    p[2] = (uint8_t)magic[2];
    p[3] = (uint8_t)magic[3];
}

/* Pack a canonical 44-byte PCM WAV header: 12-byte RIFF descriptor, 16-byte
 * fmt chunk, then the 8-byte data chunk header. The payload starts at byte
 * 44 and its contents never matter to the probe. */
static void wav_pack_header(uint8_t *buf, uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits,
                            uint32_t data_bytes)
{
    wav_put_magic(buf + 0, "RIFF");
    wav_put_u32le(buf + 4, 36 + data_bytes);
    wav_put_magic(buf + 8, "WAVE");
    wav_put_magic(buf + 12, "fmt ");
    wav_put_u32le(buf + 16, 16);
    wav_put_u16le(buf + 20, format);
    wav_put_u16le(buf + 22, channels);
    wav_put_u32le(buf + 24, rate);
    wav_put_u32le(buf + 28, rate * (uint32_t)channels * ((uint32_t)bits / 8));
    wav_put_u16le(buf + 32, (uint16_t)((uint32_t)channels * ((uint32_t)bits / 8)));
    wav_put_u16le(buf + 34, bits);
    wav_put_magic(buf + 36, "data");
    wav_put_u32le(buf + 40, data_bytes);
}

static bool wavprobe_check_accept(const char *name, const uint8_t *data, size_t size, uint64_t file_size,
                                  const struct media_audio_info *expect)
{
    struct media_audio_info info = {0};
    if (!media_audio_probe(data, size, file_size, &info)) {
        LOG_ERROR(LOG_SCOPE, "scenario wavprobe: %s was rejected", name);
        return false;
    }
    if (info.sample_rate != expect->sample_rate || info.channels != expect->channels ||
        info.bits_per_sample != expect->bits_per_sample || info.data_start != expect->data_start ||
        info.data_size != expect->data_size) {
        LOG_ERROR(LOG_SCOPE,
                  "scenario wavprobe: %s probed %u Hz, %u channels, %u bits, start %llu, size %llu; expected "
                  "%u Hz, %u channels, %u bits, start %llu, size %llu",
                  name, info.sample_rate, info.channels, info.bits_per_sample, (unsigned long long)info.data_start,
                  (unsigned long long)info.data_size, expect->sample_rate, expect->channels, expect->bits_per_sample,
                  (unsigned long long)expect->data_start, (unsigned long long)expect->data_size);
        return false;
    }
    return true;
}

/* Every reject case also pins the untouched-out contract: the probe must
 * never write the output structure on failure. */
static bool wavprobe_check_reject(const char *name, const uint8_t *data, size_t size, uint64_t file_size)
{
    struct media_audio_info info;
    info.sample_rate = 0xDEADBEEFu;
    info.channels = 0xDEADu;
    info.bits_per_sample = 0xBEEFu;
    info.data_start = 0xDEADBEEFull;
    info.data_size = 0xFEEDFACEull;
    if (media_audio_probe(data, size, file_size, &info)) {
        LOG_ERROR(LOG_SCOPE, "scenario wavprobe: %s was accepted", name);
        return false;
    }
    if (info.sample_rate != 0xDEADBEEFu || info.channels != 0xDEADu || info.bits_per_sample != 0xBEEFu ||
        info.data_start != 0xDEADBEEFull || info.data_size != 0xFEEDFACEull) {
        LOG_ERROR(LOG_SCOPE, "scenario wavprobe: %s rejected but wrote the output structure", name);
        return false;
    }
    return true;
}

static bool scenario_wavprobe(void)
{
    static uint8_t wav[WAV_FILE_BYTES];
    for (uint32_t i = 0; i < WAV_PAYLOAD_BYTES; i++)
        wav[WAV_HEADER_BYTES + i] = (uint8_t)i;

    /* valid stereo 44100 Hz 16-bit, payload 2048 */
    wav_pack_header(wav, 1, 2, WAV_STEREO_RATE, 16, WAV_PAYLOAD_BYTES);
    struct media_audio_info expect = {WAV_STEREO_RATE, 2, 16, WAV_HEADER_BYTES, WAV_PAYLOAD_BYTES};
    if (!wavprobe_check_accept("a stereo stream", wav, WAV_FILE_BYTES, WAV_FILE_BYTES, &expect))
        return false;

    /* 10-byte truncated buffer, file_size 10 */
    uint8_t tiny[10];
    for (int i = 0; i < 10; i++)
        tiny[i] = (uint8_t)('A' + i);
    wav_put_magic(tiny, "RIFF");
    if (!wavprobe_check_reject("a 10-byte buffer", tiny, 10, 10))
        return false;

    /* RIFF magic but WAVX instead of WAVE */
    wav_pack_header(wav, 1, 2, WAV_STEREO_RATE, 16, WAV_PAYLOAD_BYTES);
    wav_put_magic(wav + 8, "WAVX");
    if (!wavprobe_check_reject("a WAVX magic", wav, WAV_FILE_BYTES, WAV_FILE_BYTES))
        return false;

    /* fmt audio format code 3 (IEEE float) */
    wav_pack_header(wav, 3, 2, WAV_STEREO_RATE, 16, WAV_PAYLOAD_BYTES);
    if (!wavprobe_check_reject("a float format code", wav, WAV_FILE_BYTES, WAV_FILE_BYTES))
        return false;

    /* bits_per_sample 8 */
    wav_pack_header(wav, 1, 2, WAV_STEREO_RATE, 8, WAV_PAYLOAD_BYTES);
    if (!wavprobe_check_reject("8-bit samples", wav, WAV_FILE_BYTES, WAV_FILE_BYTES))
        return false;

    /* valid mono 22050 Hz, payload 1024 (payload ends exactly at EOF) */
    wav_pack_header(wav, 1, 1, WAV_MONO_RATE, 16, 1024);
    struct media_audio_info mono = {WAV_MONO_RATE, 1, 16, WAV_HEADER_BYTES, 1024};
    if (!wavprobe_check_accept("a mono stream", wav, WAV_HEADER_BYTES + 1024, WAV_HEADER_BYTES + 1024, &mono))
        return false;

    /* data_size = file_size, so the payload would run past EOF */
    wav_pack_header(wav, 1, 2, WAV_STEREO_RATE, 16, WAV_FILE_BYTES);
    if (!wavprobe_check_reject("an over-long payload", wav, WAV_FILE_BYTES, WAV_FILE_BYTES))
        return false;

    /* streaming contract: only a 64-byte header window while file_size stays
     * the full size - parsing reads stay in the window, the bounds check
     * uses the file size */
    wav_pack_header(wav, 1, 2, WAV_STEREO_RATE, 16, WAV_PAYLOAD_BYTES);
    if (!wavprobe_check_accept("a 64-byte header window", wav, 64, WAV_FILE_BYTES, &expect))
        return false;

    return true;
}

int main(void)
{
    const bool create_ok = scenario_create();
    const bool mutex_ok = scenario_mutex();
    const bool cond_ok = scenario_cond();
    const bool join_ok = scenario_join();
    const bool detach_ok = scenario_detach();
    const bool tls_ok = scenario_tls();
    const bool wavprobe_ok = scenario_wavprobe();

    LOG_INFO(LOG_SCOPE, "thread self-test summary: create=%s mutex=%s cond=%s join=%s detach=%s tls=%s wavprobe=%s",
             create_ok ? "PASS" : "FAIL", mutex_ok ? "PASS" : "FAIL", cond_ok ? "PASS" : "FAIL",
             join_ok ? "PASS" : "FAIL", detach_ok ? "PASS" : "FAIL", tls_ok ? "PASS" : "FAIL",
             wavprobe_ok ? "PASS" : "FAIL");

    return (create_ok && mutex_ok && cond_ok && join_ok && detach_ok && tls_ok && wavprobe_ok) ? 0 : 1;
}
