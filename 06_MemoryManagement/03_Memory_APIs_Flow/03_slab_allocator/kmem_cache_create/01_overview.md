# kmem_cache_create — Overview

> Linux **6.6 LTS** · ARM64 · SLUB allocator
> Header: `<linux/slab.h>` · Source: [`mm/slab_common.c:kmem_cache_create`](https://elixir.bootlin.com/linux/v6.6/source/mm/slab_common.c#L375)

---

## 1. One-line definition

`kmem_cache_create()` creates a **named, fixed-size object cache** on top of the SLUB allocator — a `struct kmem_cache *` that subsequent `kmem_cache_alloc`/`kmem_cache_free` calls use to get and return objects of exactly that size, alignment, and constructor semantics.

---

## 2. Prototype family

```c
/* include/linux/slab.h */
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
                                     unsigned int align, slab_flags_t flags,
                                     void (*ctor)(void *));

struct kmem_cache *kmem_cache_create_usercopy(const char *name,
                                              unsigned int size, unsigned int align,
                                              slab_flags_t flags,
                                              unsigned int useroffset, unsigned int usersize,
                                              void (*ctor)(void *));

void kmem_cache_destroy(struct kmem_cache *s);
int  kmem_cache_shrink(struct kmem_cache *s);
```

Both `kmem_cache_create` and `_usercopy` funnel into `__kmem_cache_create_args()` in 6.6.

---

## 3. Parameters

| Param         | Meaning                                                                              |
|---------------|--------------------------------------------------------------------------------------|
| `name`        | String for `/proc/slabinfo` and `/sys/kernel/slab/<name>/`. Must be globally unique. |
| `size`        | Object size in bytes. Will be rounded up internally to `align` and SLUB metadata.    |
| `align`       | Required alignment. 0 → default (`ARCH_KMALLOC_MINALIGN` = 8 on arm64).               |
| `flags`       | SLUB behavior flags (see table below).                                               |
| `ctor`        | Optional constructor invoked on each newly allocated object (used by inode/dentry caches). |

### 3.1 Common `flags`

| Flag                       | Effect                                                                |
|----------------------------|-----------------------------------------------------------------------|
| `SLAB_HWCACHE_ALIGN`       | Round alignment up to L1 cache-line size (64 B on arm64). Prevents false sharing. |
| `SLAB_PANIC`               | If creation fails, panic (used during early boot for critical caches). |
| `SLAB_RECLAIM_ACCOUNT`     | Accounts pages as reclaimable in `/proc/meminfo` (`SReclaimable`).    |
| `SLAB_TYPESAFE_BY_RCU`     | Objects may be re-used while RCU readers still hold pointers; safe because the **type** of the object doesn't change. |
| `SLAB_CACHE_DMA`           | Use `ZONE_DMA` for backing pages.                                     |
| `SLAB_CACHE_DMA32`         | Use `ZONE_DMA32`.                                                     |
| `SLAB_ACCOUNT`             | Memcg accounting (objcg-based).                                       |
| `SLAB_NO_MERGE`            | Prevent the SLUB cache-merge optimization from folding this cache into another. |
| `SLAB_POISON`              | Pre-fill freed objects with `0x6b` (debug).                           |
| `SLAB_RED_ZONE`            | Place red zones around objects (debug).                                |
| `SLAB_STORE_USER`          | Record the alloc/free call stacks (debug).                            |

---

## 4. Why a dedicated cache vs. `kmalloc`?

| Reason                                          | Dedicated cache | `kmalloc-N` bucket |
|-------------------------------------------------|-----------------|--------------------|
| Objects have a custom constructor               | required        | impossible         |
| `SLAB_TYPESAFE_BY_RCU` semantics                | required        | impossible         |
| Objects benefit from cache-aligned packing      | yes             | no                 |
| Want named visibility in `/proc/slabinfo`       | yes             | folded             |
| Need `kmem_cache_shrink()` to drop empty slabs  | yes             | indirect           |
| Avoid sharing freelist with unrelated callers (security) | yes (`SLAB_NO_MERGE`) | no |
| Constant odd size (e.g., 144 B)                 | tight packing    | rounds to 192 B    |

Kernel uses dedicated caches for `inode_cache`, `dentry`, `task_struct`, `mm_struct`, `vm_area_struct`, `filp`, `kernfs_node_cache`, etc.

---

## 5. SLUB cache merging

By default ([`CONFIG_SLAB_MERGE_DEFAULT=y`](https://elixir.bootlin.com/linux/v6.6/source/mm/Kconfig)), SLUB merges new caches into compatible existing caches to reduce internal fragmentation. Two caches merge if their (size, align, flags) match and neither has `SLAB_NO_MERGE`, a constructor, or `SLAB_TYPESAFE_BY_RCU`. The merged cache shows aliases in `/sys/kernel/slab/<canonical>/aliases`.

Boot parameter to disable globally: `slab_nomerge`.

---

## 6. Context constraints

- **Sleepable**. Takes `slab_mutex` and may allocate page tables / sysfs entries.
- Typically called once, at module init.
- **Never** call from atomic context or with locks held.

---

## 7. Lifecycle

```
   module_init()
     -> my_cache = kmem_cache_create("my_obj", sizeof(struct my_obj),
                                     0, SLAB_HWCACHE_ALIGN, NULL);

   ... in steady state:
     -> obj = kmem_cache_alloc(my_cache, GFP_KERNEL);
     -> kmem_cache_free(my_cache, obj);

   module_exit()
     -> kmem_cache_destroy(my_cache);   // see ../kmem_cache_destroy/
```

---

## 8. Failure modes

| Cause                                            | Return / behavior                       |
|--------------------------------------------------|-----------------------------------------|
| Duplicate `name`                                 | WARN + NULL (SLUB allows but warns).    |
| `size == 0` or > `KMALLOC_MAX_SIZE`              | NULL.                                   |
| `align` not a power of two                       | rounded up.                             |
| OOM during cache descriptor allocation           | NULL.                                   |
| `SLAB_PANIC` set and creation fails              | kernel panic.                           |

---

## 9. Minimal usage

```c
static struct kmem_cache *my_cache;

static int __init my_init(void)
{
    my_cache = kmem_cache_create("my_obj",
                                 sizeof(struct my_obj),
                                 0,
                                 SLAB_HWCACHE_ALIGN | SLAB_RECLAIM_ACCOUNT,
                                 NULL);
    if (!my_cache)
        return -ENOMEM;
    return 0;
}

static void __exit my_exit(void)
{
    kmem_cache_destroy(my_cache);
}
```

With a constructor (rare — historic SLAB-era pattern, still supported):

```c
static void my_ctor(void *p)
{
    struct my_obj *o = p;
    INIT_LIST_HEAD(&o->list);
    spin_lock_init(&o->lock);
}
my_cache = kmem_cache_create("my_obj", sizeof(*o), 0, 0, my_ctor);
```

Note: the constructor runs on **slab creation** (a whole page of objects at once), not per allocation. Each `kmem_cache_alloc` returns an already-constructed object — modify before free, and the next user sees those modifications. Most modern code skips the constructor and explicitly initializes after `kmem_cache_alloc`.

---

## 10. Cross-references

- Internals → [02_internals.md](02_internals.md)
- ARM64 call flow → [03_arm64_callflow.md](03_arm64_callflow.md)
- Memory map → [04_memory_map.md](04_memory_map.md)
- Interview Q&A → [05_interview_qna.md](05_interview_qna.md)
- Alloc/free siblings → [`../kmem_cache_alloc/`](../kmem_cache_alloc/), [`../kmem_cache_free/`](../kmem_cache_free/), [`../kmem_cache_destroy/`](../kmem_cache_destroy/)
