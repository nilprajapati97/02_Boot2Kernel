# 05 — GFP Flags

## 1. What are GFP Flags?

`GFP` = **Get Free Pages**. Flags that control **how** the allocator behaves:
- Which memory zone to allocate from
- Whether the allocator can sleep/reclaim
- How hard to try before failing

---

## 2. Common Flags Reference

| Flag | Context | Sleep? | Reclaim? | Description |
|------|---------|--------|----------|-------------|
| `GFP_KERNEL` | Process | ✅ | ✅ | Standard kernel allocation. Most common. |
| `GFP_ATOMIC` | IRQ/BH | ❌ | ❌ | Non-sleeping. May use emergency reserves. |
| `GFP_NOWAIT` | Any | ❌ | ❌ | Non-blocking; no reclaim. |
| `GFP_NOIO` | IO path | ✅ | No IO | No block IO during reclaim (avoids deadlock) |
| `GFP_NOFS` | FS path | ✅ | No FS | No filesystem calls during reclaim |
| `GFP_USER` | User page | ✅ | ✅ | For user-space mappings |
| `GFP_DMA` | DMA | Depends | Depends | ZONE_DMA (< 16 MiB on x86) |
| `GFP_DMA32` | DMA32 | Depends | Depends | ZONE_DMA32 (< 4 GiB on x86_64) |
| `GFP_HIGHUSER` | User | ✅ | ✅ | Can use ZONE_HIGHMEM |

---

## 3. Modifier Flags (combine with `|`)

| Modifier | Meaning |
|----------|---------|
| `__GFP_ZERO` | Zero-fill returned pages (equivalent to kzalloc) |
| `__GFP_NOFAIL` | Must not fail — loop forever (use sparingly) |
| `__GFP_HIGH` | Can use emergency memory reserves |
| `__GFP_NOWARN` | Suppress OOM warning on failure |
| `__GFP_COMP` | Return compound page (for huge pages) |
| `__GFP_RECLAIM` | Allow reclaim of pages |
| `__GFP_IO` | Allow block I/O during reclaim |
| `__GFP_FS` | Allow filesystem calls during reclaim |
| `__GFP_MOVABLE` | Page is movable (memory compaction) |

---

## 4. Decision Flowchart

```mermaid
flowchart TD
    A{Are you in IRQ/BH\nor cannot sleep?} 
    A -- Yes --> B[GFP_ATOMIC]
    A -- No --> C{In filesystem\ncall path?}
    C -- Yes --> D[GFP_NOFS]
    C -- No --> E{In block IO\ncall path?}
    E -- Yes --> F[GFP_NOIO]
    E -- No --> G[GFP_KERNEL]
    G --> H{Need DMA-able\nmemory?}
    H -- Yes, < 16MiB --> I[GFP_KERNEL | GFP_DMA]
    H -- Yes, < 4GiB --> J[GFP_KERNEL | GFP_DMA32]
    H -- No --> K[Done: use GFP_KERNEL]
```

---

## 5. Examples

```c
/* Normal process context — use GFP_KERNEL */
buf = kmalloc(512, GFP_KERNEL);

/* IRQ handler — use GFP_ATOMIC */
buf = kmalloc(64, GFP_ATOMIC);

/* DMA coherent buffer — use GFP_DMA */
buf = kmalloc(PAGE_SIZE, GFP_KERNEL | GFP_DMA);

/* Zero-filled */
buf = kmalloc(256, GFP_KERNEL | __GFP_ZERO);
/* or equivalently: */
buf = kzalloc(256, GFP_KERNEL);

/* Critical: must not fail */
buf = kmalloc(PAGE_SIZE, GFP_KERNEL | __GFP_NOFAIL);
```

---

## 6. What happens on failure?

- `GFP_KERNEL`: OOM killer invoked if memory tight, allocator tries reclaim
- `GFP_ATOMIC`: Returns NULL immediately if no free pages
- `__GFP_NOFAIL`: Retries forever — use only when truly necessary

```c
ptr = kmalloc(size, GFP_KERNEL);
if (!ptr)
    return -ENOMEM;  /* Always handle NULL return */
```

---

## 7. Source Files

| File | Description |
|------|-------------|
| `include/linux/gfp.h` | Flag definitions |
| `mm/page_alloc.c` | Allocator logic |
| `mm/oom_kill.c` | OOM killer |

---

## 8. Related Concepts
- [02_Buddy_Allocator.md](./02_Buddy_Allocator.md) — Pages returned by alloc_pages()
- [04_kmalloc_And_vmalloc.md](./04_kmalloc_And_vmalloc.md) — Uses these flags
- [01_Pages_And_Zones.md](./01_Pages_And_Zones.md) — Zones selected by GFP flags
