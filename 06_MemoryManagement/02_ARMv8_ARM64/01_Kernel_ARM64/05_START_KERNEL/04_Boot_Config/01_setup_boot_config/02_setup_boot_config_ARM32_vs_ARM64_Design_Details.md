# setup_boot_config() — ARM32 vs ARM64 Architecture Design Details
## Source: init/main.c | Kernel: Linux ARM/ARM64

---

## Table of Contents

1. [Architecture Overview: How Bootloader Hands Off to Kernel](#1-architecture-overview-how-bootloader-hands-off-to-kernel)
2. [initrd_start / initrd_end: How They Are Set on ARM32 vs ARM64](#2-initrd_start--initrd_end-how-they-are-set-on-arm32-vs-arm64)
3. [Physical Memory Map Differences](#3-physical-memory-map-differences)
4. [Bootloader Differences: U-Boot (ARM32) vs UEFI/U-Boot (ARM64)](#4-bootloader-differences-u-boot-arm32-vs-uefiuboot-arm64)
5. [Device Tree / FDT: chosen node and initrd binding](#5-device-tree--fdt-chosen-node-and-initrd-binding)
6. [COMMAND_LINE_SIZE Differences](#6-command_line_size-differences)
7. [Virtual Address Space Differences](#7-virtual-address-space-differences)
8. [memblock_alloc() Behavior: ARM32 vs ARM64](#8-memblock_alloc-behavior-arm32-vs-arm64)
9. [CONFIG_BLK_DEV_INITRD: Compile-Time Gate](#9-config_blk_dev_initrd-compile-time-gate)
10. [CONFIG_BOOT_CONFIG_EMBED on ARM Platforms](#10-config_boot_config_embed-on-arm-platforms)
11. [GRUB on ARM64 UEFI: The 4-Byte Alignment Issue](#11-grub-on-arm64-uefi-the-4-byte-alignment-issue)
12. [KASLR (Kernel Address Space Layout Randomization) Impact](#12-kaslr-kernel-address-space-layout-randomization-impact)
13. [XIP (Execute-In-Place) ARM32 Systems: Edge Case](#13-xip-execute-in-place-arm32-systems-edge-case)
14. [Endianness: LE vs BE ARM](#14-endianness-le-vs-be-arm)
15. [Comparison Table: ARM32 vs ARM64 for setup_boot_config()](#15-comparison-table-arm32-vs-arm64-for-setup_boot_config)
16. [boot_command_line Population Path per Architecture](#16-boot_command_line-population-path-per-architecture)

---

## 1. Architecture Overview: How Bootloader Hands Off to Kernel

### ARM32 (32-bit ARMv7 and earlier)

On ARM32, the kernel entry convention (defined in
`arch/arm/kernel/head.S`) requires:

```
CPU registers at kernel entry:
  r0 = 0 (machine type code is deprecated in DT-based systems)
  r1 = machine type number OR 0xFFFFFFFF for DT-only
  r2 = physical address of ATAGs list OR Device Tree Binary (DTB)
  pc = kernel physical entry point
  MMU = OFF, caches = OFF, IRQs = OFF
```

The bootloader (typically U-Boot) places the kernel, DTB, and initrd
in RAM and jumps to the kernel entry address.

### ARM64 (64-bit ARMv8-A and later)

On ARM64, the kernel entry convention (`arch/arm64/kernel/head.S`) requires:

```
CPU registers at kernel entry:
  x0 = physical address of Device Tree Binary (DTB)
  x1 = 0 (reserved)
  x2 = 0 (reserved)
  x3 = 0 (reserved)
  pc = kernel physical entry point
  MMU = OFF, caches = OFF, IRQs = OFF, EL1 or EL2
```

Note: ARM64 has **no ATAGs support**. DTB is mandatory.
U-Boot and UEFI (via GRUB/systemd-boot) both pass the DTB address in `x0`.

### What This Means for setup_boot_config()

Both architectures must have `initrd_start` and `initrd_end` populated
**before** `setup_boot_config()` is called. These are populated during
`setup_arch()`:

- ARM32: `arch/arm/kernel/setup.c` → `parse_tags()` or `setup_machine_fdt()`
- ARM64: `arch/arm64/kernel/setup.c` → `setup_machine_fdt()` → `early_init_dt_scan()`

---

## 2. initrd_start / initrd_end: How They Are Set on ARM32 vs ARM64

### ARM32 Path (ATAGs)

When booting with ATAGs (legacy, non-DT):
```c
// arch/arm/kernel/atags_parse.c
static int __init parse_tag_initrd2(const struct tag *tag)
{
    phys_addr_t start = tag->u.initrd.start;
    phys_addr_t size  = tag->u.initrd.size;
    initrd_start = __phys_to_virt(start);
    initrd_end   = initrd_start + size;
    return 0;
}
```

### ARM32 Path (Device Tree)

```c
// drivers/of/fdt.c → early_init_dt_scan_chosen()
// reads /chosen/linux,initrd-start and /chosen/linux,initrd-end
// from DTB, stores in initrd_start, initrd_end
```

### ARM64 Path (Device Tree — mandatory)

```c
// arch/arm64/kernel/setup.c
void __init setup_arch(char **cmdline_p)
{
    ...
    early_init_dt_scan(phys_to_virt(params));
    // This calls early_init_dt_scan_chosen() which sets:
    //   boot_command_line[]
    //   initrd_start, initrd_end
    ...
}
```

### DTB /chosen Node Format

```dts
/ {
    chosen {
        bootargs = "console=ttyAMA0 root=/dev/sda1";
        linux,initrd-start = <0x0 0x84000000>;   /* 64-bit on ARM64 */
        linux,initrd-end   = <0x0 0x85000000>;   /* 64-bit on ARM64 */
    };
};
```

On ARM32, `linux,initrd-start/end` can be 32-bit cells.
On ARM64, they are typically 64-bit (two 32-bit cells = `<high low>`).

The helper `early_init_dt_scan_chosen()` in `drivers/of/fdt.c` reads both:
```c
prop = of_get_flat_dt_prop(node, "linux,initrd-start", &len);
if (prop) {
    phys_addr_t start = of_read_number(prop, len/4);
    initrd_start = (unsigned long)__va(start);
}
prop = of_get_flat_dt_prop(node, "linux,initrd-end", &len);
if (prop) {
    phys_addr_t end = of_read_number(prop, len/4);
    initrd_end = (unsigned long)__va(end);
}
```

`__va()` converts physical to virtual addresses. The virtual mapping is
established by the early page tables set up in `head.S` before
`start_kernel()` is entered.

### Crucially: initrd_end Is a Virtual Address by the Time setup_boot_config() Runs

```c
static void * __init get_boot_config_from_initrd(size_t *_size)
{
    data = (char *)initrd_end - BOOTCONFIG_MAGIC_LEN;  // virtual address
    ...
    data = ((void *)hdr) - size;    // arithmetic on virtual addresses
    initrd_end = (unsigned long)data;   // shrink (virtual)
```

The kernel accesses initrd memory through virtual addresses. The early
identity mapping or linear mapping (`PAGE_OFFSET + phys`) makes this work
on both architectures.

---

## 3. Physical Memory Map Differences

### Typical ARM32 System (e.g., Cortex-A9, 1 GB RAM)

```
Physical Address Space (32-bit, 4 GB max):
┌────────────────────────────────────┐  0x0000_0000
│ SoC peripheral registers           │
├────────────────────────────────────┤  0x1000_0000 (varies per SoC)
│ DRAM start                         │
├────────────────────────────────────┤  0x1000_8000  ← typical kernel load
│ Kernel image (zImage/uImage)       │
├────────────────────────────────────┤
│ DTB (flattened device tree)        │
├────────────────────────────────────┤
│ initrd / initramfs                 │  ← initrd_start (phys)
│   [cpio archive]                   │
│   [bootconfig trailer]             │
│                                    │  ← initrd_end   (phys)
├────────────────────────────────────┤
│ Free RAM                           │
└────────────────────────────────────┘  0x5000_0000 (1 GB boundary)
```

### Typical ARM64 System (e.g., Cortex-A53/A72, 4 GB or more RAM)

```
Physical Address Space (48-bit IPA, system-dependent):
┌────────────────────────────────────┐  0x0000_0000_0000_0000
│ (0 to DRAM base varies)            │
├────────────────────────────────────┤  0x0000_0000_4000_0000  (ARM64 DRAM often at 1 GB)
│ DRAM start                         │
├────────────────────────────────────┤  0x0000_0000_4008_0000  ← kernel default TEXT_OFFSET
│ Kernel Image (flat binary)         │
├────────────────────────────────────┤
│ DTB                                │
├────────────────────────────────────┤
│ initrd / initramfs                 │  ← initrd_start (phys)
│   [cpio archive]                   │
│   [bootconfig trailer]             │
│                                    │  ← initrd_end   (phys)
├────────────────────────────────────┤
│ Free RAM (potentially GBs)         │
└────────────────────────────────────┘

With KASLR: kernel image loaded at randomized offset within DRAM
```

### Impact on get_boot_config_from_initrd()

The function uses `(unsigned long)data < initrd_start` to detect
if the bootconfig size field claims a larger extent than the initrd:

```c
if ((unsigned long)data < initrd_start) {
    pr_err("bootconfig size %d is greater than initrd size %ld\n",
           size, initrd_end - initrd_start);
    return NULL;
}
```

On ARM32: `initrd_start` is a 32-bit virtual address (e.g., `0xC200_0000`).
On ARM64: `initrd_start` is a 64-bit virtual address (e.g., `0xFFFF_0000_8400_0000`).

The comparison `(unsigned long)data < initrd_start` works correctly on
both because `unsigned long` is 32-bit on ARM32 and 64-bit on ARM64,
matching the pointer width.

---

## 4. Bootloader Differences: U-Boot (ARM32) vs UEFI/U-Boot (ARM64)

### ARM32: U-Boot (Dominant Bootloader)

U-Boot for ARM32 uses commands like:
```sh
# U-Boot script
setenv bootargs "console=ttyS0 root=/dev/mmcblk0p2 bootconfig"
fatload mmc 0:1 0x10008000 uImage          # kernel
fatload mmc 0:1 0x10800000 initrd.img      # initrd with bootconfig appended
fatload mmc 0:1 0x10000100 board.dtb       # DTB
bootm 0x10008000 0x10800000 0x10000100
```

The `bootm` command:
1. Decompresses kernel if needed
2. Sets ATAG_INITRD2 or DTB `/chosen/linux,initrd-start/end`
3. Sets up `r2` = DTB physical address
4. Jumps to kernel

### ARM64: U-Boot (DT-based) and UEFI + GRUB2

**Path 1: U-Boot (direct)**
```sh
# U-Boot for ARM64
setenv bootargs "console=ttyAMA0 earlycon root=/dev/sda1 bootconfig"
fatload mmc 0:1 $kernel_addr_r Image        # flat binary
fatload mmc 0:1 $ramdisk_addr_r initrd.img  # initrd
fatload mmc 0:1 $fdt_addr_r     board.dtb   # DTB
booti $kernel_addr_r $ramdisk_addr_r:$filesize $fdt_addr_r
```

`booti` (boot ARM64 Image):
- Does NOT decompress (ARM64 Image is uncompressed by default)
- Updates DTB `/chosen/linux,initrd-start` and `linux,initrd-end`
- Passes DTB physical address in `x0`

**Path 2: UEFI + GRUB2 (e.g., Raspberry Pi 4, Ampere systems, cloud VMs)**
```
UEFI Firmware
  └── GRUB2 EFI binary (grubaa64.efi)
       └── grub.cfg:
             linux  /EFI/vmlinuz console=ttyAMA0 bootconfig
             initrd /EFI/initrd.img
             boot
```

GRUB2 calls UEFI `LoadFile2` protocol to load the initrd. The kernel EFI stub
(`arch/arm64/kernel/efi-entry.S`, `drivers/firmware/efi/libstub/`) processes
this, setting `initrd_start` and `initrd_end` from the EFI memory map.

**Critical GRUB2 behavior that affects setup_boot_config():**
GRUB2 may pad the initrd to a 4-byte boundary before passing it to the kernel.
This is why the code searches for `BOOTCONFIG_MAGIC` in a 4-byte window:
```c
for (i = 0; i < 4; i++) {
    if (!memcmp(data, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN))
        goto found;
    data--;
}
```
This issue is primarily observed on **ARM64 UEFI/GRUB2** systems, not ARM32/U-Boot.

---

## 5. Device Tree / FDT: chosen node and initrd binding

### ARM32 DTB chosen node (32-bit cells)

```dts
/ {
    #address-cells = <1>;
    #size-cells = <1>;

    chosen {
        bootargs = "console=ttyS0,115200 root=/dev/mmcblk0p2 bootconfig";
        linux,initrd-start = <0x18000000>;    /* 32-bit physical address */
        linux,initrd-end   = <0x18800000>;
    };
};
```

### ARM64 DTB chosen node (64-bit cells, two 32-bit words)

```dts
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    chosen {
        bootargs = "console=ttyAMA0,115200 root=/dev/sda1 bootconfig";
        linux,initrd-start = <0x00000000 0x84000000>;  /* hi, lo = 0x84000000 */
        linux,initrd-end   = <0x00000000 0x85000000>;  /* hi, lo = 0x85000000 */
    };
};
```

### How early_init_dt_scan_chosen() Reads These

```c
/* drivers/of/fdt.c */
int __init early_init_dt_scan_chosen(char *cmdline)
{
    ...
    /* initrd */
    prop = of_get_flat_dt_prop(node, "linux,initrd-start", &l);
    if (prop) {
        phys_addr_t start = of_read_number(prop, OF_ROOT_NODE_ADDR_CELLS_DEFAULT);
        initrd_start = (unsigned long)__va(start);
    }
    prop = of_get_flat_dt_prop(node, "linux,initrd-end", &l);
    if (prop) {
        phys_addr_t end = of_read_number(prop, OF_ROOT_NODE_ADDR_CELLS_DEFAULT);
        initrd_end = (unsigned long)__va(end);
    }
    ...
}
```

`of_read_number()` handles both 32-bit and 64-bit cell widths transparently.
On ARM64, it reads two cells and combines them into a 64-bit physical address.

---

## 6. COMMAND_LINE_SIZE Differences

### ARM32

```c
/* arch/arm/include/asm/setup.h */
#define COMMAND_LINE_SIZE 1024
```
Some ARM32 SoC configs override this to 2048.

### ARM64

```c
/* arch/arm64/include/asm/setup.h */
#define COMMAND_LINE_SIZE 2048
```

Some ARM64 server configs (ACPI-based, many PCI devices) use 4096.

### Why This Matters for Bootconfig

The motivating use case for bootconfig is **exactly this limit**.
On ARM32 with 1024-byte cmdline, complex embedded systems quickly hit the limit.
Bootconfig allows the kernel config to be moved to the initrd trailer, leaving
the cmdline for only the essential `bootconfig` activation token:

```
cmdline: "console=ttyS0 root=/dev/mmcblk0p2 bootconfig"
         ← only 52 bytes, well under 1024 limit
```

All the detailed configuration (hundreds of bytes) lives in the bootconfig
data appended to the initrd.

The `tmp_cmdline` buffer in `setup_boot_config()` is sized to `COMMAND_LINE_SIZE`:
```c
static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;
strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
```
This is architecture-specific — 1024 on ARM32, 2048 on ARM64.

---

## 7. Virtual Address Space Differences

### ARM32 Virtual Memory Layout (3G/1G split, common config)

```
0x0000_0000 ─── 0xBFFF_FFFF  : User space (3 GB)
0xC000_0000 ─── 0xFFFF_FFFF  : Kernel space (1 GB)
  0xC000_0000 = PAGE_OFFSET (PHYS_OFFSET mapped here)
  0xC000_0000 + RAM_size = end of linear map

initrd at phys 0x1800_0000 → virt 0xD800_0000 (offset by PHYS_OFFSET=0xC000_0000)
initrd_start = 0xD800_0000
initrd_end   = 0xD880_0000
```

### ARM64 Virtual Memory Layout (48-bit VA, 4 KB pages)

```
0x0000_0000_0000_0000 ─── 0x0000_FFFF_FFFF_FFFF  : User space
                           (gap — unmapped)
0xFFFF_0000_0000_0000 ─── 0xFFFF_FFFF_FFFF_FFFF  : Kernel space
  0xFFFF_0000_0000_0000 = PAGE_OFFSET (linear map starts)
  vmalloc region, modules region, etc.

initrd at phys 0x0000_0000_8400_0000
  → virt 0xFFFF_0000_8400_0000 (PAGE_OFFSET + phys)
initrd_start = 0xFFFF_0000_8400_0000
initrd_end   = 0xFFFF_0000_8500_0000
```

### Impact on Pointer Arithmetic in get_boot_config_from_initrd()

```c
data = (char *)initrd_end - BOOTCONFIG_MAGIC_LEN;
```

On ARM32: `char *` is a 32-bit pointer. `initrd_end` is `unsigned long` (32-bit).
On ARM64: `char *` is a 64-bit pointer. `initrd_end` is `unsigned long` (64-bit).

The cast `(char *)initrd_end` is valid on both — `unsigned long` always matches
pointer width on Linux (guaranteed by ABI).

```c
hdr = (u32 *)(data - 8);
```

Reads the 8-byte header [size, csum] just before the magic. On ARM64, this
virtual address could be `0xFFFF_0000_84FF_FFF4` — a perfectly valid 64-bit
kernel virtual address.

---

## 8. memblock_alloc() Behavior: ARM32 vs ARM64

`setup_boot_config()` indirectly causes `memblock_alloc()` calls through:
- `xbc_init()` — allocates `xbc_data` and `xbc_nodes`
- `xbc_make_cmdline()` — allocates `extra_command_line` and `extra_init_args`

### ARM32 memblock

```c
/* mm/memblock.c */
phys_addr_t __init memblock_alloc_range_nid(...)
{
    // Searches free regions in memblock.memory[]
    // Returns highest available address (top-down allocation by default)
    // On ARM32: returns 32-bit physical address
}
```

ARM32 memblock region addresses are 32-bit `phys_addr_t`. The returned pointer
from `memblock_alloc()` is in the linear kernel virtual map (`phys_to_virt()`).

### ARM64 memblock

Same code, but `phys_addr_t` is 64-bit. The linear map on ARM64 covers the full
physical memory range. Allocations can be above 4 GB (unlike ARM32 where all
physical memory accessible to early boot code is typically below 4 GB).

### Constraints Applied by xbc_alloc_mem()

```c
/* lib/bootconfig.c (kernel path) */
static inline void * __init xbc_alloc_mem(size_t size)
{
    return memblock_alloc(size, SMP_CACHE_BYTES);
}
```

`SMP_CACHE_BYTES`:
- ARM32: typically 32 bytes (L1 cache line = 32 bytes)
- ARM64: typically 64 bytes (L1 cache line = 64 bytes)

The `xbc_nodes` array (8192 * 8 = 64 KB) will be 64-byte aligned on ARM64,
which ensures no false cache line sharing during the parse phase.

---

## 9. CONFIG_BLK_DEV_INITRD: Compile-Time Gate

```c
#ifdef CONFIG_BLK_DEV_INITRD
static void * __init get_boot_config_from_initrd(size_t *_size)
{
    // Full implementation
}
#else
static void * __init get_boot_config_from_initrd(size_t *_size)
{
    return NULL;  // stub
}
#endif
```

### ARM32 Systems Without initrd

Some deeply embedded ARM32 systems boot from:
- XIP (Execute-In-Place) NOR flash with built-in rootfs
- NAND with UBIFS mounted directly
- MMC without initrd

These systems set `CONFIG_BLK_DEV_INITRD=n`. In this case,
`get_boot_config_from_initrd()` always returns NULL.

For these systems, bootconfig can still work via `CONFIG_BOOT_CONFIG_EMBED=y`,
which bakes the bootconfig into the kernel image at build time.

### ARM64 Server/Desktop Systems

ARM64 systems (servers, phones, SBCs) almost universally use initrd/initramfs.
`CONFIG_BLK_DEV_INITRD=y` is standard. The full `get_boot_config_from_initrd()`
implementation is always compiled in.

---

## 10. CONFIG_BOOT_CONFIG_EMBED on ARM Platforms

### Use Cases

| Platform Type | Typical Config | Reason |
|--------------|---------------|--------|
| ARM32 deeply embedded (no initrd) | `EMBED=y` | No initrd available |
| ARM32 factory/manufacturing | `EMBED=y` | Fixed config per production batch |
| ARM64 server with initrd | `EMBED=n` | initrd-based bootconfig is flexible |
| ARM64 phone (Android GKI) | `EMBED=n` | Vendor bootconfig in vendor_boot.img |

### Android ARM64: vendor_boot.img Bootconfig

Android 12+ uses a special bootconfig section in `vendor_boot.img`:
```
vendor_boot.img structure:
  [vendor ramdisk]
  [bootconfig data]      ← treated like initrd trailer
  [vendor_boot header]   ← references the bootconfig offset
```

The Android boot loader sets up `initrd_start`/`initrd_end` to include this
region, allowing `get_boot_config_from_initrd()` to find and parse it.

---

## 11. GRUB on ARM64 UEFI: The 4-Byte Alignment Issue

### The Problem

When GRUB2 loads an initrd via UEFI `LoadFile2`:
1. GRUB reads the initrd file size from the filesystem
2. GRUB rounds up the allocation size to a 4-byte boundary
3. If the initrd file size is not 4-byte aligned, GRUB adds 1-3 padding bytes
4. These padding bytes appear **after** the `#BOOTCONFIG\n` magic

Example:
```
initrd file on disk: 10,485,789 bytes (not 4-byte aligned)
GRUB-allocated size: 10,485,792 bytes (rounded up to nearest 4)
Padding added:       3 zero bytes

initrd_end (as set by EFI stub) = initrd_start + 10,485,792
Actual BOOTCONFIG\n magic at:    initrd_start + 10,485,789 - 12
initrd_end - BOOTCONFIG_MAGIC_LEN = initrd_start + 10,485,780  ← 9 bytes too far
```

Without the 4-iteration search loop, `memcmp` would check the wrong location
and return NULL, silently ignoring the bootconfig.

### ARM32 / U-Boot: Not Affected

U-Boot `bootm` command uses the exact `filesize` from the ATAG or DTB property.
No alignment padding is applied. The single `memcmp` at `initrd_end - 12`
would work without the loop. The loop is harmless (just performs 3 extra
comparisons on adjacent bytes).

---

## 12. KASLR (Kernel Address Space Layout Randomization) Impact

### ARM64 KASLR (CONFIG_RANDOMIZE_BASE)

ARM64 supports KASLR — the kernel image is loaded at a randomized physical
address within DRAM at boot time. The randomization is applied by the EFI stub
or by U-Boot before jumping to the kernel.

**Does KASLR affect setup_boot_config()?**

No — `setup_boot_config()` does not reference any kernel image addresses.
It only accesses:
- `initrd_start`, `initrd_end` (set from DTB/UEFI, not kernel image addresses)
- `boot_command_line[]` (a static `.init.data` array, KASLR randomizes all of kernel together)
- `memblock` allocations (from DRAM, unrelated to kernel image position)

KASLR randomizes the kernel `.text` / `.data` / `.bss` placement. The initrd
is placed by the bootloader independently. The virtual address of `initrd_start`
after `__va()` conversion is also unaffected by kernel KASLR because the linear
map offset (`PAGE_OFFSET`) shifts with the kernel, and `__va()` uses the same offset.

### ARM32 KASLR

ARM32 has limited KASLR support (`CONFIG_RANDOMIZE_BASE` on some SoCs).
Same conclusion: `setup_boot_config()` is unaffected.

---

## 13. XIP (Execute-In-Place) ARM32 Systems: Edge Case

On XIP ARM32 systems (kernel runs directly from NOR flash):
- There is typically no initrd (NOR flash contains rootfs directly)
- `CONFIG_BLK_DEV_INITRD` may be `n`
- `initrd_start` = 0, `initrd_end` = 0

`get_boot_config_from_initrd()` exits immediately:
```c
if (!initrd_end)
    return NULL;
```

If bootconfig is needed on XIP systems, `CONFIG_BOOT_CONFIG_EMBED=y` is used.
The embedded bootconfig is linked into the kernel image itself (in NOR flash),
accessible at `embedded_bootconfig_data[]`.

---

## 14. Endianness: LE vs BE ARM

### ARM Big-Endian (ARMBE, rare but exists)

Some network/telecoms ARM32 systems run in big-endian mode.
The bootconfig trailer uses **little-endian** u32 values explicitly:
```c
hdr = (u32 *)(data - 8);
size = le32_to_cpu(hdr[0]);    // le32_to_cpu handles BE swap
csum = le32_to_cpu(hdr[1]);    // le32_to_cpu handles BE swap
```

`le32_to_cpu()` is a no-op on LE ARM and performs byte-swap on BE ARM.
The bootconfig tool (`tools/bootconfig/bootconfig.c`) always writes LE.
This ensures cross-platform compatibility.

### ARM64 Endianness

ARM64 can run in LE or BE mode. The Linux kernel on ARM64 defaults to LE.
BE8 ARM64 kernels exist for networking SoCs (e.g., some Marvell OcteonTX).
The same `le32_to_cpu()` pattern handles this correctly.

---

## 15. Comparison Table: ARM32 vs ARM64 for setup_boot_config()

| Aspect | ARM32 | ARM64 |
|--------|-------|-------|
| **Pointer size** | 32-bit | 64-bit |
| **`unsigned long` size** | 32-bit | 64-bit |
| **`initrd_start/end` type** | `unsigned long` (32-bit virt addr) | `unsigned long` (64-bit virt addr) |
| **COMMAND_LINE_SIZE** | 1024 or 2048 | 2048 or 4096 |
| **Bootloader** | U-Boot (dominant) | U-Boot or UEFI+GRUB2 |
| **initrd setup** | ATAGs or DTB /chosen | DTB /chosen (mandatory) or UEFI |
| **GRUB 4-byte alignment** | Rare / not seen | Common (UEFI GRUB2) |
| **KASLR** | Limited support | Full support (not affecting bootconfig) |
| **SMP_CACHE_BYTES** | 32 bytes | 64 bytes |
| **XIP possible?** | Yes (NOR flash) | Extremely rare |
| **CONFIG_BOOT_CONFIG_EMBED use** | Factory/XIP embedded | Rare; initrd preferred |
| **Android bootconfig** | Rare | Yes (vendor_boot.img) |
| **memblock phys_addr_t** | 32-bit | 64-bit |
| **Endianness** | LE (default), BE possible | LE (default), BE possible |
| **DTB initrd cell width** | 1 cell (32-bit) | 2 cells (64-bit) |
| **`tmp_cmdline` buffer size** | 1024 or 2048 bytes | 2048 or 4096 bytes |

---

## 16. boot_command_line Population Path per Architecture

### ARM32

```
U-Boot sets ATAG_CMDLINE or DTB /chosen/bootargs
       │
       ▼
arch/arm/kernel/setup.c: setup_arch()
  └── setup_machine_fdt() or parse_tags()
       └── early_init_dt_scan_chosen() or parse_tag_cmdline()
            └── strlcpy(boot_command_line, cmdline, COMMAND_LINE_SIZE)
       │
       ▼
start_kernel()
  └── setup_boot_config()
       └── strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE)
            └── parse_args() → scans for "bootconfig" token
```

### ARM64

```
UEFI/U-Boot passes DTB physical address in x0
       │
       ▼
arch/arm64/kernel/setup.c: setup_arch()
  └── setup_machine_fdt(__phys_to_virt(dtb_phys))
       └── early_init_dt_scan()
            └── early_init_dt_scan_chosen()
                 └── strlcpy(boot_command_line, p, COMMAND_LINE_SIZE)
       │
       ▼
start_kernel()
  └── setup_boot_config()
       └── strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE)
            └── parse_args() → scans for "bootconfig" token
```

The `setup_boot_config()` function itself is **architecture-neutral C code**.
All architecture differences are resolved before `setup_boot_config()` is entered,
specifically in `setup_arch()` which populates `initrd_start`, `initrd_end`,
and `boot_command_line` in architecture-specific ways.

---

*Document End*
*Source reference: init/main.c, arch/arm/kernel/setup.c, arch/arm64/kernel/setup.c*
*drivers/of/fdt.c, lib/bootconfig.c, include/linux/bootconfig.h*
*Kernel version: Linux 6.x*
