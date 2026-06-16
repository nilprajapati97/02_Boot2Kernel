# arm_efi_init() — ARM32 vs ARM64 Design Details

## 1. EFI Support Prevalence

| | ARM32 | ARM64 |
|--|-------|-------|
| UEFI prevalence | Rare (some development boards, NXP) | Common (servers, RPi4, most modern SBCs) |
| EFI stub | `arch/arm/boot/compressed/efi-header.S` | `arch/arm64/kernel/efi-entry.S` |
| Init function | `arm_efi_init()` in `arch/arm/kernel/efi.c` | `efi_init()` called directly in setup_arch() |
| Primary use case | Embedded development boards | ARM64 servers (Ampere, ThunderX), RPi4 |

---

## 2. ARM32 arm_efi_init()

```c
/* arch/arm/kernel/efi.c */
void __init arm_efi_init(void)
{
    if (!efi_enabled(EFI_BOOT))
        return;

    efi_init();
}
```

ARM32 wraps `efi_init()` in an arch-specific function. The ARM32-specific parts:
- EFI memory map is passed as a physical pointer by the stub
- ARM32 uses 32-bit physical addresses (or 40-bit with LPAE)
- EFI System Table pointer is 32-bit or 64-bit depending on UEFI bitness

### ARM32 EFI Stub Details

```
arch/arm/boot/compressed/efi-header.S:
  .byte 'M','Z'           # PE/COFF magic — UEFI recognizes this as executable
  ...
  .long efi_pe_entry      # Entry point for UEFI loader
```

ARM32 EFI is based on the ARM PE32 executable format. The UEFI firmware for ARM32 is relatively rare and follows the UEFI ARM ABI.

---

## 3. ARM64 EFI Setup

ARM64 calls `efi_init()` directly in `setup_arch()` (no `arm_efi_init()` wrapper):

```c
/* arch/arm64/kernel/setup.c */
void __init setup_arch(char **cmdline_p)
{
    ...
    efi_init();     /* ← directly, no arm_efi_init wrapper */
    ...
}
```

### ARM64 EFI Stub Details

```
arch/arm64/kernel/efi-entry.S:
  /* UEFI entry point */
  efi_pe_entry:
    /* x0 = EFI handle, x1 = EFI System Table pointer */
    bl efi_entry         /* arch/arm64/kernel/efi_stub.S */
```

ARM64 uses the PE32+ (64-bit COFF) format. The UEFI specification has specific ARM64 calling conventions.

### ARM64 UEFI Memory Map — 64-bit Physical Addresses

ARM64 UEFI can describe memory above 4GB:

```c
typedef struct {
    u32 type;
    u64 physical_start;   /* 64-bit — can describe >4GB regions */
    u64 virtual_start;
    u64 num_pages;
    u64 attribute;
} efi_memory_desc_t;
```

ARM32 with LPAE can also use 64-bit physical addresses in `efi_memory_desc_t`, but in practice most ARM32 UEFI implementations limit to 32-bit addresses.

---

## 4. EFI Runtime Services: ARM32 vs ARM64

UEFI Runtime Services are functions in firmware that remain callable after OS boot. ARM32 and ARM64 have different mechanisms for this:

### ARM32 EFI Runtime Services

```
Physical:  EFI runtime code at physical address 0x20000000
           (reserved in EFI memory map as EFI_RUNTIME_SERVICES_CODE)

After paging_init():
  efi_enter_virtual_mode() maps 0x20000000 → kernel VA 0xBF000000 (modules area)
  Runtime service calls jump to 0xBF000000
```

ARM32 must map EFI runtime in a **predictable location** because EFI runtime code uses absolute addresses internally. The modules area is a natural choice.

### ARM64 EFI Runtime Services

```
Physical:  EFI runtime code at any PA (e.g., 0x4000000000)

After paging_init():
  efi_enter_virtual_mode() creates identity mapping or
  maps to dedicated EFI runtime VA region
  Runtime calls use x18 as GOT base (PC-relative addressing)
```

ARM64 EFI can use PC-relative addressing (`adrp` instructions), making runtime code position-independent. No fixed VA required.

---

## 5. Secure Boot Implementation Comparison

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Secure Boot standard | UEFI Secure Boot (if UEFI present) | UEFI Secure Boot (common on servers) |
| grub-efi | grub-efi-arm | grub-efi-arm64 |
| Shim | Rarely used (embedded focus) | Common (Ubuntu, Fedora ARM64) |
| MOK (Machine Owner Keys) | Rarely deployed | Standard on distros |
| Secure Boot enforcement | Via efi_enabled(EFI_SECURE_BOOT) | Same |
| kernel .efi image | vmlinuz.efi (ARM32 PE32) | Image.efi (ARM64 PE32+) |

---

## 6. EFI Memory Map Impact on Zone Setup

### ARM32 (32-bit PA)

```
EFI Memory Map (ARM32 example):
  [0x00000000 - 0x00FFFFFF] EFI_RESERVED_MEMORY_TYPE   → memblock_reserve
  [0x01000000 - 0x0FFFFFFF] EFI_CONVENTIONAL_MEMORY    → available RAM
  [0x1FF00000 - 0x1FFFFFFF] EFI_RUNTIME_SERVICES_DATA  → memblock_reserve
  [0x20000000 - 0x20007FFF] EFI_RUNTIME_SERVICES_CODE  → memblock_reserve

Total available after EFI reservations: ~255MB of conventional memory
DMA zone: up to arm_dma_limit (may be 0x0FFFFFFF if mdesc->dma_zone_size = 256MB)
```

### ARM64 (64-bit PA)

```
EFI Memory Map (ARM64 server example):
  [0x0000000000000000 - 0x0000000040000000] EFI_CONVENTIONAL_MEMORY  (1GB)
  [0x0000000040000000 - 0x000000023FFFFFFF] EFI_CONVENTIONAL_MEMORY  (7GB)
  [0x00000002FFF00000 - 0x00000002FFFFFFFF] EFI_RUNTIME_SERVICES_CODE → reserved
  [0x000000FF00000000 - 0x000000FF003FFFFF] EFI_ACPI_RECLAIM_MEMORY   → reserved until parsed

ARM64 ZONE_DMA:   0 – 1GB
ARM64 ZONE_DMA32: 0 – 4GB
ARM64 ZONE_NORMAL: 4GB – top
```

ARM64's 64-bit PA allows EFI regions to span large physical ranges. No 4GB constraint.

---

## 7. EFI Capsule Updates

UEFI supports **Capsule Updates** — firmware update mechanism where the OS writes a firmware image to a UEFI variable, and the firmware applies it on next reboot. Both ARM32 and ARM64 support this if the UEFI firmware implements it. Linux exposes this via `/sys/firmware/efi/esrt/` (ESRT table) and `fwupdate`/`fwupdmgr` tools.

ARM64 servers (Ampere, ThunderX) use this heavily for firmware updates in data center deployments.

---

## 8. Comparison Table: arm_efi_init() ARM32 vs ARM64

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Function name | `arm_efi_init()` | Direct `efi_init()` in setup_arch |
| PE/COFF format | PE32 (32-bit) | PE32+ (64-bit) |
| Physical address width | 32-bit (or 40-bit LPAE) | 64-bit (48/52-bit PA) |
| UEFI boot prevalence | Rare | Common |
| Runtime services VA | Fixed location (modules area) | Position-independent (PC-relative) |
| Secure Boot | Rarely deployed | Standard on servers/distros |
| EFI_STUB config | `CONFIG_EFI_STUB` | `CONFIG_EFI_STUB` (default y) |
| ACPI support | Limited | Full ACPI support |
| EFI capsule updates | Supported (rarely used) | Common (server firmware updates) |
| SMBIOS/DMI tables | Via EFI config tables | Via EFI config tables |
