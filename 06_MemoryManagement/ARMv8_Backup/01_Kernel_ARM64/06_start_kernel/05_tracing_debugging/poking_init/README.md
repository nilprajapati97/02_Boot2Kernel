# `poking_init()` — Text Patching Infrastructure

## Purpose

Initializes the memory management infrastructure needed to safely patch kernel text (code) at runtime. On x86-64, this creates a special temporary mapping used during `text_poke_bp()` to avoid issues with the kernel's read-only text mapping.

## Source File

`arch/x86/kernel/alternative.c`

```c
void __init poking_init(void)
{
    spinlock_init(&poking_lock);
    
    // Allocate a fake "task" whose page tables can be used
    // to create writable mappings of kernel text
    poking_mm = copy_init_mm();
    
    // Find a suitable virtual address range in that mm
    // (Must be outside the normal kernel range to avoid TLB conflicts)
    poking_addr = TASK_UNMAPPED_BASE;
}
```

## Why is This Needed?

The kernel text (`.text` section) is mapped **read-only** in the kernel page tables (for security and to catch bugs). But runtime patching (ftrace, live patches, jump labels) needs to write to it.

### The `text_poke_bp()` Strategy

```
Problem: Can't write to read-only kernel text

Solution:
1. Create a TEMPORARY writable mapping of the target page
   → Uses a separate page table (poking_mm) to avoid RO flag
2. Disable the function being patched via INT3 breakpoint
   → Prevents CPUs from executing the bytes being modified
3. Write the new bytes to the temporary writable mapping
4. Wait for all CPUs to acknowledge (stop_machine or IPI)
5. Remove the INT3, install final bytes
6. Remove the temporary writable mapping
```

This is safe because:
- The INT3 breakpoint catches any CPU that tries to execute the patched bytes mid-patch
- The INT3 handler redirects execution safely
- The temporary mapping is per-CPU, avoiding coherency issues

## Text Patching Users

| Mechanism | When Used |
|-----------|-----------|
| `ftrace_init()` | mcount→NOP patching at boot |
| `jump_label_init()` | NOP→JMP patching |
| `apply_alternatives()` | CPU feature-based patching |
| `livepatch` | Runtime function replacement |

## `poking_mm` Design

The `poking_mm` is a minimal `mm_struct` cloned from `init_mm`. It provides page tables that can map a kernel physical page as **writable** using a virtual address that doesn't conflict with the normal kernel VA mapping.

```
Normal kernel VA:  0xffffffff81001234 → physical 0x01001234 (RO)
Poking VA:         0x0000000001001234 → physical 0x01001234 (RW, temporary)
```

## Pre-conditions

- `copy_init_mm()` requires kmalloc
- `vmalloc` available for page table allocation

## Post-conditions

- `text_poke_bp()` is safe to call
- `poking_mm` allocated and configured
- `poking_lock` initialized

## Kconfig

- `CONFIG_X86_64`: This implementation is x86-64 specific
- Other architectures have simpler mechanisms (e.g., ARM64 uses `aarch64_insn_patch_text()`)

## Cross-references

- [Phase overview](../README.md)
- `ftrace_init()`: [../ftrace_init/README.md](../ftrace_init/README.md)
- `jump_label_init()`: [../../03_parameter_parsing/jump_label_init/README.md](../../03_parameter_parsing/jump_label_init/README.md)
