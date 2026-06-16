# Day 02 — Exception Levels & PL011 UART "Hello"

> **Goal**: Drop from EL2 to EL1, initialize the PL011 UART at `0x0900_0000`, implement `printk`, and print a "Hello from nkernel" line over the serial console.
>
> **Why today**: Without serial output we are debugging blind. Dropping to EL1 now (instead of staying at EL2) matches Linux's model and avoids reconfiguring later when we add virtualization-sensitive sysregs (`SCTLR_EL1`, `VBAR_EL1`, `TCR_EL1`). Both must happen before any "real" subsystem.

---

## 1. Background Theory

### 1.1 Exception Levels
| EL | Typical role | Sysreg suffix |
|---|---|---|
| EL3 | Secure monitor (firmware) | `_EL3` |
| EL2 | Hypervisor | `_EL2` |
| EL1 | OS kernel | `_EL1` |
| EL0 | User application | `_EL0` |

QEMU `-cpu cortex-a72` boots at **EL2** with virtualization extensions. Linux convention: drop to EL1 as quickly as possible.

### 1.2 EL2 → EL1 transition mechanics
- Configure **`HCR_EL2.RW = 1`** → EL1 executes in AArch64 (not AArch32).
- Set **`SPSR_EL2`** to "fake" return state: `M[3:0] = 0b0101` (EL1h), DAIF masked.
- Set **`ELR_EL2`** to the address we want to resume at (a label in EL1 code).
- Initialize **`SP_EL1`** (since we'll use SP_EL1 in EL1h).
- Mirror sane defaults into **`SCTLR_EL1`** (MMU off, but RES1 bits set), **`CPTR_EL2`** (no FP trap), **`CNTHCTL_EL2`** (allow EL1 access to timer), **`CNTVOFF_EL2 = 0`**.
- Execute `eret`. CPU is now at EL1 with `PC = ELR_EL2`.

### 1.3 PL011 UART (ARM PrimeCell)
Memory-mapped at `0x0900_0000` on QEMU virt. Key registers:
| Offset | Name | Use |
|---|---|---|
| `0x00` | `UARTDR` | Data register (R/W) |
| `0x18` | `UARTFR` | Flag register (TXFF bit 5, RXFE bit 4) |
| `0x24` | `UARTIBRD` | Integer baud rate divisor |
| `0x28` | `UARTFBRD` | Fractional baud rate divisor |
| `0x2C` | `UARTLCR_H` | Line control (WLEN, FEN) |
| `0x30` | `UARTCR` | Control (UARTEN, TXE, RXE) |
| `0x38` | `UARTIMSC` | Interrupt mask set/clear |
| `0x3C` | `UARTRIS` / `UARTMIS` | Raw / masked interrupt status |

For QEMU we can skip baud (it's emulated), but configure for parity with real silicon.

---

## 2. Design

### 2.1 New files
```
arch/arm64/boot/head.S          (extended: EL2->EL1 drop)
drivers/uart/pl011.c            (new)
include/kernel/printk.h         (new)
include/kernel/io.h             (new — readl/writel)
kernel/printk.c                 (new — sprintf-like core)
lib/string.c                    (new — memset/memcpy/strlen)
lib/printf.c                    (new — vsnprintf)
```

### 2.2 Call graph
```
_start (EL2)
  └─ el2_to_el1
        └─ eret -> el1_entry
                     └─ uart_init()
                     └─ printk("Hello, nkernel on QEMU virt\n")
                     └─ kmain(dtb)
```

---

## 3. Implementation

### 3.1 `head.S` additions (EL2→EL1 drop)
```asm
    .section ".text.boot","ax"
    .global _start
_start:
    /* Park secondaries (same as Day 1) */
    mrs     x1, MPIDR_EL1
    and     x1, x1, #0xFF
    cbnz    x1, .Lpark

    /* Determine current EL */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    .Lin_el1            // already at EL1 (e.g. real HW)

    /* --- EL2 setup --- */
    /* HCR_EL2: RW=1 (EL1 is AArch64) */
    mov     x0, #(1 << 31)
    msr     HCR_EL2, x0

    /* CPTR_EL2: don't trap FP/SVE from EL1/EL0 (basic) */
    mov     x0, #0x33ff
    msr     CPTR_EL2, x0

    /* CNTHCTL_EL2: EL1 timer/counter access */
    mrs     x0, CNTHCTL_EL2
    orr     x0, x0, #3          // EL1PCEN | EL1PCTEN
    msr     CNTHCTL_EL2, x0
    msr     CNTVOFF_EL2, xzr

    /* SCTLR_EL1: RES1 bits, MMU off */
    mov     x0, #0x0800
    movk    x0, #0x30d0, lsl #16
    msr     SCTLR_EL1, x0

    /* SPSR_EL2: EL1h, DAIF=1111 */
    mov     x0, #0x3c5
    msr     SPSR_EL2, x0

    adr     x0, .Lin_el1
    msr     ELR_EL2, x0
    eret

.Lin_el1:
    /* Set up SP_EL1 boot stack */
    adrp    x1, __stack_top
    add     x1, x1, :lo12:__stack_top
    mov     sp, x1

    /* Clear BSS */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:  cmp     x1, x2 ; b.ge 2f
    str     xzr, [x1], #8 ; b 1b
2:
    bl      kmain
3:  wfe
    b       3b

.Lpark:
    wfe
    b       .Lpark
```

### 3.2 `include/kernel/io.h`
```c
#ifndef _KERNEL_IO_H
#define _KERNEL_IO_H
#include <kernel/types.h>

static inline void writel(u32 v, u64 addr)
{
    asm volatile("str %w0, [%1]" :: "r"(v), "r"(addr) : "memory");
}
static inline u32 readl(u64 addr)
{
    u32 v;
    asm volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}
#define dmb_sy()  asm volatile("dmb sy" ::: "memory")
#define dsb_sy()  asm volatile("dsb sy" ::: "memory")
#define isb()     asm volatile("isb" ::: "memory")
#endif
```

### 3.3 `drivers/uart/pl011.c`
```c
#include <kernel/io.h>
#include <kernel/types.h>

#define UART0_BASE   0x09000000UL
#define UART_DR      (UART0_BASE + 0x00)
#define UART_FR      (UART0_BASE + 0x18)
#define UART_IBRD    (UART0_BASE + 0x24)
#define UART_FBRD    (UART0_BASE + 0x28)
#define UART_LCRH    (UART0_BASE + 0x2C)
#define UART_CR      (UART0_BASE + 0x30)
#define UART_IMSC    (UART0_BASE + 0x38)

#define FR_TXFF      (1 << 5)

void uart_init(void)
{
    writel(0, UART_CR);             /* disable */
    writel(0, UART_IMSC);           /* mask all IRQs */
    writel(26, UART_IBRD);          /* QEMU ignores; for HW = 115200 @ 48MHz */
    writel(3,  UART_FBRD);
    writel((3 << 5) | (1 << 4), UART_LCRH);  /* 8N1, FIFO en */
    writel((1 << 0) | (1 << 8) | (1 << 9), UART_CR); /* UARTEN|TXE|RXE */
}

void uart_putc(char c)
{
    while (readl(UART_FR) & FR_TXFF) ;
    writel((u32)c, UART_DR);
    if (c == '\n')
        uart_putc('\r');            /* CR for raw terminals — optional */
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}
```

### 3.4 `lib/printf.c` (excerpt — `vsnprintf` core)
```c
/* Minimal printf supporting %s %c %d %u %x %p %lx %ld */
#include <kernel/types.h>
#include <stdarg.h>

static int emit(char **bp, char *end, char c) {
    if (*bp < end) **bp = c;
    (*bp)++; return 1;
}
static int emit_str(char **bp, char *end, const char *s) {
    int n = 0; while (*s) n += emit(bp, end, *s++); return n;
}
static int emit_num(char **bp, char *end, u64 v, int base, int sign) {
    char buf[32]; int i = 0;
    if (sign && (s64)v < 0) { emit(bp, end, '-'); v = (u64)(-(s64)v); }
    do { int d = v % base; buf[i++] = d < 10 ? '0'+d : 'a'+d-10; v /= base; } while (v);
    int n = i; while (i--) emit(bp, end, buf[i]);
    return n;
}

int vsnprintf(char *out, size_t sz, const char *fmt, va_list ap)
{
    char *bp = out, *end = out + sz;
    while (*fmt) {
        if (*fmt != '%') { emit(&bp, end, *fmt++); continue; }
        fmt++;
        int lng = 0;
        if (*fmt == 'l') { lng = 1; fmt++; }
        switch (*fmt++) {
        case 's': emit_str(&bp, end, va_arg(ap, const char *)); break;
        case 'c': emit(&bp, end, (char)va_arg(ap, int)); break;
        case 'd': emit_num(&bp, end, lng ? va_arg(ap, long) : va_arg(ap, int), 10, 1); break;
        case 'u': emit_num(&bp, end, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0); break;
        case 'x': emit_num(&bp, end, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0); break;
        case 'p': emit_str(&bp, end, "0x"); emit_num(&bp, end, (u64)va_arg(ap, void*), 16, 0); break;
        case '%': emit(&bp, end, '%'); break;
        }
    }
    if (bp < end) *bp = 0; else if (sz) end[-1] = 0;
    return bp - out;
}
```

### 3.5 `kernel/printk.c`
```c
#include <kernel/printk.h>
#include <stdarg.h>

extern void uart_puts(const char *);
extern int vsnprintf(char *, size_t, const char *, va_list);

int printk(const char *fmt, ...)
{
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uart_puts(buf);
    return n;
}
```

### 3.6 Update `kernel/main.c`
```c
#include <kernel/types.h>
#include <kernel/printk.h>
extern void uart_init(void);

void kmain(u64 dtb_phys)
{
    uart_init();
    printk("Hello, nkernel on QEMU virt!\n");
    printk("DTB @ %p\n", (void *)dtb_phys);
    for (;;) asm volatile("wfe");
}
```

---

## 4. ARM64 Cheat-Sheet (Day 2)

```
HCR_EL2.RW         bit 31   AArch64 at EL1
SPSR_EL2.M[3:0]    0101     EL1h target mode
SCTLR_EL1 RES1     0x30d00800 (bits 22,23,28,29 + others)
CNTHCTL_EL2        EL1PCEN(1) | EL1PCTEN(0)
eret                         return using ELR_ELx + SPSR_ELx
```

---

## 5. Pitfalls

1. **Wrong `SPSR_EL2.M`**: any value other than `0101` (EL1h) lands in EL1t or aborts. Symptom: silent hang.
2. **Forgetting `SCTLR_EL1` RES1 bits** (`0x30d00800`): UNDEF or unpredictable behavior on `eret`.
3. **PL011 TX while FIFO full**: must poll `TXFF` or characters are dropped.
4. **`adr` vs `adrp`**: `adr` is ±1 MiB only — fine for `.Lin_el1` (same section), use `adrp+add` for far symbols.
5. **`va_list` w/ `-mgeneral-regs-only`**: works fine since all args are integers; avoid `%f` in `printk`.

---

## 6. Verification

```bash
make run
# Expect on serial:
#   Hello, nkernel on QEMU virt!
#   DTB @ 0x40000000   (or similar)
```

CI smoke marker: grep `^Hello, nkernel`.

GDB check:
```
(gdb) hb el1_entry
(gdb) c
(gdb) p/x $CurrentEL    # = 0x4  (EL1 << 2)
```

---

## 7. Stretch

- Add `printk` log levels: `KERN_INFO "..."` prefix (string literal `"\001""6"` Linux-style or simpler `[INFO] `).
- Implement `uart_getc()` with `FR.RXFE` polling for Day 26's shell.
- Hook a panic-time emergency `puts` that bypasses the formatter.

---

## 8. References

- ARM ARM §D13 (PSTATE/`SPSR`), §D7.2.95 (`HCR_EL2`).
- ARM PrimeCell UART (PL011) Technical Reference Manual, DDI 0183.
- Linux `arch/arm64/kernel/head.S` `el2_setup` macro (reference only).
