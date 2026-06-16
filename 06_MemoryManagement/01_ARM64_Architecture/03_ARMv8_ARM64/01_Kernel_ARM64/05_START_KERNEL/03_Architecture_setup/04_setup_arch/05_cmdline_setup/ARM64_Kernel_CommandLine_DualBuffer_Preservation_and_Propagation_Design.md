# ARM64 Kernel Command Line: Dual-Buffer Preservation and Propagation Design

**Document ID   :** ARM64-KDD-SETUP-ARCH-004  
**Subsystem     :** Architecture Setup — `setup_arch()`  
**Source File   :** `arch/arm/kernel/setup.c` (ARM32) / `arch/arm64/kernel/setup.c` (ARM64)  
**Kernel Version:** Linux 6.x mainline  
**Author        :** Kernel Exploration Series  
**Date          :** 2026-05-08  
**Status        :** Final  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)  
2. [Code Under Analysis](#2-code-under-analysis)  
3. [Motivation — Why Two Buffers?](#3-motivation--why-two-buffers)  
4. [Variable Definitions and Memory Layout](#4-variable-definitions-and-memory-layout)  
   - 4.1 [COMMAND_LINE_SIZE](#41-command_line_size)  
   - 4.2 [boot_command_line](#42-boot_command_line)  
   - 4.3 [cmd_line (ARM32) / direct boot_command_line (ARM64)](#43-cmd_line-arm32--direct-boot_command_line-arm64)  
   - 4.4 [cmdline_p](#44-cmdline_p)  
5. [How boot_command_line Gets Its Value](#5-how-boot_command_line-gets-its-value)  
   - 5.1 [ARM64 Boot Protocol (UEFI / FDT path)](#51-arm64-boot-protocol-uefi--fdt-path)  
   - 5.2 [FDT /chosen node parsing](#52-fdt-chosen-node-parsing)  
   - 5.3 [CONFIG_CMDLINE fallback](#53-config_cmdline-fallback)  
6. [Deep Dive: strscpy()](#6-deep-dive-strscpy)  
   - 6.1 [API Signature](#61-api-signature)  
   - 6.2 [Internal Algorithm](#62-internal-algorithm)  
   - 6.3 [Why NOT strcpy / strncpy / strlcpy?](#63-why-not-strcpy--strncpy--strlcpy)  
   - 6.4 [Return Value Semantics](#64-return-value-semantics)  
7. [Line-by-Line Dissection of the Three-Line Snippet](#7-line-by-line-dissection-of-the-three-line-snippet)  
8. [Concrete End-to-End Example on ARM64 (Raspberry Pi 4 / Cortex-A72)](#8-concrete-end-to-end-example-on-arm64-raspberry-pi-4--cortex-a72)  
   - 8.1 [Bootloader Stage](#81-bootloader-stage)  
   - 8.2 [FDT Parsing Stage](#82-fdt-parsing-stage)  
   - 8.3 [setup_arch() Stage](#83-setup_arch-stage)  
   - 8.4 [start_kernel() Continuation](#84-start_kernel-continuation)  
   - 8.5 [Memory State Walkthrough](#85-memory-state-walkthrough)  
9. [ARM32 vs ARM64 Comparison](#9-arm32-vs-arm64-comparison)  
10. [Call Chain and Data Flow Diagram](#10-call-chain-and-data-flow-diagram)  
11. [Downstream Consumers of cmdline_p](#11-downstream-consumers-of-cmdline_p)  
    - 11.1 [setup_command_line()](#111-setup_command_line)  
    - 11.2 [parse_early_param()](#112-parse_early_param)  
    - 11.3 [parse_args() — Booting kernel](#113-parse_args--booting-kernel)  
    - 11.4 [/proc/cmdline](#114-proccmdline)  
12. [Security Analysis](#12-security-analysis)  
13. [Edge Cases and Boundary Conditions](#13-edge-cases-and-boundary-conditions)  
14. [Memory Map of the .init.data Section](#14-memory-map-of-the-initdata-section)  
15. [Summary Reference Table](#15-summary-reference-table)  
16. [References](#16-references)  

---

## 1. Executive Summary

During the very early Linux kernel boot on ARM32/ARM64, the kernel must handle an immutable **original command line** (received verbatim from the bootloader) alongside a **mutable working copy** (which will be parsed, tokenised, and modified in-place by various kernel subsystems).

The three-line snippet:

```c
/* populate cmd_line too for later use, preserving boot_command_line */
strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
*cmdline_p = cmd_line;
```

implements this **dual-buffer** design:

| Buffer | Purpose | Modified? |
|---|---|---|
| `boot_command_line[]` | Permanent read-only record of what the bootloader passed | **Never** |
| `cmd_line[]` (ARM32) | Working copy handed to the rest of the kernel | **Yes** (parsed in-place) |
| `saved_command_line` | Heap copy preserved for `/proc/cmdline` | **No** (allocated later) |

On **ARM64**, the design is slightly different: `setup_arch()` sets `*cmdline_p = boot_command_line` directly, and ARM64 does not declare a local `cmd_line[]` — but the principle is identical. This document analyses both variants.

---

## 2. Code Under Analysis

**File:** `arch/arm/kernel/setup.c`  
**Function:** `setup_arch(char **cmdline_p)`  
**Lines:** ~1137–1139

```c
/* populate cmd_line too for later use, preserving boot_command_line */
strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
*cmdline_p = cmd_line;
```

**Surrounding context** (abbreviated):

```c
void __init setup_arch(char **cmdline_p)
{
    ...
    setup_initial_init_mm(_text, _etext, _edata, _end);

    /* ─── FOCUS AREA ─────────────────────────────────────────────── */
    /* populate cmd_line too for later use, preserving boot_command_line */
    strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
    *cmdline_p = cmd_line;
    /* ──────────────────────────────────────────────────────────────── */

    early_fixmap_init();
    early_ioremap_init();
    parse_early_param();
    ...
}
```

---

## 3. Motivation — Why Two Buffers?

### The Problem

`parse_args()` — which parses kernel parameters like `console=ttyS0`, `root=/dev/sda1`, etc. — works **in-place**. It walks the string and replaces delimiter characters with `\0` to produce an array of individual parameter strings. After parsing, the buffer no longer resembles the original command line.

If there were only one buffer:

```
Before:  "console=ttyAMA0 root=/dev/mmcblk0p2 rw"
After:   "console=ttyAMA0\0root=/dev/mmcblk0p2\0rw\0"
                          ^^                  ^^
                     overwritten by NUL      overwritten
```

Subsequent code reading the "original" would see a truncated string.

### The Solution: Two Buffers

```
boot_command_line[]  ───────────────────────────────►  NEVER TOUCHED after initial fill
                                                        Used by: crashkernel parsing,
                                                                 saved_command_line,
                                                                 /proc/cmdline

cmd_line[]  (copy)  ────────────────────────────────►  HANDED to parse_args / parse_early_param
                                                        MAY BE DESTROYED by in-place parsing
```

### Why `strscpy` specifically?

Because `cmd_line` is declared `__initdata` (lives in `.init.data` section, will be freed after boot), and both source and destination are kernel-controlled fixed-size buffers. `strscpy` is the modern Linux API for bounded safe string copy.

---

## 4. Variable Definitions and Memory Layout

### 4.1 `COMMAND_LINE_SIZE`

| Architecture | Definition Location | Value |
|---|---|---|
| ARM32 | `arch/arm/include/uapi/asm/setup.h` | `1024` bytes |
| ARM64 | `arch/arm64/include/uapi/asm/setup.h` | `2048` bytes |
| Generic fallback | `include/uapi/asm-generic/setup.h` | `512` bytes |

ARM64 doubles the buffer because:
- UEFI command lines can be substantially longer than ATAGs.
- `bootconfig` may append extra parameters.
- Modern embedded and server systems use more parameters.

```c
/* arch/arm64/include/uapi/asm/setup.h */
#define COMMAND_LINE_SIZE   2048
```

### 4.2 `boot_command_line`

**Definition** (`init/main.c`, line 138):

```c
/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
```

| Attribute | Detail |
|---|---|
| Section | `.init.data` (freed after `free_initmem()`) |
| Scope | Global — visible to all translation units via `extern` in `<linux/init.h>` |
| Written by | `early_init_dt_scan_chosen()` via FDT `/chosen` `bootargs` property |
| Read by | `setup_arch()`, `reserve_crashkernel()`, `setup_command_line()`, `parse_early_param()` |
| Modified after fill? | **Never** (treated as read-only after initial population) |

**Declaration visible globally** (`include/linux/init.h`, line 143):

```c
extern char __initdata boot_command_line[];
```

### 4.3 `cmd_line` (ARM32) / direct `boot_command_line` (ARM64)

**ARM32 definition** (`arch/arm/kernel/setup.c`):

```c
static char __initdata cmd_line[COMMAND_LINE_SIZE];
```

| Attribute | Detail |
|---|---|
| Linkage | `static` — local to `setup.c` |
| Section | `.init.data` |
| Purpose | Mutable working copy of the command line |
| Lifetime | Exists until `free_initmem()` is called |

**ARM64 approach** (`arch/arm64/kernel/setup.c`, line 297):

```c
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
    setup_initial_init_mm(_stext, _etext, _edata, _end);

    *cmdline_p = boot_command_line;    /* ← ARM64 directly points at boot_command_line */
    ...
}
```

ARM64 does **not** make a private copy here. Instead, it points `cmdline_p` directly at `boot_command_line`. The copy-before-parse is then performed inside `parse_early_param()` via a local `tmp_cmdline` buffer (see Section 11.2).

### 4.4 `cmdline_p`

**Type:** `char **` — a pointer-to-pointer-to-char  
**Declared in** `start_kernel()` (`init/main.c`):

```c
void start_kernel(void)
{
    char *command_line;      /* ← this is the object cmdline_p points at */
    ...
    setup_arch(&command_line);   /* ← &command_line == cmdline_p */
    setup_command_line(command_line);
    ...
}
```

The double-pointer indirection allows `setup_arch()` — which is called with the *address* of `command_line` — to set `command_line` to wherever the architecture wants it to point. After return, `command_line` in `start_kernel()` holds the address of the (copy of the) command line.

```
start_kernel stack frame:
┌────────────────────────────┐
│  char *command_line        │  ◄── cmdline_p == &command_line
│  (initially undefined)     │
└────────────────────────────┘

After setup_arch() returns (ARM32):
┌────────────────────────────┐
│  char *command_line  ──────┼──► cmd_line[]  (in .init.data)
└────────────────────────────┘       │
                                     │ contains copy of boot_command_line
```

---

## 5. How `boot_command_line` Gets Its Value

### 5.1 ARM64 Boot Protocol (UEFI / FDT path)

```
Bootloader (U-Boot / UEFI)
    │
    │  Passes FDT blob address in x0 (per ARM64 boot protocol)
    │  FDT contains:  /chosen { bootargs = "..."; }
    ▼
primary_entry   (arch/arm64/kernel/head.S)
    │
    ▼
start_kernel   (init/main.c)
    │
    ▼
setup_arch(&command_line)
    │
    ├── setup_machine_fdt(__fdt_pointer)
    │       └── early_init_dt_scan(dt_virt)
    │               └── early_init_dt_scan_nodes()
    │                       └── early_init_dt_scan_chosen(boot_command_line)
    │                               └── strscpy(boot_command_line,
    │                                           fdt_prop_bootargs,
    │                                           COMMAND_LINE_SIZE)
    │                                            ▲
    │                              boot_command_line[] is NOW POPULATED
    │
    └── *cmdline_p = boot_command_line
```

### 5.2 FDT /chosen node parsing

**Source:** `drivers/of/fdt.c`, function `early_init_dt_scan_chosen()`:

```c
int __init early_init_dt_scan_chosen(char *cmdline)
{
    int l, node;
    const char *p;
    const void *fdt = initial_boot_params;

    node = fdt_path_offset(fdt, "/chosen");
    ...

    /* Retrieve command line from /chosen/bootargs */
    p = of_get_flat_dt_prop(node, "bootargs", &l);
    if (p != NULL && l > 0)
        strscpy(cmdline, p, min(l, COMMAND_LINE_SIZE));
    ...
}
```

The function receives `boot_command_line` as its `cmdline` argument and copies the FDT `bootargs` string into it. After this call, `boot_command_line` is populated with the verbatim bootloader string.

### 5.3 CONFIG_CMDLINE fallback

If no bootargs are present in the FDT `/chosen` node, the kernel falls back to a compile-time default:

```c
#ifdef CONFIG_CMDLINE
  #if defined(CONFIG_CMDLINE_EXTEND)
    strlcat(cmdline, " ", COMMAND_LINE_SIZE);
    strlcat(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
  #elif defined(CONFIG_CMDLINE_FORCE)
    strscpy(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
  #else
    /* use kernel's default only if bootloader gave nothing */
    if (!((char *)cmdline)[0])
        strscpy(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
  #endif
#endif
```

| Config | Behavior |
|---|---|
| `CONFIG_CMDLINE_EXTEND` | Append compile-time string to bootloader string |
| `CONFIG_CMDLINE_FORCE` | Completely replace bootloader string with compile-time string |
| Neither | Use compile-time string only if bootloader gave nothing |

---

## 6. Deep Dive: `strscpy()`

### 6.1 API Signature

```c
/* lib/string.c, line 122 */
ssize_t strscpy(char *dest, const char *src, size_t count);
```

| Parameter | Description |
|---|---|
| `dest` | Destination buffer |
| `src` | Source string (NUL-terminated) |
| `count` | Maximum number of bytes to write (including terminating NUL) |
| **return** | Number of bytes copied (excluding NUL), or `-E2BIG` if src was truncated |

### 6.2 Internal Algorithm

The actual implementation in `lib/string.c` uses a **word-at-a-time** optimisation on aligned memory for throughput, falling back to byte-by-byte copy for unaligned data or small sizes. Simplified logic:

```
strscpy(dest, src, count):
    if count == 0 → return -E2BIG

    Phase 1 — word-at-a-time (if aligned):
        while remaining ≥ sizeof(unsigned long):
            read word from src
            if word contains NUL byte:
                write partial word to dest (mask bytes after NUL)
                return position of NUL
            write full word to dest
            advance pointers

    Phase 2 — byte-at-a-time:
        while count > 0:
            c = *src
            *dest = c
            if c == '\0' → return bytes_written
            advance, decrement count

    /* Reached count without NUL — force-terminate */
    dest[count-1] = '\0'
    return -E2BIG
```

**Key guarantee:** `dest` is **always NUL-terminated**, even if `src` is longer than `count`.

### 6.3 Why NOT `strcpy` / `strncpy` / `strlcpy`?

| Function | Problem |
|---|---|
| `strcpy(dest, src)` | No length limit — buffer overflow if `src > dest` size |
| `strncpy(dest, src, n)` | Does NOT guarantee NUL-termination if `src` fills the buffer; pads with extra NULs wastefully |
| `strlcpy(dest, src, n)` | Traverses the entire `src` to return `strlen(src)` even when truncating — unnecessary scan |
| `strscpy(dest, src, n)` | Always NUL-terminates; stops scanning at `n` bytes; returns useful length or `-E2BIG` |

Linux kernel policy (enforced by `checkpatch.pl`) prefers `strscpy` over the others for fixed-size kernel buffers since Linux 4.3.

### 6.4 Return Value Semantics

```c
ssize_t ret = strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
```

| Scenario | Return value |
|---|---|
| `boot_command_line` fits in `COMMAND_LINE_SIZE` | Positive integer = number of characters copied |
| `boot_command_line` is longer than `COMMAND_LINE_SIZE-1` | `-E2BIG` (and `cmd_line` is truncated + NUL-terminated) |
| `count == 0` | `-E2BIG` immediately |

In the kernel code shown, the return value is **discarded** — the kernel does not panic if the command line was truncated, it simply uses whatever fit. This is acceptable because `COMMAND_LINE_SIZE` (2048 on ARM64) is large enough for all practical boot scenarios.

---

## 7. Line-by-Line Dissection of the Three-Line Snippet

```c
/* Line 1: Comment */
/* populate cmd_line too for later use, preserving boot_command_line */
```

This comment encapsulates the entire design intent:
- **"populate cmd_line"** — create a copy, not an alias.
- **"for later use"** — downstream kernel code needs a modifiable command line string.
- **"preserving boot_command_line"** — the original must remain intact.

---

```c
/* Line 2: The copy operation */
strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);
```

**What this does step by step:**

1. `boot_command_line` — source. Contains the verbatim string placed there by `early_init_dt_scan_chosen()`.
2. `cmd_line` — destination. A `static char __initdata` array of `COMMAND_LINE_SIZE` bytes declared in `setup.c`.
3. `COMMAND_LINE_SIZE` — the bound. Prevents `cmd_line` overflow regardless of `boot_command_line` length.
4. The call copies bytes from `boot_command_line` into `cmd_line` until either:
   - A NUL byte is found (normal path), or
   - `COMMAND_LINE_SIZE - 1` bytes are copied (truncation path, NUL is then written at position `COMMAND_LINE_SIZE - 1`).
5. After the call: `cmd_line` is an independent, NUL-terminated string identical to (or a truncation of) `boot_command_line`.

**Memory state after line 2:**

```
boot_command_line[]:  "console=ttyAMA0 root=/dev/mmcblk0p2 rw\0........"
                       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                       UNTOUCHED — exactly as written by FDT parser

cmd_line[]:           "console=ttyAMA0 root=/dev/mmcblk0p2 rw\0........"
                       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                       INDEPENDENT COPY — safe to destroy by parsing
```

---

```c
/* Line 3: The pointer assignment */
*cmdline_p = cmd_line;
```

**What this does step by step:**

1. `cmdline_p` is `char **` — it is the address of `command_line` local variable in `start_kernel()`.
2. Dereferencing `cmdline_p` gives `command_line` (the `char *` in `start_kernel()`'s stack frame).
3. Assigning `cmd_line` to it makes `command_line` point to the working-copy buffer.
4. When `setup_arch()` returns, `start_kernel()` has `command_line` pointing at `cmd_line[]`, ready to pass to `setup_command_line(command_line)` and `parse_args()`.

**Pointer topology after line 3:**

```
start_kernel() stack:
┌──────────────────────────┐
│  char *command_line      │────────────────────────┐
│  (was uninitialized)     │                        │
└──────────────────────────┘                        ▼
                                         .init.data section:
cmdline_p ──► &command_line             ┌────────────────────────────────────────┐
                                        │ cmd_line[2048]                         │
                                        │ "console=ttyAMA0 root=/dev/mmcblk0p2 rw\0" │
                                        └────────────────────────────────────────┘
```

---

## 8. Concrete End-to-End Example on ARM64 (Raspberry Pi 4 / Cortex-A72)

### 8.1 Bootloader Stage

U-Boot on Raspberry Pi 4 with the following environment variable set:

```sh
# U-Boot command prompt
setenv bootargs "console=ttyAMA0,115200 root=/dev/mmcblk0p2 rootfstype=ext4 \
                 elevator=deadline rootwait net.ifnames=0 quiet"
```

U-Boot places this string into the FDT `/chosen` node before loading the kernel:

```
FDT blob in RAM at 0x00000000_08000000:
  /chosen {
      bootargs = "console=ttyAMA0,115200 root=/dev/mmcblk0p2 rootfstype=ext4 elevator=deadline rootwait net.ifnames=0 quiet";
      linux,initrd-start = <0x12000000>;
      linux,initrd-end   = <0x12800000>;
  };
```

### 8.2 FDT Parsing Stage

`early_init_dt_scan_chosen(boot_command_line)` reads `bootargs` from the FDT:

```c
/* Inside early_init_dt_scan_chosen(), drivers/of/fdt.c */

p = of_get_flat_dt_prop(chosen_node, "bootargs", &l);
/* p → "console=ttyAMA0,115200 root=/dev/mmcblk0p2 ..."  */
/* l = 107 bytes (including NUL)                          */

strscpy(boot_command_line, p, min(107, 2048));
/* → boot_command_line[] is now:                          */
/* "console=ttyAMA0,115200 root=/dev/mmcblk0p2 rootfstype=ext4 elevator=deadline rootwait net.ifnames=0 quiet\0" */
```

### 8.3 `setup_arch()` Stage

```c
/* ARM32 path (arch/arm/kernel/setup.c) */

/* State before these lines:
 *   boot_command_line = "console=ttyAMA0,115200 root=... quiet\0"
 *   cmd_line          = "\0\0\0\0\0\0..."  (uninitialized / zeroed .init.data)
 *   *cmdline_p        = (garbage / uninitialized)
 */

strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);

/* State after strscpy:
 *   boot_command_line = "console=ttyAMA0,115200 root=... quiet\0"   ← unchanged
 *   cmd_line          = "console=ttyAMA0,115200 root=... quiet\0"   ← fresh copy
 *   *cmdline_p        = (still garbage)
 */

*cmdline_p = cmd_line;

/* State after assignment:
 *   boot_command_line = "console=ttyAMA0,115200 root=... quiet\0"   ← unchanged
 *   cmd_line          = "console=ttyAMA0,115200 root=... quiet\0"   ← working copy
 *   *cmdline_p        = cmd_line   ← command_line in start_kernel() now valid
 */
```

### 8.4 `start_kernel()` Continuation

Back in `start_kernel()` (`init/main.c`):

```c
void start_kernel(void)
{
    char *command_line;

    ...
    setup_arch(&command_line);
    /*
     * command_line → "console=ttyAMA0,115200 root=/dev/mmcblk0p2 ... quiet"
     */

    setup_boot_config();
    /*
     * Makes a tmp copy internally, parses bootconfig — boot_command_line untouched
     */

    setup_command_line(command_line);
    /*
     * Allocates saved_command_line (for /proc/cmdline):
     *   saved_command_line = heap copy of boot_command_line (always the original)
     *
     * Allocates static_command_line:
     *   static_command_line = heap copy of command_line (the working copy)
     *
     * After this: boot_command_line is copied to saved_command_line permanently
     */

    pr_notice("Kernel command line: %s\n", saved_command_line);
    /* prints: "console=ttyAMA0,115200 root=/dev/mmcblk0p2 ... quiet" */

    parse_early_param();
    /*
     * Internally:
     *   strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
     *   parse_early_options(tmp_cmdline);
     *   → consumes "console=ttyAMA0,115200" → earlycon setup
     *   tmp_cmdline is destroyed, boot_command_line is safe
     */

    parse_args("Booting kernel", static_command_line, ...);
    /*
     * static_command_line is parsed IN-PLACE:
     *   "console=ttyAMA0,115200\0root=/dev/mmcblk0p2\0rootfstype=ext4\0..."
     *   Each param handler is called: console_setup(), root_dev_setup(), etc.
     */
}
```

### 8.5 Memory State Walkthrough

The following shows how bytes in each buffer evolve across the boot sequence:

```
Step 1: After early_init_dt_scan_chosen()
─────────────────────────────────────────
boot_command_line[0..107]:
  c o n s o l e = t t y A M A 0 , 1 1 5 2 0 0   r o o t = ...
  0x63 0x6F 0x6E 0x73 0x6F 0x6C 0x65 0x3D ...                \0
boot_command_line[108..2047]:  all 0x00

cmd_line[0..2047]:  all 0x00 (untouched)


Step 2: After strscpy(cmd_line, boot_command_line, 2048)
─────────────────────────────────────────────────────────
boot_command_line[0..107]:  "console=ttyAMA0,115200 ... quiet\0"   (UNCHANGED)
cmd_line[0..107]:            "console=ttyAMA0,115200 ... quiet\0"   (NEW COPY)
cmd_line[108..2047]:         all 0x00


Step 3: After *cmdline_p = cmd_line
────────────────────────────────────
command_line (start_kernel local) → points to cmd_line[0]
boot_command_line: still "console=ttyAMA0,115200 ... quiet\0"


Step 4: After parse_early_param() (uses internal tmp_cmdline)
──────────────────────────────────────────────────────────────
boot_command_line: "console=ttyAMA0,115200 ... quiet\0"  (UNCHANGED)
cmd_line:          "console=ttyAMA0,115200 ... quiet\0"  (unchanged at this point)


Step 5: After parse_args() on static_command_line
───────────────────────────────────────────────────
boot_command_line: "console=ttyAMA0,115200 ... quiet\0"  (UNCHANGED — always)
static_command_line (heap):
  "console=ttyAMA0,115200\0root=/dev/mmcblk0p2\0rootfstype=ext4\0..."
   ^--- NUL-replaced by parse_args, but boot_command_line is safe
```

---

## 9. ARM32 vs ARM64 Comparison

| Aspect | ARM32 | ARM64 |
|---|---|---|
| `COMMAND_LINE_SIZE` | 1024 bytes | 2048 bytes |
| Local `cmd_line[]` buffer | Yes — `static char __initdata cmd_line[COMMAND_LINE_SIZE]` | **No** — not declared |
| `*cmdline_p =` | `cmd_line` (the copy) | `boot_command_line` (direct) |
| FDT pointer source | `__atags_pointer` (ATAG or FDT) | `__fdt_pointer` (always FDT) |
| `setup_machine_fdt()` location | `arch/arm/kernel/devtree.c` | `arch/arm64/kernel/setup.c` |
| `early_init_dt_scan_chosen()` | Same function — arch-independent | Same function — arch-independent |
| Copy-before-parse | In `setup_arch()` itself | Deferred to `parse_early_param()` |
| Boot protocol | ATAG or FDT, via r2 register | FDT only, via x0 register |
| Register at kernel entry | r0=0, r1=machine type, r2=ATAGs/FDT ptr | x0=FDT ptr, x1=x2=x3=0 |

**ARM64 `setup_arch()` key lines** (`arch/arm64/kernel/setup.c`):

```c
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
    setup_initial_init_mm(_stext, _etext, _edata, _end);

    *cmdline_p = boot_command_line;    /* ← direct, no local copy here */

    kaslr_init();
    early_fixmap_init();
    early_ioremap_init();
    setup_machine_fdt(__fdt_pointer);  /* ← fills boot_command_line */
    jump_label_init();
    parse_early_param();               /* ← uses internal tmp_cmdline */
    ...
}
```

Note that on ARM64, `*cmdline_p = boot_command_line` appears **before** `setup_machine_fdt()`. This means `command_line` in `start_kernel()` initially points to an empty buffer, which is then filled when `setup_machine_fdt()` runs. The pointer is already correctly set because both point to the same array.

---

## 10. Call Chain and Data Flow Diagram

```
ARM64 Boot Call Chain
══════════════════════════════════════════════════════════════════════

[ Bootloader (U-Boot / UEFI) ]
        │  x0 = FDT physical address
        │  x1 = x2 = x3 = 0
        ▼
[ primary_entry ]  arch/arm64/kernel/head.S
        │  Sets up stack, MMU for identity map
        ▼
[ start_kernel() ]  init/main.c
        │
        │  char *command_line;        ← will be set by setup_arch()
        │
        ├──► setup_arch(&command_line)
        │           │
        │           ├── setup_initial_init_mm()
        │           │
        │           ├── *cmdline_p = boot_command_line  ← ARM64 direct assignment
        │           │     (boot_command_line is still empty at this moment)
        │           │
        │           ├── setup_machine_fdt(__fdt_pointer)
        │           │       └── early_init_dt_scan(dt_virt)
        │           │               └── early_init_dt_scan_nodes()
        │           │                       └── early_init_dt_scan_chosen(boot_command_line)
        │           │                               └── strscpy(boot_command_line,
        │           │                                           fdt_bootargs,
        │           │                                           COMMAND_LINE_SIZE)
        │           │                                           ▲
        │           │                                   NOW POPULATED
        │           │                                   "console=... root=..."
        │           │
        │           └── parse_early_param()
        │                   │
        │                   └── strscpy(tmp_cmdline,        ← local stack/initdata copy
        │                               boot_command_line,
        │                               COMMAND_LINE_SIZE)
        │                   └── parse_early_options(tmp_cmdline) ← destroys tmp_cmdline
        │                       (boot_command_line SAFE)
        │
        │  command_line → boot_command_line → "console=... root=..."
        │
        ├──► setup_boot_config()
        │       └── strscpy(tmp_cmdline, boot_command_line, ...)
        │           (another safe internal copy, boot_command_line SAFE)
        │
        ├──► setup_command_line(command_line)
        │       ├── saved_command_line = memblock_alloc(...)
        │       │       └── strcpy(saved_command_line, boot_command_line)
        │       │           ▲ permanent heap copy for /proc/cmdline
        │       │
        │       └── static_command_line = memblock_alloc(...)
        │               └── strcpy(static_command_line, command_line)
        │                   ▲ will be given to parse_args() — will be destroyed
        │
        ├──► parse_early_param()        ← second call is a no-op (done flag set)
        │
        └──► parse_args("Booting kernel", static_command_line, ...)
                └── static_command_line is parsed IN-PLACE
                    boot_command_line ──────────────────► NEVER TOUCHED
                    saved_command_line ─────────────────► NEVER TOUCHED
```

---

## 11. Downstream Consumers of `cmdline_p`

### 11.1 `setup_command_line()`

**File:** `init/main.c`

```c
static void __init setup_command_line(char *command_line)
{
    ...
    saved_command_line = memblock_alloc(len + ilen, SMP_CACHE_BYTES);
    static_command_line = memblock_alloc(len, SMP_CACHE_BYTES);

    strcpy(saved_command_line + xlen, boot_command_line);  /* from boot_command_line */
    strcpy(static_command_line + xlen, command_line);       /* from working copy      */
    ...
    saved_command_line_len = strlen(saved_command_line);
}
```

- `saved_command_line` → memblock-allocated heap copy of `boot_command_line`. Never modified. Exposed via `/proc/cmdline`.
- `static_command_line` → memblock-allocated heap copy of the working copy. Passed to `parse_args()`.

### 11.2 `parse_early_param()`

**File:** `init/main.c`

```c
void __init parse_early_param(void)
{
    static int done __initdata;
    static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

    if (done)
        return;

    /* Safe copy — boot_command_line preserved */
    strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
    parse_early_options(tmp_cmdline);   /* modifies tmp_cmdline in-place */
    done = 1;
}
```

Note the exact same pattern — `strscpy` into a local buffer before parsing. This demonstrates the **invariant**: `boot_command_line` is **never** passed to any function that modifies strings in-place.

### 11.3 `parse_args()` — Booting kernel

```c
after_dashes = parse_args("Booting kernel",
                           static_command_line,
                           __start___param,
                           __stop___param - __start___param,
                           -1, -1, NULL, &unknown_bootoption);
```

`parse_args()` tokenises `static_command_line` in-place. For each `key=value` pair it finds the matching kernel parameter and calls its handler function. After this call, `static_command_line` is fragmented with NUL bytes — unusable as a string — but `boot_command_line` and `saved_command_line` remain intact.

### 11.4 `/proc/cmdline`

**File:** `fs/proc/cmdline.c`

```c
static int cmdline_proc_show(struct seq_file *m, void *v)
{
    seq_puts(m, saved_command_line);
    seq_putc(m, '\n');
    return 0;
}
```

The `/proc/cmdline` interface exposes `saved_command_line`, which was copied from `boot_command_line` by `setup_command_line()`. This is why the user sees the **original** bootloader command line, not the in-place-parsed version.

---

## 12. Security Analysis

### 12.1 Buffer Overflow Prevention

`strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE)` is safe because:
- `cmd_line` is exactly `COMMAND_LINE_SIZE` bytes.
- `strscpy` never writes more than `COMMAND_LINE_SIZE` bytes (including the NUL terminator).
- Even a maliciously crafted FDT with a bootargs longer than `COMMAND_LINE_SIZE` will be silently truncated with guaranteed NUL termination.

### 12.2 Injection via boot_command_line

The command line is trusted data from the bootloader. On a secured boot chain (Secure Boot / ARM TrustZone), the FDT is authenticated before the kernel processes it. If Secure Boot is not enabled, an attacker with physical access can modify the FDT — but this is outside the kernel's threat model.

### 12.3 KASLR and Command Line Privacy

On ARM64 with `CONFIG_RANDOMIZE_BASE` (KASLR), `kaslr_init()` is called after `*cmdline_p = boot_command_line`. KASLR reads the `nokaslr` parameter from the command line. Because `parse_early_param()` uses a copy, the KASLR logic never corrupts `boot_command_line`.

### 12.4 No Heap Allocation at This Point

The snippet uses only statically-allocated `.init.data` buffers. No heap/memblock allocation occurs here. This is critical because at this point in `setup_arch()`, the memory allocator may not yet be fully initialised (memblock is available, but slab/buddy allocators are not). Using statically allocated `__initdata` buffers is the safe and correct approach.

---

## 13. Edge Cases and Boundary Conditions

| Edge Case | Behaviour |
|---|---|
| `boot_command_line` is empty (`"\0"`) | `strscpy` copies one NUL byte; `cmd_line` is `""` |
| `boot_command_line` is exactly `COMMAND_LINE_SIZE - 1` chars + NUL | Fits exactly; return value = `COMMAND_LINE_SIZE - 1` |
| `boot_command_line` is `COMMAND_LINE_SIZE` chars with no NUL | Truncated to `COMMAND_LINE_SIZE - 1`; NUL appended; return `-E2BIG` |
| `COMMAND_LINE_SIZE` is 0 | `strscpy` returns `-E2BIG` immediately; `cmd_line` unchanged |
| `boot_command_line` contains embedded NUL bytes | Copy stops at first embedded NUL — rest is silently dropped |
| `CONFIG_CMDLINE_FORCE` set | `boot_command_line` was overwritten by `strscpy(boot_command_line, CONFIG_CMDLINE, ...)` in FDT parser; the copy here still works correctly |
| `cmdline_p` is NULL | Kernel would fault on `*cmdline_p = cmd_line` — this cannot happen as `start_kernel()` always passes `&command_line` |

---

## 14. Memory Map of the `.init.data` Section

Both `boot_command_line` and `cmd_line` (ARM32) are declared `__initdata`, meaning they reside in the `.init.data` ELF section, which is freed after all `__init` functions have run.

```
Kernel Image Layout (ARM64, simplified)
════════════════════════════════════════════════════════════
  Virtual Address                Content
  ───────────────────────────────────────────────────────
  KIMAGE_VADDR (e.g. 0xFFFF800008000000)
  ├── .text        — Kernel code (_stext .. _etext)
  ├── .rodata      — Read-only data
  ├── .data        — Initialized read/write data
  ├── .bss         — Zero-initialized data
  ├── .init.text   — __init functions (freed post-boot)
  └── .init.data   — __initdata variables (freed post-boot)
       ├── boot_command_line[2048]    ← 0x...  filled by FDT parser
       ├── __fdt_pointer              ← physical address of FDT
       ├── mmu_enabled_at_boot
       └── ... other __initdata ...
  ───────────────────────────────────────────────────────
  (ARM32 only, in arch/arm/kernel/setup.c .init.data):
       └── cmd_line[1024]             ← copy of boot_command_line
════════════════════════════════════════════════════════════

After free_initmem() (called from kernel_init_freeable()):
  .init.text and .init.data pages are returned to the buddy allocator.
  boot_command_line[] and cmd_line[] no longer exist.
  But saved_command_line (memblock-allocated heap) persists → /proc/cmdline still works.
```

---

## 15. Summary Reference Table

| Item | ARM32 Value / Location | ARM64 Value / Location |
|---|---|---|
| `COMMAND_LINE_SIZE` | `1024` (`arch/arm/include/uapi/asm/setup.h`) | `2048` (`arch/arm64/include/uapi/asm/setup.h`) |
| `boot_command_line` definition | `char __initdata boot_command_line[COMMAND_LINE_SIZE]` in `init/main.c:138` | Same |
| `boot_command_line` populated by | `early_init_dt_scan_chosen()` via ATAG or FDT | `early_init_dt_scan_chosen()` via FDT |
| Local `cmd_line[]` | `static char __initdata cmd_line[COMMAND_LINE_SIZE]` in `arch/arm/kernel/setup.c` | **Not used** |
| `*cmdline_p =` | `cmd_line` | `boot_command_line` |
| Copy function | `strscpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE)` | No copy in `setup_arch()`; copy deferred to `parse_early_param()` |
| `strscpy` defined in | `lib/string.c:122` | Same |
| `cmdline_p` type | `char **` — address of `command_line` in `start_kernel()` | Same |
| Who reads `*cmdline_p` after return | `setup_command_line()`, `parse_args()` in `start_kernel()` | Same |
| `saved_command_line` | heap copy of `boot_command_line` (via `memblock_alloc`) | Same |
| `/proc/cmdline` source | `saved_command_line` | Same |
| `boot_command_line` ever modified? | **Never** after `early_init_dt_scan_chosen()` | **Never** |

---

## 16. References

| Reference | Location |
|---|---|
| Code under analysis | `arch/arm/kernel/setup.c`, `setup_arch()`, lines ~1137–1139 |
| ARM64 equivalent | `arch/arm64/kernel/setup.c`, `setup_arch()`, line ~297 |
| `boot_command_line` definition | `init/main.c`, line 138 |
| `COMMAND_LINE_SIZE` (ARM64) | `arch/arm64/include/uapi/asm/setup.h`, line 25 |
| `COMMAND_LINE_SIZE` (generic) | `include/uapi/asm-generic/setup.h`, line 5 |
| `strscpy` implementation | `lib/string.c`, lines 122–188 |
| `early_init_dt_scan_chosen` | `drivers/of/fdt.c`, lines 1153–1218 |
| `early_init_dt_scan_nodes` | `drivers/of/fdt.c`, lines 1289–1305 |
| `start_kernel` | `init/main.c`, function `start_kernel()` |
| `setup_command_line` | `init/main.c`, function `setup_command_line()` |
| `parse_early_param` | `init/main.c`, lines 762–774 |
| `/proc/cmdline` | `fs/proc/cmdline.c` |
| ARM64 Boot Protocol | ARM document: *ARM64 Linux Boot Protocol* |
| Linux Kernel Docs: Kernel Parameters | `Documentation/admin-guide/kernel-parameters.rst` |

---

*End of Document*  
*Document ID: ARM64-KDD-SETUP-ARCH-004*
