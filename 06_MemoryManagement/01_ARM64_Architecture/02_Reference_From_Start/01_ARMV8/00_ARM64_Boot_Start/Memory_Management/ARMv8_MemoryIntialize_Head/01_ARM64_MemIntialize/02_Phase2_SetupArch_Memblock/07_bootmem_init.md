# `bootmem_init()` — Zone Discovery, DMA Limits, CMA, NUMA

**Source:** `arch/arm64/mm/init.c` lines 195–260
**Phase:** Memblock Era
**Memory Allocator:** Memblock
**Called by:** `setup_arch()`
**Calls:** `arch_numa_init()`, `dma_limits_init()`, `dma_contiguous_reserve()`, `arch_reserve_crashkernel()`

---

## What This Function Does

Determines the **zone layout** and **NUMA topology** for the system. This function doesn't create the zones (that's `free_area_init()` in Phase 3), but it calculates the boundaries and reserves special memory regions.

---

## How It Works With Memory

### Memory State Before

- Memblock fully configured (all RAM added, kernel/DTB/initrd reserved)
- Linear map created (all RAM accessible)
- No zone information exists

### Memory State After

- Zone boundaries calculated (DMA, DMA32, NORMAL PFN ranges)
- CMA region reserved in memblock
- Crashkernel region reserved (if configured)
- NUMA topology determined (node-to-memory mapping)

---

## Step-by-Step Execution

### Step 1: Calculate PFN Range

```c
void __init bootmem_init(void)
{
    unsigned long min, max;

    min = PFN_UP(memblock_start_of_DRAM());    // First page frame number
    max = PFN_DOWN(memblock_end_of_DRAM());    // Last page frame number

    max_pfn = max_low_pfn = max;
    min_low_pfn = min;
```

**PFN (Page Frame Number)** = physical_address / PAGE_SIZE

```
Example:
  memblock_start_of_DRAM() = 0x4000_0000  (1 GB)
  memblock_end_of_DRAM()   = 0x1_8000_0000 (6 GB)

  min_low_pfn = PFN_UP(0x4000_0000)   = 0x40000  (page 262144)
  max_pfn     = PFN_DOWN(0x1_8000_0000) = 0x180000 (page 1572864)
```

These globals are used throughout the memory subsystem:

| Global | Value | Meaning |
|--------|-------|---------|
| `min_low_pfn` | First PFN | Lowest usable page frame |
| `max_pfn` | Last PFN | Highest page frame (total system memory indicator) |
| `max_low_pfn` | Same as max_pfn on ARM64 | No HIGHMEM on 64-bit (all RAM is "low") |

---

### Step 2: NUMA Topology Discovery

```c
    arch_numa_init();
```

Discovers NUMA (Non-Uniform Memory Access) topology from DTB or ACPI.

**From DTB:**

```
/ {
    // CPU to node mapping
    cpus {
        cpu@0 { numa-node-id = <0>; };
        cpu@1 { numa-node-id = <0>; };
        cpu@2 { numa-node-id = <1>; };
        cpu@3 { numa-node-id = <1>; };
    };

    // Memory to node mapping
    memory@40000000 { numa-node-id = <0>; reg = <...>; };
    memory@880000000 { numa-node-id = <1>; reg = <...>; };

    // Distance matrix
    distance-map {
        entry = <0 0 10>,   // Node 0 to Node 0: distance 10
                <0 1 20>,   // Node 0 to Node 1: distance 20
                <1 0 20>,   // Node 1 to Node 0: distance 20
                <1 1 10>;   // Node 1 to Node 1: distance 10
    };
};
```

`arch_numa_init()` does:
1. Reads NUMA node IDs from DTB/ACPI
2. Associates memblock regions with NUMA nodes (sets `region->nid`)
3. Builds distance tables
4. If no NUMA info: single node (nid=0) for all memory

---

### Step 3: DMA Zone Boundary Calculation

```c
    dma_limits_init();
```

Determines which memory zones exist and their boundaries. ARM64 can have up to 3 zones:

```c
static void __init dma_limits_init(void)
{
    // Get DMA addressing limits from DTB/ACPI
    phys_addr_t max_zone_dma = 0;
    phys_addr_t max_zone_dma32 = 0;

    // Device Tree: check all device "dma-ranges" properties
    // Find the most restrictive DMA mask

    // Typical results:
    // RPi4: zone_dma_limit = 1GB (30-bit DMA)
    // Generic: zone_dma_limit = 4GB (32-bit DMA)
    // Server: zone_dma_limit = 4GB

    zone_dma_limit = max_zone_dma;
    zone_dma32_limit = max_zone_dma32;
}
```

### Zone Layout Decision

| Zone | PFN Range | Purpose |
|------|-----------|---------|
| `ZONE_DMA` | `min_pfn` → `dma_limit_pfn` | Memory for devices with limited DMA addressing |
| `ZONE_DMA32` | `min_pfn` → `min(max_pfn, 4GB_pfn)` | Memory for 32-bit DMA devices |
| `ZONE_NORMAL` | Above DMA32 → `max_pfn` | General purpose memory |

```
Physical Memory Layout with Zones:

0                   1GB            4GB              max_pfn
├───────────────────┼──────────────┼─────────────────┤
│     ZONE_DMA      │  ZONE_DMA32  │   ZONE_NORMAL   │
│  (restricted DMA) │ (32-bit DMA) │ (general use)   │
├───────────────────┼──────────────┼─────────────────┤

Note: ZONE_DMA is a subset of ZONE_DMA32 on ARM64
```

**Why zones matter:**
- Some hardware (e.g., old PCI, USB) can only DMA to low physical addresses
- When the kernel needs memory for DMA buffers, it allocates from the appropriate zone
- This prevents the DMA zone from being exhausted by regular allocations

---

### Step 4: Reserve CMA Region

```c
    dma_contiguous_reserve(arm64_dma32_phys_limit);
```

**CMA (Contiguous Memory Allocator):**
- Reserves a large contiguous region (default: 8MB or configurable via `cma=` boot param)
- This region is used for devices that need physically contiguous DMA buffers
- Unlike regular reserves, CMA pages **can be used by movable allocations** when not needed for DMA
- When a DMA buffer is needed, movable pages are migrated out and the contiguous region is returned

```c
dma_contiguous_reserve(phys_limit):
  1. Determine CMA size (default CMA_SIZE or "cma=XXX" boot parameter)
  2. Find a contiguous region in memblock below phys_limit
  3. memblock_reserve(cma_base, cma_size)
  4. Mark region as CMA (special flag)
```

**Example:**

```
CMA size = 64 MB (configured via kernel or boot param)
phys_limit = 0x1_0000_0000 (4 GB — must be in ZONE_DMA32)

memblock finds: [0xBC00_0000 — 0xC000_0000) = 64 MB contiguous
memblock_reserve(0xBC00_0000, 0x400_0000)

This region:
  - Stays in memblock.memory (RAM exists)
  - Added to memblock.reserved (claimed for CMA)
  - Later: pages freed to buddy with MIGRATE_CMA type
  - Buddy allows MOVABLE allocations from CMA pages
  - When DMA needs contiguous memory: migrate movable pages out, allocate CMA
```

---

### Step 5: Reserve Crashkernel Region

```c
    arch_reserve_crashkernel();
```

If `crashkernel=XXX` is on the kernel command line, reserves memory for the crash dump kernel:

```
crashkernel=256M:
  - memblock_reserve() a 256 MB contiguous region
  - This region will hold a secondary kernel for crash dumps
  - On kernel panic, kexec loads and runs this kernel from the reserved region
  - The crash kernel can then dump memory to disk for post-mortem analysis
```

---

### Step 6: Dump Memblock State

```c
    memblock_dump_all();
}
```

Prints the complete memblock state to the kernel log (dmesg):

```
[    0.000000] MEMBLOCK configuration:
[    0.000000]  memory size = 0x100000000 reserved size = 0x04a00000
[    0.000000]  memory.cnt  = 0x2
[    0.000000]  memory[0x0]     [0x40000000-0xbfffffff], 0x80000000 bytes flags: 0x0
[    0.000000]  memory[0x1]     [0x100000000-0x17fffffff], 0x80000000 bytes flags: 0x0
[    0.000000]  reserved.cnt  = 0x5
[    0.000000]  reserved[0x0]   [0x40800000-0x427fffff], 0x02000000 bytes flags: 0x0  (kernel)
[    0.000000]  reserved[0x1]   [0x45080000-0x4508ffff], 0x00010000 bytes flags: 0x0  (DTB)
[    0.000000]  reserved[0x2]   [0x48000000-0x48ffffff], 0x01000000 bytes flags: 0x0  (initrd)
[    0.000000]  reserved[0x3]   [0xbc000000-0xbfffffff], 0x04000000 bytes flags: 0x0  (CMA)
[    0.000000]  reserved[0x4]   [0xf0000000-0xffffffff], 0x10000000 bytes flags: 0x0  (crashkernel)
```

---

## Data Flow Summary

```
bootmem_init()
│
├── PFN calculation → min_low_pfn, max_pfn (global)
│
├── arch_numa_init() → memblock_region.nid (per-region)
│
├── dma_limits_init() → zone_dma_limit, zone_dma32_limit (global)
│
├── dma_contiguous_reserve() → memblock_reserve(CMA region)
│
├── arch_reserve_crashkernel() → memblock_reserve(crashkernel region)
│
└── memblock_dump_all() → kernel log
```

---

## Key Takeaways

1. **Zones are about DMA capability** — not about memory speed or type
2. **PFN is the universal currency** — page frame numbers are used throughout the buddy allocator
3. **CMA is clever** — reserved for DMA but usable for movable pages when not needed
4. **NUMA topology matters** — the buddy allocator prefers local-node memory for performance
5. **This function doesn't create zones** — it only calculates boundaries. `free_area_init()` creates them.
6. **`memblock_dump_all()` is your debugging friend** — check dmesg to see the final memblock state
