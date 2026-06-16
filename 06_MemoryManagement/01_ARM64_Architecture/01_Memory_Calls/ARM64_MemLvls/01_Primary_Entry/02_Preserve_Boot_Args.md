# `preserve_boot_args` — Save FDT and Bootloader Arguments

**Source:** `arch/arm64/kernel/head.S` lines 161–175

## Purpose

Save the four registers (`x0`–`x3`) passed by the bootloader before they are clobbered by subsequent function calls. Most critically, `x0` holds the **FDT (Flattened Device Tree) physical address** — the kernel's only way to discover what hardware exists.

## Code Walkthrough

```asm
SYM_CODE_START_LOCAL(preserve_boot_args)
    mov   x21, x0                   ; x21 = FDT pointer (callee-saved)

    adr_l x0, boot_args             ; x0 = address of boot_args[] array
    stp   x21, x1, [x0]            ; store x0, x1 at boot_args[0..1]
    stp   x2, x3, [x0, #16]        ; store x2, x3 at boot_args[2..3]
```

### Line-by-line:

1. **`mov x21, x0`** — The FDT address arrives in `x0` (per ARM64 boot protocol). Move it to `x21` immediately. `x21` is callee-saved, so it survives all future `bl` calls until `start_kernel()`.

2. **`adr_l x0, boot_args`** — Load the address of the `boot_args` global array. This is a `u64[4]` declared in C code that records the original bootloader arguments for debugging.

3. **`stp x21, x1, [x0]`** — Store pair: save `x21` (original `x0` = FDT) and `x1` at `boot_args[0]` and `boot_args[1]`.

4. **`stp x2, x3, [x0, #16]`** — Store pair: save `x2` and `x3` at `boot_args[2]` and `boot_args[3]`.

### Cache Maintenance Fork

```asm
    cbnz  x19, 0f                   ; if MMU was on, skip to label 0
    dmb   sy                        ; data memory barrier (full system)

    add   x1, x0, #0x20            ; x1 = boot_args + 32 (end of 4×8 bytes)
    b     dcache_inval_poc          ; tail call: invalidate cache for boot_args
```

**MMU was OFF path:**
- `dmb sy` — ensures the `stp` stores above are visible to all observers before cache invalidation
- `dcache_inval_poc` — invalidate the cache lines covering `boot_args` (32 bytes). With MMU off, the stores went directly to RAM, but speculative cache fills may have loaded stale data for those addresses. Invalidation discards those stale lines.
- This is a **tail call** (`b` not `bl`) — `dcache_inval_poc` will `ret` directly to `primary_entry`.

```asm
0:  str_l x19, mmu_enabled_at_boot, x0
    ret
```

**MMU was ON path:**
- Store the `x19` value (SCTLR with M bit) into the `mmu_enabled_at_boot` global variable. This records for later C code that the system booted with MMU enabled (e.g., EFI boot).
- No cache invalidation needed — the cache is coherent when MMU is on.

## Data Flow

```
Bootloader                        preserve_boot_args
┌──────────┐                      ┌─────────────────────┐
│ x0 = FDT │ ──────────────────►  │ x21 = FDT (saved)   │
│ x1 = arg1│ ──────────────────►  │ boot_args[0] = FDT  │
│ x2 = arg2│ ──────────────────►  │ boot_args[1] = arg1 │
│ x3 = arg3│ ──────────────────►  │ boot_args[2] = arg2 │
└──────────┘                      │ boot_args[3] = arg3 │
                                  └─────────────────────┘
```

## Why `x21` and Not a Global Variable?

At this point in boot:
- The MMU may be off — addresses are physical, not virtual
- There's no stack yet (stack is set up in the NEXT step)
- Storing to a global variable requires cache maintenance (done above for `boot_args`)
- A callee-saved register (`x21`) is the cheapest, most reliable "variable" available

`x21` carries the FDT pointer all the way through:
1. `primary_entry` → `__primary_switch` → passes as `x1` to `__pi_early_map_kernel`
2. `__primary_switched` → stores into `__fdt_pointer` global (with MMU on, cache coherent)
3. `start_kernel()` → C code reads `__fdt_pointer`

## Key Takeaway

After `preserve_boot_args` returns:
- **`x21`** = FDT physical address (persists until `start_kernel()`)
- **`boot_args[0..3]`** = saved in RAM for debugging
- **`mmu_enabled_at_boot`** = set if EFI/MMU-on boot (MMU-on path only)
