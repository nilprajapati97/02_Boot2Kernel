# Phase 3: Parameter Parsing

## Overview

After architecture setup, the kernel processes its command line parameters. This phase converts the raw command line string into active kernel configuration and prepares init arguments. It also initializes the "static keys" mechanism that nearly all subsequent kernel code depends on.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`jump_label_init()`](jump_label_init/README.md) | `kernel/jump_label.c` | Patch NOP sites for static keys |
| 2 | [`parse_early_param()`](parse_early_param/README.md) | `init/main.c` | Process `early_param()` handlers |
| 3 | [`parse_args()`](parse_args/README.md) | `kernel/params.c` | Full kernel parameter parsing |
| 4 | [`random_init_early()`](random_init_early/README.md) | `drivers/char/random.c` | Seed CRNG from cmdline |

## IRQ State

- **Entry**: Disabled
- **Exit**: Disabled

## Memory State

- **Entry**: buddy allocator available (after `mm_core_init()` in Phase 4) — wait, actually these calls happen **before** `mm_core_init()`
- **Correction**: Still `memblock` only — these calls happen at lines 904–917, before `mm_core_init()` at line 927

## Key Outputs of This Phase

- Static branch predictions are armed (jump_label_init)
- `early_param("earlycon", ...)` etc. handlers have run
- Kernel parameters like `nokaslr`, `console=`, `mem=` are applied
- `argv_init[]` and `envp_init[]` are populated with init arguments
- CRNG has an early seed from the command line

## The Three `parse_args()` Calls

`parse_args()` is called three times in `start_kernel()`:

1. **Line 906**: Parse full kernel cmdline → call module params, core params
2. **Line 911**: Parse everything after `--` → fill `argv_init[]` for init
3. **Line 913**: Parse `extra_init_args` (from bootconfig `init.*`) → fill `argv_init[]`

## Function Index

- [jump_label_init/](jump_label_init/README.md)
- [parse_early_param/](parse_early_param/README.md)
- [parse_args/](parse_args/README.md)
- [random_init_early/](random_init_early/README.md)
