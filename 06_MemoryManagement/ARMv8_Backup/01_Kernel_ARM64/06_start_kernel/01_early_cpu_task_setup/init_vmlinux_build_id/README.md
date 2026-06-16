# `init_vmlinux_build_id()` — ELF Build-ID Recording

## Purpose

Reads the ELF `.note.gnu.build-id` section embedded in the vmlinux image and makes the build-ID available via `/sys/kernel/notes` and crash dumps. The build-ID is a SHA1 or UUID hash that uniquely identifies this exact kernel binary — essential for matching kernel crash dumps to the correct debug symbols (DWARF info).

## Source File

`kernel/buildid.c` (called from `init/main.c`)

## What Is a Build-ID?

A GNU ELF build-ID is a 160-bit (SHA1) hash embedded in the binary at link time via:

```
--build-id=sha1
```

In the kernel ELF:
```
.note.gnu.build-id:
  namesz: 4 ("GNU\0")
  descsz: 20 (SHA1 hash bytes)
  type:   NT_GNU_BUILD_ID (3)
  desc:   <20 bytes of SHA1>
```

## Why Early?

The build-ID must be captured before any code could potentially modify the `.note` section or before `free_initmem()` reclaims init sections. Recording it early ensures it's always available for crash reporting.

## Relation to Kernel Crash Dumps

When a kernel oops or panic occurs, the crash dump includes the build-ID. Tools like `crash`, `drgn`, and `gdb` use the build-ID to:
1. Locate the correct `vmlinux` debug binary
2. Validate that symbols match the running kernel
3. Load the correct DWARF frame information

## Pre-conditions

- The ELF `.note.gnu.build-id` section must be present (it is for distribution kernels; optional for custom builds)

## Post-conditions

- `vmlinux_build_id[]` array contains the 20-byte SHA1 build-ID
- `/sys/kernel/notes` can expose this to user space

## IRQ State

IRQs on or off — read-only scan of `.note` section in memory.

## Kconfig Dependencies

- `CONFIG_VMLINUX_EXPOSE_RAWDATA`: Controls whether build-ID is exposed via sysfs
- Build-ID embedding requires `--build-id` linker flag (set in top-level `Makefile`)

## Cross-references

- [Phase overview](../README.md)
