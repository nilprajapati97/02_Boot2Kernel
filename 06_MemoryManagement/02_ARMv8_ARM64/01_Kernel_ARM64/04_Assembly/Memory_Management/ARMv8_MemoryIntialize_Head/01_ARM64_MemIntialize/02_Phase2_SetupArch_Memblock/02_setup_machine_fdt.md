# `setup_machine_fdt()` — Device Tree Parsing & Memory Discovery

**Source:** `arch/arm64/kernel/setup.c` (called from `setup_arch()`)
**Phase:** Memblock Era — First memblock operations
**Memory Allocator:** Memblock (first `memblock_add()` calls happen here)
**Called by:** `setup_arch()`
**Calls:** `fixmap_remap_fdt()`, `early_init_dt_scan()`, `memblock_reserve()`

---

## What This Function Does

Parses the **Device Tree Blob (DTB)** to discover physical memory. This is where the kernel first learns **how much RAM exists and where it is located**.

The DTB is a binary data structure passed by the bootloader that describes the hardware. The `/memory` nodes in the DTB tell the kernel about RAM banks.

---

## How It Works With Memory

### Memory Discovery Flow

```
Bootloader provides DTB at physical address (x21 from primary_entry)
         │
         ▼
setup_machine_fdt(__fdt_pointer)
         │
         ├── fixmap_remap_fdt()        Map DTB into kernel VA via fixmap
         │
         ├── early_init_dt_scan()      Parse DTB structure
         │   │
         │   ├── /memory node #1       → memblock_add(0x4000_0000, 0xC000_0000)
         │   ├── /memory node #2       → memblock_add(0x8_8000_0000, 0x8000_0000)
         │   └── ... more banks
         │
         └── memblock_reserve(dt_phys, dt_size)    Reserve DTB itself
```

### Before This Function

| What | Status |
|------|--------|
| `memblock.memory` | Empty (no RAM known) |
| `memblock.reserved` | Empty |
| DTB | Physical address in `__fdt_pointer`, not mapped |

### After This Function

| What | Status |
|------|--------|
| `memblock.memory` | All RAM banks registered |
| `memblock.reserved` | DTB region reserved |
| DTB | Mapped via fixmap, accessible at virtual address |

---

## Step-by-Step Execution

### Step 1: Map DTB via Fixmap

```c
void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
```

**Problem:** The DTB is at a physical address, but the CPU is running with the MMU on. We need a virtual address to read it.

**Solution:** Use the **fixmap** — a set of pre-determined virtual addresses for early boot mappings.

**How fixmap works:**

```
Fixed Virtual Address (compile-time constant):
  FIX_FDT = 0xFFFF_FFFE_FA00_0000 (example, depends on config)

fixmap_remap_fdt():
  1. Calculate which fixmap slot(s) to use
  2. For each 2MB chunk of DTB:
     - Write PTE: fixmap_VA → dt_phys | PAGE_KERNEL (RW, Cacheable)
  3. Flush TLB for the fixmap entries
  4. Return the virtual address
```

**Memory allocated:** Only PTE entries (already pre-allocated in init_pg_dir fixmap region). No memblock allocation.

**Why not use the linear map?**
- The linear map doesn't exist yet! `paging_init()` hasn't run.
- Fixmap provides a pre-arranged virtual address that works before any allocator is ready.

---

### Step 2: Validate DTB

```c
if (!dt_virt || !early_init_dt_verify(dt_virt)) {
    // DTB is invalid — panic
    while (true) cpu_relax();
}
```

**DTB Validation:**
- Check magic number: `0xD00DFEED` (big-endian)
- Verify DTB size is reasonable
- Check structure block alignment

---

### Step 3: Parse DTB — Discover Memory

```c
early_init_dt_scan(dt_virt);
```

This function walks the DTB tree and processes specific nodes:

#### 3a. Parse `/chosen` Node

```
/chosen {
    bootargs = "console=ttyS0,115200 root=/dev/mmcblk0p2";
    linux,initrd-start = <0x48000000>;
    linux,initrd-end   = <0x49000000>;
};
```

- Extracts kernel command line (`bootargs`)
- Records initrd physical address range

#### 3b. Parse `/memory` Nodes — The Key Operation

```
/ {
    memory@40000000 {
        device_type = "memory";
        reg = <0x00 0x40000000 0x00 0xC0000000>;   // Base=1GB, Size=3GB
    };

    memory@880000000 {
        device_type = "memory";
        reg = <0x08 0x80000000 0x00 0x80000000>;   // Base=34GB, Size=2GB
    };
};
```

For each `/memory` node with `device_type = "memory"`:

```c
// Pseudocode of early_init_dt_add_memory_arch()
void early_init_dt_add_memory_arch(u64 base, u64 size)
{
    // Validate
    if (size == 0) return;

    // Alignment to page size
    if (base & ~PAGE_MASK) {
        size -= PAGE_SIZE - (base & ~PAGE_MASK);
        base = PAGE_ALIGN(base);
    }
    size &= PAGE_MASK;  // Round down to page boundary

    // ADD TO MEMBLOCK
    memblock_add(base, size);
}
```

**`memblock_add(base, size)`** registers a physical RAM region:

```
memblock.memory.regions[] BEFORE:
  (empty)

memblock_add(0x4000_0000, 0xC000_0000):
  memblock.memory.regions[0] = { base=0x4000_0000, size=0xC000_0000 }
  memblock.memory.cnt = 1

memblock_add(0x8_8000_0000, 0x8000_0000):
  memblock.memory.regions[1] = { base=0x8_8000_0000, size=0x8000_0000 }
  memblock.memory.cnt = 2
```

**See:** [04_memblock_internals.md](04_memblock_internals.md) for `memblock_add()` algorithm details.

---

### Step 4: Reserve DTB Memory

```c
early_init_dt_reserve_memory();
// or explicitly:
memblock_reserve(__pa(dt_virt), fdt_totalsize(dt_virt));
```

**Why reserve?**
- The DTB must not be overwritten — the kernel reads it throughout boot
- `memblock_reserve()` marks this region as "in use"
- Later allocations from memblock will skip this region

```
memblock.reserved.regions[] AFTER:
  [0] = { base=dt_phys, size=dt_size }
```

---

### Step 5: Machine Name and Model

```c
const char *name = of_flat_dt_get_machine_name();
pr_info("Machine model: %s\n", name);
```

Extracts the top-level `model` or `compatible` property for identification:

```
/ {
    model = "Raspberry Pi 4 Model B Rev 1.4";
    compatible = "raspberrypi,4-model-b", "brcm,bcm2711";
};
```

---

## DTB Memory Layout

```
DTB Structure in Memory:

┌─────────────────────────────────────┐ dt_phys
│ Header (40 bytes)                   │
│   magic: 0xD00DFEED                 │
│   totalsize: N bytes                │
│   off_dt_struct: offset to nodes    │
│   off_dt_strings: offset to strings │
│   off_mem_rsvmap: offset to rsv map │
├─────────────────────────────────────┤
│ Memory Reserve Map                  │
│   (firmware-reserved regions)       │
├─────────────────────────────────────┤
│ Structure Block                     │
│   FDT_BEGIN_NODE "/"                │
│   ├── "memory@40000000"            │
│   │     reg = <base, size>          │
│   ├── "chosen"                     │
│   │     bootargs = "..."            │
│   └── ...                          │
├─────────────────────────────────────┤
│ Strings Block                       │
│   "device_type\0"                   │
│   "reg\0"                           │
│   "bootargs\0"                      │
│   ...                               │
└─────────────────────────────────────┘ dt_phys + totalsize
```

---

## Example: Complete Memory Discovery

For a board with 4GB RAM split across two banks:

```
DTB:
  /memory@40000000 { reg = <0x0 0x40000000  0x0 0x80000000>; }  // 2GB @ 1GB
  /memory@100000000 { reg = <0x1 0x00000000  0x0 0x80000000>; } // 2GB @ 4GB

After setup_machine_fdt():

memblock.memory:
  ┌─────────────────────────────────────────────────────┐
  │ Region 0: [0x0_4000_0000 — 0x0_C000_0000) = 2 GB   │
  │ Region 1: [0x1_0000_0000 — 0x1_8000_0000) = 2 GB   │
  └─────────────────────────────────────────────────────┘
  Total: 4 GB

memblock.reserved:
  ┌─────────────────────────────────────────────────────┐
  │ Region 0: [dt_phys — dt_phys + dt_size)  = DTB      │
  └─────────────────────────────────────────────────────┘
```

---

## Key Takeaways

1. **DTB is the source of truth** — without it, the kernel has no idea how much RAM exists
2. **Fixmap is the first mapping tool** — enables reading DTB before the linear map exists
3. **`memblock_add()` is the first memory registration** — from DTB `/memory` nodes
4. **DTB must be reserved** — otherwise memblock might allocate from that region and corrupt it
5. **Memory discovery is passive** — the kernel doesn't probe RAM; it trusts the DTB/bootloader
6. **Multiple memory banks** — ARM64 systems often have non-contiguous RAM (multiple `/memory` nodes)
