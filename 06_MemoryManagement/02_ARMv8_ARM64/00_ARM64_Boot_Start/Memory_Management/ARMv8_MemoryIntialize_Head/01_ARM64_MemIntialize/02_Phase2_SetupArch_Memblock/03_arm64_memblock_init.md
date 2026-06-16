# `arm64_memblock_init()` — Memblock Trimming & Reservations

**Source:** `arch/arm64/mm/init.c` lines 170–260
**Phase:** Memblock Era
**Memory Allocator:** Memblock (configured but not yet used for allocation)
**Called by:** `setup_arch()`
**Calls:** `memblock_remove()`, `memblock_reserve()`, `memblock_mem_limit_remove_map()`, `early_init_fdt_scan_reserved_mem()`

---

## What This Function Does

After `setup_machine_fdt()` registers all RAM banks with memblock, this function **trims and adjusts** the memblock configuration:

1. Removes memory above hardware physical address limits
2. Calculates `memstart_addr` — the base of the linear map
3. Removes memory that doesn't fit in the kernel's virtual address space
4. Applies `mem=` command line limits
5. Reserves the kernel image, initrd, and DTB reserved-memory nodes

---

## How It Works With Memory

### Memblock State Transitions

```
BEFORE arm64_memblock_init():
  memblock.memory = [all RAM from DTB]     ← May include unmappable regions
  memblock.reserved = [DTB]

AFTER arm64_memblock_init():
  memblock.memory = [only mappable RAM]    ← Trimmed to fit VA/PA limits
  memblock.reserved = [DTB, kernel, initrd, reserved-mem, ...]
```

---

## Step-by-Step Execution

### Step 1: Calculate Linear Region Size

```c
s64 linear_region_size = PAGE_END - _PAGE_OFFSET(vabits_actual);
```

The **linear region** is the portion of kernel VA space dedicated to the direct/linear map of physical RAM.

| VA Config | `PAGE_OFFSET` | `PAGE_END` | Linear Region Size |
|-----------|--------------|-----------|-------------------|
| 48-bit | `0xFFFF_0000_0000_0000` | `0xFFFF_8000_0000_0000` | 128 TB |
| 52-bit | `0xFFF0_0000_0000_0000` | `0xFFFF_8000_0000_0000` | ~4 PB |

**Why it matters:** Physical RAM that exceeds this size cannot be mapped in the linear map and must be removed from memblock.

---

### Step 2: Remove Memory Above PA Limit

```c
memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);
```

**`PHYS_MASK_SHIFT`** = maximum physical address bits supported by the CPU (detected from `ID_AA64MMFR0_EL1.PARange`).

| CPU PA Bits | `PHYS_MASK_SHIFT` | Max Physical Address |
|------------|-------------------|---------------------|
| 48-bit | 48 | 256 TB |
| 52-bit | 52 | 4 PB |

This removes any DTB-reported memory above what the CPU can physically address. (Rare, but possible with incorrect DTBs.)

---

### Step 3: Calculate `memstart_addr` — Linear Map Base

```c
memstart_addr = round_down(memblock_start_of_DRAM(), ARM64_MEMSTART_ALIGN);
```

**`memstart_addr`** is the most important variable in the linear map. It defines the mapping formula:

```
virtual_address = physical_address - memstart_addr + PAGE_OFFSET
physical_address = virtual_address - PAGE_OFFSET + memstart_addr
```

**Alignment requirement** (`ARM64_MEMSTART_ALIGN`):

| Page Size | Alignment | Why |
|-----------|-----------|-----|
| 4 KB | 1 GB (`PUD_SIZE`) | Enables 1GB PUD block mappings in the linear map |
| 16 KB | 32 MB (`CONT_PMD_SIZE`) | Contiguous PMD entries for TLB efficiency |
| 64 KB | 512 MB (`PMD_SIZE`) | PMD block mapping size |

**Example:**
```
memblock_start_of_DRAM() = 0x4020_0000  (first byte of RAM from DTB)
ARM64_MEMSTART_ALIGN     = 0x4000_0000  (1 GB for 4K pages)

memstart_addr = round_down(0x4020_0000, 0x4000_0000) = 0x4000_0000

Linear map formula:
  VA 0xFFFF_0000_0000_0000  →  PA 0x4000_0000
  VA 0xFFFF_0000_0000_1000  →  PA 0x4000_1000
  ...
```

---

### Step 4: Check Linear Map Capacity

```c
if ((memblock_end_of_DRAM() - memstart_addr) > linear_region_size)
    pr_warn("Memory doesn't fit in the linear mapping, "
            "some will be excluded\n");
```

If total physical RAM exceeds the linear region size (128TB for 48-bit VA), some memory must be excluded.

---

### Step 5: Trim Memory That Doesn't Fit

```c
// Remove memory above the linear map limit
memblock_remove(max_t(u64, memstart_addr + linear_region_size,
                      __pa_symbol(_end)),
                ULLONG_MAX);

// Remove memory below memstart_addr that can't be mapped
if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
    memblock_remove(0, memstart_addr);
}
```

**What gets removed:**

```
Physical Address Space:
┌──────────────────────────────────────────────────────────┐
│  REMOVED                │ KEPT (mappable)    │ REMOVED   │
│  (below memstart_addr)  │                    │ (above    │
│                         │                    │  limit)   │
├─────────────────────────┼────────────────────┼───────────┤
0                    memstart_addr      memstart_addr     ∞
                                        + linear_region_size
```

---

### Step 6: Apply `mem=` Boot Parameter Limit

```c
if (memory_limit != PHYS_ADDR_MAX) {
    memblock_mem_limit_remove_map(memory_limit);
    memblock_add(__pa_symbol(_text), (u64)(_end - _text));
}
```

If the user specified `mem=512M` on the kernel command line:
1. `memblock_mem_limit_remove_map(512M)` removes all memory above 512MB
2. Re-adds the kernel image region (it must always be accessible regardless of `mem=`)

**Use case:** Testing with less RAM, or working around hardware bugs.

---

### Step 7: Handle 52-bit VA Fallback

```c
if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52)) {
    // CPU doesn't support 52-bit VA, adjust memstart_addr
    memstart_addr -= _PAGE_OFFSET(48) - _PAGE_OFFSET(52);
}
```

If the kernel was compiled for 52-bit VA but the CPU only supports 48-bit, the linear map base must be adjusted so all RAM fits within the smaller VA range.

---

### Step 8: Reserve Initrd

```c
if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
    // Validate initrd is within linear map
    u64 base = phys_initrd_start;
    u64 size = phys_initrd_size;

    if (base + size > memstart_addr + linear_region_size) {
        pr_err("initrd outside linear mapping, disabling\n");
        phys_initrd_size = 0;
    } else {
        memblock_reserve(base, size);
        memblock_mark_nomap(base, size);  // Don't include in linear map
    }
}
```

The **initrd** (initial ramdisk) contains the initial root filesystem. It was loaded by the bootloader and must be preserved.

```
memblock.reserved AFTER:
  [0] = DTB region
  [1] = initrd region   ← NEW
```

---

### Step 9: Reserve Kernel Image

```c
memblock_reserve(__pa_symbol(_text), _end - _text);
```

Reserves the entire kernel image (text + data + BSS) so memblock never allocates from this region.

```
memblock.reserved AFTER:
  [0] = DTB region
  [1] = initrd region
  [2] = kernel image [__pa(_text) — __pa(_end)]   ← NEW
```

---

### Step 10: Process DTB Reserved-Memory Nodes

```c
early_init_fdt_scan_reserved_mem();
```

The DTB can declare firmware-reserved regions:

```
/ {
    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;

        // GPU memory (RPi4)
        gpu_mem: framebuffer@3e000000 {
            reg = <0x0 0x3e000000 0x0 0x02000000>;
            no-map;
        };

        // Secure world memory (TrustZone)
        optee@30000000 {
            reg = <0x0 0x30000000 0x0 0x01000000>;
            no-map;
        };
    };
};
```

For each reserved-memory node:
- `memblock_reserve(base, size)` — mark as reserved
- If `no-map` property: `memblock_mark_nomap(base, size)` — also exclude from linear map

---

## Complete Memblock State After arm64_memblock_init()

```
memblock.memory (available RAM, trimmed):
┌─────────────────────────────────────────────────────┐
│ [0x4000_0000 — 0xC000_0000)    Bank 0: 2 GB        │
│ [0x1_0000_0000 — 0x1_8000_0000) Bank 1: 2 GB       │
│                                 Total: 4 GB         │
└─────────────────────────────────────────────────────┘

memblock.reserved (claimed regions):
┌─────────────────────────────────────────────────────┐
│ [0x4508_0000 — 0x4509_0000)   DTB            64 KB  │
│ [0x4800_0000 — 0x4900_0000)   initrd         16 MB  │
│ [0x4080_0000 — 0x4280_0000)   kernel image   32 MB  │
│ [0x3E00_0000 — 0x4000_0000)   GPU memory     32 MB  │
│ [0x3000_0000 — 0x3100_0000)   OP-TEE         16 MB  │
└─────────────────────────────────────────────────────┘

memstart_addr = 0x4000_0000
linear_region_size = 128 TB (48-bit VA)
```

---

## Key Takeaways

1. **`memstart_addr` is the anchor** — it defines the linear map formula for the entire system lifetime
2. **Alignment enables block mappings** — 1GB alignment allows single PUD entries to map 1GB of RAM
3. **Multiple removal passes** — memory is trimmed for PA limits, VA limits, and user limits
4. **Kernel image is always kept** — even `mem=` limits re-add the kernel region
5. **DTB reserved-memory is firmware truth** — GPU, TrustZone, and other firmware regions must not be touched
6. **`no-map` vs regular reserve** — `no-map` regions are not even included in the linear map page tables
