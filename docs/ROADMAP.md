# uniOS Roadmap to v1.0

> One focused feature per version. Ship a working OS at 1.0.

## Current: v0.4.1 ✓
**Shell Piping & Text Processing + Polish**
- [x] Command piping (`cmd1 | cmd2 | cmd3`)
- [x] `wc` - line/word/char count
- [x] `head` / `tail` - first/last N lines
- [x] `grep` - pattern search (case-insensitive)
- [x] Improved error messages with context

---

## v0.5.0 — Shell Scripts
**Goal: Run sequences of commands from files**
- [ ] `run <script>` - Execute commands from file
- [ ] Comments in scripts (`#`)
- [ ] Variables (`$NAME`, `set NAME=value`)
- [ ] Basic conditionals (`if`, `else`)

---

## v0.6.0 — Directories
**Goal: Hierarchical filesystem**
- [ ] Directory support in uniFS
- [ ] `mkdir`, `rmdir`, `cd`, `pwd` commands
- [ ] Path resolution (`foo/bar/file.txt`)
- [ ] Update `ls` for directories

---

## v0.7.0 — Background Tasks
**Goal: Run commands without blocking**
- [ ] `&` suffix for background execution
- [ ] `jobs` - list running tasks
- [ ] `kill <id>` - terminate task
- [ ] Task status display

---

## v0.8.0 — Text Editor
**Goal: Edit files within the OS**
- [ ] `edit <file>` - Simple line editor
- [ ] Insert, delete, save operations
- [ ] Arrow key navigation
- [ ] Syntax-free, minimal UI

---

## v0.9.0 — Polish & Stability
**Goal: Prepare for stable release**
- [ ] Fix all known bugs
- [ ] Improve error messages
- [ ] Performance optimization
- [ ] Documentation cleanup
- [ ] Code cleanup and comments

---

## v1.0.0 — Stable Release
**Goal: A complete, documented, educational OS**
- [ ] All features working reliably
- [ ] Comprehensive `help` and docs
- [ ] Clean codebase
- [ ] Website and README finalized
- [ ] Tagged release on GitHub

---

## Guidelines

1. **One theme per version** — Don't mix unrelated features
2. **Keep it buildable** — Every version must boot and work
3. **Test before bump** — Verify in QEMU before releasing
4. **Document as you go** — Update README/docs with each release
