#include "wm_metrics.h"
#include "wm_window.h"

static bool process_is_alive(uint32_t pid)
{
    if (pid == 0)
        return false;
    // signal 0 is standard for existence check.
    return syscall2(SYS_KILL, (uint64_t)pid, 0) == 0;
}

// Adoption-failure tracking: a registry entry whose window cannot be created
// (bad shm, size mismatch, mapping failure) used to be retried EVERY frame,
// burning syscalls forever. Key by (shm_id, owner_pid) so a recycled fd of a
// different process is not punished for an earlier window's failure.
namespace {

struct AdoptionFailure
{
    int shm_id;
    uint32_t owner_pid;
    int count;
    uint64_t last_ticks;
};

constexpr int ADOPTION_FAILURE_SLOTS = 64;
constexpr int ADOPTION_FAILURE_LIMIT = 8;
AdoptionFailure g_adoption_failures[ADOPTION_FAILURE_SLOTS];

int adoption_failure_slot(int shm_id, uint32_t owner_pid)
{
    int empty = -1;
    for (int i = 0; i < ADOPTION_FAILURE_SLOTS; i++) {
        if (g_adoption_failures[i].count > 0 && g_adoption_failures[i].shm_id == shm_id &&
            g_adoption_failures[i].owner_pid == owner_pid)
            return i;
        if (empty < 0 && g_adoption_failures[i].count == 0)
            empty = i;
    }
    return empty;
}

bool adoption_exhausted(int shm_id, uint32_t owner_pid)
{
    const int slot = adoption_failure_slot(shm_id, owner_pid);
    if (slot < 0 || g_adoption_failures[slot].count < ADOPTION_FAILURE_LIMIT)
        return false;
    // Expire the tombstone so a client that fixes its buffer can recover.
    if (get_ticks() - g_adoption_failures[slot].last_ticks > 5000)
        g_adoption_failures[slot].count = ADOPTION_FAILURE_LIMIT - 1;
    return g_adoption_failures[slot].count >= ADOPTION_FAILURE_LIMIT;
}

void adoption_note_failure(int shm_id, uint32_t owner_pid)
{
    int slot = adoption_failure_slot(shm_id, owner_pid);
    if (slot < 0)
        slot = 0;
    if (g_adoption_failures[slot].count == 0) {
        g_adoption_failures[slot].shm_id = shm_id;
        g_adoption_failures[slot].owner_pid = owner_pid;
    }
    if (g_adoption_failures[slot].count < ADOPTION_FAILURE_LIMIT)
        g_adoption_failures[slot].count++;
    g_adoption_failures[slot].last_ticks = get_ticks();
}

void adoption_clear(int shm_id, uint32_t owner_pid)
{
    const int slot = adoption_failure_slot(shm_id, owner_pid);
    if (slot >= 0)
        g_adoption_failures[slot] = {};
}

} // namespace

void wm_adopt_windows(Registry *registry)
{
    if (registry->window_count <= 2)
        return;
    uint32_t max_windows = registry->window_count > MAX_WINDOWS ? MAX_WINDOWS : registry->window_count;
    for (uint32_t i = WM_FIRST_USER_WINDOW; i < max_windows; i++) {
        WindowEntry &e = registry->windows[i];
        if (!e.ready)
            continue;

        asm volatile("lfence" ::: "memory");

        if (!gui_shm_id_is_valid(e.shm_id) || e.w <= 0 || e.h <= 0 || !e.owner_pid || !e.title[0])
            continue;
        if (find_window_by_entry(&e) >= 0 || find_window_by_shm(e.shm_id) >= 0)
            continue;
        // An entry whose owner died before or during adoption (crash,
        // kill) never completes: retrying it burns syscalls, and its
        // recycled (shm_id, pid) pair poisons the adoption key of a
        // relaunched app that reuses the same fd and pid. Reset the
        // slot so gui_reserve_window_slot can hand it out again.
        if (!process_is_alive(e.owner_pid)) {
            adoption_clear(e.shm_id, e.owner_pid);
            damage_reset(&e.damage);
            e.ready = false;
            asm volatile("sfence" ::: "memory");
            e.shm_id = WIN_SHM_INVALID;
            asm volatile("sfence" ::: "memory");
            continue;
        }
        if (adoption_exhausted(e.shm_id, e.owner_pid))
            continue;
        if (add_win_internal(e.shm_id, e.x, e.y, e.w, e.h, e.title, &e.damage, &e, (e.flags & WIN_FLAG_TRANSPARENT))) {
            adoption_clear(e.shm_id, e.owner_pid);
        } else {
            adoption_note_failure(e.shm_id, e.owner_pid);
        }
    }
}

static void reap_exited_children()
{
    int status = 0;
    int pid;
    while ((pid = waitpid_nohang(-1, &status)) > 0) {
        for (int i = 0; i < g_window_count; i++) {
            if (g_windows[i].owner_pid == (uint32_t)pid) {
                close_window(i, false); // owner already dead: do not kill by (possibly recycled) PID
                i--;                    // Adjust index as close_window compacts the array by shifting elements down
            }
        }
    }
}

void wm_reap_dead_owners()
{
    // Reap dead processes before compositing to avoid dereferencing stale SHM buffers.
    reap_exited_children();
    static uint32_t s_liveness_check_counter = 0;
    if (++s_liveness_check_counter >= 30) {
        s_liveness_check_counter = 0;
        for (int i = 0; i < g_window_count;) {
            Window &w = g_windows[i];
            if (w.owner_pid != 0 && !process_is_alive(w.owner_pid)) {
                close_window(i, false); // owner already dead: do not kill by (possibly recycled) PID
                continue;
            }
            ++i;
        }
    }
}
