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

> [!NOTE]
> Variable increments are not natively supported in the current shell. Loops are primarily intended for polling or persistent diagnostic tasks.

## Built-in Commands

The shell includes built-in support for:
- **File System**: `ls`, `cd`, `pwd`, `cat`, `touch`, `rm`, `mkdir`, `cp`, `mv`.
- **System Info**: `mem`, `ps`, `uptime`, `date`, `uname`, `sysinfo`, `cpuinfo`.
- **Networking**: `ping`, `resolve`.
- **Process**: `kill`, `exec`, `reboot`, `poweroff`.

## Limits and Constraints

- **Variable Count**: Max 32 variables per session.
- **Variable Length**: Max 256 bytes per value.
- **Script Length**: Max 256 lines.
- **Loop Iterations**: Safety cap at 10,000 iterations.
- **Nesting**: Max 16 levels of nested blocks.
