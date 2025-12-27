# uniOS Shell Scripting Guide

uniOS shell scripting capabilities allow you to automate tasks with variables, control flow, and reusable script files.

## Quick Start

```bash
# Create a script using \n for newlines
write hello.sh "# My first script\nset NAME=World\necho Hello, $NAME!"

# Run it
run hello.sh
```

**Creating scripts line by line:**
```bash
touch myscript.sh
append myscript.sh "set N=3"
append myscript.sh "while $N > 0"
append myscript.sh "  echo $N"
append myscript.sh "  set N=$N-1"
append myscript.sh "end"
run myscript.sh
```

## Variables

### Setting Variables
```bash
set NAME=value       # Set a variable
set COUNT=5          # Numeric value
set MSG=Hello World  # Spaces allowed in values
```

> [!NOTE]
> The parser is whitespace-sensitive.
> - `set A=B` ✅
> - `set A = B` ❌ (Parser sees 3 arguments)

### Using Variables
```bash
echo $NAME           # Prints variable value
cat $FILENAME        # Use in any command
```

### Arithmetic
```bash
set I=0
set I=$I+1           # Increment: I becomes 1
set I=$I-1           # Decrement: I becomes 0
set X=$Y+10          # Add 10 to Y
```

### Special Variables
```bash
echo $?              # Last command exit status (0=success)
```

### Managing Variables
```bash
set                  # List all variables
unset NAME           # Remove a variable
```

## Control Flow

### Conditionals
```bash
if $NAME == hello
    echo Name is hello
endif

if $COUNT > 0
    echo Count is positive
else
    echo Count is zero or negative
endif
```

### Operators
| Operator | Description |
|----------|-------------|
| `==` | Equal (string comparison) |
| `!=` | Not equal |
| `<` | Less than (numeric) |
| `>` | Greater than (numeric) |
| `<=` | Less or equal (numeric) |
| `>=` | Greater or equal (numeric) |

### Loops
```bash
set I=0
while $I < 5
    echo Iteration $I
    set I=$I+1
end
```

## Comments

Lines starting with `#` are ignored:
```bash
# This is a comment
echo This runs  # Inline comments NOT supported
```

## Example Scripts

### Countdown
```bash
# countdown.sh - Count down from 5
set N=5
while $N > 0
    echo $N...
    set N=$N-1
end
echo Liftoff!
```

### File Processor
```bash
# process.sh - Show file info
set FILE=data.txt
if $FILE == data.txt
    echo Processing data file
    cat $FILE | wc
else
    echo Unknown file
endif
```

### System Check
```bash
# syscheck.sh - System information
echo === System Check ===
version
echo
echo === Memory ===
mem
echo
echo === Files ===
ls | wc
echo files in filesystem
```

## Limits

- Maximum 32 variables
- Maximum 256 characters per variable value
- Maximum 256 lines per script
- Maximum 10,000 iterations (infinite loop protection)
- Maximum 16 nested control blocks

## Error Messages

| Error | Cause |
|-------|-------|
| `'else' without matching 'if'` | Mismatched control flow |
| `'endif' without matching 'if'` | Missing `if` statement |
| `'end' without matching 'while'` | Missing `while` statement |
| `Unclosed control block` | Missing `endif` or `end` |
| `Script exceeded maximum iterations` | Infinite loop detected |
