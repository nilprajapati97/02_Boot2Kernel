# kmem_cache_create — Memory Map (ARM64)

> Linux 6.6 · ARM64 · 48-bit VA · 4 KB pages.
> Cache **creation** itself doesn't move much memory — the heavy lifting
> (slab pages, objects) happens on the first `kmem_cache_alloc`. This
> document maps where everything lands.

---

## 1. The pieces of a created cache

```
+----------------------------+    +-----------------------------+
| struct kmem_cache (one)    |    | struct kmem_cache_cpu       |
| - in slab_caches list      |    | - one per CPU               |
| - in linear map (slab)     |    | - in percpu area (linear)   |
| - allocated from           |    | - tpidr_el1-relative        |
|   "kmem_cache" cache-of-   |    +-----------------------------+
|   caches                   |
|                            |    +-----------------------------+
|                            |    | struct kmem_cache_node      |
|                            |    | - one per NUMA node         |
|                            |    | - linear map (slab)         |
|                            |    | - allocated from            |
|                            |    |   "kmem_cache_node" cache    |
+----------------------------+    +-----------------------------+
                |
                v   (later, on first alloc)
+--------------------------------------------+
| Slab page(s) of 2^order pages              |
| - in linear map                            |
| - struct page (in vmemmap region) tagged   |
|   PG_slab, slab->slab_cache = s             |
| - contains N objects + free pointers       |
+--------------------------------------------+
```

---

## 2. Where the descriptor lives

```
  ARM64 kernel VA
  +----------------------------------+ 0xffff_ffff_ffff_ffff
  | FIXMAP / PCI / VMEMMAP / KASAN   |
  +----------------------------------+
  | MODULES area                     |  module text/data for loadable modules
  +----------------------------------+
  | VMALLOC area                     |
  +----------------------------------+ VMALLOC_START
  | LINEAR MAP   <- slab pages live here, including:                       |
  |               struct kmem_cache descriptors,                           |
  |               struct kmem_cache_node descriptors,                      |
  |               struct kmem_cache_cpu (via percpu base region),          |
  |               and the actual slab pages with objects.                  |
  |                                                                        |
  |   PAGE_OFFSET = 0xffff_8000_0000_0000                                  |
  +----------------------------------+
```

---

## 3. Per-CPU layout

Per-CPU data on arm64 is accessed as `tpidr_el1 + offset_in_per_cpu_section`. At boot, the kernel reserves a per-CPU chunk region and `tpidr_el1` is set per-CPU. The `__percpu` annotation makes the compiler/linker put the symbol's offset (not address) in the per-cpu section; access is:

```
   var = this_cpu_ptr(&s->cpu_slab)
     => x_tmp = tpidr_el1 + offsetof(per_cpu_chunk, cpu_slab) + (s->cpu_slab as offset)
```

`s->cpu_slab` itself stores an *offset*, not a pointer; `this_cpu_ptr` adds `tpidr_el1` at runtime.

---

## 4. Where the slab pages come from

When the first `kmem_cache_alloc(s, ...)` triggers `new_slab()`:

```
  alloc_pages(gfp, oo_order(s->oo))
     -> buddy in zone (ZONE_NORMAL on arm64 server typically)
     -> 2^order contiguous pages
     -> already mapped in linear map (no PTE work)
     -> struct page[] (in vmemmap) tagged PG_slab
```

For a 128-byte cache on a 4K-page arm64 system, `oo.order` is typically 0 or 1 (1–2 pages per slab). For larger objects (e.g., `task_struct` ~8 KB), order may be 2 or 3.

---

## 5. The vmemmap entry

Each page of RAM has a `struct page` in the vmemmap region:

```
  VMEMMAP_START + pfn * sizeof(struct page)
```

For a slab page, `struct page` is overlaid with `struct slab` ([`mm/slab.h`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab.h#L11)):

```c
struct slab {
    unsigned long __page_flags;
    struct kmem_cache *slab_cache;   /* back-pointer to s */
    struct slab *next;
    int slabs;                       /* on a CPU partial chain */
    union {
        struct list_head slab_list;  /* on n->partial */
        struct rcu_head rcu_head;    /* SLAB_TYPESAFE_BY_RCU */
    };
    void *freelist;
    union { unsigned long counters; struct { unsigned inuse:16; unsigned objects:15; unsigned frozen:1; }; };
    ...
};
```

So finding the owning cache from any object pointer is:

```
   obj -> folio -> struct slab -> slab->slab_cache
```

O(1), no lookup tables.

---

## 6. After `kmem_cache_create` returns (steady state)

```
   slab_caches:  c_kmalloc-8 -> c_kmalloc-16 -> ... -> c_my_obj (new) -> ...

   /proc/slabinfo:
     my_obj    0   0   128   32   1 : tunables ...
            (active, num, size, objperslab, pagesperslab)

   /sys/kernel/slab/my_obj/   <- sysfs entries appear
```

No slab pages allocated yet — "0 active, 0 total".

---

## 7. After the first allocation

```
  /proc/slabinfo:
    my_obj    1    32   128   32   1
                  ^^   one slab of 32 objects exists

  Physical layout:
     one contiguous page in linear map, divided into 32 × 128-byte slots,
     freelist linked, one slot in use.
```

---

## 8. Verify on a live system

```text
# Inspect a cache after creation:
$ cat /sys/kernel/slab/my_obj/object_size
128
$ cat /sys/kernel/slab/my_obj/order
0
$ cat /sys/kernel/slab/my_obj/cpu_slabs
# - active and partial per-CPU slab counts

# See merged aliases:
$ ls /sys/kernel/slab/ | head
$ cat /sys/kernel/slab/dentry/aliases

# Per-cache stats:
$ cat /proc/slabinfo | head
```

---

## 9. Cross-references

- Cache internals → [02_internals.md](02_internals.md)
- Alloc memory map → [`../kmem_cache_alloc/04_memory_map.md`](../kmem_cache_alloc/04_memory_map.md)
- Destroy memory map → [`../kmem_cache_destroy/04_memory_map.md`](../kmem_cache_destroy/04_memory_map.md)
- kmalloc bucket layout for comparison → [`../../01_basic_allocation/kmalloc/04_memory_map.md`](../../01_basic_allocation/kmalloc/04_memory_map.md)
