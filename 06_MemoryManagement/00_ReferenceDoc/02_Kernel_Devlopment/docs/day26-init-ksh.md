# Day 26 — `init` + `ksh`: Interactive Shell

> **Goal**: Implement two userspace programs: a minimal `init` (PID 1) that sets up `/dev`, mounts `/proc` (stub), launches `/bin/ksh`, and reaps zombies; and `ksh`, a tiny shell supporting prompt, `cd`, `exit`, command execution via `fork+execve+wait4`, and basic I/O redirection (`>`, `<`).
>
> **Why today**: Phase 6 milestone — *the kernel is interactive*. You can type commands.

---

## 1. Background

### 1.1 PID 1 responsibilities
1. Cannot exit. If it does, kernel panics.
2. Must reap orphan zombies (kernel reparents them here on Day 19).
3. Sets initial file descriptors for itself and inherits to children (`/dev/console` → fd 0,1,2).
4. Mounts / verifies essential filesystems.
5. Spawns the shell or service manager.

### 1.2 Shell pipeline
A line of input → tokenize on whitespace + redirection ops → fork child → optionally `dup2` file descriptors for redirection → `execve` → parent `wait4`.

For day 26: support **one** command per line; no pipelines (`|`) — that's stretch.

---

## 2. `init`

### 2.1 `user/bin/init.c`
```c
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

extern long __syscall3(long,long,long,long);
extern long __syscall1(long,long);
#define SYS_clone   220
#define SYS_execve  221
#define SYS_wait4   260
#define SYS_mkdirat 34
#define SYS_mount   40

static int forkit(void) { return __syscall3(SYS_clone, 0, 0, 0); }
static int execve_path(const char *p, char *const argv[], char *const envp[]) {
    return __syscall3(SYS_execve, (long)p, (long)argv, (long)envp);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv;
    write(1, "nkernel init starting\n", 22);

    /* Reaper loop: fork the shell, restart on exit, reap any zombies in between */
    for (;;) {
        int pid = forkit();
        if (pid == 0) {
            char *av[] = {"/bin/ksh", 0};
            execve_path("/bin/ksh", av, envp);
            write(2, "init: exec ksh failed\n", 22);
            _exit(127);
        }
        /* Parent: reap any zombie (including the shell), then re-spawn shell when it exits */
        int st, who;
        do {
            who = __syscall3(SYS_wait4, -1, (long)&st, 0);
        } while (who != pid && who > 0);
        write(1, "init: shell exited, restarting\n", 32);
    }
}
```

### 2.2 init lives at `/init` in initramfs
The kernel loader (`kmain`) does `execve("/init")` once VFS is up. If it's missing, panic.

---

## 3. `ksh`

### 3.1 Architecture
```
main()
  loop:
    print prompt
    read line (from fd 0)
    parse line into argv[] + redir info
    if builtin -> handle in shell
    else fork; child sets up redir; execve; parent wait4
```

### 3.2 `user/bin/sh/ksh.c`
```c
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

extern long __syscall3(long,long,long,long);
extern long __syscall1(long,long);
#define SYS_clone   220
#define SYS_execve  221
#define SYS_wait4   260
#define SYS_chdir   49
#define SYS_dup3    24
#define SYS_openat  56

#define MAX_ARG 32

static int readline(char *buf, int max) {
    int n = 0; char c;
    while (n < max - 1) {
        long r = read(0, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c == 8 || c == 127) { if (n) { n--; write(1, "\b \b", 3); } continue; }
        write(1, &c, 1);
        buf[n++] = c;
    }
    write(1, "\n", 1);
    buf[n] = 0;
    return n;
}

struct cmd {
    char *argv[MAX_ARG];
    int argc;
    char *infile;
    char *outfile;
};

static int parse(char *line, struct cmd *c) {
    c->argc = 0; c->infile = 0; c->outfile = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '<') { p++; while (*p == ' ') p++; c->infile = p; while (*p && *p != ' ') p++; if (*p) *p++ = 0; continue; }
        if (*p == '>') { p++; while (*p == ' ') p++; c->outfile = p; while (*p && *p != ' ') p++; if (*p) *p++ = 0; continue; }
        c->argv[c->argc++] = p;
        while (*p && *p != ' ' && *p != '<' && *p != '>') p++;
        if (*p) *p++ = 0;
    }
    c->argv[c->argc] = 0;
    return c->argc;
}

static int builtin(struct cmd *c) {
    if (!c->argc) return 1;
    if (!strcmp(c->argv[0], "exit")) _exit(c->argv[1] ? atoi(c->argv[1]) : 0);
    if (!strcmp(c->argv[0], "cd"))   { __syscall1(SYS_chdir, (long)(c->argv[1] ? c->argv[1] : "/")); return 1; }
    if (!strcmp(c->argv[0], "pwd"))  { /* getcwd syscall */ return 1; }
    return 0;
}

static void redir(struct cmd *c) {
    if (c->infile) {
        int fd = __syscall3(SYS_openat, -100, (long)c->infile, 0 /*O_RDONLY*/);
        if (fd >= 0) { __syscall3(SYS_dup3, fd, 0, 0); close(fd); }
    }
    if (c->outfile) {
        int fd = __syscall3(SYS_openat, -100, (long)c->outfile, 1 | 0100 | 01000 /*O_WRONLY|O_CREAT|O_TRUNC*/);
        if (fd >= 0) { __syscall3(SYS_dup3, fd, 1, 0); close(fd); }
    }
}

extern char **environ;
int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; environ = envp;
    char line[256]; struct cmd c;
    write(1, "Welcome to nkernel\n", 19);
    for (;;) {
        write(1, "$ ", 2);
        if (readline(line, sizeof line) < 0) break;
        if (parse(line, &c) == 0) continue;
        if (builtin(&c)) continue;
        long pid = __syscall3(SYS_clone, 0, 0, 0);
        if (pid == 0) {
            redir(&c);
            __syscall3(SYS_execve, (long)c.argv[0], (long)c.argv, (long)environ);
            write(2, "ksh: exec failed\n", 17);
            _exit(127);
        }
        int st;
        __syscall3(SYS_wait4, pid, (long)&st, 0);
    }
    return 0;
}
```

### 3.3 `dup3` syscall (kernel side)
Add to syscall table — copies one fd to another:
```c
long sys_dup3(struct pt_regs *r) {
    int old = r->regs[0], new = r->regs[1];
    struct files_struct *fs = current()->files;
    if (old < 0 || old >= OPEN_MAX || !fs->fd[old]) return -9;
    if (new < 0 || new >= OPEN_MAX) return -9;
    if (fs->fd[new]) {
        struct file *f = fs->fd[new]; fs->fd[new] = NULL;
        if (f->f_op->release) f->f_op->release(f->f_inode, f);
        kfree(f);
    }
    /* Day-26 simplification: refcount the file rather than dup, but for redirection inside a
       child this is fine because the child will exec immediately. */
    fs->fd[new] = fs->fd[old]; fs->fd[old] = NULL;
    return new;
}
```
(Proper `dup` requires `file` refcounting — promote to stretch.)

### 3.4 `chdir` syscall (kernel side)
```c
long sys_chdir(struct pt_regs *r) {
    char path[128];
    if (copy_from_user(path, (void*)r->regs[0], 128)) return -14;
    struct dentry *d = path_lookup(path);
    if (!d || !S_ISDIR(d->d_inode->i_mode)) return -2;
    current()->cwd = d;
    return 0;
}
```
`path_lookup` must consult `current()->cwd` when path is relative — add that.

---

## 4. Wiring it up

### 4.1 initramfs contents
```
/init                 ← user/bin/init
/bin/ksh              ← user/bin/sh/ksh
/bin/hello            ← user/bin/hello
/bin/cat              ← user/bin/cat (Day 25 stretch)
/dev/                 ← directory (entries get added Day 27)
/mnt/                 ← directory for ext2 mount
```

### 4.2 Boot sequence
```
kmain
 ├ mm/vmm/sched init
 ├ virtio/ext2/ramfs registration
 ├ mount ramfs(/) + cpio_populate
 ├ kthread_create(init_userland)
 └ schedule loop

init_userland kthread:
  setup_console_fds(current)
  load_elf("/init") -> execve
```

---

## 5. Pitfalls

1. **PID 1 dying**: kernel must `panic("init exited")`. We retry exec internally in `init.c` but also have a kernel guard.
2. **Shell loops on EOF**: if `read` returns 0 (EOF), we break — but our console blocks; document.
3. **Buffer overruns in parser**: 32 arg slots, 256-char line. Truncate with a clear message.
4. **No path search**: typing `ksh` instead of `/bin/ksh` won't work — implement `$PATH` later.
5. **Backspace handling**: depends on terminal mode — UART is raw, so the shell does its own echo + erase, which we did.

---

## 6. Verification (Phase 6 milestone)

```
[INFO] init: exec /init
nkernel init starting
Welcome to nkernel
$ /bin/hello
hello from userspace
$ cd /mnt
$ /bin/cat hello.txt
persistent
$ exit
init: shell exited, restarting
$
```

GDB: at `ksh` prompt, run `info threads` → idle, init, ksh, and any forked child.

---

## 7. Stretch

- Pipelines (`|`): two `openat`s + `dup3` + double fork.
- Job control: `&` and `wait`. Requires process groups (`setpgid`).
- History buffer in `ksh` with arrow-key navigation (ANSI escape parsing).
- `$VAR` expansion + `set/unset`.

---

## 8. References

- xv6 `sh.c` — the closest in spirit to our `ksh`.
- BusyBox `init` — production reaper loop pattern.
- POSIX *Shell Command Language* (very abridged subset here).
