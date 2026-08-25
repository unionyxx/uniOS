# VFS

The virtual filesystem switch lives in `src/fs/vfs.cpp`. There are no device files: framebuffer, display, input, and sound are syscalls, and stdin/stdout/stderr are special-cased in the read/write paths.

## Core Objects

- `VNode`: inode id, size, uid, mode, is_dir, ops table, filesystem data, and a volatile refcount.
- `VNodeOps`: read, write, readdir, lookup, create, mkdir, unlink, rename, truncate, sync, close.
- `Mount`: path (64 chars), root vnode, flags, linked list. Mount lookup picks the **longest matching path prefix**.

Path resolution merges relative paths against the process cwd, normalizes `.`/`..` (up to 256 segments), and walks segments through `ops->lookup` with refcounting at each hop.

## Permissions

Uid-only model: uid 0 bypasses checks; otherwise owner bits apply when the uid matches, else other bits (no groups). Defaults are `0644` for files and `0755` for directories; storage-guarded mount roots are forced to `0777`.

## Open Semantics

- `O_CREAT` creates through the parent directory's `create` op, then looks up.
- `O_TRUNC` is refused while the same file is open through another descriptor.
- `O_APPEND` anchors every write at the current EOF.
- Descriptors: 128 per process, allocation starts at fd 3.

Reads and writes snapshot the descriptor and pin the vnode under the fd lock. Regular files on uniFS and FAT32 go through the page cache; pipes and memfds call their ops directly. Growing writes update the vnode size and propagate it to other open vnodes of the same file.

## Page Cache

A fixed table of 512 entries, one 4 KiB page each, with last-access accounting for eviction:

- Cache entries hold vnode references; the dirty flag clears only on successful writeback.
- Eviction is bounded: unflushable victims are aged rather than livelocked.
- Closing the last fd of a file purges its entries — repointing them to a still-open vnode of the same file, or flushing and dropping them.
- `SYS_SYNC` flushes all dirty pages and calls each mounted filesystem's sync op.

## Links, Deletes, Renames

- `unlink` refuses files that are currently open (identity compared by file, not pointer).
- `rmdir` requires an empty directory.
- `rename` requires both parents on the same filesystem ops table and write permission.

There is no `lseek` syscall; offsets move through read/write and kernel-internal seek only.

## Pipes

`SYS_PIPE` returns a read fd and a write fd backed by a 4 KiB ring buffer (up to 64 live pipes):

- Blocking read/write sleep on data/space wait queues; EINTR on pending fatal signals.
- Read returns 0 when all write ends are closed (EOF); write fails on broken pipe.
- Closing either end wakes both queues.
- `pipe_is_ready()` reports EPOLLIN/EPOLLOUT/EPOLLHUP/EPOLLERR for epoll.

Shell pipelines and the terminal app use real kernel pipes; the shell's line editor additionally uses `epoll` on pipe fds.

## memfd

`SYS_MEMFD_CREATE` creates an anonymous memory file (name up to 63 bytes) backed by up to 4096 lazily allocated pages (16 MiB). Reads/writes copy in 16 KiB chunks under the memfd lock. Truncate grows or shrinks the page array.

`SYS_MMAP` on a memfd fd:

- `MAP_SHARED` maps the backing frames with `PTE_SHARED` and bumps frame refcounts.
- `MAP_PRIVATE` copies pages into the mapping.
- `MAP_ANONYMOUS` maps fresh zeroed frames.

The window manager validates memfd-backed client buffers with `SYS_FSIZE` before mapping them.
