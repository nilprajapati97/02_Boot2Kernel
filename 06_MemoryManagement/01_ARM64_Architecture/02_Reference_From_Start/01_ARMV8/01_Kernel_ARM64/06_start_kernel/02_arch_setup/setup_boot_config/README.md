# `setup_boot_config()` — Bootconfig Parsing

## Purpose

Parses the bootconfig data blob (if present) from the initrd or embedded in the kernel image, and converts `kernel.*` bootconfig keys into additional kernel command line parameters and `init.*` keys into additional init arguments.

## Source File

`init/main.c`

## What is Bootconfig?

Bootconfig is a key-value configuration format introduced in Linux 5.1 as an alternative to the kernel command line. It allows longer, structured configurations that would be impractical in the flat command line format:

```
# Bootconfig example
kernel {
    printk.devkmsg = "on"
    lockdown = "confidentiality"
}
init {
    systemd.unit = "rescue.target"
}
```

Bootconfig data can be:
1. **Appended to initrd** — The bootloader appends the config after the initrd with a specific magic and checksum
2. **Embedded in the kernel** — Compiled-in via `CONFIG_BOOT_CONFIG_EMBED`

## How Bootconfig is Located in initrd

The bootconfig blob is appended at the **end** of the initrd with this trailer:

```
[bootconfig data][4-byte size][4-byte checksum][12-byte BOOTCONFIG_MAGIC]
```

`get_boot_config_from_initrd()` searches backward from `initrd_end` for the magic string.

## What `setup_boot_config()` Does

```
setup_boot_config()
  ├── get_boot_config_from_initrd(&size)     // Find blob in initrd
  ├── xbc_get_embedded_bootconfig(&size)     // Or use embedded config
  ├── parse_args("bootconfig", ..., bootconfig_params)
  │       // Scans cmdline for "bootconfig" keyword
  ├── xbc_init(data, size, &msg, &pos)       // Parse the XBC format
  ├── xbc_make_cmdline("kernel")             // Convert kernel.* → cmdline
  └── xbc_make_cmdline("init")               // Convert init.* → init args
```

## Result

After `setup_boot_config()`:
- `extra_command_line` points to a memblock-allocated string of `kernel.*` params
- `extra_init_args` points to a memblock-allocated string of `init.*` params
- These are prepended to the kernel command line in `setup_command_line()`

## Pre-conditions

- `initrd_start` / `initrd_end` are set (by `setup_arch()`)
- `boot_command_line` contains the original kernel command line

## Post-conditions

- `extra_command_line` set (or NULL if no `kernel.*` keys)
- `extra_init_args` set (or NULL if no `init.*` keys)
- `bootconfig_found` flag set if "bootconfig" was in the cmdline

## IRQ State

IRQs **disabled** — memblock allocation, pure data parsing.

## Kconfig Dependencies

- `CONFIG_BOOT_CONFIG`: Required to enable bootconfig support
- `CONFIG_BOOT_CONFIG_FORCE`: Treat all boots as if `bootconfig` was on cmdline
- `CONFIG_BOOT_CONFIG_EMBED`: Embed a bootconfig file directly in the kernel

## Cross-references

- [Phase overview](../README.md)
- `setup_command_line()` — uses `extra_command_line`: [../setup_command_line/README.md](../setup_command_line/README.md)
