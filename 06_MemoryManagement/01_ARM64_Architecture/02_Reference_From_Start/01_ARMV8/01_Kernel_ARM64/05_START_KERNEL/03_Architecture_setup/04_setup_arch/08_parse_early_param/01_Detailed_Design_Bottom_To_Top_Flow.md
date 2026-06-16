# parse_early_param() — Detailed Design: Bottom-to-Top Flow

## 1. Position in setup_arch() Boot Sequence

```
setup_arch()
  ├── early_fixmap_init()          ← fixmap hardware setup
  ├── early_ioremap_init()         ← early I/O mapping
  └── parse_early_param()          ← *** THIS FUNCTION *** (line 1144)
        └── do_early_param()
              └── obs_kernel_param[] table entries
```

`parse_early_param()` is the **first kernel function that reads kernel command-line arguments** after the low-level boot infrastructure is ready. It runs before MMU full page tables are set up, before memblock is finalized, and before any driver subsystem is initialized.

---

## 2. Why This Position?

The command line is available very early — passed by the bootloader in ATAGs or FDT. Many subsequent setup functions (`early_mm_init`, `adjust_lowmem_bounds`, memblock) depend on boot parameters like:
- `mem=` — override memory size
- `vmalloc=` — change vmalloc window size
- `earlyprintk=` / `earlycon=` — debug output
- `nokaslr`, `kasan_` — security config

If `parse_early_param()` ran later, those parameters would already be missed by dependent subsystems.

---

## 3. Source Code Walkthrough

### 3.1 Entry Point: `parse_early_param()`

**File:** `init/main.c`

```c
/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
    static int done __initdata;
    static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

    if (done)
        return;

    /* All fall through to do_early_param. */
    strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
    parse_early_options(tmp_cmdline);
    done = 1;
}
```

**Key points:**
- **`done` guard**: Prevents double execution (arch may call it early, then generic code calls again).
- **`tmp_cmdline`**: Makes a local copy so that the original `boot_command_line[]` is never modified. This is critical — `boot_command_line` must survive intact for later `/proc/cmdline` and user-space visibility.
- Both `tmp_cmdline` and `done` are `__initdata` — freed after boot.

### 3.2 `parse_early_options()`

**File:** `init/main.c`

```c
void __init parse_early_options(char *cmdline)
{
    parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
               do_early_param);
}
```

Calls the generic `parse_args()` with a custom handler `do_early_param`. The `NULL` for `struct kernel_param *params` and `0` for `num` means no static param table is used — instead the handler is invoked for every `key=value` token.

### 3.3 `parse_args()` — The Command-Line Tokenizer

**File:** `kernel/params.c`

```c
char *parse_args(const char *doing,
                 char *args,
                 const struct kernel_param *params,
                 unsigned num,
                 s16 min_level, s16 max_level,
                 void *arg,
                 int (*unknown)(char *param, char *val,
                                const char *doing, void *arg))
```

**Tokenization algorithm:**
1. Walk `args` character by character.
2. Skip whitespace (space, tab, newline).
3. Handle quoted strings (`"..."`) so spaces inside quotes are preserved.
4. Find `=` to split key and value.
5. Call `unknown` handler (`do_early_param`) for each token.
6. Returns pointer past the last parsed character, or `NULL`.

### 3.4 `do_early_param()` — The Callback

**File:** `init/main.c`

```c
static int __init do_early_param(char *param, char *val,
                                  const char *unused, void *arg)
{
    const struct obs_kernel_param *p;

    for (p = __setup_start; p < __setup_end; p++) {
        if ((p->early && parameq(param, p->str)) ||
            (strcmp(param, "console") == 0 &&
             strcmp(p->str, "earlycon") == 0)) {
            if (p->setup_func(val) != 0)
                pr_warn("Malformed early option '%s'\n", param);
        }
    }
    /* We accept everything at this stage. */
    return 0;
}
```

**Key design elements:**
- Iterates over `__setup_start` to `__setup_end` — the `obs_kernel_param` table built at link time.
- Only calls handlers where `p->early == 1`.
- Special case: `console=` is aliased to `earlycon` for early console setup.
- Returns `0` always — unknown parameters are silently accepted (not errors).

### 3.5 The `__setup_start` / `__setup_end` Table

**Linker script section:** `include/asm-generic/vmlinux.lds.h`

```ld
__setup_start = .;
KEEP(*(.init.setup))
__setup_end = .;
```

**How entries get into `.init.setup`:** Macros in source files:

```c
/* registers an "early" handler */
#define early_param(str, fn)                          \
    __setup_param(str, fn, fn, 1)                     /* early=1 */

/* registers a regular (non-early) handler */
#define __setup(str, fn)                              \
    __setup_param(str, fn, fn, 0)                     /* early=0 */

#define __setup_param(str, unique_id, fn, early)      \
    static const char __setup_str_##unique_id[] __initconst \
        __aligned(1) = str;                           \
    static struct obs_kernel_param __setup_##unique_id \
        __used __section(".init.setup")               \
        __attribute__((aligned((sizeof(long)))))      \
        = { __setup_str_##unique_id, fn, early }
```

**`obs_kernel_param` structure:**

```c
struct obs_kernel_param {
    const char *str;           /* parameter name string */
    int (*setup_func)(char *); /* handler function */
    int early;                 /* 1 = process in parse_early_param */
};
```

### 3.6 Examples of Early Parameters

| Source file | Parameter | Handler | Purpose |
|------------|-----------|---------|---------|
| `arch/arm/mm/mmu.c` | `vmalloc=` | `early_vmalloc` | Set vmalloc window size |
| `arch/arm/kernel/setup.c` | `mem=` | `early_mem` | Override memory size |
| `kernel/printk/printk.c` | `loglevel=` | `loglevel` | Set console log level |
| `drivers/tty/serial/earlycon.c` | `earlycon=` | `param_setup_earlycon` | Set up early UART console |
| `kernel/params.c` | `nomodule` | — | Disable module loading |

---

## 4. Data Flow and Memory State

```
boot_command_line[]  (read-only after boot, set by head.S / FDT parsing)
        │
        ▼  (strlcpy — defensive copy)
tmp_cmdline[]        (local __initdata copy, modified in-place by tokenizer)
        │
        ▼  parse_args()
   token: "vmalloc=240M"
        │
        ▼  do_early_param()
   scan obs_kernel_param table in .init.setup section
        │  p->str == "vmalloc", p->early == 1
        ▼
   early_vmalloc("240M")
        │
        └─► vmalloc_size = 240 * 1024 * 1024
```

---

## 5. Call Tree (Bottom-Up, Most-Specific to Entry)

```
early_vmalloc()                  ← example handler (arch/arm/mm/mmu.c)
  called by do_early_param()
    called by parse_args()       ← kernel/params.c
      called by parse_early_options()  ← init/main.c
        called by parse_early_param()  ← init/main.c
          called by setup_arch()       ← arch/arm/kernel/setup.c
```

---

## 6. What Happens in Hardware During parse_early_param()

At this point in the boot sequence:
- **MMU is ON** (enabled by head.S before reaching C code)
- **Early fixmap is active** (set up by `early_fixmap_init()`)
- **Early ioremap is active** (set up by `early_ioremap_init()`)
- **Caches are enabled** by CP15 setup in `setup_processor()`
- **boot_command_line[]** is mapped in the kernel's initial linear mapping

No hardware state changes during `parse_early_param()` itself. It is purely a software operation — reading a string, comparing character arrays, and calling function pointers stored in read-only `.init.setup` section.

The **side effects are indirect**: handlers like `early_vmalloc()` modify global variables (`vmalloc_size`) which are later read by `adjust_lowmem_bounds()` to compute page table layout.

---

## 7. Security Considerations

- **The copy**: `tmp_cmdline` prevents command line modification from propagating back to `boot_command_line`. If a handler corrupts the string during tokenization, the original is safe.
- **`done` guard**: Prevents re-running — a second call would risk re-applying parameters that already modified global state.
- **No dynamic allocation**: Entire function runs with only stack + static `__initdata`. Safe before memblock is finalized.
- **Unknown parameters silently accepted**: By design — real parameter validation happens in `parse_args()` with the static param table during `start_kernel()` after boot.

---

## 8. Relationship to Late Parameter Parsing

There are **two rounds** of command-line parsing in the kernel:

| Round | Function | When | Handlers |
|-------|----------|------|---------|
| 1 (early) | `parse_early_param()` | Before MMU full setup | `early_param()` — `early=1` |
| 2 (late) | `parse_args()` in `start_kernel()` | After SMP + modules | `__setup()` — `early=0`; `module_param()` |

Early parameters have **no return value checking** beyond a warning. Late parameters can return error codes that cause kernel messages.

---

## 9. Interview Q&A

**Q1: Why does `parse_early_param()` copy the command line before parsing?**
> `parse_args()` modifies the string in-place during tokenization (it inserts null terminators at `=` and at parameter boundaries). If it operated on `boot_command_line` directly, `boot_command_line` would become garbled and `/proc/cmdline` would show corrupted output. The copy is an intentional defensive design.

**Q2: What is the `done` static variable protecting against?**
> ARM (and other architectures) may call `parse_early_param()` very early in `setup_arch()` before the generic `start_kernel()` would call it. Without the guard, parameters could be processed twice — handlers that are not idempotent (e.g., those that call `memblock_remove()`) would break the memory map.

**Q3: How does `early_param("vmalloc", early_vmalloc)` get linked into the kernel?**
> The macro `early_param` expands to `__setup_param` which declares a `struct obs_kernel_param` with `__section(".init.setup")`. The linker script collects all such objects between `__setup_start` and `__setup_end`. `do_early_param()` iterates this range at runtime.

**Q4: Can early handlers allocate memory?**
> No. At this point `memblock_alloc()` is technically available but the memblock reserved regions haven't been fully configured yet (that happens in `arm_memblock_init()` later). Early handlers should only modify global variables, not allocate memory.

**Q5: What is the difference between `early_param()` and `__setup()`?**
> `early_param(str, fn)` sets `early=1` → processed by `parse_early_param()` before page tables. `__setup(str, fn)` sets `early=0` → processed by the late `parse_args()` call in `start_kernel()` after most subsystems are up. Use `early_param` when the handler sets values used by boot code; use `__setup` for values only needed at or after driver init.

**Q6: What happens if two different `early_param()` declarations use the same string?**
> Both handlers will be called. The `do_early_param()` loop does not stop at the first match. This is intentional to allow stacking of handlers for complex parameters like `mem=`.

**Q7: What does `parameq()` do?**
> It compares parameter names case-insensitively and treats `-` and `_` as equivalent (e.g., `no-kaslr` matches `no_kaslr`). This is a kernel convention to make command lines more user-friendly.

**Q8: Why is `tmp_cmdline` marked `__initdata`?**
> Both `tmp_cmdline` and `done` are only needed during `__init` code. Marking them `__initdata` places them in the `.init.data` section which is freed by `free_initmem()` late in the boot sequence, recovering that physical RAM.
