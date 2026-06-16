# `boot_init_stack_canary()` — Stack Smashing Protection

## Purpose

Initializes the stack canary value for the boot CPU's initial task (`init_task`). The canary is a random value placed at the base of every kernel stack; if a stack overflow overwrites it, `__stack_chk_fail()` detects the corruption and panics before exploitation.

## Source File

`arch/x86/include/asm/stackprotector.h`

```c
static __always_inline void boot_init_stack_canary(void)
{
    u64 canary;
    u64 tsc;
    
    // Get random bytes from CRNG
    get_random_bytes(&canary, sizeof(canary));
    
    // XOR with TSC for additional unpredictability
    tsc = rdtsc();
    canary ^= tsc;
    
    // Ensure canary is never 0 (some compilers check for 0)
    canary &= CANARY_MASK;
    
    // Store in the current task's thread_info
    current->stack_canary = canary;
    
    // Also write to GS:40 (the per-CPU canary location on x86-64)
    this_cpu_write(fixed_percpu_data.stack_canary, canary);
}
```

## Stack Layout with Canary

```
High address: [Task Stack Top] ← RSP starts here
              [Stack frames grow downward]
              ...
              [Stack frame for function A]
              [Stack frame for function B]
              [Canary value] ← GCC inserts read/write here
Low address:  [Stack guard page] ← unmapped, causes fault if hit
              [thread_info] ← task metadata
```

## How GCC Uses the Canary

When compiled with `-fstack-protector-strong`, GCC automatically:

```c
void vulnerable_function(char *user_input) {
    char buf[16];                    // Local buffer
    u64 saved_canary;
    
    // Function prologue (inserted by GCC):
    saved_canary = __stack_chk_guard; // Read from GS:40
    
    // Vulnerable code:
    strcpy(buf, user_input);  // Could overflow!
    
    // Function epilogue (inserted by GCC):
    if (__stack_chk_guard != saved_canary)
        __stack_chk_fail();   // Canary overwritten! Panic!
}
```

## `__stack_chk_guard` and GS Segment

On x86-64, the canary is stored at a fixed offset from the GS segment base:

```c
// Per-CPU data fixed at a known GS offset:
DEFINE_PER_CPU_ALIGNED(struct fixed_percpu_data, fixed_percpu_data) = {
    /* ... */
    .stack_canary = 0,  // Set during boot_init_stack_canary()
};
```

GCC accesses it as `%gs:40` — a single instruction, zero overhead.

## Why After `random_init()`?

The canary must come from a cryptographically secure RNG to be unpredictable. If the attacker can predict the canary value, they can overwrite it with the same value and bypass the check. Using `get_random_bytes()` after `random_init()` ensures the CRNG is fully seeded.

## Per-Process Stack Canaries

At `fork()` time, each new process gets its own random canary:

```c
// In copy_process():
current->stack_canary = get_random_long();
```

All threads within a process share the same canary (since they share the GS segment setup).

## Kconfig

- `CONFIG_STACKPROTECTOR`: Enable basic stack protection
- `CONFIG_STACKPROTECTOR_STRONG`: Enable for all functions with arrays/structs

## Cross-references

- [Phase overview](../README.md)
- `random_init()`: [../random_init/README.md](../random_init/README.md) — canary source
- `set_task_stack_end_magic()`: [../../01_early_cpu_task_setup/set_task_stack_end_magic/README.md](../../01_early_cpu_task_setup/set_task_stack_end_magic/README.md) — related stack safety
