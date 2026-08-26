# System Calls

The syscall surface is defined in `include/uapi/syscalls.h` and dispatched by a single switch in `src/kernel/core/syscall.cpp`.

## ABI

Userspace invokes syscalls with the `syscall` instruction via inline wrappers (`src/usr/libc/syscall.h`):

- Number in `rax`; arguments in `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`; return value in `rax`. `rcx` and `r11` are clobbered.
- Per-core MSRs: `STAR` (kernel CS `0x08`, user selector base with the AMD SYSRET quirk), `LSTAR` = `syscall_entry`, `SFMASK` clears TF/IF/DF/NT/RF/AC, `EFER.SCE` enabled.
- The entry path (`src/arch/x86_64/boot/usermode.asm`) swaps to the kernel stack via per-CPU GS data, builds a `SyscallFrame` (registers plus saved `rip/cs/rflags/rsp/ss`), and enables interrupts — syscalls run with interrupts on.
- On return, pending signals are checked, volatile registers are zeroed (no kernel state leaks to user), and `o64 sysret` returns. Non-canonical `rcx` takes an `iretq` fallback instead.
- A legacy `int 0x80` gate (DPL 3) reaches the same handler; userspace does not use it.

Error conventions: classic calls return `(uint64_t)-1`; extended calls (270 and up) return negative errno values (`-4` EINTR, `-9` EBADF, `-12` ENOMEM, `-14` EFAULT, `-19` ENODEV, `-22` EINVAL, `-24` EMFILE, `-32` EPIPE). User pointers are validated against the process VMA list; string copies are bounded; data copies use SMAP-aware fixup paths.

## Files and Descriptors

| # | Name | Purpose |
| --- | --- | --- |
| 0 | `SYS_READ` | Read from fd; stdin polls keyboard + serial |
| 1 | `SYS_WRITE` | Write to fd; stdout/stderr go to boot log / serial |
| 2 | `SYS_OPEN` | Open with O_CREAT/O_TRUNC/O_APPEND |
| 3 | `SYS_CLOSE` | Close fd |
| 4 | `SYS_STAT` | Stat a path |
| 22 | `SYS_PIPE` | Create a pipe (two fds) |
| 33 | `SYS_DUP2` | Duplicate fd |
| 78 | `SYS_GETDENTS` | Stateful readdir: one entry per call |
| 83 | `SYS_MKDIR` | Create directory |
| 87 | `SYS_UNLINK` | Delete file |
| 224 | `SYS_RENAME` | Rename file or directory |
| 225 | `SYS_RMDIR` | Remove empty directory |
| 277 | `SYS_FTRUNCATE` | Truncate open file (FAT32: to 0 only) |
| 279 | `SYS_FSIZE` | Size of an open file (used to validate memfd buffers) |
| 280 | `SYS_SYNC` | Flush page cache and filesystem state |
| 281 | `SYS_LSEEK` | Reposition fd offset (`SEEK_SET/CUR/END` from `uapi/fs.h`) |

## Processes, Threads, Signals

| # | Name | Purpose |
| --- | --- | --- |
| 13 | `SYS_SIGACTION` | Install or query a signal handler |
| 15 | `SYS_SIGRETURN` | Return from a signal handler |
| 24 | `SYS_YIELD` | Voluntary reschedule |
| 37 | `SYS_KILL` | Send a signal to a pid |
| 39 | `SYS_GETPID` | Current pid |
| 57 | `SYS_FORK` | Clone the process (CoW) |
| 59 | `SYS_EXEC` | Replace the process image |
| 60 | `SYS_EXIT` | Exit with status |
| 61 | `SYS_WAIT4` | Wait for children (`WNOHANG` = 1) |
| 62 | `SYS_GETPROCS` | Snapshot the process list |
| 102 | `SYS_GETUID` | Current uid |
| 105 | `SYS_SETUID` | Set uid (root only) |
| 271 | `SYS_THREAD_CREATE` | Create a thread in the same address space |

## Memory

| # | Name | Purpose |
| --- | --- | --- |
| 9 | `SYS_MMAP` | Anonymous or memfd-backed mapping |
| 10 | `SYS_MUNMAP` | Unmap a range |
| 270 | `SYS_FUTEX` | FUTEX_WAIT / FUTEX_WAKE (keyed by physical page) |
| 272-274 | `SYS_EPOLL_CREATE/CTL/WAIT` | Epoll instances over fds |
| 275 | `SYS_MPROTECT` | Change protections on mapped pages |
| 276 | `SYS_MEMFD_CREATE` | Anonymous memory-file fd (max 16 MiB) |

## Display, GUI, Input, Sound

| # | Name | Purpose |
| --- | --- | --- |
| 205-207 | `SYS_SOUND_PLAY/WRITE/CONFIG` | File playback, raw PCM push, stream format |
| 282 | `SYS_SOUND_STREAM_OPEN` | Open streaming playback (rate, channels, 16-bit); stops previous playback |
| 283 | `SYS_SOUND_STREAM_END` | No more stream data; drain queued PCM then auto-close |
| 284 | `SYS_SOUND_STOP` | Abort the stream and stop the card |
| 285 | `SYS_SOUND_PAUSE` | Pause stream playback (idempotent) |
| 286 | `SYS_SOUND_RESUME` | Resume stream playback (idempotent) |
| 287 | `SYS_SOUND_STATUS` | Fill `sound_status` (`uapi/sound.h`): played/queued bytes, format, flags |
| 288 | `SYS_SOUND_VOLUME` | Card master volume 0-100 |
| 208 | `SYS_FB_INFO` | Framebuffer geometry |
| 209 | `SYS_FB_MMAP` | Map the framebuffer at `0x200000000` (WC) |
| 210 | `SYS_GET_EVENT` | Pop an event from the process queue (blocking flag) |
| 211 | `SYS_FB_FLUSH` | Store fence for WC writes |
| 212 | `SYS_FB_BLIT` | Full-frame blit from a user buffer |
| 214 | `SYS_FB_BLIT_RECT` | Rect blit |
| 244 | `SYS_POST_EVENT` | Push an event into another process's queue |
| 245 | `SYS_GUI_REGISTER_WM` | Caller becomes the window manager |
| 246 | `SYS_GUI_SET_FOCUS` | WM-only: set the focused pid |
| 247-262 | `SYS_DISPLAY_*` | KMS-style display API (see below) |

Display syscalls: `GET_CAPS` 247, `PRESENT` 248, `WAIT` 249, `GET_STATUS` 250, `QUERY_CONNECTORS` 252, `GET_MODES` 253, `SET_MODE` 254, `BUFFER_CREATE` 255, `BUFFER_MAP` 256, `BUFFER_DESTROY` 257, `COMPOSE_SUBMIT` 258, `EVENT_WAIT` 259, `ATOMIC_COMMIT` 260, `BUFFER_SET_WM_ACCESS` 261, `SURFACE_IMPORT` 262. Present requests carry up to 128 damage rects; compose requests up to 32 layers and 32 damage rects. See [Display](display.md).

## Shared Memory

| # | Name | Purpose |
| --- | --- | --- |
| 240 | `SYS_SHM_GET` | Allocate a block (max 16 MiB), returns id 0-63 |
| 241 | `SYS_SHM_MAP` | Map at `0x300000000 + id * 16 MiB` |
| 242 | `SYS_SHM_FREE` | Unmap and release |
| 243 | `SYS_SHM_INFO` | Block size |
| 251 | `SYS_SHM_UNMAP` | Unmap only |
| 263 | `SYS_SHM_GET_OWNER` | Owner pid |
| 278 | `SYS_FD_TRANSFER` | Copy an fd's vnode into another process (pid 0 = the WM) |

## Network

| # | Name | Purpose |
| --- | --- | --- |
| 220 | `SYS_SOCKET` | AF_INET; SOCK_STREAM = TCP, SOCK_DGRAM = UDP |
| 221 | `SYS_BIND` | Bind a port |
| 222/223 | `SYS_SENDTO/RECVFROM` | UDP datagrams |
| 229/230/233 | `SYS_CONNECT/SEND/RECV` | TCP stream |
| 234 | `SYS_CLOSESOCKET` | Close a socket |
| 235 | `SYS_RESOLVE` | DNS hostname to IPv4 |

Socket handles are not file descriptors: handle = `kind << 12 | index`. See [Networking](networking.md).

## System

| # | Name | Purpose |
| --- | --- | --- |
| 169 | `SYS_REBOOT` | Reboot |
| 231 | `SYS_POWEROFF` | ACPI poweroff |
| 201 | `SYS_GETTIME` | RTC wall clock |
| 202 | `SYS_GETUPTIME` | Seconds since boot |
| 203 | `SYS_GETMEMINFO` | Total/free/used memory |
| 204 | `SYS_GETSYSINFO` | Kernel commit, bootloader, timer Hz, CPU count, debug flag |
| 213 | `SYS_GET_TSC_FREQ` | Calibrated TSC frequency in MHz (0 if uncalibrated) |
| 215 | `SYS_GET_TICKS` | Raw timer ticks |
| 216 | `SYS_SLEEP_MS` | Sleep for at least the given milliseconds (real sleep, signal-interruptible) |
| 226 | `SYS_GETVOLUMES` | Volume list (max 16) |
| 227/228 | `SYS_STORAGE_GET_MODE/SET_MODE` | Storage guard mode; SET is WM-only |
| 232 | `SYS_SET_QUIET` | Toggle boot framebuffer logging |
| 236 | `SYS_GETRANDOM` | Up to 64 KiB from a seeded xorshift* PRNG (not cryptographic) |
