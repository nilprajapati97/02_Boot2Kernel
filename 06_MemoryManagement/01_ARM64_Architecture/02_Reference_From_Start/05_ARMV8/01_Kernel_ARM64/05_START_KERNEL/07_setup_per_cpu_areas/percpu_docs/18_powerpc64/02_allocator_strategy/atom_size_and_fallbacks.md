# PPC64 Allocator Strategy: `atom_size` and Fallback Behavior

## Source
- `arch/powerpc/kernel/setup_64.c:844-878`

---

## `atom_size` Decision Logic

```c
if (IS_ENABLED(CONFIG_PPC_BOOK3E_64))
    atom_size = SZ_1M;
else if (radix_enabled())
    atom_size = PAGE_SIZE;
else if (IS_ENABLED(CONFIG_PPC_64S_HASH_MMU))
    atom_size = (mmu_linear_psize == MMU_PAGE_4K) ? PAGE_SIZE : SZ_1M;
```

### Why this matters
- `atom_size` controls grouping/alignment granularity of percpu units.
- PPC64 ties this to linear mapping/MMU behavior for practical allocation efficiency.

---

## Fallback Logic

```c
if (pcpu_chosen_fc != PCPU_FC_PAGE)
    rc = pcpu_embed_first_chunk(...);

if (rc < 0)
    rc = pcpu_page_first_chunk(...);

if (rc < 0)
    panic(...);
```

Meaning:
1. Try embedded first chunk when allowed.
2. On failure, fall back to page mode.
3. Panic only if both fail.

This gives PPC64 robust boot behavior with architecture-tuned first attempt and generic safety fallback.
