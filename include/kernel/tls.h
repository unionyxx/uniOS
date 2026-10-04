#pragma once

#include <stdint.h>

struct Process;

/* Map one anonymous RW user region [TLS block: tls_size bytes starting at
 * fs_base - padded][alignment gap: zero][UniTcb: 16 bytes], copy tls_size
 * bytes from template_src (a kernel-readable address; null = zero-fill),
 * write the TCB (self, tid = proc->pid) and set proc->fs_base. Returns
 * the mapping's user start address, 0 on failure. Does not touch
 * tls_lo/tls_len - the caller records them.
 *
 * Layout: padded = align_up(tls_size, tls_align) — the linker's TPOFF
 * offsets round the block to p_align alone, and the kernel must match
 * them exactly or every __thread variable sits at the wrong address
 * (the old 16 floor here corrupted any p_align < 16 image). The mapping
 * covers padded + 16 bytes; fs_base = mapping start + padded; the TCB
 * occupies [fs_base, fs_base + 16). The TLS block starts exactly at
 * fs_base - padded, so a non-multiple-of-align tls_size leaves the
 * alignment gap [fs_base - padded + tls_size, fs_base) directly below
 * the TCB, zero from the fresh frames. The block's start is page
 * aligned, so every __thread variable's own alignment holds. */
uint64_t tls_install(Process *proc, const void *template_src, uint64_t tls_size, uint64_t tls_align);
