# Early Stack Setup

**Source:** `arch/arm64/kernel/head.S` lines 89–91

## Purpose

Set up a minimal stack so that C functions (specifically `__pi_create_init_idmap`) can be called. Until this point, the CPU has been running pure assembly with **no stack at all**.

## Code

```asm
adrp  x1, early_init_stack      ; load page-aligned address of stack buffer
mov   sp, x1                    ; set stack pointer
mov   x29, xzr                  ; frame pointer = 0 (bottom of call chain)
```

## Instruction Breakdown

### `adrp x1, early_init_stack`

- **`adrp`** = Address of Page, Relative to PC
- Computes: `x1 = (PC & ~0xFFF) + (symbol_page_offset << 12)`
- Result: the **page-aligned physical address** of `early_init_stack`
- Works with MMU off because it uses PC-relative addressing (no virtual→physical translation needed)

### `mov sp, x1`

- Sets the Stack Pointer to the top of the pre-allocated stack buffer
- ARM64 stacks grow **downward** — `sp` starts at the high address and decreases with each push

### `mov x29, xzr`

- `x29` is the **Frame Pointer** (FP) in AAPCS64
- Setting it to 0 marks this as the **bottom frame** — stack unwinders will stop here
- Without this, a stack trace would follow garbage FP values into random memory

## What is `early_init_stack`?

A statically allocated buffer in the kernel's data section. It provides just enough space for the early boot C functions to use local variables and make nested calls.

```
Memory Layout:
                    ┌──────────────────┐
                    │                  │  ← sp starts here (high address)
                    │  Stack grows ↓   │
                    │                  │
                    │                  │
                    │  early_init_stack│
                    │  (static buffer) │
                    └──────────────────┘  ← low address
```

## Why Not Use the Final Kernel Stack?

At this point:
- The MMU is off — the kernel's virtual address space doesn't exist yet
- `init_task`'s stack is at a virtual address — inaccessible without page tables
- `early_init_stack` is a physical address that works with MMU off
- It's a temporary bootstrap — replaced by the real stack in `__primary_switched` later

## Stack Lifecycle

| Phase | Stack | Why |
|-------|-------|-----|
| `primary_entry` start | None | Pure assembly, no calls yet |
| After line 91 | `early_init_stack` | Needed for `__pi_create_init_idmap` (C function) |
| `__primary_switch` | `early_init_stack` (re-set) | Re-established after `__enable_mmu` |
| `__primary_switched` | `init_task` stack | Real kernel stack via `init_cpu_task` macro |

## Key Takeaway

Three instructions create the minimum viable stack for early boot C code. This stack is temporary — it exists only to bridge the gap between pure assembly and the real kernel task stack set up in Phase 6 (`__primary_switched`).
