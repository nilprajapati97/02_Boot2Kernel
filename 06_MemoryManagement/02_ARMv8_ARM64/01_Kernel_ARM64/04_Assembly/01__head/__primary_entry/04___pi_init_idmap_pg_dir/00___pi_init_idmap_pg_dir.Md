## `__pi_init_idmap_pg_dir` & `__pi_create_init_idmap` — Deep Dive

---

### The Fundamental Problem

When the kernel first gets control, the MMU is **off**. The CPU executes at a **physical address** (e.g., `0x40200000`). But the kernel is **linked** to run at a **virtual address** (e.g., `0xFFFF800010000000`).

Before the MMU can be turned on, a page table must exist that maps **physical = virtual** (identity map) for the boot code. Without this, the moment `SCTLR_EL1.M` is set, the PC is still a physical address — and the CPU would fault trying to fetch the next instruction.

---

### What `__pi_init_idmap_pg_dir` Is

```
vmlinux.lds.S:
  __pi_init_idmap_pg_dir = .;        ← linker symbol, marks start
  . += INIT_IDMAP_DIR_SIZE;          ← reserves N pages of .bss
  __pi_init_idmap_pg_end = .;        ← marks end
```

It is a **statically allocated, linker-reserved region** inside the kernel image (`.bss` equivalent — zero-initialized). It is the raw physical memory that will hold the identity-map page table hierarchy.

**`__pi_` prefix** = "Position Independent" — these symbols are accessed via `adrp` (PC-relative) so they work regardless of where the kernel was loaded, before relocation.

---

### How Big Is It?

```
INIT_IDMAP_DIR_SIZE = (INIT_IDMAP_DIR_PAGES + EARLY_IDMAP_EXTRA_PAGES) × PAGE_SIZE

Where INIT_IDMAP_DIR_PAGES = EARLY_PAGES(levels, KIMAGE_VADDR, kimage_limit, 1)
  = 1 PGD page
  + PUD pages covering the range
  + PMD pages covering the range
  + PTE pages covering the range
```

Computed at build time based on:
- Number of translation levels (`INIT_IDMAP_PGTABLE_LEVELS` — typically 3 or 4)
- Size of the kernel image (`_stext` → `_end`)
- Extra pages for alignment and FDT mapping

Typically **4–8 pages (16–32 KB)** depending on config.

---

### What `__pi_create_init_idmap` Does

```c
// arch/arm64/kernel/pi/map_range.c
asmlinkage phys_addr_t __init create_init_idmap(pgd_t *pg_dir, ptdesc_t clrmask)
{
    phys_addr_t ptep = (phys_addr_t)pg_dir + PAGE_SIZE;  // page allocator starts after PGD

    // Map kernel TEXT:  _stext → __initdata_begin   (ROX: Read-Only Executable)
    map_range(&ptep, _stext, __initdata_begin,
              (phys_addr_t)_stext, text_prot, ...);

    // Map kernel DATA:  __initdata_begin → _end      (RW: Read-Write)
    map_range(&ptep, __initdata_begin, _end,
              (phys_addr_t)__initdata_begin, data_prot, ...);

    return ptep;  // returns end of used page table region
}
```

**Key insight**: `virt == phys` for every mapping. Virtual address = physical address. That is the identity map.

---

### Call Flow

```
primary_entry
│
│  [Stack is now set up: sp = early_init_stack]
│
├── adrp  x0, __pi_init_idmap_pg_dir   ← x0 = phys addr of pg_dir buffer
│         (PC-relative, MMU-safe, position-independent)
│
├── mov   x1, xzr                       ← clrmask = 0
│         (no permission bits to clear)
│
└── bl    __pi_create_init_idmap        arch/arm64/kernel/pi/map_range.c
          │
          │  Input:  x0 = pg_dir (start of reserved buffer)
          │          x1 = clrmask (bits to strip from page attrs)
          │
          ├── ptep = pg_dir + PAGE_SIZE  ← sub-page allocator pointer
          │         (first page = PGD, rest carved from here)
          │
          ├── map_range(_stext → __initdata_begin)
          │     Build PGD → PUD → PMD → PTE chain
          │     phys == virt for every entry
          │     Permissions: ROX (read-only, executable)
          │     Reason: kernel text must be executable, not writable
          │
          ├── map_range(__initdata_begin → _end)
          │     Same identity mapping
          │     Permissions: RW (read-write, not executable)
          │     Reason: data/bss/stack must be writable
          │
          └── return ptep   → x0 = end of used region
                              (used by caller for cache maintenance)
```

---

### Physical Memory Layout After This Call

```
Physical Memory
────────────────────────────────────────────────────────────
  __pi_init_idmap_pg_dir ──►  [ PGD page          ] 4KB
                              [ PUD page(s)        ] 4KB each
                              [ PMD page(s)        ] 4KB each
                              [ PTE page(s)        ] 4KB each
  __pi_init_idmap_pg_end ──►  (end of reserved region)
                              [ early_init_stack   ] 4KB
────────────────────────────────────────────────────────────

  Each PTE entry maps:  phys_addr  →  virt_addr = phys_addr
                                      (identity: virt == phys)
```

---

### Why Two Separate mappings (text vs data)?

| Region | Protection | Why |
|--------|-----------|-----|
| `_stext` → `__initdata_begin` | `ROX` (Read, no-Write, Execute) | W^X: writable pages must not be executable — security policy |
| `__initdata_begin` → `_end` | `RW` (Read, Write, no-Execute) | Data, BSS, stacks need write access but must not be executable |

This enforces **W^X (Write XOR Execute)** from the very first moment the MMU is enabled — not an afterthought.

---

### How It's Used Downstream

```
__pi_create_init_idmap returns x0 = end of used region
│
├── [MMU OFF path in primary_entry]
│     x1 = x0  (end)
│     x0 = __pi_init_idmap_pg_dir  (start)
│     dcache_inval_poc(start, end)
│     → Invalidate page table cache lines so
│       MMU page table walker sees correct data
│
└── [__primary_switch]
      x2 = __pi_init_idmap_pg_dir
      bl __enable_mmu
      → Loaded into TTBR0_EL1
      → This is the active page table when MMU turns ON
      → Identity map ensures PC remains valid after MMU enable
      → Discarded after __pi_early_map_kernel sets up real kernel mappings
```

---

### Interview-Level Insight: Why Identity Map and Not Just Skip?

When `set_sctlr_el1` sets the `M` bit (MMU enable), the **very next instruction fetch** goes through the MMU. If the table didn't have a mapping for the current PC, the CPU would take a translation fault — **before exception vectors are installed** — and hang.

The identity map ensures `phys PC == virt PC`, so the CPU continues executing without interruption. Once `__pi_early_map_kernel` maps the kernel at its final virtual address and `__primary_switched` runs, the identity map is never needed again — it is superseded by `swapper_pg_dir`.You've used 70% of your weekly rate limit. Your weekly rate limit will reset on April 27 at 5:30 AM. [Learn More](https://aka.ms/github-copilot-rate-limit-error)
