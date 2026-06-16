# `parse_early_param()` — Early Parameter Processing

## Purpose

Scans the kernel command line for parameters registered with `early_param()` and calls their handler functions. These are parameters that must be processed before general parameter parsing — typically things like console setup, memory layout modifications, and KASLR control.

## Source File

`init/main.c`

```c
void __init parse_early_param(void)
{
    static int done __initdata;
    static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

    if (done)
        return;

    strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
    parse_early_options(tmp_cmdline);
    done = 1;
}
```

## What is an `early_param`?

```c
// Declaration:
early_param("nokaslr", cmdline_parse_nokaslr);

// This is equivalent to:
static struct obs_kernel_param __setup_nokaslr  \
    __used __section(".init.setup")             \
    __aligned(__alignof__(struct obs_kernel_param)) \
    = { "nokaslr", cmdline_parse_nokaslr, .early = 1 };
```

Early params are stored in the `.init.setup` section (between `__setup_start` and `__setup_end`). `parse_early_param()` iterates this section and calls handlers for entries with `.early = 1`.

## Common `early_param` Handlers

| Parameter | Handler | Effect |
|-----------|---------|--------|
| `earlycon=` | `setup_earlycon()` | Enable early console before `console_init()` |
| `earlyprintk=` | `setup_earlyprintk()` | Legacy early printk |
| `nokaslr` | `cmdline_parse_nokaslr()` | Disable KASLR (already happened, records decision) |
| `mem=` | `setup_mem_size()` | Limit usable RAM |
| `memmap=` | `early_memmap()` | Reserve/mark memory ranges |
| `debug` | `debug_kernel()` | Set `console_loglevel = CONSOLE_LOGLEVEL_DEBUG` |
| `quiet` | `quiet_kernel()` | Set `console_loglevel = CONSOLE_LOGLEVEL_QUIET` |
| `loglevel=` | `loglevel()` | Set console log level |
| `initrd=` | `early_initrd()` | Override initrd location |

## Difference from `__setup()`

```c
// early_param: called from parse_early_param() — very early, before full init
early_param("console", setup_earlycon);

// __setup: called during parse_args() — later, after more infrastructure
__setup("console=", console_setup);
```

Both share the `.init.setup` section but are distinguished by the `.early` flag.

## Why a Temporary Copy?

`parse_args()` modifies the command line string in-place (NUL-terminates parameters). `parse_early_param()` uses a temporary copy `tmp_cmdline` to avoid corrupting `boot_command_line` before `setup_command_line()` has saved a permanent copy.

## Pre-conditions

- `boot_command_line` set by `setup_arch()`
- `__setup_start` / `__setup_end` linker symbols valid

## Post-conditions

- All `early_param()` handlers have been called
- `earlycon` may be active (console output works)
- Memory reservation modifications have been applied

## IRQ State

IRQs **disabled**.

## Cross-references

- [Phase overview](../README.md)
- `parse_args()`: [../parse_args/README.md](../parse_args/README.md)
