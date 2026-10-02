#pragma once

#include <stdint.h>

struct Process;

/* Map one anonymous RW user region [pad][TLS block: tls_size bytes, ends at
 * fs_base][UniTcb: 16 bytes], copy tls_size bytes from template_src (a
 * kernel-readable address; null = zero-fill), write the TCB (self,
 * tid = proc->pid) and set proc->fs_base. Returns the mapping's user start
 * address, 0 on failure. Does not touch tls_lo/tls_len - the caller
 * records them.
 *
 * Layout: padded = align_up(tls_size, max(tls_align, 16)); the mapping
 * covers padded + 16 bytes; fs_base = mapping start + padded; the TCB
 * occupies [fs_base, fs_base + 16). The TLS block ends exactly at fs_base
 * so link-time TPOFF32 offsets hold; alignment padding sits below the
 * block, never between the block and the TCB. */
uint64_t tls_install(Process *proc, const void *template_src, uint64_t tls_size, uint64_t tls_align);
