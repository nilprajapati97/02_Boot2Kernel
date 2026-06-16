# Secondary CPU Setup

Secondary CPUs also run `__cpu_setup`, but the surrounding context differs from the primary boot CPU.

## Why secondaries need their own `__cpu_setup`

Each CPU has its own architectural register state. Even if the primary CPU already enabled the global kernel environment, every secondary CPU must still program its own local EL1 control registers before joining normal kernel execution.

## Secondary path summary

1. enter secondary startup path
2. normalize exception level with `init_kernel_el`
3. optionally check for 52-bit VA support constraints
4. call `__cpu_setup`
5. call `__enable_mmu`
6. enter `__secondary_switched`
7. continue into `secondary_start_kernel`

## What is shared versus per-CPU

### Shared
- kernel image
- kernel page tables
- global code path design

### Per-CPU
- local TLB contents
- local system register state
- local transition into active MMU context

## Why this matters

A common beginner mistake is to think `__cpu_setup` is only part of the first CPU boot. In reality, it is part of establishing correct per-CPU memory-management state.
