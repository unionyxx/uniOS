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

Lines starting with `#` are treated as comments. External programs are launched by name (resolved to `/bin/<name>.elf`) or by path; there is no separate `exec` builtin. There is no command substitution — command output cannot be captured into variables.

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

`if` blocks allow basic branching:

```sh
if $? == 0
    echo "Operation successful"
else
    echo "Operation failed"
endif
```

`test` evaluates the same conditions interactively. Supported operators:

- **String**: `==`, `!=`
- **Numeric**: `<`, `>`, `<=`, `>=`
- A single operand is true when it is non-empty and not `0`.

## Loops

`while` blocks repeat as long as a condition holds; `end` closes the block and re-evaluates the `while` line:

```sh
# Ask until the operator answers "no".
set AGAIN=yes
while $AGAIN == yes
    echo "Running maintenance pass..."
    sleep 1000
    read AGAIN
end
```

For integer arithmetic, use `expr` (it evaluates and prints the result; without command substitution it cannot feed a variable):

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
- **Variable Length**: Max 127 bytes per value (128-byte slot including terminator); names up to 31 bytes.
- **Aliases**: 32 slots, same name/value sizes.
- **Script Length**: Max 256 lines (file reads capped at 32 KiB).
- **Loop Iterations**: Safety cap at 10,000 iterations per script run.
- **Nesting**: Max 16 levels of nested blocks.
- **History**: Last 32 commands.
- **Pipe Buffers**: 4 KiB per pipe stage; pipelines up to 10 stages.
