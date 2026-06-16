# kzalloc — Memory Map

> Identical placement to `kmalloc`: returned pointer lives in the
> **ARM64 linear (direct) map**, physical backing from `ZONE_NORMAL`
> (or `ZONE_DMA32` / `ZONE_DMA` if requested).
> See [`../kmalloc/04_memory_map.md`](../kmalloc/04_memory_map.md) for the
> full ASCII layout — this doc focuses on the **delta**: what guarantees the
> zero state gives you and where padding may still leak.

---

## 1. Same VA, same PA, plus a zero invariant

```
   kernel TTBR1 half (high addresses)
   ┌───────────────────────────────────────┐
   │  VMALLOC / IOREMAP / MODULES / VMEMMAP│
   ├───────────────────────────────────────┤
   │  LINEAR MAP (PAGE_OFFSET ..)          │   <-- kzalloc returns here
   │   ┌─────────────────────────────┐     │
   │   │ slab page (one or 2^order)  │     │
   │   │  ┌──────┬──────┬──────┐     │     │
   │   │  │ obj0 │ obj1 │ obj2 │ ... │ <-- kzalloc'd object: every byte 0
   │   │  │ 0..0 │ 0..0 │ 0..0 │     │     │
   │   │  └──────┴──────┴──────┘     │     │
   │   └─────────────────────────────┘     │
   └───────────────────────────────────────┘ PAGE_OFFSET = 0xffff_8000_0000_0000
```

Numeric constants, GFP→zone mapping, page attributes — all identical to kmalloc.

---

## 2. What is guaranteed zero, what isn't

```
                 |<-- object_size --->|<--- padding --->|
   bucket:       | byte0  ...  byteN-1| pad  ...  pad-M |
                 ^                    ^                 ^
                 |  GUARANTEED 0 by   |  NOT zeroed by  |
                 |   kzalloc/memset   |  kzalloc        |
```

- `object_size`  → user-visible size (what `sizeof()` returned).
- `s->size`      → bucket slot (next power-of-2 or kmalloc-96/192).
- Padding bytes from `object_size` to `s->size` are **not** cleared by `kzalloc`.

For info-leak safety when exposing the buffer to userspace, use `kzalloc + memzero_explicit(p, ksize(p))` or allocate a tighter `kmem_cache` whose `object_size == size`.

---

## 3. ARM64 page attributes (linear map slab page)

| Attribute              | Value                          |
|------------------------|--------------------------------|
| MAIR                   | `MT_NORMAL` (cacheable WB-WA)  |
| Shareability           | Inner-shareable                |
| `AP[2:1]`              | EL1 RW, EL0 no-access          |
| `UXN`, `PXN`           | both 1 (data, no exec)         |
| `nG`                   | 0 (global)                     |
| `DCZID_EL0.DZP`        | 0 → `DC ZVA` allowed (zeroing fast) |

---

## 4. Physical-side picture (unchanged from kmalloc)

```
   high PA
            +---------------------------+
            |  ZONE_NORMAL              | <-- kzalloc(GFP_KERNEL)
            +---------------------------+ 4 GB
            |  ZONE_DMA32               | <-- kzalloc(GFP_DMA32)
            +---------------------------+ ~1 MB
            |  ZONE_DMA (rare on arm64) | <-- kzalloc(GFP_DMA)
   low PA   +---------------------------+
```

The chosen zone comes from the GFP mask in `kmalloc_type(flags)` — the `__GFP_ZERO` bit does not influence zone selection.

---

## 5. Large-kzalloc (> 8 KB) on the page allocator

```
   |--- one folio (2^order pages) in linear map ---|
   | page0 | page1 | page2 | ... | page(2^order-1)  |
   | 0..0  | 0..0  | 0..0  | ... | 0..0             |   <-- cleared by prep_new_page()
```

Each page is cleared by `clear_page()` (uses `DC ZVA`) before `__kmalloc_large_node()` returns the linear-map VA. The folio is tagged so `kfree()` routes to `free_large_kmalloc()`.

---

## 6. Verify on a live system

```text
# A kzalloc'd object is indistinguishable from a kmalloc'd one in slabinfo:
$ grep kmalloc-256 /proc/slabinfo
kmalloc-256          1245   1280    256   16    1  ...

# Confirm zeroing default:
$ cat /sys/kernel/mm/init_on_alloc
1   # if CONFIG_INIT_ON_ALLOC_DEFAULT_ON=y

# DC ZVA availability:
$ grep -i 'dczid\|cpu0' /proc/cpuinfo
# (no direct sysfs; check via boot dmesg: "CPU features: detected: ARMv8.2-DCZID")
```

---

## 7. Cross-references

- Full memory map (kmalloc) → [`../kmalloc/04_memory_map.md`](../kmalloc/04_memory_map.md)
- Zeroing internals → [02_internals.md](02_internals.md)
- ARM64 `DC ZVA` discussion → [03_arm64_callflow.md](03_arm64_callflow.md)
