# `page_address_init()` — High Memory Page Address Tracking

## Overview

| Attribute    | Value                                          |
|-------------|--------------------------------------------------|
| **Function** | `page_address_init(void)`                       |
| **Source**   | `mm/page_alloc.c`                               |
| **Config**   | Only meaningful on `CONFIG_HIGHMEM` (32-bit only)|
| **Purpose**  | Initialize the hash table that maps highmem `struct page *` to their current kernel virtual addresses |

---

## Why It Exists (32-bit Context)

On **32-bit** kernels, physical RAM can exceed the kernel's directly-addressable virtual address space (typically 896MB of a 1GB kernel VA space). Memory beyond 896MB is called **HIGHMEM** and cannot be permanently mapped into the kernel's virtual address space.

To temporarily access a highmem page, the kernel calls `kmap(page)` which:
1. Finds a free slot in the `PKMAP` area (Permanent Kernel Map)
2. Updates the page table to map the physical page there
3. Returns the virtual address

When done, `kunmap(page)` removes the mapping.

The problem: given a `struct page *` for a highmem page, how do you find its current virtual address? You need a hash table: `page → virtual_address`.

---

## 64-bit Kernels

On **64-bit** kernels, the virtual address space is enormous (128TB on x86-64). All physical memory is permanently mapped in the kernel's `direct_map` region. There is **no highmem** — `page_address_init()` is essentially a no-op:

```c
// mm/page_alloc.c
void __init page_address_init(void)
{
#if defined(CONFIG_HIGHMEM) && !defined(WANT_PAGE_VIRTUAL)
    int i;
    for (i = 0; i < ARRAY_SIZE(page_address_htable); i++) {
        INIT_LIST_HEAD(&page_address_htable[i].lh);
        spin_lock_init(&page_address_htable[i].lock);
    }
    page_address_cmd_chain_init();
#endif
}
```

Modern 64-bit systems skip the body entirely.

---

## Highmem Zones (32-bit)

```
Physical Address
  [0MB ..... 896MB]  → ZONE_NORMAL   (permanently mapped, virt = phys + PAGE_OFFSET)
  [896MB ... 4GB]   → ZONE_HIGHMEM  (not permanently mapped, need kmap)
```

---

## Interview Q&A

### Q1: Why is highmem irrelevant on 64-bit systems?
**A:** x86-64 has a 64-bit virtual address space (practically 48 or 57 bits). The kernel reserves the upper half for itself — that's 128TB of kernel virtual address space on 4-level paging. Even the most RAM-dense servers today have <8TB RAM, so all physical memory can be permanently mapped via `phys_to_virt(pfn << PAGE_SHIFT)`. The `__pa()` / `__va()` macros on x86-64 do simple arithmetic, not hash table lookups. The entire HIGHMEM complexity is a 32-bit legacy.

### Q2: What is `kmap_atomic()` and why is it different from `kmap()`?
**A:** `kmap()` can sleep (if no PKMAP slots are available, it waits). `kmap_atomic()` cannot sleep — it uses per-CPU temporary slots (fixmap area) and disables preemption + pagefaults for the duration. It's used in interrupt handlers and other atomic contexts. On 64-bit, both are no-ops since all memory is already mapped, but the API remains for portability with drivers that were written for 32-bit.
