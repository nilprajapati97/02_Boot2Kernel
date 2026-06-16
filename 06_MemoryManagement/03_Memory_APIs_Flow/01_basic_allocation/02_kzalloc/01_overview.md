# kzalloc — Overview

> Linux **6.6 LTS** · ARM64 · SLUB allocator
> Header: `<linux/slab.h>` · Source: [`include/linux/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/slab.h#L726)

---

## 1. One-line definition

`kzalloc()` is literally `kmalloc(size, flags | __GFP_ZERO)` — same allocator, same memory region, just guarantees the buffer is zero-filled before return.

---

## 2. Prototype

```c
/* include/linux/slab.h */
static inline void *kzalloc(size_t size, gfp_t flags)
{
    return kmalloc(size, flags | __GFP_ZERO);
}

static inline void *kzalloc_node(size_t size, gfp_t flags, int node)
{
    return kmalloc_node(size, flags | __GFP_ZERO, node);
}
```

It is a **`static inline`** wrapper — there is no separate symbol in `mm/slub.c`. The compiler folds it straight into the `kmalloc` path with `__GFP_ZERO` set.

---

## 3. Why a dedicated wrapper?

| Reason                             | Detail                                                                 |
|------------------------------------|------------------------------------------------------------------------|
| Readability                        | Explicit signal that the buffer must start zeroed (security-sensitive structs, info-leak avoidance). |
| Correctness                        | Forgetting `memset(p, 0, size)` after `kmalloc` is a recurring CVE class — `kzalloc` removes the chance. |
| Performance                        | `__GFP_ZERO` lets the allocator zero only the **object**, not the full slab page; with `CONFIG_INIT_ON_ALLOC_DEFAULT_ON` it can also use optimized zeroing paths (e.g., `DC ZVA` on ARM64). |
| Memcg + KASAN integration          | The zero pass happens after KASAN unpoisoning, inside SLUB's hot path, so quarantine/poison tracking stays correct. |

---

## 4. Where the zeroing actually happens

Inside `slab_alloc_node()` (fast path) and `___slab_alloc()` (slow path), after the object is detached from the freelist:

```c
/* mm/slub.c — simplified */
maybe_wipe_obj_freeptr(s, object);
if (unlikely(slab_want_init_on_alloc(gfpflags, s)))
    memset(object, 0, s->object_size);
```

`slab_want_init_on_alloc()` ([`mm/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab.h#L755)) returns true when:

- `gfpflags & __GFP_ZERO` (set by `kzalloc`), **or**
- `init_on_alloc` is globally enabled (`CONFIG_INIT_ON_ALLOC_DEFAULT_ON` or `init_on_alloc=1` boot arg).

Note it zeroes `s->object_size`, **not** `ksize(p)`. Padding bytes between `object_size` and the bucket size remain uninitialized — important for `copy_to_user` info-leak hardening.

---

## 5. ARM64 specific: how zeroing is implemented

On ARM64, `memset(p, 0, n)` with size aligned to the cache line uses the **`DC ZVA`** instruction (Data Cache Zero by VA) — see [`arch/arm64/lib/memset.S`](https://elixir.bootlin.com/linux/v6.6/source/arch/arm64/lib/memset.S). `DC ZVA` zeroes one cache line (typically 64 B) in a single instruction, without first reading it from memory — much faster than `STR` loops.

`DCZID_EL0` exposes the zero-block size; the assembly path checks it once and dispatches. For small `kzalloc` sizes (≤ one cache line) the implementation falls back to plain `STP` (store pair) of zero registers.

---

## 6. Parameters, return, context — same as kmalloc

See [`../kmalloc/01_overview.md`](../kmalloc/01_overview.md) §3, §4, §8. Everything that's true for `kmalloc` (size limits, GFP semantics, context rules) applies verbatim to `kzalloc`.

---

## 7. Decision matrix — `kzalloc` vs alternatives

| Need                                       | Use                       |
|--------------------------------------------|---------------------------|
| Small, zeroed, single object               | **`kzalloc`**             |
| Array of N zeroed objects (overflow-checked) | `kcalloc(n, size, flags)` |
| Already memset-ing manually after kmalloc  | switch to `kzalloc`       |
| Want explicit non-zero init (e.g. poison)  | `kmalloc` + your init     |
| Large (> 8 KB) zeroed buffer               | `kzalloc` still works; falls into `__kmalloc_large_node` which honors `__GFP_ZERO`. |
| Page-granular zeroed buffer                | `get_zeroed_page(flags)` ([`include/linux/gfp.h`](https://elixir.bootlin.com/linux/v6.6/source/include/linux/gfp.h#L344)) |

---

## 8. Minimal usage

```c
#include <linux/slab.h>

struct creds *c = kzalloc(sizeof(*c), GFP_KERNEL);
if (!c)
    return -ENOMEM;
/* every field is guaranteed 0 / NULL / false */
```

---

## 9. Common pitfalls

| Pitfall                                                | Fix |
|--------------------------------------------------------|-----|
| Assuming padding bytes are zero (`copy_to_user` leak)  | Use `memzero_explicit` or zero the full `ksize(p)` if you'll copy padding. |
| Using `kzalloc` for huge buffers in atomic context     | The zero pass still happens; `GFP_ATOMIC` still applies. Consider deferred work. |
| Calling on already-zero allocator-returned memory      | Harmless but a wasted `memset` — prefer to know your call site. |
| Mixing with `kmem_cache_alloc` + ctor                  | If your cache has a `ctor`, `__GFP_ZERO` and ctor are mutually exclusive (`WARN_ON`). |

---

## 10. Cross-references

- Internals → [02_internals.md](02_internals.md)
- ARM64 call flow → [03_arm64_callflow.md](03_arm64_callflow.md)
- Memory map → [04_memory_map.md](04_memory_map.md)
- Interview Q&A → [05_interview_qna.md](05_interview_qna.md)
- Parent (kmalloc) → [`../kmalloc/`](../kmalloc/)
