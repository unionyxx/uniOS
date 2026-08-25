# Shell Scripting Reference

The uniOS userspace shell supports basic scripting for diagnostics, automation, and system tasks.

## Running Scripts

Scripts are plain text files containing shell commands. Run a script with:

```sh
run /data/startup.sh
```

You can also use `source` to run a script within the current shell context:

```sh
source diagnostics.sh
```

Lines starting with `#` are treated as comments.

External programs are launched by name (resolved to `/bin/<name>.elf`) or by path; there is no separate `exec` builtin.

## Variables

### Setting and Unsetting
Variables are session-local. Use `set` to define or update a variable:

```sh
set PATH=/bin
set OS_NAME=uniOS
```

To remove a variable:
```sh
unset OS_NAME
```

### Accessing Variables
Prefix the variable name with `$` to expand its value:

```sh
echo $PATH
```

The special variable `$?` contains the exit status of the previous command (0 for success, non-zero for error).

### Aliases
`alias` defines a name that expands before command parsing; `unalias` removes it:

```sh
alias ll=ls
unalias ll
```

## Conditionals

`if` blocks allow for basic branching.

```sh
if $STATUS == 0
    echo "Operation successful"
else
    echo "Operation failed"
endif
```

Supported operators:
- **String**: `==`, `!=`
- **Numeric**: `<`, `>`, `<=`, `>=`

## Loops

`while` blocks repeat execution as long as a condition is met.

```sh
# Example: Waiting for a file to appear
while ! stat /data/ready.txt
    sleep 500
end
```

For integer arithmetic, use `expr` (it evaluates and prints the result; there is no command substitution):

```sh
expr 2 + 3
```

## Pipes

Pipeable commands can be chained with `|` (4 KiB pipe buffers):

```sh
cat /etc/shell.rc | grep set | wc
```

Pipeable builtins include `cat`, `wc`, `head`, `tail`, `grep`, `sort`, `uniq`, `rev`, `tac`, `nl`, and `tr`.

## Built-in Commands

The shell includes built-in support for:

- **File System**: `ls`, `cd`, `pwd`, `cat`, `stat`, `touch`, `rm`, `rmdir`, `mkdir`, `cp`, `mv`, `tree`, `find`, `du`, `df`, `mount`, `write`, `append`, `hexdump`.
- **Text / Pipes**: `echo`, `wc`, `head`, `tail`, `grep`, `sort`, `uniq`, `rev`, `tac`, `nl`, `tr`.
- **System Info**: `mem`, `kheap`, `ps`, `uptime`, `date`, `version`, `uname`, `sysinfo`, `cpuinfo`, `dmesg`, `storage`.
- **Networking**: `ping`, `resolve`.
- **Audio**: `sound` (kernel parser), `play` (userspace WAV parser).
- **Scripting / Session**: `run`, `source`, `set`, `unset`, `alias`, `unalias`, `read`, `test`, `expr`, `time`, `sleep`, `env`, `history`, `which`, `type`, `random`, `true`, `false`, `quiet`.
- **Process / Power**: `kill`, `reboot`, `poweroff`, `help`, `clear`, `exit`.

## Limits and Constraints

- **Variable Count**: Max 32 variables per session.
- **Variable Length**: Max 127 bytes per value (128-byte slot including terminator).
- **Script Length**: Max 256 lines.
- **Loop Iterations**: Safety cap at 10,000 iterations per script run.
- **Nesting**: Max 16 levels of nested blocks.
- **History**: Last 32 commands.
- **Pipe Buffers**: 4 KiB per pipe stage.
