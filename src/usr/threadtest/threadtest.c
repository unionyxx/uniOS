// threadtest.elf - boot-launched userspace pthread self-test (debug builds).
// The six scenarios map 1:1 onto the summary line the smoke suite greps, and
// that summary is the ONLY log line containing '=': its field tokens are the
// unambiguous serial markers. Every scenario is deterministic - the timedwait
// cycles anchor each producer delay to the consumer's announced park instead
// of relying on scheduling latency.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uapi/fs.h>
#include <uapi/tcb.h>

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

static pthread_mutex_t g_tls_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tls_cond = PTHREAD_COND_INITIALIZER;
static volatile int g_tls_completed;
/* Slot i is written only by worker i before its completion handshake and
 * read by the leader after the join, so no slot is ever shared. */
static pthread_t g_tls_self_seen[TLS_THREADS];
static int g_tls_tcb_bad[TLS_THREADS];
static int g_tls_canary_bad[TLS_THREADS];

static void *tls_worker(void *arg)
{
    uintptr_t idx = (uintptr_t)arg;

    UniTcb *tcb;
    __asm__ __volatile__("movq %%fs:0x0, %0" : "=r"(tcb));
    g_tls_self_seen[idx] = pthread_self();
    g_tls_tcb_bad[idx] = (tcb->self != (uint64_t)(uintptr_t)tcb) || (tcb->tid != pthread_self());
    g_tls_canary_bad[idx] = (tls_canary != TLS_CANARY);

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

int main(void)
{
    const bool create_ok = scenario_create();
    const bool mutex_ok = scenario_mutex();
    const bool cond_ok = scenario_cond();
    const bool join_ok = scenario_join();
    const bool detach_ok = scenario_detach();
    const bool tls_ok = scenario_tls();

    LOG_INFO(LOG_SCOPE, "thread self-test summary: create=%s mutex=%s cond=%s join=%s detach=%s tls=%s",
             create_ok ? "PASS" : "FAIL", mutex_ok ? "PASS" : "FAIL", cond_ok ? "PASS" : "FAIL",
             join_ok ? "PASS" : "FAIL", detach_ok ? "PASS" : "FAIL", tls_ok ? "PASS" : "FAIL");

    return (create_ok && mutex_ok && cond_ok && join_ok && detach_ok && tls_ok) ? 0 : 1;
}
