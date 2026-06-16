# early_ioremap_reset() — Detailed Design

## 1. Context: The Bridge Between Two Eras

```
setup_arch():
  ├── early_ioremap_init()           ← dir 07: initialized early fixmap
  │       │ Set up pte_offset_fixmap = pte_offset_early_fixmap
  │       │ Kernel uses fixmap for FDT access, EFI memmap, etc.
  │       │
  │   [All early_ioremap() calls use early fixmap]
  │
  ├── arm_memblock_init()            ← memblock now finalized
  ├── adjust_lowmem_bounds() #2
  └── early_ioremap_reset()         ← *** THIS FUNCTION ***
        │ Switch: pte_offset_fixmap = pte_offset_late_fixmap
        │ Transition from early fixmap to permanent fixmap
        │
  └── paging_init()                 ← builds permanent page tables
        │ Fixmap PTEs now allocated from permanent page tables
```

`early_ioremap_reset()` is a **one-time function pointer switch** that transitions the fixmap mechanism from its early boot implementation to its permanent runtime implementation. After this call, fixmap PTE operations use the permanent page table infrastructure.

---

## 2. Source Code

**File:** `mm/early_ioremap.c`

```c
void __init early_ioremap_reset(void)
{
    early_ioremap_shutdown();
    after_paging_init = 1;
    pte_offset_fixmap = pte_offset_late_fixmap;
}
```

Three actions:

### 2.1 early_ioremap_shutdown()

```c
void __init early_ioremap_shutdown(void)
{
    int i;

    for (i = 0; i < FIX_BTMAPS_SLOTS; i++) {
        if (prev_map[i])
            WARN(1, "slot %d in use (addr %p).\n", i, prev_map[i]);
    }
}
```

Validates that no early ioremap mappings are still active. If `prev_map[i]` is non-NULL, someone called `early_ioremap()` without a matching `early_iounmap()`. This would be a bug — the mapping would be permanently leaked in the fixmap.

### 2.2 after_paging_init = 1

```c
static int after_paging_init __initdata = 0;
```

This flag is checked by `early_ioremap()` to detect misuse:

```c
void __iomem *early_ioremap(resource_size_t phys_addr, unsigned long size)
{
    return __early_ioremap(phys_addr, size, FIXMAP_PAGE_IO);
}

static void __iomem *__early_ioremap(...)
{
    if (WARN_ON_ONCE(after_paging_init))
        return NULL;
    /* ... proceed with early fixmap ... */
}
```

After `early_ioremap_reset()`, any call to `early_ioremap()` will WARN and return NULL. This enforces that early ioremap is only used during the early boot phase. Drivers must use `ioremap()` instead.

### 2.3 pte_offset_fixmap = pte_offset_late_fixmap

```c
/* mm/early_ioremap.c */
pte_t * (*pte_offset_fixmap)(pmd_t *dir, unsigned long addr);

/* Before reset: */
pte_offset_fixmap = pte_offset_early_fixmap;

/* After reset: */
pte_offset_fixmap = pte_offset_late_fixmap;
```

This function pointer switch is the core of the transition. Two different functions provide PTE offset calculation:

**`pte_offset_early_fixmap`**: Uses a pre-allocated static PTE table. Before `paging_init()` sets up the full page table hierarchy, there are no PMD/PUD entries for the fixmap region. Early fixmap uses a manually-bootstrapped PTE table.

**`pte_offset_late_fixmap`**: Uses the actual page table hierarchy. After `paging_init()`, the fixmap VA region has proper PMD entries. `pte_offset_late_fixmap` walks the page tables normally.

---

## 3. The Fixmap Mechanism

Fixmaps are a region of **compile-time-known** virtual addresses reserved for special mappings:

```c
/* arch/arm/include/asm/fixmap.h */
enum fixed_addresses {
    FIX_EARLYCON_MEM_BASE,  /* early console MMIO */
    FIX_TEXT_POKE0,         /* text patching */
    FIX_TEXT_POKE1,
    __end_of_permanent_fixed_addresses,

    /* Early IO/ioremap is below */
    FIX_BTMAP_END = __end_of_permanent_fixed_addresses,
    FIX_BTMAP_BEGIN = FIX_BTMAP_END + TOTAL_FIX_BTMAPS - 1,
    __end_of_fixed_addresses
};
```

Each `fixed_addresses` enum value maps to a specific virtual address:

```c
#define __fix_to_virt(x)  (FIXADDR_TOP - ((x) << PAGE_SHIFT))
```

`FIXADDR_TOP` is near the top of the kernel VA (below `0xFFFF0000` on ARM32).

Fixmaps are used for:
- `earlycon`: Map UART MMIO before paging_init
- `early_ioremap`: Map any device memory during early boot
- `kmap_atomic`: Temporary highmem page mappings (uses fixmap slots)
- `set_fixmap()`: Permanent special-purpose mappings (text poke, etc.)

---

## 4. Why Reset Is Needed Before paging_init()

`paging_init()` calls `prepare_page_table()` which clears all PGD/PMD entries for the kernel VA range. This clearing would include the PMD entry that covers the fixmap VA region. After clearing:
- The early fixmap PTE table (a statically allocated array) is disconnected from the page table walk
- Any attempt to use the early fixmap after `prepare_page_table()` would fault

`early_ioremap_reset()` is called **before** `paging_init()` to:
1. Verify all early ioremap mappings are released (no dangling refs)
2. Arm the protection against post-reset `early_ioremap()` calls
3. Switch to `pte_offset_late_fixmap` so `set_fixmap()` uses the new page tables

After `paging_init()`, `set_fixmap()` uses the permanent page tables built by `paging_init()`. The fixmap VA region now has a real PMD entry. `pte_offset_late_fixmap` does a normal page-table walk.

---

## 5. Call Sequence (Bottom-Up)

```
WARN()                       ← lib/bug.c (for leaked mappings warning)
        ▲
early_ioremap_shutdown()     ← mm/early_ioremap.c
        ▲
early_ioremap_reset()        ← mm/early_ioremap.c
        ▲
setup_arch()                 ← arch/arm/kernel/setup.c (after 2nd adjust_lowmem_bounds)
```

---

## 6. Timeline of Fixmap Usage

```
early_ioremap_init()      → bootstrap: allocate early PTE table, connect it
                             pte_offset_fixmap = pte_offset_early_fixmap

[FDT parsing]             → early_ioremap() to access FDT bytes
[EFI memmap access]       → early_ioremap() for EFI memory descriptor array
[earlycon output]         → uses FIX_EARLYCON_MEM_BASE fixmap slot

early_ioremap_reset()     → early_iounmap() must have been called for all above
                             pte_offset_fixmap = pte_offset_late_fixmap

paging_init()             → prepare_page_table() clears old PTEs
                             map_lowmem() sets up direct map
                             early_fixmap_shutdown() re-initializes fixmap with new PTEs

[Runtime]                 → set_fixmap() / clear_fixmap() use late fixmap
                             kmap_atomic() uses fixmap slots
                             ioremap() uses vmalloc region (not fixmap)
```

---

## 7. Interview Q&A

**Q1: What is the purpose of early_ioremap_reset() in one sentence?**
> `early_ioremap_reset()` transitions the fixmap PTE mechanism from the early boot implementation (static PTE table) to the runtime implementation (walks the permanent page tables), while verifying no early ioremap mappings were left active.

**Q2: What happens if early_ioremap() is called after early_ioremap_reset()?**
> `early_ioremap()` checks the `after_paging_init` flag. If it's non-zero, `WARN_ON_ONCE()` fires (prints a stack trace to dmesg) and the function returns NULL. The caller gets a NULL pointer and typically crashes. This is intentional — using early ioremap after the reset is a programming bug. The correct function to use post-reset is `ioremap()`.

**Q3: What is the difference between early_ioremap() and ioremap()?**
> `early_ioremap()` maps a physical address to a fixmap VA slot — a pre-determined virtual address from the fixmap enum. It's fast (no memory allocation) and works before paging_init() because it uses a pre-allocated PTE table. But it has only a few slots (FIX_BTMAPS_SLOTS) and each slot is limited in size. `ioremap()` allocates a VA range from the vmalloc region, creates PTE entries, and handles arbitrary sizes. It requires the vmalloc infrastructure (which requires paging_init() to have run).

**Q4: What is pte_offset_early_fixmap vs pte_offset_late_fixmap?**
> Both return a PTE pointer for a virtual address within the fixmap region. `pte_offset_early_fixmap` returns an entry in a statically-allocated PTE array (bypasses normal page table walk — no PMD entry needed). `pte_offset_late_fixmap` uses `pmd_offset(pud_offset(pgd_offset_k(addr), addr), addr)` — the normal page table walk. After `paging_init()` creates proper PMD entries for the fixmap region, the late version works correctly and returns PTE entries within the newly-allocated PTE page.

**Q5: Why does early_ioremap_shutdown() warn instead of panic if a slot is in use?**
> A slot being in use means early_ioremap() was called without a matching early_iounmap(). This is a bug in the early boot code. However, `panic()` at this point would prevent any debug output (the system would just hang or reset). `WARN()` prints a stack trace showing which slot leaked, which provides crucial debugging information. The system then continues — typically the leaked mapping will cause a fault in paging_init() anyway, at which point a full panic with register dump occurs.
