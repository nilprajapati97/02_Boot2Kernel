# `setup_command_line()` — Command Line Buffer Allocation

## Purpose

Allocates permanent copies of the kernel command line using `memblock_alloc()` and stores them in the global pointers `saved_command_line` (preserved forever) and `static_command_line` (parsed in-place). This ensures that `/proc/cmdline` always reflects the original command line even after parameter parsing modifies it.

## Source File

`init/main.c`

## The Three Command Line Variables

| Variable | Type | Contents | Modified? |
|----------|------|----------|-----------|
| `boot_command_line` | `char[]` | Original from bootloader (arch-specific) | Never |
| `saved_command_line` | `char *` | Complete saved copy (for `/proc/cmdline`) | Never |
| `static_command_line` | `char *` | Working copy for `parse_args()` | **Yes** — NUL-terminated in-place |

## What This Function Does

```c
static void __init setup_command_line(char *command_line)
{
    // len = extra_command_line length + boot_command_line length + 1
    // ilen = extra_init_args length + 4 (for " -- ")

    saved_command_line = memblock_alloc(len + ilen, SMP_CACHE_BYTES);
    static_command_line = memblock_alloc(len, SMP_CACHE_BYTES);

    // Build saved_command_line:
    //   [extra_command_line] [boot_command_line] [ -- extra_init_args]
    // Build static_command_line:
    //   [extra_command_line] [arch command_line]
}
```

## Why Two Copies?

- **`saved_command_line`** is used for `/proc/cmdline` and crash diagnostics. It must be the full original command line (including bootconfig-derived params) and must never be modified.
- **`static_command_line`** is passed to `parse_args()`, which NUL-terminates parameter names in-place. After parsing, it contains garbled data.

## The `--` Separator

If the command line contains `--`, everything after it is for `init`, not the kernel:

```
kernel_param1 kernel_param2 -- init_arg1 init_arg2
```

- `saved_command_line` includes everything including the `--` and init args
- `static_command_line` includes only the kernel parameters (before `--`)
- `parse_args()` returns a pointer to `after_dashes` (the init args part)

## `saved_command_line_len`

Also set here: `saved_command_line_len = strlen(saved_command_line)`. This is used by `do_initcalls()` to allocate a fresh copy of the command line for each initcall level's `parse_args()` call.

## Pre-conditions

- `extra_command_line` and `extra_init_args` set by `setup_boot_config()`
- `boot_command_line` set by `setup_arch()`

## Post-conditions

- `saved_command_line` — permanent, complete cmdline
- `static_command_line` — mutable working copy
- `saved_command_line_len` — length of saved copy

## IRQ State

IRQs **disabled** — memblock allocation only.

## Cross-references

- [Phase overview](../README.md)
- `setup_boot_config()` — provides `extra_command_line`: [../setup_boot_config/README.md](../setup_boot_config/README.md)
