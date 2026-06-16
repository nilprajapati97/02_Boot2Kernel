# arm_efi_init() — System Design Approach and Q&A

## 1. Why arm_efi_init() Exists Before Memory Initialization

UEFI firmware passes a **memory map** to the OS. This map contains all memory regions and their types (conventional RAM, reserved firmware, runtime services, ACPI tables, etc.). The kernel MUST process this map before making any memory allocation decisions because:

1. **EFI_RUNTIME_SERVICES regions**: Must be reserved before memblock can allocate them.
2. **EFI_ACPI_RECLAIM regions**: Must be kept until ACPI tables are parsed; then freed.
3. **EFI_RESERVED regions**: Hardware-specific reserved areas; must not be used as RAM.

If `arm_efi_init()` runs after `arm_memblock_init()`, these regions might already be marked available in memblock, and the kernel could allocate them as general memory — corrupting firmware data.

---

## 2. The Layered EFI Boot Architecture

```
Layer 4: OS Runtime
         /sys/firmware/efi/efivars/  (read/write UEFI variables)
         efi_enter_virtual_mode()    (maps EFI runtime into kernel VA)
                 ▲
Layer 3: Kernel Boot (arm_efi_init area)
         efi_init() → parse memory map, parse config tables
         memblock reservations for EFI regions
                 ▲
Layer 2: EFI Stub (ExitBootServices)
         GetMemoryMap() → freezes memory map
         ExitBootServices() → firmware transitions to runtime mode
                 ▲
Layer 1: UEFI Firmware
         Loads kernel image as EFI application
         Provides Boot + Runtime services
                 ▲
Layer 0: Hardware
         SPI flash, eMMC boot partition, PXE boot
```

`arm_efi_init()` operates in Layer 3 — it bridges the firmware's Layer 2 output (memory map) with the kernel's memory management.

---

## 3. Dependency Graph

```
                UEFI Stub (ExitBootServices)
                        │
                        │ passes EFI memory map pointer
                        ▼
                arm_efi_init() → efi_init()
                ├── efi_memmap_init_early()
                │       │ uses early_ioremap (fixmap)
                ├── efi_config_parse_tables()
                │       │ locates ACPI RSDP, SMBIOS, etc.
                └── efi.flags updated
                        │
                        ▼
              arm_memblock_init()
              └── efi_memblock reserve regions
                        │
                        ▼
              paging_init() → permanent EFI region mapping
                        │
                        ▼
              efi_enter_virtual_mode()   (late, post-boot)
              └── runtime services callable from kernel VA
```

---

## 4. Design Alternatives

### Alternative A: Parse EFI Memory Map in head.S (assembly)

Parse the UEFI memory map in early assembly before the C runtime. Rejected:
- UEFI memory map format is complex (variable-length descriptor array)
- Would require significant assembly infrastructure for struct access
- Early assembly must be minimal and architecture-specific

### Alternative B: Trust bootloader to reserve EFI regions in FDT

Have the EFI stub insert `reserved-memory` nodes in the FDT for all EFI reserved regions. Simpler for the kernel. Problem:
- Duplicates information (UEFI memory map AND FDT reserved memory)
- FDT reserved-memory is limited in expressive power vs UEFI memory types
- Runtime services regions need special handling the FDT doesn't express

### Alternative C: Process EFI map only when first needed

Lazy initialization — only process the EFI memory map when an EFI API is first called. Rejected:
- The EFI physical memory map pointer passed by the stub might point to memory that's been reclaimed as general RAM by the time the lazy init runs
- EFI memory reservations must happen before any memory allocation

---

## 5. Security Architecture: UEFI Secure Boot Chain

```
Hardware Root of Trust (TPM / Fuse-based key)
        │
        ▼
UEFI Firmware (signed by OEM key)
        │ verifies signature against PK (Platform Key) → db/dbx
        ▼
Bootloader (signed with db key, e.g., Microsoft 3rd party CA)
        │
        ▼
Shim (signed, loads grub or kernel directly)
        │
        ▼
GRUB (signed with MOK — Machine Owner Key)
        │
        ▼
Linux Kernel (signed with MOK or db)
        │
        ▼
Kernel modules (if lockdown mode: must be signed too)
```

`arm_efi_init()` → `efi_config_parse_tables()` locates the Secure Boot database variables in the EFI variable store. Later, `efi_enabled(EFI_SECURE_BOOT)` reflects whether this chain is enforced. When Secure Boot is active, the kernel enforces:
- Module signature requirement
- `kexec` with signed images only
- No `CONFIG_KALLSYMS` symbol writing
- No direct `/dev/mem` access

---

## 6. EFI Memmap and KASLR (ARM64)

ARM64 with KASLR randomizes the kernel load address. UEFI must provide enough conventional memory at the load address for this to work:

```
EFI Boot flow with KASLR:
1. UEFI loads EFI stub to any available conventional memory
2. EFI stub calls GetMemoryMap() to find largest contiguous conventional region
3. EFI stub randomizes kernel load address within that region
4. EFI stub loads uncompressed kernel Image to randomized address
5. ExitBootServices()
6. Kernel runs at randomized address
7. arm_efi_init() records EFI memory map (which now shows stub region as used)
```

If the EFI memory map has fragmented conventional memory, KASLR has fewer valid addresses — reducing randomization entropy. This is a subtle interaction between UEFI firmware quality and security.

---

## 7. System Design Q&A

**Q: What is the EFI System Table and how does arm_efi_init() use it?**
> The EFI System Table is the root data structure of UEFI — it contains pointers to firmware services, configuration tables (ACPI RSDP, SMBIOS), and the firmware vendor string. The EFI stub saves the EFI System Table pointer in `efi.systab`. `efi_init()` uses this to locate config tables: `efi_config_parse_tables()` iterates `systab.tables[]` looking for ACPI_TABLE_GUID (RSDP), SMBIOS_TABLE_GUID, and other standard tables. This is how the kernel discovers ACPI tables on UEFI systems without needing to scan memory.

**Q: What happens to EFI runtime services after paging_init() changes the page tables?**
> EFI runtime code is mapped at specific physical addresses. After `paging_init()` sets up new page tables, those physical addresses might not be mapped at the same virtual addresses. Before calling any EFI runtime service, `efi_enter_virtual_mode()` must be called. It calls `SetVirtualAddressMap()` — a UEFI boot service — telling the firmware the new virtual addresses where runtime service code and data will be mapped. After this call, the firmware patches its own internal pointers to use the new VAs. The kernel must never call EFI runtime services between `paging_init()` and `efi_enter_virtual_mode()`.

**Q: Why does ARM32 need an arm_efi_init() wrapper while ARM64 calls efi_init() directly?**
> Historically, ARM32 EFI support was added later and had some ARM-specific initialization. The wrapper allowed inserting ARM32-specific logic. In practice, current ARM32 `arm_efi_init()` is just a guard (`if (!efi_enabled(EFI_BOOT)) return`) plus a call to `efi_init()`. On ARM64, the UEFI support was designed from the start, so `efi_init()` is called directly. The difference is historical, not architectural.

**Q: What is the ESRT (EFI System Resource Table) and what does it have to do with firmware updates?**
> ESRT is a UEFI configuration table that lists all firmware components (CPU microcode, BIOS, ME firmware, NIC firmware) along with their current version and Firmware Management Protocol GUID. Linux exposes ESRT via `/sys/firmware/efi/esrt/entries/`. The `fwupdmgr` tool reads ESRT to know which firmware components exist, checks a remote server for updates, and uses UEFI capsule update mechanism to apply them. `arm_efi_init()` → `efi_config_parse_tables()` locates the ESRT in the EFI config tables.

**Q: How does the kernel protect EFI runtime memory from being corrupted by device drivers?**
> EFI runtime service pages are marked as reserved in memblock (via `efi_memblock_x86_reserve_range` or equivalent). They never enter the buddy allocator, so no `kmalloc` or `vmalloc` can return them. After `paging_init()`, these pages are mapped with `PAGE_KERNEL` (or `PAGE_KERNEL_EXEC` for code pages). Since they're not in any zone, they can't be freed or reused. An additional protection: if `CONFIG_EFI_MIXED=n`, EFI runtime services run with a separate EFI page table that only maps EFI regions — a driver bug that corrupts kernel memory can't corrupt EFI memory because they use different page tables.
