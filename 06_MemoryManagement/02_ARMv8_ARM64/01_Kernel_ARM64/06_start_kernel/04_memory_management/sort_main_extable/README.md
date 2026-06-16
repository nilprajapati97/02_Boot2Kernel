# `sort_main_extable()` — Sort the Exception Table

## Purpose

Sorts the kernel's exception table (`.ex_table` ELF section) by address so that exception handlers can be found quickly via binary search during fault handling. This is required before any code that uses exception table lookups can run safely.

## Source File

`lib/extable.c`

```c
void __init sort_main_extable(void)
{
    if (main_extable_sort_needed && __stop___ex_table > __start___ex_table) {
        pr_notice("Sorting __ex_table...\n");
        sort_extable(__start___ex_table, __stop___ex_table);
    }
}
```

## What is the Exception Table?

The exception table maps **faulting instruction addresses** to **fixup handlers**. When the kernel accesses user memory and takes a page fault, instead of crashing, the fault handler looks up the faulting address in the exception table and jumps to the fixup code.

### Example: `copy_from_user()`

```c
// Simplified x86 inline assembly in copy_from_user:
1:  mov (%rsi), %al       // Load from user address — may fault!
2:  mov %al, (%rdi)       // Store to kernel buffer
    // ...

// Exception table entry (added by _ASM_EXTABLE macro):
.section __ex_table, "a"
    .long (1b - .)    // If instruction at 1: faults...
    .long (fixup - .) // ...jump to fixup

fixup:
    // Set return value = -EFAULT, zero remainder of buffer
    mov $-EFAULT, %eax
    ret
```

## The `exception_table_entry` Structure

```c
struct exception_table_entry {
    int insn;    // Relative offset to faulting instruction
    int fixup;   // Relative offset to fixup code
    int data;    // Handler type / register info
};
```

## Why Sorting is Needed

At link time, exception table entries from different object files are concatenated in arbitrary order. To find an entry quickly, the table must be sorted by `insn` address so binary search works.

Without sorting, every page fault would require a linear scan — O(n) instead of O(log n).

## Who Added Entries Out of Order?

Entries come from:
- Core kernel (`arch/x86/lib/copy_user_64.S`)
- Drivers (device I/O using `get_user`/`put_user`)
- Module code (sorted separately when loaded)

The link order is not guaranteed to be sorted by instruction address.

## Pre-conditions

- `__start___ex_table` and `__stop___ex_table` linker symbols valid
- Called before any code that uses `copy_from_user` (which technically can't run yet anyway since no user space exists)

## Post-conditions

- Exception table sorted; `search_exception_tables()` is O(log n)
- Page faults in kernel context that were expected (copy_from_user, etc.) will be handled correctly

## Cross-references

- [Phase overview](../README.md)
- `trap_init()`: [../trap_init/README.md](../trap_init/README.md) — sets up fault handler that uses exception table
