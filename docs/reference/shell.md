# Shell

The shell (`/bin/shell.elf`, `src/usr/shell/`) is a line-oriented command interpreter with builtins, pipelines, redirection, scripting, history, and tab completion. The terminal app hosts it as a child process wired through pipes.

## Starting Programs

External programs are resolved as `/bin/<name>.elf` (falling back to `/bin/<name>`) or run by path. There is no separate `exec` builtin. See [Shell scripting](scripting.md) for variables, conditionals, and loops.

## Builtins

- **File system**: `ls`, `cd`, `pwd`, `cat`, `stat`, `touch`, `rm`, `rmdir`, `mkdir`, `cp`, `mv`, `tree`, `find`, `du`, `df`, `mount`, `write`, `append`, `hexdump`.
- **Text / pipes**: `echo`, `wc`, `head`, `tail`, `grep`, `sort`, `uniq`, `rev`, `tac`, `nl`, `tr`.
- **System info**: `mem`, `kheap`, `ps`, `uptime`, `date`, `version`, `uname`, `sysinfo`, `cpuinfo`, `dmesg`, `storage`.
- **Networking**: `ping`, `resolve` (ICMP echo is not exposed to userland; `ping` resolves the target and says so).
- **Audio**: `sound` (kernel parser), `play` (userspace WAV parser).
- **Scripting / session**: `run`, `source`, `set`, `unset`, `alias`, `unalias`, `read`, `test`, `expr`, `time`, `sleep`, `env`, `history`, `which`, `type`, `random`, `true`, `false`, `quiet`.
- **Process / power**: `kill`, `reboot`, `poweroff`, `help`, `clear`, `exit`.

`storage` shows and sets the storage guard mode (off / read-only / writable). `df` and `mount` list volumes with devices, mount points, and flags.

## Pipelines and Redirection

- Pipelines: up to 10 stages joined with `|`, each junction a real kernel pipe (4 KiB); stages run as forked children with `dup2`'d ends, waited in order.
- Pipeable builtins include `cat`, `wc`, `head`, `tail`, `grep`, `sort`, `uniq`, `rev`, `tac`, `nl`, `tr`.
- Redirection: `<`, `>`, `>>` via `dup2` with backup descriptors.
- Exit codes: builtin failure 1, unknown command 127, failed external launch 126, otherwise the child's status. `$?` exposes the last status and the prompt marker reflects it.

```sh
cat /etc/shell.rc | grep set | wc
```

## Line Editing

- Movement: left/right, Home/End, Ctrl-A/E.
- Editing: backspace, Delete, Ctrl-D (delete or exit on empty line), Ctrl-K (kill to end), Ctrl-U (kill line), Ctrl-W (kill word), Ctrl-L (clear).
- History: up/down with consecutive-duplicate suppression; 32 entries (`history`).
- Tab completion: builtin names at the command position; path completion from `SYS_GETDENTS` elsewhere (unique suffix or common prefix, listing on ambiguity).

## Session

- Prompt: `root@unios <cwd> $` (or `!` when the last command failed).
- Initial cwd: `/data` when present, else `/`.
- Startup: sources `/etc/shell.rc`, falling back to `/data/shell.rc`.
