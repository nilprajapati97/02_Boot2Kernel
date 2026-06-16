# `setup_arch()` — Memory Map (e820 & memblock)

## Overview

A critical sub-task of `setup_arch()` is establishing the kernel's complete view of physical memory. This involves reading the BIOS/firmware memory map (the e820 table on x86), sanitizing it, and loading it into `memblock` — the kernel's early boot memory allocator.

## The e820 Memory Map

The BIOS/UEFI firmware provides a map of physical address ranges with their types. The bootloader reads this via BIOS interrupt 0x15 (function E820h) and stores it in `boot_params.e820_table`.

### e820 Region Types

| Type | Value | Meaning |
|------|-------|---------|
| `E820_TYPE_RAM` | 1 | Usable RAM |
| `E820_TYPE_RESERVED` | 2 | Reserved by firmware (MMIO, etc.) |
| `E820_TYPE_ACPI` | 3 | ACPI reclaimable memory |
| `E820_TYPE_NVS` | 4 | ACPI NVS (preserve across sleep) |
| `E820_TYPE_UNUSABLE` | 5 | Memory with errors |
| `E820_TYPE_PMEM` | 7 | Persistent memory (NVDIMM) |

### Example e820 Table (typical x86-64 system)

```
[mem 0x0000000000000000-0x000000000009efff] usable
[mem 0x000000000009f000-0x00000000000fffff] reserved
[mem 0x0000000000100000-0x00000000bffdffff] usable
[mem 0x00000000bffe0000-0x00000000bfffffff] reserved
[mem 0x00000000fed40000-0x00000000fed44fff] reserved
[mem 0x0000000100000000-0x000000023fffffff] usable
```

## memblock — The Early Memory Allocator

`memblock` is a simple two-list memory allocator used exclusively during early boot (before the page allocator is ready):

- **`memblock.memory`** — all physical memory regions (RAM)
- **`memblock.reserved`** — regions that cannot be used (kernel, initrd, ACPI, etc.)

Free memory available for `memblock_alloc()` = `memory - reserved`.

### Key memblock Operations in `setup_arch()`

```c
// Add RAM regions from e820
memblock_add(start, size);

// Reserve the kernel image itself
memblock_reserve(__pa_symbol(_text),
                 __pa_symbol(_end) - __pa_symbol(_text));

// Reserve initrd
memblock_reserve(ramdisk_image, ramdisk_size);

// Reserve ACPI tables
memblock_reserve(acpi_tables_start, acpi_tables_size);
```

## What `e820__memory_setup()` Does

1. Copies e820 entries from `boot_params` to `e820_table`
2. Calls `e820__update_table()` — sorts and merges overlapping/adjacent entries
3. Sanitizes entries (removes zero-size entries, clips to 64-bit bounds)
4. Calls `e820__memblock_setup()` — adds usable RAM to `memblock.memory`

## Memory Reservations

After loading the e820 map, `setup_arch()` makes these reservations:

| What | Why |
|------|-----|
| Kernel image (`_text` to `_end`) | Protect the running kernel |
| Initrd/initramfs | Used later for initial rootfs |
| BIOS data areas (0x0–0x100000) | Legacy BIOS structures |
| ACPI tables | Used by drivers throughout boot |
| CPU microcode | Loaded early for security |
| Kernel page tables | Can't be overwritten |
| Per-CPU areas | Allocated here via memblock |

## Result

After `setup_arch()` completes the memory map phase:

```
memblock.memory = all usable RAM (e.g., 8 GB)
memblock.reserved = kernel + initrd + ACPI + page tables + per-CPU
Available for memblock_alloc() = 8 GB - ~30 MB
```

## Transition to Buddy Allocator

`memblock` is a temporary allocator. In `mm_core_init()` (Phase 4), all free memblock regions are freed to the buddy allocator via `memblock_free_all()`. After that, `memblock` is no longer used (except for forensic/debug queries).

## Cross-references

- [setup_arch overview](../README.md)
- `mm_core_init()` — where memblock is replaced: [../../../04_memory_management/mm_core_init/README.md](../../../04_memory_management/mm_core_init/README.md)
