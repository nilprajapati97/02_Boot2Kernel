# Day 25 — `nlibc`: A Minimal C Library for Userspace

> **Goal**: Build a small, static C library (`nlibc.a`) that user programs link against. It provides the syscall stubs, `crt0`, `string.h`, `stdio.h` (line-buffered), `malloc/free`, `errno`, and `_exit`. This lets us write user binaries in plain C without depending on glibc/musl.
>
> **Why today**: The shell on Day 26 and all later userspace use `printf`, `read`, `malloc`. We do not link against a host libc — that would drag in features our kernel lacks (TLS, dynamic linking, etc.).

---

## 1. Background

### 1.1 Boot of a userspace binary (`crt0`)
After kernel `eret` to `e_entry`:
1. `_start` reads `argc`/`argv`/`envp` from the stack.
2. Sets up `errno = 0`, initializes BSS (loader already zeroed it via PT_LOAD memsz).
3. Calls `__libc_init()` (heap, stdio, env).
4. Calls `main(argc, argv, envp)`.
5. Calls `_exit(ret)`.

### 1.2 Syscall stub pattern (AArch64)
```asm
.global _syscall6
_syscall6:                 // long _syscall6(num, a0..a5)
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0
    ret
```

### 1.3 Errno convention
The kernel returns `-errno` in `x0` for failures. The libc wrappers map:
```c
if ((u64)ret >= (u64)-4095) { errno = -(long)ret; return -1; }
return ret;
```

---

## 2. Design

### 2.1 Directory layout
```
user/
  nlibc/
    include/
      stdio.h string.h stdlib.h unistd.h errno.h sys/syscall.h
    src/
      crt0.S
      syscall.S
      stdio.c
      string.c
      malloc.c
      errno.c
    Makefile
  bin/
    hello.c
    cat.c
    sh/   (Day 26)
```

### 2.2 Build flow
- `nlibc.a` is built **without** the host libc (`-nostdlib -nostdinc -ffreestanding -static`).
- User programs link as: `gcc -nostdlib -static crt0.o user.o -lnlibc -o user.elf`.

---

## 3. Implementation

### 3.1 `crt0.S`
```asm
.section .text._start, "ax", %progbits
.global _start
_start:
    // argc at [sp], argv[] starts at [sp+8], envp follows
    ldr x0, [sp]            // argc
    add x1, sp, #8          // argv
    add x2, x0, #1
    add x2, sp, x2, lsl #3
    add x2, x2, #8          // envp = argv + argc + 1
    bl  __libc_init
    bl  main
    bl  _exit
1:  b   1b
```

### 3.2 `syscall.S` — one stub for 0..6 args
```asm
.section .text
.global __syscall0
.global __syscall1
.global __syscall2
.global __syscall3
.global __syscall4
.global __syscall5
.global __syscall6
__syscall6:
    mov x8, x0
    mov x0, x1; mov x1, x2; mov x2, x3
    mov x3, x4; mov x4, x5; mov x5, x6
    svc #0
    ret
__syscall5: mov x8,x0; mov x0,x1; mov x1,x2; mov x2,x3; mov x3,x4; mov x4,x5; svc #0; ret
__syscall4: mov x8,x0; mov x0,x1; mov x1,x2; mov x2,x3; mov x3,x4; svc #0; ret
__syscall3: mov x8,x0; mov x0,x1; mov x1,x2; mov x2,x3; svc #0; ret
__syscall2: mov x8,x0; mov x0,x1; mov x1,x2; svc #0; ret
__syscall1: mov x8,x0; mov x0,x1; svc #0; ret
__syscall0: mov x8,x0; svc #0; ret
```

### 3.3 `unistd.c` — system call wrappers
```c
#include <errno.h>
extern long __syscall3(long,long,long,long);
extern long __syscall1(long,long);
extern long __syscall0(long);

#define SYS_read 63
#define SYS_write 64
#define SYS_exit 93
#define SYS_openat 56
#define SYS_close 57
#define SYS_brk 214

static long ret_or_err(long r) {
    if ((unsigned long)r >= (unsigned long)-4095) { errno = -r; return -1; }
    return r;
}

long read (int fd, void *b, unsigned long n) { return ret_or_err(__syscall3(SYS_read, fd, (long)b, n)); }
long write(int fd, const void *b, unsigned long n) { return ret_or_err(__syscall3(SYS_write, fd, (long)b, n)); }
void _exit(int c) { __syscall1(SYS_exit, c); for(;;); }
int  open (const char *p, int fl, ...) { return ret_or_err(__syscall3(SYS_openat, -100/*AT_FDCWD*/, (long)p, fl)); }
int  close(int fd) { return ret_or_err(__syscall1(SYS_close, fd)); }
void *sbrk(long inc) {
    static unsigned long cur;
    if (!cur) cur = __syscall1(SYS_brk, 0);
    long nb = cur + inc;
    long g = __syscall1(SYS_brk, nb);
    if (g != nb) { errno = 12; return (void*)-1; }
    void *old = (void*)cur; cur = nb; return old;
}
```

### 3.4 `malloc.c` (bump+freelist)
```c
typedef struct hdr { unsigned long size; struct hdr *next; } hdr;
static hdr *free_list;

void *malloc(unsigned long n) {
    n = (n + 15) & ~15UL;
    hdr **pp = &free_list;
    while (*pp) {
        if ((*pp)->size >= n) {
            hdr *b = *pp; *pp = b->next;
            return (void*)(b + 1);
        }
        pp = &(*pp)->next;
    }
    hdr *b = sbrk(sizeof(hdr) + n);
    if ((long)b == -1) return 0;
    b->size = n; return (void*)(b + 1);
}
void free(void *p) {
    if (!p) return;
    hdr *b = (hdr*)p - 1;
    b->next = free_list; free_list = b;
}
```

### 3.5 `stdio.c` — minimal `printf` + line-buffered stdout
```c
#include <stdarg.h>
extern long write(int,const void*,unsigned long);

static char outbuf[256]; static int outlen;
static void flush(void){ if (outlen){ write(1, outbuf, outlen); outlen=0; } }
static void putc1(char c){ outbuf[outlen++]=c; if(c=='\n' || outlen==sizeof outbuf) flush(); }
static void puts1(const char*s){ while(*s) putc1(*s++); }
static void putd(long v){
    char b[24]; int i=0; int neg=v<0; unsigned long u = neg ? -v : v;
    do { b[i++] = '0' + u%10; u/=10; } while(u);
    if (neg) b[i++]='-';
    while (i--) putc1(b[i]);
}
static void putx(unsigned long v){
    char *d="0123456789abcdef"; char b[16]; int i=0;
    do { b[i++]=d[v&0xf]; v>>=4; } while(v);
    while (i--) putc1(b[i]);
}
int printf(const char *f, ...) {
    va_list ap; va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { putc1(*f); continue; }
        f++;
        switch (*f) {
        case 'd': putd(va_arg(ap, int)); break;
        case 'l': f++; putd(va_arg(ap, long)); break;
        case 's': puts1(va_arg(ap, char*)); break;
        case 'x': putx(va_arg(ap, unsigned int)); break;
        case 'c': putc1((char)va_arg(ap, int)); break;
        case '%': putc1('%'); break;
        }
    }
    va_end(ap); flush();
    return 0;
}
```

### 3.6 `string.c`
```c
void *memcpy(void *d,const void *s,unsigned long n){char*a=d;const char*b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d,int c,unsigned long n){char*a=d;while(n--)*a++=c;return d;}
int memcmp(const void*a,const void*b,unsigned long n){const char*x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;x++;y++;}return 0;}
unsigned long strlen(const char*s){unsigned long n=0;while(s[n])n++;return n;}
char *strcpy(char*d,const char*s){char*r=d;while((*d++=*s++));return r;}
int strcmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a-*b;}
char *strchr(const char*s,int c){while(*s){if(*s==(char)c)return (char*)s;s++;}return 0;}
```

### 3.7 `__libc_init`
```c
extern char **environ;
int errno;
void __libc_init(int argc, char **argv, char **envp) {
    (void)argc; (void)argv;
    environ = envp;
}
```

### 3.8 Makefile
```makefile
CC := aarch64-linux-gnu-gcc
AR := aarch64-linux-gnu-ar
CFLAGS := -ffreestanding -nostdlib -nostdinc -Iinclude -O2 -Wall -fno-stack-protector -static
OBJS := src/syscall.o src/unistd.o src/malloc.o src/stdio.o src/string.o src/errno.o src/libc_init.o
nlibc.a: $(OBJS); $(AR) rcs $@ $^
%.o: %.c; $(CC) $(CFLAGS) -c $< -o $@
%.o: %.S; $(CC) $(CFLAGS) -c $< -o $@

# crt0.o produced separately, not in the archive:
crt0.o: src/crt0.S; $(CC) $(CFLAGS) -c $< -o $@
```

### 3.9 Linker script for user programs (`user/user.ld`)
```
ENTRY(_start)
SECTIONS {
    . = 0x400000;
    .text : { *(.text._start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
```

### 3.10 Sample hello build
```
$ aarch64-linux-gnu-gcc -ffreestanding -nostdlib -nostdinc -Iuser/nlibc/include \
    -static -T user/user.ld user/nlibc/crt0.o user/bin/hello.c user/nlibc/nlibc.a \
    -o build/initramfs_root/bin/hello
```

---

## 4. Pitfalls

1. **`printf("%d", x)` where x is `long`**: the format must match width or you'll read wrong bytes (no varargs promotion past `int`). Document; offer `%ld`.
2. **`sbrk` first call**: kernel's `brk(0)` returns current break; cache it. Don't assume zero.
3. **Static linking only**: do not emit `R_AARCH64_RELATIVE` etc. (no dynamic loader). `-static` plus a non-PIC `gcc` config is required (`-fno-PIC`).
4. **Stack alignment**: AArch64 requires `sp` 16-byte aligned at function calls. `_start` already lands aligned; if you push/pop, keep pairs.
5. **`errno` not thread-local** (we have no threads yet) — leave as a global; flag for TLS later.

---

## 5. Verification

```
$ build/initramfs_root/bin/hello
hello from userspace
$ aarch64-linux-gnu-readelf -h build/initramfs_root/bin/hello | grep -E 'Type|Entry'
  Type:                              EXEC (Executable file)
  Entry point address:               0x400078
$ qemu run → loader maps .text @ 0x400000, hits _start, printf works, exits 0.
```

`cat`-style program reading a file from ext2 should also work without modification.

---

## 6. Stretch

- `vfprintf` with float (`%f`) — soft-float (`-mgeneral-regs-only` means no NEON; use a soft impl).
- `getenv/setenv/unsetenv` operating on `environ`.
- `qsort`, `bsearch`, `atoi`, `strtol`.
- `assert.h` macro.

---

## 7. References

- musl libc — reference for clean, minimal syscall wrappers.
- *Linkers and Loaders* (Levine) — section on `crt0`.
- AArch64 PCS (Procedure Call Standard).
