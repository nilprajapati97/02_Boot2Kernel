## `early_init_stack` Setup — Call Flow & Explanation

### What These 3 Lines Do

```asm
adrp  x1, early_init_stack   // x1 = physical address of early_init_stack
mov   sp, x1                 // SP = early_init_stack (stack pointer set)
mov   x29, xzr               // frame pointer = NULL (marks top of call stack)
```

---

### Where `early_init_stack` Lives (vmlinux.lds.S)

```
Kernel image layout (physical memory)
─────────────────────────────────────────────────────
  __pi_init_pg_dir          ← page directory (INIT_DIR_SIZE bytes)
  __pi_init_pg_end          ← end of page dir
                            ┌──────────────────────┐
  (page dir end + 4KB)      │   4KB early stack    │  ← grows DOWN from top
                            │   (SZ_4K reserved    │
                            │    in linker script) │
  early_init_stack  ──────► └──────────────────────┘  ← SP points HERE (top)
─────────────────────────────────────────────────────
```

`early_init_stack` is defined as a **linker symbol** at the **top** of a statically reserved 4KB block. It is placed in the kernel image immediately after the initial page directory region, inside `.bss`. This means it has a **known physical address** at link time — no dynamic allocation needed.

---

### Call Flow

```
primary_entry
│
├── bl record_mmu_state        ← [no stack needed — no locals, no frame]
│
├── bl preserve_boot_args      ← [no stack needed — tail call path uses none]
│
│   ══════════════════════════════════════════════════
│   PROBLEM: preserve_boot_args returns, and now we
│   must call __pi_create_init_idmap() — a C function.
│   C functions REQUIRE a valid stack pointer.
│   But sp is still garbage from the bootloader!
│   ══════════════════════════════════════════════════
│
├── [Set up early stack]  ◄─── YOU ARE HERE
│   │
│   ├── adrp x1, early_init_stack
│   │     Compute page-aligned physical address of
│   │     early_init_stack symbol (linker-defined).
│   │     adrp = PC-relative, so works with/without MMU.
│   │
│   ├── mov sp, x1
│   │     Install early stack pointer.
│   │     Stack grows DOWN from this address.
│   │     4KB reserved below it (linker script: ". += SZ_4K").
│   │
│   └── mov x29, xzr
│         Frame pointer = NULL.
│         Marks this as the outermost frame.
│         Stack unwinder stops here — no previous frame exists.
│
├── adrp x0, __pi_init_idmap_pg_dir   ← first arg to C function
├── mov  x1, xzr                       ← second arg
├── bl   __pi_create_init_idmap        ← C function — NEEDS valid SP ✓
│         Can now use stack for locals,
│         saved registers, function calls
│
│   [Stack also reused later in __primary_switch]
│   ├── __primary_switch:
│   │     adrp x1, early_init_stack
│   │     mov  sp, x1              ← RESET stack after MMU enable
│   │     mov  x29, xzr
│   │     bl __pi_early_map_kernel ← another C function
│
└── [Stack abandoned at __primary_switched]
      init_cpu_task sets up final per-task kernel stack
      sp_el0 = init_task (permanent task stack takes over)
```

---

### Why `x29 = xzr` (Not Just Leaving It Random)

ARM64 stack unwinding works by following the `x29` (frame pointer) chain. Each function saves `{x29, x30}` and sets `x29 = sp`. The unwinder walks this chain until it hits a frame where `x29 == NULL` — that is the **root frame**.

Setting `x29 = 0` here explicitly declares: *"there is no caller above this point"*. Without this, crash dumps and stack traces during early boot would walk into garbage memory.

---

### Key Properties of `early_init_stack`

| Property | Value / Reason |
|----------|---------------|
| Size | 4KB (`SZ_4K` in linker script) |
| Location | Kernel `.bss`, after `__pi_init_pg_end` |
| Address | Known at link time — no allocation needed |
| Address type | Physical — MMU is still off |
| Used by | `__pi_create_init_idmap`, `__pi_early_map_kernel` |
| Lifetime | From `preserve_boot_args` return → `init_cpu_task` in `__primary_switched` |
| Why reset in `__primary_switch` | `__enable_mmu` may corrupt SP during MMU transition; safe to reuse same stack after MMU is on since it's identity-mapped |
