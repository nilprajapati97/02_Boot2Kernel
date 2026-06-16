# parse_early_param() — System Design Approach and Q&A

## 1. Why Does parse_early_param() Exist at All?

The kernel needs a way to receive configuration from the bootloader **before** the normal `sysfs`/`procfs`/`module_param()` infrastructure is ready. The command line (a simple ASCII string) is the universal channel because:

1. Every bootloader (U-Boot, GRUB, UEFI, QEMU) can pass a string.
2. No memory management is needed to read a string.
3. The FDT provides it as `/chosen/bootargs` — always parsed before C code.
4. It is human-readable and doesn't require a separate protocol.

The design challenge: **the command line must be processed in two phases** because some parameters affect hardware/memory setup (must be early), while others configure drivers (can be late, after memory management is ready).

---

## 2. Design Principles

### Principle 1: Two-Phase Parsing

The kernel separates parameters into `early` and `late` categories at compile time:

```
Compile time:
  early_param("vmalloc", fn) → obs_kernel_param { .early = 1 }
  __setup("console", fn)     → obs_kernel_param { .early = 0 }

Runtime phase 1 (parse_early_param):
  Process only .early == 1 entries

Runtime phase 2 (parse_args in start_kernel):
  Process all remaining entries
```

**Why not one phase?** At `parse_early_param()` time:
- `kmalloc()` is unavailable (slab not initialized)
- `printk()` may not be routed to hardware yet
- IRQs are disabled
- Many subsystem globals are uninitialized

Drivers cannot initialize in this environment. But memory layout configuration must happen here.

### Principle 2: Non-Destructive Parsing

The original `boot_command_line[]` must be preserved because:
- `/proc/cmdline` exports it to user space
- `kexec` uses it to pass parameters to crash kernel
- Debug tools display it

The `tmp_cmdline[]` copy ensures the tokenizer's in-place null-terminator insertions don't corrupt the original.

### Principle 3: Link-Time Registration (Compile-Time Discovery)

Instead of a runtime registration API (where handlers call `register_early_param()`), Linux uses linker-section-based registration:

```c
early_param("vmalloc", fn)
    → static struct obs_kernel_param __section(".init.setup") = { "vmalloc", fn, 1 };
```

**Advantages over runtime registration:**
- No race conditions (all entries exist before any code runs)
- No dynamic allocation needed
- Dead code elimination — if `CONFIG_ARM_LPAE=n`, the LPAE handler is never compiled in
- O(n) scan but n is small (< 100 entries typically)

---

## 3. Why This Exact Position in setup_arch()?

```
early_fixmap_init()      ← MUST be before parse_early_param
early_ioremap_init()     ← MUST be before parse_early_param
parse_early_param()      ← HERE
early_mm_init(mdesc)     ← needs vmalloc_size already set
adjust_lowmem_bounds()   ← needs vmalloc_size
arm_memblock_init()      ← needs mem= already processed
```

**The constraint graph:**

```
boot_command_line (FDT → setup_machine_fdt)
        │
        ▼ parse_early_param reads it
vmalloc_size ──────────────────► adjust_lowmem_bounds
                                        │
memblock memory (arm_add_memory) ───────►arm_lowmem_limit
                                                │
                                                ▼
                                         paging_init (maps memory)
```

If `parse_early_param()` ran after `adjust_lowmem_bounds()`, `vmalloc=128M` on the command line would be ignored — `adjust_lowmem_bounds()` would have already computed the wrong boundary.

---

## 4. Design Alternatives Considered (and Why Rejected)

### Alternative A: Static compile-time defaults only (no runtime cmdline)

Some embedded systems use this. Problem: you cannot change memory layout without recompiling the kernel. Production kernels must support different DRAM sizes, different vmalloc requirements.

### Alternative B: Parse at head.S (assembly) level

Could parse `vmalloc=` in assembly. Rejected because:
- Assembly string processing is fragile and hard to maintain
- Parameter set grows over time — impossible to maintain in assembly
- Would require reserving registers or a scratch area before the stack is set up

### Alternative C: Single-pass late parsing (no early phase)

All parameters parsed after `start_kernel()` initializes memory management. Rejected because:
- `memblock` must know about `mem=` before allocating anything
- `earlycon=` must work before printk routes to hardware
- `vmalloc=` must be set before page table sizes are computed

### Alternative D: Read from a separate config block (not cmdline)

Some RTOSes use a binary config blob. Linux rejected this for portability — every platform supports the string command line, not every platform supports a structured config blob in a known location.

---

## 5. Dependency Graph

```
                    ┌──────────────────────────────────────────┐
                    │            parse_early_param()           │
                    │                                          │
                    │  DEPENDS ON:           SETS:             │
                    │  boot_command_line[]   vmalloc_size      │
                    │  .init.setup section   memblock ranges   │
                    │  fixmap (for earlycon) earlycon state    │
                    └────────────────┬─────────────────────────┘
                                     │
              ┌──────────────────────┼──────────────────────────┐
              ▼                      ▼                           ▼
  adjust_lowmem_bounds()    arm_memblock_init()        earlycon driver
  (reads vmalloc_size)      (reads memblock ranges)    (early UART output)
```

**Hard dependency**: `early_fixmap_init()` and `early_ioremap_init()` **before** because `earlycon=` handler calls `register_earlycon()` which may map UART register via fixmap.

---

## 6. The `.init.setup` Section Lifecycle

```
Compile time:
  Each translation unit with early_param() / __setup() contributes
  an obs_kernel_param struct to .init.setup section.

Link time:
  vmlinux.lds.h collects all .init.setup entries between
  __setup_start and __setup_end symbols.

Boot time (do_early_param):
  Linear scan of __setup_start → __setup_end
  O(n × m) where n = params in cmdline, m = entries in table
  Typically < 50 entries × < 10 cmdline params = fast.

Post-init (free_initmem):
  .init.setup is part of .init section → freed
  __setup_start / __setup_end become dangling pointers
  (safe — no one calls do_early_param after init)
```

---

## 7. Security Considerations

### Command Line Injection

The command line comes from the bootloader. If the bootloader is compromised, arbitrary early parameters can be injected. Linux mitigates this via:
- Secure Boot: UEFI Secure Boot verifies the bootloader chain
- Lockdown mode: `CONFIG_SECURITY_LOCKDOWN_LSM` — certain params are blocked at early stage when in lockdown
- Module signing: `module.sig_enforce` parameter cannot be disabled via cmdline in lockdown

### Information Disclosure

`/proc/cmdline` exposes the full command line to users. If the cmdline contains sensitive data (e.g., init= paths), it is visible. Best practice: do not put secrets in the kernel command line.

### Parameter Validation

Each handler is responsible for validating its own input. `memparse()` handles overflow; most handlers clamp values with `min()`/`max()`. There is no global sanitization layer — a vulnerability in a handler is a vulnerability in early boot.

---

## 8. System Design Q&A — Interview Level

**Q: How does the kernel avoid processing the same early parameter twice if both the arch code and generic code call parse_early_param()?**
> The `static int done __initdata` guard in `parse_early_param()`. The first call sets `done = 1`. All subsequent calls return immediately. This is a classic one-shot initialization pattern.

**Q: What is the `obs_kernel_param` structure and what does "obs" stand for?**
> "obs" stands for "obsolete" — a historical reminder that `__setup()` was the old way before `module_param()`. The structure has three fields: `str` (parameter name), `setup_func` (handler), `early` (1 = process in parse_early_param). Despite the name, it's still the correct mechanism for boot parameters that don't map to module parameters.

**Q: How would you add a new architecture-specific early parameter?**
> Call `early_param("my_param", my_handler)` in any `.c` file in the arch directory. The `early_param` macro places the handler in `.init.setup` section. `do_early_param()` will find it automatically at boot. No registration call needed.

**Q: Why doesn't parse_early_param() return an error if a parameter is unknown?**
> By design — `do_early_param()` always returns 0. Unknown parameters are silently ignored at early stage. They may be handled in late parsing (`__setup()` handlers), or they may be module parameters. Strict validation would reject valid parameters intended for late processing.

**Q: How is "console=ttyS0" processed during parse_early_param()?**
> `do_early_param()` has a special case: if `param == "console"` and `p->str == "earlycon"`, it calls the earlycon handler. This allows `console=` to bootstrap early console output through the earlycon infrastructure without needing a separate `earlycon=` parameter.

**Q: What would happen if vmalloc= was not processed early enough?**
> `adjust_lowmem_bounds()` would compute `vmalloc_limit` using the default `vmalloc_size` (240MB on ARM32). If the user specified `vmalloc=128M`, the kernel would still allocate 240MB for vmalloc. This could cause memory fragmentation or waste highmem pages. The actual `vmalloc=` value would be ignored silently — a subtle, hard-to-debug misconfiguration.

**Q: How does parse_early_param relate to initrd and initial ramdisk setup?**
> Not directly — `parse_early_param()` processes the command line string. The `initrd=` parameter (if used) would be processed here, but on modern FDT-based systems the initrd location is specified in the FDT itself and is picked up during `early_init_dt_scan_memory()`. `parse_early_param()` still processes any `initrd=` override on the command line.

**Q: Is parse_early_param() SMP-safe?**
> At this point there is only one CPU running (BSP — Boot Strap Processor). Secondary CPUs are brought up much later in `smp_init()`. The `done` guard is not protected by a lock — it doesn't need to be because parse_early_param() is never called from multiple CPUs simultaneously.

**Q: Can a kernel module register an early_param handler?**
> No. `early_param()` uses the `.init.setup` linker section, which is part of the built-in kernel image. Modules are loaded after `parse_early_param()` has already run and the `.init.setup` section may have been freed. Module parameters use `module_param()` which is a completely different mechanism.
