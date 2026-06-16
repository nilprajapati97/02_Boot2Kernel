# early_ioremap_reset() — System Design Approach and Q&A

## 1. Why This Function Exists: The Bootstrapping Problem

During early boot, the kernel must map device memory (serial port for earlycon, FDT, EFI memory map) before the permanent page table infrastructure exists. The solution is `early_ioremap()` — a lightweight mechanism using pre-allocated fixmap slots and a statically-linked PTE table.

Once `paging_init()` establishes permanent page tables, the early mechanism must be **cleanly retired**:
1. No dangling mappings (would be invalidated by `prepare_page_table()`)
2. The PTE lookup function must switch to use the new permanent PTE tables
3. Any code written for the early phase must be blocked from running in the late phase

`early_ioremap_reset()` is the **transition gate** that enforces these properties.

---

## 2. Design Principle: Explicit Phase Transition

The `after_paging_init` flag and `pte_offset_fixmap` function pointer implement an **explicit phase transition**:

```
Phase EARLY:
  pte_offset_fixmap = pte_offset_early_fixmap
  after_paging_init = 0
  early_ioremap() works
  ioremap() does NOT work (vmalloc not initialized)
                ↓
   early_ioremap_reset()  ← transition gate
                ↓
Phase LATE:
  pte_offset_fixmap = pte_offset_late_fixmap
  after_paging_init = 1
  early_ioremap() does NOT work (WARN + return NULL)
  ioremap() works
  set_fixmap() works with permanent page tables
```

This is the **phase guard pattern**: software enforces that certain APIs are only available in certain phases. Crossing a phase boundary without the gate would use the wrong implementation silently.

---

## 3. Dependency Graph

```
[early_ioremap_init()]     → bm_pte[] connected to PMD entry
                             pte_offset_fixmap = pte_offset_early_fixmap
        │
[FDT access, EFI memmap]   → early_ioremap() / early_iounmap() calls
[earlycon setup]           → FIX_EARLYCON_MEM_BASE slot in use
        │
[memblock finalized]       → arm_memblock_init(), adjust_lowmem_bounds() #2
        │
[early_ioremap_reset()]
  ├── early_ioremap_shutdown() → verify all slots released
  ├── after_paging_init = 1   → blocks future early_ioremap() calls
  └── pte_offset_fixmap = pte_offset_late_fixmap → ready for permanent PTEs
        │
[paging_init()]
  ├── prepare_page_table()   → clears old PMD (bm_pte[] disconnected)
  ├── map_lowmem()           → sets up new direct-map PTEs
  └── early_fixmap_shutdown() → reconnects fixmap with new PTE allocation
        │
[Runtime: set_fixmap(), clear_fixmap(), kmap_atomic() — all use late fixmap]
```

---

## 4. Security Consideration: Why Blocking early_ioremap() Post-Reset Matters

After `paging_init()`, `early_ioremap()` would use `pte_offset_late_fixmap` but operate on fixmap slots that might conflict with `kmap_atomic()` usage. On SMP systems, multiple CPUs might race on fixmap slots originally designed for single-CPU early boot use. The `after_paging_init` guard prevents this race condition entirely by making `early_ioremap()` a no-op post-reset.

Additionally, `early_ioremap()` bypasses permission checking that `ioremap()` performs (checking if the physical address is in a valid MMIO range). Post-paging-init, permitting `early_ioremap()` could allow unchecked physical memory mappings — a security issue.

---

## 5. System Design Q&A

**Q: Why is early_ioremap_reset() called before paging_init() rather than inside it?**
> The reset must happen before `paging_init()` calls `prepare_page_table()`. `prepare_page_table()` clears all PMD entries for the kernel VA range — including the PMD entry that was set up by `early_ioremap_init()` to point to the static `bm_pte[]` array. If any early_ioremap mapping were still active when `prepare_page_table()` runs, the cleared PMD would cause a page fault when that VA is accessed. `early_ioremap_reset()` before `paging_init()` ensures all early mappings are verified closed before the page tables are rebuilt.

**Q: What is the relationship between FIX_BTMAP slots and early_ioremap() calls?**
> FIX_BTMAP (Boot-Time Memory Allocated Pages) slots are the virtual address slots used by `early_ioremap()`. Each slot is `PAGE_SIZE` aligned, and there are `FIX_BTMAPS_SLOTS` slots (typically 7). Each `early_ioremap()` call finds a free slot, writes a PTE in the fixmap PTE table pointing the slot's VA to the requested physical address, and returns the VA. `early_iounmap()` clears the PTE and marks the slot free. `early_ioremap_shutdown()` checks that all slots are free before the transition. The slot count limits how many simultaneous early mappings can exist.

**Q: Can a device driver call early_ioremap() during its probe() function?**
> No. Device probe functions run after `initcalls` execute, which is long after `paging_init()` and `early_ioremap_reset()`. At that point, `after_paging_init = 1`, and `early_ioremap()` will WARN and return NULL. Drivers must use `ioremap()` in their probe function. Only code that runs before `early_ioremap_reset()` — specifically code in `setup_arch()` or called from it — may use `early_ioremap()`.

**Q: Why is after_paging_init an __initdata variable?**
> After the kernel finishes boot initialization, all `__init` and `__initdata` memory is freed. `after_paging_init` is only needed during the boot phase to guard `early_ioremap()` calls. Once boot is complete, `early_ioremap()` is no longer called from any non-init code (it's only used during early init). Marking it `__initdata` allows that 4 bytes to be reclaimed after boot. The `early_ioremap()` function itself is also `__init` — after boot, the function code is freed too, making the check moot.

**Q: How does early_fixmap_shutdown() (inside paging_init) complete what early_ioremap_reset() started?**
> `early_ioremap_reset()` switches the function pointer so the next `set_fixmap()` call uses the late implementation. But it doesn't actually set up the permanent PTEs — it just primes the function pointer. `early_fixmap_shutdown()` (called inside `paging_init()` after the new page tables are built) calls `set_fixmap()` for any permanent fixmap entries (like `FIX_EARLYCON_MEM_BASE` if earlycon is still active). This re-creates the fixmap mappings in the new page tables. After `paging_init()` completes, both the function pointer (switched by reset) and the actual PTE entries (set by shutdown) are in their final state.
