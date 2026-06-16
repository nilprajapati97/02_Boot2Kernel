## `preserve_boot_args` — Call Flow

```
primary_entry
│
└── bl preserve_boot_args
        │
        ├── [1] Save FDT pointer
        │       x21 = x0  (FDT phys addr into callee-saved register)
        │       Reason: x0 will be clobbered by next function call
        │
        ├── [2] Write bootloader args → boot_args[]
        │       boot_args[0] = x21 (FDT)
        │       boot_args[1] = x1
        │       boot_args[2] = x2
        │       boot_args[3] = x3
        │       Reason: Registers don't survive — memory is the only safe store
        │
        ├── [3] Check MMU state (x19)
        │
        ├── x19 == 0  [MMU was OFF] ────────────────────────────────┐
        │   │                                                       │
        │   ├── dmb sy                                              │
        │   │     Memory barrier: ensure all stp stores are         │
        │   │     globally visible before cache invalidation        │
        │   │     ARM rule: dc ivac requires prior stores visible   │
        │   │                                                       │
        │   ├── x0 = boot_args start                                │
        │   │   x1 = boot_args + 32 (end of 4×8 bytes)              │
        │   │                                                       │
        │   └── b dcache_inval_poc  [TAIL CALL]                     │
        │             │                                             │
        │             │  Walk each cache line: x0 → x1              │
        │             │  dc ivac: invalidate line to PoC            │
        │             │  Evicts stale speculative prefetches        │
        │             │  Next reader forced to fetch from DRAM      │
        │             │                                             │
        │             └── ret → back to primary_entry  ◄────────────┘
        │
        └── x19 != 0  [MMU was ON] ───────────────────────────────┐
                │                                                 │
                ├── Store mmu_enabled_at_boot = x19               │
                │     Non-zero value persisted for C code         │
                │     setup_arch() queries this later             │
                │                                                 │
                └── ret → back to primary_entry  ◄────────────────┘
```

---

### Why Each Step Matters

**`x21` as FDT holder**
At this point in boot, no stack exists yet. Callee-saved registers (`x19`–`x28`) are the only safe "variables". `x21` carries the FDT address all the way through `primary_entry` → `__primary_switch` → `__primary_switched` where it is finally written to `__fdt_pointer` in virtual space.

**`boot_args[]` array**
This is the permanent kernel record of what the bootloader passed. Used by:
- `setup_arch()` — to process kernel command line and FDT
- Crash dumps / kdump — to reconstruct boot state
- `kexec` — to re-pass args to next kernel

**`dmb sy` is non-optional**
ARM memory model does **not** guarantee that a `dc ivac` (cache invalidate) sees the preceding `stp` stores unless a barrier is placed between them. Without `dmb sy`, the invalidation could race with the store — wiping the data before it lands in DRAM — causing `boot_args[]` to read as garbage.

**Tail call (`b` not `bl`)**
`preserve_boot_args` has nothing left to do after triggering `dcache_inval_poc`. Using `b` means `dcache_inval_poc` returns directly to `primary_entry` via the original `lr`. This avoids needing a stack frame, which is correct because **no stack has been set up yet** at this point.

**`mmu_enabled_at_boot`**
On the MMU-on path, C code later in boot needs to know whether the bootloader had the MMU on. `x19` won't exist by then — so it's written to a static variable that survives into the C world.You've used 67% of your weekly rate limit. Your weekly rate limit will reset on April 27 at 5:30 AM. [Learn More]
