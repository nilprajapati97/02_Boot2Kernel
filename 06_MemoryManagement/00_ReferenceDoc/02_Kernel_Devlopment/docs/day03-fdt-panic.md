# Day 03 — Early Init, Panic, DTB Walk

> **Goal**: Parse the Device Tree Blob (DTB) handed to us by QEMU to discover RAM ranges and `chosen/bootargs`; implement `panic()` with a frame-pointer backtrace; add log levels to `printk`.
>
> **Why today**: Day 8's physical allocator needs the RAM range. Day 21 needs `chosen/bootargs` for kernel cmdline. `panic()` is needed before we add anything that can fault. Without backtrace, every later bug becomes an "infinite hang" mystery.

---

## 1. Background

### 1.1 Flattened Device Tree (FDT) format
```
struct fdt_header {
    u32 magic;             // 0xd00dfeed (big-endian on disk)
    u32 totalsize;
    u32 off_dt_struct;
    u32 off_dt_strings;
    u32 off_mem_rsvmap;
    u32 version;
    u32 last_comp_version;
    u32 boot_cpuid_phys;
    u32 size_dt_strings;
    u32 size_dt_struct;
};
```
All fields are **big-endian**. The `dt_struct` block is a token stream:
| Token | Value | Meaning |
|---|---|---|
| `FDT_BEGIN_NODE` | 1 | followed by null-term name |
| `FDT_END_NODE`   | 2 | |
| `FDT_PROP`       | 3 | `u32 len, u32 nameoff`, then `len` bytes |
| `FDT_NOP`        | 4 | skip |
| `FDT_END`        | 9 | end of struct |

### 1.2 Nodes we care about today
```dts
/memory@40000000 {
    device_type = "memory";
    reg = <0x0 0x40000000 0x0 0x20000000>;  // base=0x40000000, size=0x20000000 (512MiB)
};
/chosen {
    bootargs = "console=ttyAMA0";
    linux,initrd-start = <...>;
    linux,initrd-end   = <...>;
};
```

### 1.3 Frame pointer backtrace (AArch64 AAPCS64)
- `x29` = FP; on function entry: `stp x29, x30, [sp, #-16]!; mov x29, sp`.
- Frame chain: `*FP = saved FP`, `*(FP+8) = saved LR`.
- Walk until FP == 0 or unaligned.

---

## 2. Design

### 2.1 New files
```
kernel/early/fdt.c          (parser + accessors)
include/kernel/fdt.h
kernel/panic.c
kernel/printk.c             (add levels)
include/kernel/printk.h     (KERN_* macros)
```

### 2.2 Public API
```c
/* fdt.h */
int  fdt_init(u64 dtb_phys);                 // validate magic, totalsize
int  fdt_get_memory(u64 *base, u64 *size);   // first /memory node
const char *fdt_bootargs(void);              // /chosen/bootargs or NULL
int  fdt_get_initrd(u64 *start, u64 *end);   // /chosen/linux,initrd-*

/* panic.h */
void panic(const char *fmt, ...) __attribute__((noreturn));
void dump_backtrace(void);
#define BUG_ON(c) do { if (c) panic("BUG: %s:%d %s", __FILE__, __LINE__, #c); } while (0)
```

---

## 3. Implementation

### 3.1 `kernel/early/fdt.c`
```c
#include <kernel/fdt.h>
#include <kernel/printk.h>
#include <kernel/types.h>

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

static const u8 *fdt_base;
static u32 fdt_off_struct, fdt_off_strings;

static u32 be32(const void *p)
{
    const u8 *b = p;
    return ((u32)b[0]<<24)|((u32)b[1]<<16)|((u32)b[2]<<8)|b[3];
}
static u64 be64(const void *p)
{
    return ((u64)be32(p) << 32) | be32((const u8 *)p + 4);
}

int fdt_init(u64 dtb_phys)
{
    fdt_base = (const u8 *)dtb_phys;
    if (be32(fdt_base) != FDT_MAGIC) {
        printk("[ERR] FDT: bad magic %x\n", be32(fdt_base));
        return -1;
    }
    fdt_off_struct  = be32(fdt_base + 8);
    fdt_off_strings = be32(fdt_base + 12);
    printk("[INFO] FDT: %u bytes, struct@%u strings@%u\n",
           be32(fdt_base + 4), fdt_off_struct, fdt_off_strings);
    return 0;
}

/* Returns pointer to next token (4-byte aligned), 0 if FDT_END */
static const u8 *walk(const u8 *p, const char **name_out,
                      const u8 **prop_data, u32 *prop_len,
                      const char **prop_name)
{
    u32 tok = be32(p); p += 4;
    if (tok == FDT_BEGIN_NODE) {
        *name_out = (const char *)p;
        while (*p++) ;
        p = (const u8 *)(((uintptr_t)p + 3) & ~3UL);
    } else if (tok == FDT_PROP) {
        *prop_len  = be32(p);
        u32 noff   = be32(p + 4);
        *prop_name = (const char *)(fdt_base + fdt_off_strings + noff);
        *prop_data = p + 8;
        p += 8 + *prop_len;
        p = (const u8 *)(((uintptr_t)p + 3) & ~3UL);
    } else if (tok == FDT_END) {
        return NULL;
    }
    return p;
}

/* Simplified: find first prop matching name within a node prefix */
static const u8 *find_prop(const char *node_prefix, const char *prop,
                           u32 *len_out)
{
    const u8 *p = fdt_base + fdt_off_struct;
    int in_node = 0; const char *node_name = NULL;
    while (p) {
        u32 tok = be32(p);
        if (tok == FDT_BEGIN_NODE) {
            node_name = (const char *)(p + 4);
            in_node = (strprefix(node_name, node_prefix));
            p += 4;
            while (*p++) ;
            p = (const u8 *)(((uintptr_t)p + 3) & ~3UL);
        } else if (tok == FDT_PROP) {
            u32 len  = be32(p + 4);
            u32 noff = be32(p + 8);
            const char *pname = (const char *)(fdt_base + fdt_off_strings + noff);
            if (in_node && streq(pname, prop)) {
                *len_out = len;
                return p + 12;
            }
            p += 12 + ((len + 3) & ~3U);
        } else if (tok == FDT_END_NODE) { in_node = 0; p += 4; }
          else if (tok == FDT_NOP)      { p += 4; }
          else if (tok == FDT_END)      { return NULL; }
          else                          { return NULL; }
    }
    return NULL;
}

int fdt_get_memory(u64 *base, u64 *size)
{
    u32 len;
    const u8 *reg = find_prop("memory", "reg", &len);
    if (!reg || len < 16) return -1;
    *base = be64(reg);
    *size = be64(reg + 8);
    return 0;
}

const char *fdt_bootargs(void)
{
    u32 len;
    return (const char *)find_prop("chosen", "bootargs", &len);
}
```

> `strprefix`, `streq` belong in `lib/string.c` (add today).

### 3.2 `kernel/panic.c`
```c
#include <kernel/printk.h>
#include <kernel/types.h>

extern void uart_puts(const char *);

void dump_backtrace(void)
{
    u64 fp;
    asm volatile("mov %0, x29" : "=r"(fp));
    printk("Call trace:\n");
    for (int i = 0; i < 32 && fp && !(fp & 0x7); i++) {
        u64 saved_fp = *(u64 *)(fp);
        u64 saved_lr = *(u64 *)(fp + 8);
        printk("  [%d] %p\n", i, (void *)saved_lr);
        if (saved_fp <= fp) break;          /* corruption / end */
        fp = saved_fp;
    }
}

__attribute__((noreturn))
void panic(const char *fmt, ...)
{
    asm volatile("msr daifset, #0xf" ::: "memory");   /* mask all */
    uart_puts("\n*** KERNEL PANIC ***\n");
    /* Simplified: no varargs reformat here — use printk before calling panic
       for full args; this guarantees we still emit something even if printk fails. */
    uart_puts(fmt);
    uart_puts("\n");
    dump_backtrace();
    for (;;) asm volatile("wfe");
}
```

### 3.3 Log levels in `printk.h`
```c
#define KERN_EMERG  "<0>"
#define KERN_ERR    "<3>"
#define KERN_WARN   "<4>"
#define KERN_INFO   "<6>"
#define KERN_DEBUG  "<7>"

/* In printk.c: strip leading <N> token, prefix [EMRG]/[ERR]/[WARN]/[INFO]/[DBG]. */
```

### 3.4 Update `kmain`
```c
void kmain(u64 dtb_phys)
{
    uart_init();
    printk(KERN_INFO "nkernel booting, DTB @ %p\n", (void *)dtb_phys);

    if (fdt_init(dtb_phys) < 0)
        panic("FDT init failed");

    u64 mem_base, mem_size;
    if (fdt_get_memory(&mem_base, &mem_size) == 0)
        printk(KERN_INFO "Memory: base=%p size=%lu MiB\n",
               (void *)mem_base, mem_size >> 20);

    const char *cmdline = fdt_bootargs();
    if (cmdline) printk(KERN_INFO "Cmdline: %s\n", cmdline);

    for (;;) asm volatile("wfe");
}
```

---

## 4. Pitfalls

1. **Endian mistakes**: every multi-byte FDT field is big-endian. Always use `be32/be64` helpers.
2. **Compiler reorder of MMIO/DTB reads**: DTB is normal memory; reordering OK but mark `fdt_base` `const` so it isn't cached in a stale register.
3. **Backtrace before FP set**: if you call `dump_backtrace` from `_start` before `mov x29, sp` paths, FP is garbage. Always call after entering at least one C frame.
4. **`__attribute__((no_stack_protector))`** may be needed on `panic()` once stack-protector is added (Day 29).
5. **`strprefix` matching `memory@40000000` to "memory"**: handle `@` as terminator.

---

## 5. Verification

```
make run
# Serial:
# [INFO] FDT: 0x... bytes, struct@... strings@...
# [INFO] Memory: base=0x40000000 size=512 MiB
# [INFO] Cmdline: console=ttyAMA0
```

Manual panic test: temporarily `panic("test")` in `kmain`; verify `Call trace:` lines printed and CPU halts.

---

## 6. Stretch

- Persist DTB pointer + parsed memory map into a `struct boot_info` for later subsystems.
- Walk `/memreserve/` entries (initrd, DTB itself) so Day 8 can mark them reserved.
- Implement `kallsyms_lookup(addr)` stub returning `"???+0xNN"` — actual symbol table on Day 29.

---

## 7. References

- *Devicetree Specification 0.4* (devicetree.org), §5 FDT format.
- ARM ARM §B2 (AAPCS64 frame layout).
