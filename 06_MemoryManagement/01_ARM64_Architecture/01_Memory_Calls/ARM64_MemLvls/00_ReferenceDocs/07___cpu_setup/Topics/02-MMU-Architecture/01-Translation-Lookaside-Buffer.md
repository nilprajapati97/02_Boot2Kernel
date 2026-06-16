# Translation Lookaside Buffer

The TLB caches recent address translations. If it contains stale entries when translation policy changes, the CPU may use old results.

## Why `__cpu_setup` starts with `tlbi vmalle1`

The function begins with a local EL1 stage-1 TLB invalidation. This means:

- old translations are discarded
- the CPU is less likely to use stale state after new control settings are loaded
- the early MMU transition starts from a predictable translation cache state

## Why the barrier follows

`dsb nsh` ensures the invalidation has architecturally completed before execution continues into register programming.

## Important beginner point

A page table in memory is not the same as a TLB entry inside the CPU. TLB invalidation is about the CPU's cached view of translation, not about changing the page tables themselves.
