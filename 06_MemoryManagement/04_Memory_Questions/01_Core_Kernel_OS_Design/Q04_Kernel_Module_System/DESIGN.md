# Q04 — Design a Kernel Module System with Safe Dynamic Loading/Unloading

---

## 1. Problem Statement

Linux's loadable kernel module (LKM) system allows drivers and subsystems to be inserted and removed from a running kernel without reboot. However, safe dynamic loading/unloading is non-trivial:
- A module may be in use by other code (caller has a function pointer into the module's text).
- A module may hold locks, have pending timers, or have registered callbacks that outlive the module's text.
- Module symbol resolution must handle inter-module dependencies.
- Loading must verify module integrity (signing, ABI compatibility).
- Concurrent load/unload races must be prevented without coarse global locks.

Design the kernel module subsystem from scratch, covering ELF loading, symbol resolution, reference counting, and safe unload synchronization.

---

## 2. Requirements

### 2.1 Functional Requirements
- Load a `.ko` ELF relocatable object into kernel address space.
- Resolve undefined symbols against the kernel's exported symbol table (KSYMTAB) and other loaded modules.
- Run `module_init()` and `module_exit()` callbacks.
- Reference count the module; prevent unloading while in use.
- Support `THIS_MODULE` reference from within module code.
- Export symbols from modules for use by other modules (`EXPORT_SYMBOL`).
- Handle module parameters (`module_param()`).
- Module signing verification (PKCS#7 / IMA).

### 2.2 Non-Functional Requirements
- Load time: < 100 ms for a 1 MB module.
- Zero-TOCTOU races between symbol resolution and module unload.
- No global lock held during module `init()` execution.
- Module text mapped as executable, data as non-executable (W^X).

---

## 3. Constraints & Assumptions

- x86-64 with `CONFIG_MODULES=y`, `CONFIG_MODULE_SIG=y`.
- KASLR (Kernel Address Space Layout Randomization) is active.
- Module ELF is a relocatable `.o` (ET_REL), not a shared library (ET_DYN).
- Module signing uses RSA-2048 + SHA-256.

---

## 4. Architecture Overview

```
  User Space                              Kernel Space
  ┌─────────────┐                         ┌──────────────────────────────────────┐
  │  insmod /   │──── finit_module() ────►│  sys_finit_module()                  │
  │  modprobe   │     syscall              │       │                              │
  └─────────────┘                         │       ▼                              │
                                          │  load_module()                       │
                                          │  ┌─────────────────────────────────┐ │
                                          │  │ 1. ELF header validation        │ │
                                          │  │ 2. Signature check (PKCS#7)     │ │
                                          │  │ 3. Section layout & allocation  │ │
                                          │  │    vmalloc(text), vmalloc(data) │ │
                                          │  │ 4. Symbol resolution (KSYMTAB)  │ │
                                          │  │ 5. Relocation (R_X86_64_*)      │ │
                                          │  │ 6. set_memory_ro(text)          │ │
                                          │  │    set_memory_x(text)           │ │
                                          │  │ 7. do_init_module() → init()    │ │
                                          │  └─────────────────────────────────┘ │
                                          │                                      │
                                          │  Module Registry (modules list)      │
                                          │  ┌──────────┐ ┌──────────┐          │
                                          │  │ mod_A    │→│ mod_B    │→ ...      │
                                          │  │ refcount │ │ refcount │          │
                                          │  └──────────┘ └──────────┘          │
                                          └──────────────────────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 Module Descriptor
```c
struct module {
    enum module_state state;    /* UNFORMED, COMING, LIVE, GOING */
    struct list_head  list;     /* linked into global modules list */
    char              name[MODULE_NAME_LEN];

    /* ELF layout in kernel address space */
    void             *core_layout.base;   /* .text, .rodata, .data */
    unsigned int      core_layout.size;
    void             *init_layout.base;   /* .init.text (freed after init) */
    unsigned int      init_layout.size;

    /* Symbol table */
    const struct kernel_symbol *syms;     /* EXPORT_SYMBOL table */
    unsigned int                num_syms;
    const struct kernel_symbol *gpl_syms; /* EXPORT_SYMBOL_GPL table */
    unsigned int                num_gpl_syms;

    /* Dependencies */
    struct module    **deps;              /* modules this one uses */
    unsigned int      num_deps;
    struct list_head  source_list;        /* modules using this one */
    struct list_head  target_list;

    /* Reference counting */
    atomic_t          refcnt;            /* # active references */

    /* Init/exit */
    int  (*init)(void);
    void (*exit)(void);

    /* Per-CPU data */
    unsigned int      percpu_size;
    void             *percpu;

    /* Module parameters */
    struct kernel_param *kp;
    unsigned int         num_kp;
};
```

### 5.2 Kernel Symbol Export Entry
```c
struct kernel_symbol {
    unsigned long value_offset;   /* offset from __start___ksymtab to symbol address */
    const char   *name_offset;    /* offset to symbol name string */
    const char   *namespace_offset; /* optional namespace (e.g., "NVIDIAGPU") */
};
/* Lookup is O(log N) binary search on sorted ksymtab */
```

### 5.3 Module Dependency Tracking
```c
struct module_use {
    struct list_head source_list;  /* node in module-using-us list */
    struct list_head target_list;  /* node in module-we-use list */
    struct module   *source;       /* the module depending on target */
    struct module   *target;       /* the module being depended upon */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 ELF Section Layout

A `.ko` file is an ET_REL (relocatable object). Sections are mapped into kernel vmalloc space:
```
.text          → vmalloc(text_size), set_memory_exec() + set_memory_ro()
.rodata        → vmalloc(rodata_size), set_memory_ro()
.data          → vmalloc(data_size), writable, NX
.bss           → vmalloc(bss_size), zeroed, writable, NX
.init.text     → vmalloc(init_size), freed after module_init() returns
.percpu        → alloc_percpu()
__ksymtab      → read-only; exported symbols for other modules
__param        → module parameter descriptors
```

**W^X enforcement:** After relocation and before `init()`:
```c
set_memory_ro((unsigned long)mod->core_layout.base, text_pages);
set_memory_x((unsigned long)mod->core_layout.base, text_pages);
/* data sections are already RW and NX by vmalloc default */
```

### 6.2 Symbol Resolution Algorithm

```
for each undefined symbol in module's ELF symtab:
    1. Search __ksymtab in vmlinux (binary search, sorted by name)
    2. If not found, search each loaded module's __ksymtab
    3. If GPL-only: check module license == "GPL" or fails with -EACCES
    4. If namespace: check module explicitly imports the namespace
    5. If still not found: -ENOENT, load fails
    6. Record module_use entry for dependency tracking
```

Symbols are stored as **relative offsets** (not absolute addresses) for KASLR compatibility — the actual address is `&__start___ksymtab + entry->value_offset`.

### 6.3 ELF Relocation Application

For each relocation entry (SHT_RELA section):
```c
switch (ELF64_R_TYPE(rela->r_info)) {
case R_X86_64_64:      /* 64-bit absolute reference */
    *loc = sym_value + rela->r_addend;
    break;
case R_X86_64_PC32:    /* 32-bit PC-relative (most function calls) */
    *loc32 = sym_value + rela->r_addend - (u64)loc;
    break;
case R_X86_64_PLT32:   /* PLT-style — same as PC32 in kernel (no PLT) */
    *loc32 = sym_value + rela->r_addend - (u64)loc;
    break;
/* ... 20+ relocation types total */
}
```

### 6.4 Reference Counting and Safe Unload

```c
/* A caller (e.g., another module or kernel) acquiring a reference */
bool try_module_get(struct module *mod)
{
    if (mod->state != MODULE_STATE_LIVE)
        return false;
    /* atomic increment — no lock needed */
    atomic_inc(&mod->refcnt);
    return true;
}

void module_put(struct module *mod)
{
    atomic_dec(&mod->refcnt);
    /* wake rmmod if it's waiting for refcnt == 0 */
    wake_up(&module_wq);
}
```

**rmmod path:**
```
rmmod → delete_module() syscall
    → set state to MODULE_STATE_GOING
    → wait until atomic_read(&mod->refcnt) == 0
    → call mod->exit()
    → remove from modules list
    → free_module(mod) → vfree(core_layout.base)
```

**The race condition to solve:** Between `atomic_read(refcnt)` and `try_module_get()` — solved by checking `mod->state == MODULE_STATE_GOING` inside `try_module_get()`. Callers using `THIS_MODULE` already hold a reference.

### 6.5 Live Patch Consideration (ftrace-based)

`CONFIG_LIVEPATCH` uses module infrastructure to replace function bodies at runtime via ftrace trampolines. The new function is in a special livepatch module. Consistency model: either switch all tasks to new function atomically (per-task patching via `klp_patch_task()`) or use the stack-based switch mechanism.

### 6.6 Module Signing Verification

```c
/* load_module() calls: */
mod_verify_sig(buf, info);
    → pkcs7_parse_message(sig_start, sig_len)
    → pkcs7_verify(msg, VERIFYING_MODULE_SIGNATURE)
    → check against builtin keyring + secondary_trusted_keys keyring
    → return -EKEYREJECTED if signature invalid
```

Controlled by `/proc/sys/kernel/modules_disabled` (1 = no new modules) and `CONFIG_MODULE_SIG_FORCE`.

---

## 7. Trade-off Analysis

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| Module text allocation | `vmalloc` | `module_alloc` (execmem) | `execmem` handles `CONFIG_STRICT_MODULE_RWX` via `set_memory_*` |
| Symbol lookup | Binary search on sorted ksymtab | Hash table | Binary search has better cache behavior for small tables |
| Refcount for unload | Atomic counter | RCU | Atomic is simpler; RCU for readers, spinlock for writers is overkill here |
| Init section handling | Free init section after init() | Keep forever | `.init.text` can be 30-50% of module size; freeing reclaims memory |
| Unload wait | `wait_event()` on refcnt==0 | Busy poll | `wait_event` is efficient; busy poll wastes CPU |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| Module loader | `kernel/module/main.c` | `load_module()`, `do_init_module()` |
| ELF relocation | `kernel/module/kallsyms.c` | `apply_relocations()` |
| Symbol table | `kernel/module/main.c` | `resolve_symbol()`, `find_symbol()` |
| Module signing | `kernel/module/signing.c` | `mod_verify_sig()` |
| Memory protection | `arch/x86/mm/pat/set_memory.c` | `set_memory_ro()`, `set_memory_x()` |
| Refcounting | `include/linux/module.h` | `try_module_get()`, `module_put()` |
| Module params | `kernel/params.c` | `parse_args()`, `module_param()` |
| Module state machine | `include/linux/module.h` | `enum module_state` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 Symbol Not Found at Load Time
```bash
insmod mymod.ko
# dmesg: "Unknown symbol foo_bar (err -2)"
# Fix: ensure foo_bar is EXPORT_SYMBOL'd in its module
# Check: cat /proc/kallsyms | grep foo_bar
```

### 9.2 Module Refuses to Unload (refcount stuck)
```bash
rmmod mymod
# "ERROR: Module mymod is in use"
lsmod | grep mymod   # check "Used by" column
# If no user shown, may have leaked reference:
cat /sys/module/mymod/refcnt
# Use: CONFIG_DEBUG_MODULE_REFCOUNT for stack traces
```

### 9.3 Oops After Unload (use-after-free on module text)
**Symptom:** Kernel oops at address in `[module space]` for a module that's been removed.
**Debug:** `addr2line` against the `.ko` file.
```bash
# Get the load address from /proc/modules (before unload)
cat /proc/modules | grep mymod
# Column 6 is base address: 0xffffffffc0abc000
# Then: addr2line -e mymod.ko $((oops_rip - 0xffffffffc0abc000))
```

### 9.4 Module Parameter Validation
```bash
modprobe mymod param1=invalid_value
# dmesg: "mymod: parameter param1 rejected"
# Implementation: module_param validators via param_check_*() callbacks
```

---

## 10. Performance Considerations

- **`.init.text` section freeing:** After `init()` runs, `free_pages()` reclaims the init section. On a kernel with thousands of modules (GPU driver, filesystem, etc.), this can save tens of MB.
- **Per-CPU module data:** `alloc_percpu()` for per-CPU state avoids cache-line contention across CPUs for per-module statistics.
- **kallsyms overhead:** `kallsyms_lookup()` is O(N) linear scan — avoid calling in hot paths. Cache the address after first lookup.
- **vmalloc fragmentation:** Heavy module load/unload cycles fragment the vmalloc address space. `CONFIG_DEBUG_VIRTUAL` can track this.
- **sysfs/debugfs entries:** Each `module_param` creates a sysfs entry — avoid excessive parameters for performance-sensitive modules.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. `MODULE_STATE_*` state machine — especially the `GOING` state race with `try_module_get()`.
2. ELF ET_REL vs ET_DYN distinction — kernel modules are relocatable objects, not shared libraries.
3. Symbol namespace feature (added in 5.4) — relevant for NVIDIA proprietary exports.
4. W^X: `set_memory_ro + set_memory_x` after relocation — security requirement.
5. Init section freeing — practical memory optimization detail that shows depth.
6. Live patching architecture built on top of module infrastructure.
7. `EXPORT_SYMBOL_GPL` enforcement — and why NVIDIA historically used `EXPORT_SYMBOL` wrapper modules.
