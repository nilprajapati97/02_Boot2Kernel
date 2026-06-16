# arm_efi_init() — Detailed Design: Bottom-to-Top Flow

## 1. Position in setup_arch() Boot Sequence

```
setup_arch()
  ├── xen_early_init()             ← Xen detection done
  └── arm_efi_init()               ← *** THIS FUNCTION *** (line 1153)
        ├── efi_init()             (if EFI_BOOT set in FDT)
        │     ├── efi_memmap_init_early()
        │     ├── efi_config_parse_tables()
        │     └── efi_memmap_map()
        └── (or no-op if not EFI boot)
```

`arm_efi_init()` processes the **UEFI (Unified Extensible Firmware Interface) memory map** if the system was booted by a UEFI firmware. It makes the UEFI memory map available to the kernel so that `arm_memblock_init()` can correctly identify and reserve UEFI-controlled memory regions.

---

## 2. What Is UEFI on ARM?

UEFI is a firmware interface that replaces the legacy BIOS on x86 and is increasingly used on ARM (especially ARM64 servers, but also some ARM32 boards like the NXP i.MX8 with UEFI firmware). UEFI:
- Provides a standardized firmware boot protocol
- Passes a memory map to the OS describing RAM, reserved regions, ACPI tables, etc.
- Provides runtime services (SetVariable, GetTime, ResetSystem)
- Can load the OS as an EFI application (`Image` format)

---

## 3. Source Code

**File:** `arch/arm/kernel/efi.c`

```c
void __init arm_efi_init(void)
{
    if (!efi_enabled(EFI_BOOT))
        return;

    /*
     * Based on the UEFI Memory Map, we need to properly register
     * UEFI regions with memblock.
     */
    efi_init();
}
```

**EFI_BOOT flag check:**
`efi_enabled(EFI_BOOT)` tests a bit in `efi.flags` which was set by the stub loader. On non-UEFI systems (FDT-only embedded ARM boards), this flag is clear and `arm_efi_init()` returns immediately — zero overhead.

### 3.1 The EFI Stub Loader

On UEFI-booted systems, the kernel image has an **EFI Stub** (`arch/arm/boot/compressed/efi-header.S`). The UEFI firmware loads the kernel as an EFI application. The stub:
1. Calls UEFI `GetMemoryMap()` to get the memory descriptor table.
2. Calls `ExitBootServices()` — freezes the memory map.
3. Jumps to the kernel entry point, passing:
   - Device Tree blob (possibly UEFI-generated)
   - Pointer to the EFI memory map
   - `efi.flags |= EFI_BOOT`

### 3.2 `efi_init()` — Key Operations

```c
/* drivers/firmware/efi/efi.c + arch-specific */
void __init efi_init(void)
{
    /* Map the UEFI memory map passed by the stub */
    efi_memmap_init_early(&efi_memdesc);

    /* Parse UEFI configuration tables (ACPI, SMBIOS, etc.) */
    efi_config_parse_tables();

    /* Build the runtime virtual map for EFI runtime services */
    /* (will be finalized in efi_enter_virtual_mode() later) */
}
```

### 3.3 `efi_memmap_init_early()`

The UEFI memory map is an array of `efi_memory_desc_t` structures:

```c
typedef struct {
    u32 type;               /* EFI_RESERVED_MEMORY_TYPE, EFI_LOADER_CODE, EFI_CONVENTIONAL_MEMORY, ... */
    u64 physical_start;     /* start of region */
    u64 virtual_start;      /* filled by OS when mapping runtime services */
    u64 num_pages;          /* size in 4KB EFI pages */
    u64 attribute;          /* EFI_MEMORY_WB, EFI_MEMORY_UC, EFI_MEMORY_RUNTIME, ... */
} efi_memory_desc_t;
```

`efi_memmap_init_early()` maps the memory map into kernel virtual space using `early_memremap()` (which uses early fixmap) so it can be read.

### 3.4 What Memory Types Mean for memblock

`arm_memblock_init()` (called after `arm_efi_init()`) calls `efi_memblock_x86_reserve_range()` or the ARM equivalent to reserve EFI regions:

| EFI Type | Action in memblock |
|----------|-------------------|
| `EFI_RESERVED_MEMORY_TYPE` | `memblock_reserve()` |
| `EFI_RUNTIME_SERVICES_CODE` | `memblock_reserve()` — EFI runtime code |
| `EFI_RUNTIME_SERVICES_DATA` | `memblock_reserve()` — EFI runtime data |
| `EFI_CONVENTIONAL_MEMORY` | Left as available RAM |
| `EFI_UNUSABLE_MEMORY` | `memblock_reserve()` — hardware broken |
| `EFI_ACPI_RECLAIM_MEMORY` | Reserved until ACPI tables parsed; freed later |
| `EFI_LOADER_CODE/DATA` | Available — bootloader's memory, OS can reclaim |

---

## 4. EFI Runtime Services — The Long-Term Impact

EFI runtime services (GetTime, GetVariable, ResetSystem) must remain callable even after the OS kernel is running. This requires:
- EFI runtime code and data pages remain mapped in kernel virtual space
- The EFI memory regions are marked `EFI_MEMORY_RUNTIME` in the memory map

`arm_efi_init()` starts the process. Later, `efi_enter_virtual_mode()` (called after paging_init from `late_initcall`) sets up the EFI virtual map so runtime service calls use kernel virtual addresses.

---

## 5. EFI Variables and NVRAM

UEFI provides a non-volatile variable store (NVRAM) via `SetVariable()`/`GetVariable()`. Linux exposes this through:
- `/sys/firmware/efi/efivars/` — read/write UEFI variables
- Used by GRUB, systemd-boot for Boot Manager configuration
- UEFI Secure Boot variables (`db`, `dbx`, `PK`, `KEK`)

`arm_efi_init()` → `efi_config_parse_tables()` locates the EFI System Table which contains the pointer to the UEFI variable services.

---

## 6. Call Tree (Bottom-Up)

```
efi_memory_desc_t[]           ← from UEFI stub (physical pointer)
        ▲
efi_memmap_init_early()       ← maps the array via early fixmap
        ▲
efi_config_parse_tables()     ← parses EFI System Table, ACPI, SMBIOS
        ▲
efi_init()                    ← drivers/firmware/efi/efi.c
        ▲
arm_efi_init()                ← arch/arm/kernel/efi.c
        ▲
setup_arch()                  ← arch/arm/kernel/setup.c:1153
```

---

## 7. What Happens in Hardware

On a UEFI-booted system, `arm_efi_init()`:
1. **No hardware register writes** — purely maps and reads memory structures.
2. Uses **early fixmap** (set up by `early_fixmap_init()`) to temporarily map the EFI memory map at a known virtual address.
3. Reads EFI memory descriptors from the mapped memory.
4. The fixmap mapping is temporary — the EFI memory map is later permanently mapped in `efi_enter_virtual_mode()`.

On non-UEFI systems: `arm_efi_init()` returns at the first line — zero hardware interaction.

---

## 8. Interview Q&A

**Q1: What is the difference between UEFI ExitBootServices and normal kernel boot?**
> In UEFI boot, the firmware maintains the "boot services environment" until the OS calls `ExitBootServices()`. The EFI stub calls this after getting the memory map. After `ExitBootServices()`, all UEFI boot services (memory allocation, protocol interfaces, etc.) are unavailable. Only **runtime services** (GetTime, GetVariable, ResetSystem) remain. The kernel must only use runtime services after this point — which is why `arm_efi_init()` just reads the memory map; it never calls boot services.

**Q2: Why is arm_efi_init() positioned after xen_early_init() but before adjust_lowmem_bounds()?**
> EFI and Xen are mutually exclusive in normal ARM deployments — you don't run UEFI firmware under Xen or vice versa. The ordering positions both early enough that their memory reservations (EFI reserved regions, Xen-granted regions) are visible to `arm_memblock_init()` and `adjust_lowmem_bounds()`. If EFI runs after memblock is finalized, UEFI runtime service memory might get allocated as regular RAM, corrupting the firmware.

**Q3: What happens if arm_efi_init() doesn't reserve an EFI_RUNTIME_SERVICES_DATA region?**
> EFI runtime service code calls (e.g., `SetVariable()`) would overwrite whatever the kernel allocated in that physical range. Depending on what's there: page tables (page table corruption), kernel data (kernel data corruption), or another driver's buffer. This would manifest as random crashes or security vulnerabilities, often hours or days into runtime when EFI runtime services are actually called.

**Q4: How does UEFI Secure Boot work with the Linux kernel?**
> UEFI Secure Boot is a chain-of-trust: UEFI firmware verifies the bootloader signature against keys in the Secure Boot database (`db`). The bootloader verifies the kernel signature. Linux's EFI stub participates by verifying its own signature is valid before running. Once booted, the kernel can read and enforce Secure Boot policies via EFI variables (`/sys/firmware/efi/efivars/`). `arm_efi_init()` → `efi_config_parse_tables()` locates the Secure Boot variable service during boot.

**Q5: Can ARM32 boot via UEFI?**
> Yes, though it's rare. Platforms like NXP i.MX8 (ARM64 primarily, but some ARM32 variants) and some development boards support UEFI. The EDK2 firmware project provides an ARM32 UEFI implementation. `arm_efi_init()` in `arch/arm/kernel/efi.c` exists precisely to handle these cases. The code path is identical in concept to ARM64, though ARM32 UEFI is much less common than ARM64 UEFI (servers, Raspberry Pi 4).
