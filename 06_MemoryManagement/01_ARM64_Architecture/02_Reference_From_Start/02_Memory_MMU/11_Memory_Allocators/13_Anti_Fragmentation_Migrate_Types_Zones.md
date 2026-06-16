# Anti-Fragmentation: MIGRATE Types and Memory Zones

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Memory fragmentation: the bane of long-running Linux systems
  
  External fragmentation:
    Many small free blocks scattered everywhere
    Unable to satisfy large contiguous allocation (huge pages, CMA, DMA)
    Total free memory may be large, but usable max-order block is tiny
  
  Root cause: mixing movable and unmovable allocations on same pages
    Unmovable: kernel slab (struct file, struct inode, etc.)
    Movable:   user anonymous pages, page cache
    
    Time T=0: [User|Kernel|User|Kernel|User|Kernel] interleaved
    Time T=1: User pages freed [     |Kernel|     |Kernel|     |Kernel]
    Time T=2: Free blocks separated by immovable kernel pages → fragmented!
  
  Solution: migration type segregation
    Allocate UNMOVABLE pages from UNMOVABLE pool
    Allocate MOVABLE pages from MOVABLE pool
    If MOVABLE pool is fragmented: COMPACT (can move them)
    UNMOVABLE pool: may fragment, but those allocations are stable anyway
  
  Result: UNMOVABLE pool fragments slowly (kernel data)
          MOVABLE pool stays defragmentable (can always compact)
```

---

## 2. Migration Types

```c
/* include/linux/mmzone.h */

enum migratetype {
    MIGRATE_UNMOVABLE,    // Can't be moved: kernel struct pages, kmalloc data
    MIGRATE_MOVABLE,      // Can be moved: user anonymous pages, page cache
    MIGRATE_RECLAIMABLE,  // Can be reclaimed: page cache, slab-reclaimable
    MIGRATE_HIGHATOMIC,   // Emergency reserves (used by GFP_ATOMIC when zones low)
    MIGRATE_CMA,          // CMA pages (movable, but CMA owns the region)
    MIGRATE_ISOLATE,      // Being isolated for migration (temporary)
    MIGRATE_TYPES,        // Count
};

// Each buddy free_area has separate lists per migratetype:
struct free_area {
    struct list_head free_list[MIGRATE_TYPES];
    unsigned long nr_free;
};

Assignment of migration type based on GFP flags:
  gfp_migratetype(gfp_flags):
    GFP_KERNEL:           → MIGRATE_UNMOVABLE
    GFP_USER:             → MIGRATE_MOVABLE  
    GFP_HIGHUSER_MOVABLE: → MIGRATE_MOVABLE
    __GFP_RECLAIMABLE:    → MIGRATE_RECLAIMABLE
    __GFP_MOVABLE:        → MIGRATE_MOVABLE

Migration type of slab caches:
  kmem_cache_create(... SLAB_RECLAIM_ACCOUNT):  MIGRATE_RECLAIMABLE
  kmem_cache_create(... 0):                      MIGRATE_UNMOVABLE
  
  Examples:
    vm_area_cachep:   MIGRATE_UNMOVABLE (kernel data, cannot be moved)
    filp_cachep:      MIGRATE_UNMOVABLE
    fs_cachep:        MIGRATE_UNMOVABLE
    inode_cache:      MIGRATE_RECLAIMABLE (can be reclaimed under pressure)
    dentry_cache:     MIGRATE_RECLAIMABLE
```

---

## 3. Fallback Mechanism

```
Fallback: when preferred migratetype list is empty, steal from another

fallbacks[MIGRATE_TYPES][4]:
  // Order of fallback types for each migratetype:
  MIGRATE_UNMOVABLE → fallback: MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE, MIGRATE_TYPES
  MIGRATE_MOVABLE   → fallback: MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE, MIGRATE_TYPES
  MIGRATE_RECLAIMABLE → fallback: MIGRATE_UNMOVABLE, MIGRATE_MOVABLE, MIGRATE_TYPES

steal_suitable_fallback(zone, order, migratetype):
  // Find a free block in a fallback migratetype
  for each fallback type in fallbacks[migratetype]:
    if free_list[fallback_type] non-empty:
      page = take from that list
      
      // Critical decision: change whole block's type or just this page?
      if (order >= pageblock_order):
        // Large block: change the entire pageblock's type
        set_pageblock_migratetype(page, migratetype)
        // All future frees of pages in this block → go to migratetype list
      else:
        // Small allocation: keep block's type but use page anyway
        // This causes minor "contamination" of the block
      
      break;

pageblock_order: typically MAX_ORDER-1 = 10 (4MB blocks)
  ARM64: each 4MB aligned chunk of memory is one "pageblock"
  Each pageblock has one migratetype assigned (in a bitmap)
  
  pageblock_flags:
    4 bits per pageblock: PB_migrate (3 bits), PB_migrate_skip (1 bit)
    Stored in a global bitmap: usemap_size calculation at boot

Effect of type segregation over time:
  System boot: mix of types across all pageblocks
  Hours later:
    Low addresses: mostly UNMOVABLE (kernel slab allocated from there)
    Mid addresses: mostly MOVABLE (user pages)
    High addresses: mostly MOVABLE + free
  
  Result: compaction can move MOVABLE pages from mid to high,
          freeing large contiguous MOVABLE region in mid for huge pages
          Without touching UNMOVABLE low-address blocks at all
```

---

## 4. ARM64 Memory Zones

```
ARM64 zone layout (typical server with 512GB RAM):

ZONE_DMA (0 to ~1GB):
  Exists for compatibility with devices needing DMA below 1GB
  arm64: CONFIG_ZONE_DMA enabled for SoCs with <1GB DMA-capable devices
  Size: first 1GB of physical memory (or platform-configured)
  arm64_dma_phys_limit: set by arch/arm64/mm/init.c based on dtb/cmdline
  Small on ARM64 servers (most devices support 64-bit DMA)

ZONE_DMA32 (0 to 4GB):
  For devices with 32-bit DMA limitation
  arm64: CONFIG_ZONE_DMA32 = enabled when system has >4GB RAM AND 32-bit DMA devices
  Typical: first 4GB of physical RAM
  PCIe devices on ARM64 that can't set DMA mask to 64-bit use GFP_DMA32

ZONE_NORMAL (1GB or 4GB to end of RAM):
  The main general-purpose zone
  All normal kernel allocations (GFP_KERNEL) come from here
  Largest zone on modern ARM64 servers

ZONE_MOVABLE (optional):
  Created only if kernelcore= or movablecore= boot parameters given
  OR: numa=fake, or memory hotplug
  Contains only MOVABLE pages (allows memory hotplug: can evacuate zone)
  Useful for: memory hotplug in data center (add/remove DIMMs)
  ARM64 cloud instances: often have ZONE_MOVABLE for balloon driver

Zone watermarks (per zone):
  pages_min:  OOM territory (emergency reserves only)
  pages_low:  kswapd wakes up to reclaim
  pages_high: kswapd stops reclaiming
  
  watermark[WMARK_MIN] = pages_min
  watermark[WMARK_LOW] = pages_low  
  watermark[WMARK_HIGH] = pages_high
  
  /proc/zoneinfo:
    Node 0, zone   Normal
      pages free     1234567
            min        45678
            low        67890
            high       90123
            ...
```

---

## 5. Interview Questions & Answers

**Q1: Why does the Linux kernel have MIGRATE_RECLAIMABLE as a separate type from MIGRATE_MOVABLE? Aren't both "can be removed from physical memory"?**

Yes, both can be removed, but through different mechanisms:

**MIGRATE_MOVABLE**: the page's content is PRESERVED during migration. The page is physically moved to a new location, and all PTEs/pointers are updated to the new location. The original physical location is then freed. Used for: user anonymous pages, user file mappings.

**MIGRATE_RECLAIMABLE**: the page's content can be DISCARDED or written back, freeing the original location without copying. Used for: page cache pages (can be dropped and re-read from disk), inode/dentry cache (can be freed and recreated). These pages don't need to be "moved" — they can simply be freed. This is CHEAPER than migration (no copy needed).

The separation allows the allocator to:
- Use MIGRATE_RECLAIMABLE pages for **reclaim** before compaction (kswapd reclaims these first — cheaper than moving)
- Use MIGRATE_MOVABLE pages for **compaction** (move them, then free original location)
- Avoid trying to "move" reclaimable pages when simply dropping them is more efficient

In practice, compaction treats both MOVABLE and RECLAIMABLE as "can be evacuated," but reclaim prefers RECLAIMABLE pages (can be freed without copy overhead).

**Q2: What is MIGRATE_HIGHATOMIC and when is it used?**

`MIGRATE_HIGHATOMIC` is a small emergency reserve of physically contiguous pages, kept in every zone, specifically for `GFP_ATOMIC` allocations when the zone is below the `min` watermark (normal allocations refused).

Mechanism:
- Each zone reserves a small percentage of pages in `MIGRATE_HIGHATOMIC` lists
- These are only accessible when `alloc_flags & ALLOC_HIGH` is set (which happens for GFP_ATOMIC with `__GFP_HIGH`)
- Normal GFP_KERNEL allocations cannot use them even if they would otherwise steal from HIGHATOMIC

Use case: interrupt handlers need to allocate memory NOW. The driver interrupt handler cannot afford to fail — it must complete the interrupt processing. HIGHATOMIC provides last-resort pages for these critical interrupt-context allocations.

The size is controlled by `vm.min_free_kbytes` / `lowmem_reserve_ratio` and the zone's `lowmem_reserve[]` array.

On ARM64 systems: ARM GIC (Generic Interrupt Controller) interrupt handlers that allocate memory must use `GFP_ATOMIC`. If the system is under memory pressure, the HIGHATOMIC reserve ensures these can still succeed.

---

## 6. Quick Reference

| Migratetype | Pages | Can Compact? | Can Reclaim? |
|---|---|---|---|
| UNMOVABLE | Kernel slab, modules | NO | NO |
| MOVABLE | User anonymous, file cache | YES (copy) | YES (write-then-free) |
| RECLAIMABLE | Page cache, slab-reclaimable | NO (just reclaim) | YES |
| HIGHATOMIC | Emergency reserve | NO (protected) | NO |
| CMA | CMA region pages | YES (move out) | YES |
| ISOLATE | Being migrated | — | — |

| ARM64 Zone | Physical Range | Used For |
|---|---|---|
| ZONE_DMA | 0 to ~1GB | Legacy low-address DMA |
| ZONE_DMA32 | 0 to 4GB | 32-bit DMA devices |
| ZONE_NORMAL | 1GB/4GB to end | All normal allocations |
| ZONE_MOVABLE | Configured range | Hotplug / migration |
