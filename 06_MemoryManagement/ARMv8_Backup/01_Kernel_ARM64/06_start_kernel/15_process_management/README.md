# Phase 15: Process Management Initialization

## Overview

Initializes the kernel's process management subsystem: PID allocation, virtual memory areas for anonymous mappings, task_struct slab caches, credential management, and namespace infrastructure.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`pid_idr_init()`](pid_idr_init/README.md) | `kernel/pid.c` | PID namespace and allocation |
| 2 | [`anon_vma_init()`](anon_vma_init/README.md) | `mm/rmap.c` | Anonymous VMA slab cache |
| 3 | [`fork_init()`](fork_init/README.md) | `kernel/fork.c` | task_struct slab + RLIMIT_NPROC |
| 4 | [`cred_init()`](cred_init/README.md) | `kernel/cred.c` | Credentials slab cache |
| 5 | [`proc_caches_init()`](proc_caches_init/README.md) | `kernel/fork.c` | signal/files/mm slab caches |
| 6 | [`uts_ns_init()`](uts_ns_init/README.md) | `kernel/utsname.c` | UTS namespace |

## IRQ State

- **Entry**: Enabled
- **Exit**: Enabled

## What These Initialize

Together, these functions set up everything needed to call `do_fork()` / `clone()` and create the first real process (`init`).

## Function Index

- [pid_idr_init/](pid_idr_init/README.md)
- [anon_vma_init/](anon_vma_init/README.md)
- [fork_init/](fork_init/README.md)
- [cred_init/](cred_init/README.md)
- [proc_caches_init/](proc_caches_init/README.md)
- [uts_ns_init/](uts_ns_init/README.md)
