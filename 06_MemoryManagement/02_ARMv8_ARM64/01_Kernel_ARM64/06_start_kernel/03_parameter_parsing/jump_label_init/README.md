# `jump_label_init()` — Static Keys / Jump Labels

## Purpose

Scans the kernel's `.jump_table` ELF section and patches all static key sites to their initial state (NOP or JMP) based on whether each key is initially enabled or disabled. This establishes the zero-overhead conditional code mechanism used pervasively throughout the kernel.

## Source File

`kernel/jump_label.c`

## The Problem: Cheap Runtime Conditionals

Many kernel features need to be conditionally active based on runtime state (e.g., tracing enabled/disabled, a network protocol loaded/unloaded). A naive approach:

```c
if (tracing_enabled)   // This reads a global variable on every function call
    trace_something();
```

This is expensive in the hot path. On modern CPUs, even a correctly-predicted branch adds ~1 cycle overhead at every call site.

## The Solution: Static Keys

A **static key** (also called a "jump label") patches the machine code itself:

```
When KEY is DISABLED:
    code: NOP NOP NOP NOP NOP    (5 bytes of NOP)
    // no branch taken, no overhead

When KEY is ENABLED:
    code: JMP 0x<target>          (5-byte relative jump)
    // jumps to the enabled code path
```

The NOP→JMP patching is done atomically at runtime using `text_poke_bp()`.

## What `jump_label_init()` Does

```c
void __init jump_label_init(void)
{
    struct jump_entry *iter;
    struct static_key *key;

    // Iterate all entries in __jump_table section
    for (iter = __start___jump_table; iter < __stop___jump_table; iter++) {
        key = jump_entry_key(iter);
        // If key is initially enabled, patch to JMP; otherwise leave as NOP
        if (static_key_enabled(key))
            arch_jump_label_transform(iter, JUMP_LABEL_JMP);
    }
    static_key_initialized = true;
}
```

## The `__jump_table` Section

Each `static_branch_likely()` / `static_branch_unlikely()` call site emits a record in `__jump_table`:

```c
struct jump_entry {
    s32 code;    // Relative offset to the instruction to patch
    s32 target;  // Relative offset to the branch target
    long key;    // Relative offset to the static_key variable
};
```

## Usage Pattern

```c
// Declare a static key (initially disabled):
DEFINE_STATIC_KEY_FALSE(my_feature_enabled);

// Use in hot path:
if (static_branch_unlikely(&my_feature_enabled)) {
    // compiled to NOP when disabled; JMP when enabled
    do_expensive_thing();
}

// Enable at runtime (patches all call sites):
static_branch_enable(&my_feature_enabled);

// Disable again:
static_branch_disable(&my_feature_enabled);
```

## Pre-conditions

- `__start___jump_table` and `__stop___jump_table` linker symbols valid
- Text section must be writable (temporarily, via `set_memory_rw()`)

## Post-conditions

- All static key call sites are in their correct initial state
- `static_key_initialized = true` — subsequent `static_branch_*()` calls are safe

## IRQ State

IRQs **disabled** — modifies kernel text.

## Kconfig Dependencies

- `CONFIG_JUMP_LABEL`: Enables the optimization; without it, falls back to simple `if` statements
- `CONFIG_HAVE_ARCH_JUMP_LABEL`: Arch support flag (x86, ARM64, RISC-V, etc.)

## Cross-references

- [Phase overview](../README.md)
- `ftrace_init()` — also uses code patching: [../../05_tracing_debugging/ftrace_init/README.md](../../05_tracing_debugging/ftrace_init/README.md)
