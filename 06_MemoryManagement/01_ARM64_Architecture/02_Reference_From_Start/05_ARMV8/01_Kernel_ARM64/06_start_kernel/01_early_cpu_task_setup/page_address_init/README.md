# `page_address_init()` — Highmem Page Address Table

## Purpose

Initializes the `page_address_htable` hash table used on 32-bit systems with high memory (HIGHMEM). This table provides the mapping from `struct page *` to the kernel virtual address where that page is temporarily mapped (via `kmap()`).

## Source File

`mm/highmem.c`

```c
void __init page_address_init(void)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(page_address_htable); i++) {
        INIT_LIST_HEAD(&page_address_htable[i].lh);
        spin_lock_init(&page_address_htable[i].lock);
    }
}
```

## Background: HIGHMEM on 32-bit Systems

On 32-bit Linux with more than ~896 MB of RAM, not all physical memory can be directly mapped into the 1 GB kernel address space. Memory above the direct-mapping limit (called **HIGHMEM**) must be temporarily mapped using `kmap()` / `kmap_atomic()`.

When a highmem page is mapped, the kernel records the mapping in `page_address_htable` so that `page_address(page)` can quickly return the current virtual address of a highmem page.

## Relevance on 64-bit Systems

On 64-bit systems (x86-64, ARM64), this function is a **no-op** because:
- The kernel has a 128 TB virtual address space
- All physical memory is directly mapped in the `vmemmap` / `direct_map` region
- `page_address(page)` is a simple arithmetic calculation — no hash table needed

The function is still called for source code uniformity, but the implementation is empty on 64-bit builds.

## Pre-conditions

- Static `page_address_htable[]` array must be in `.bss` (zero-initialized)

## Post-conditions

- `page_address_htable[]` is initialized with empty lists and initialized spin locks
- `page_address()` is safe to call on any page

## IRQ State

IRQs **disabled** — pure static data initialization.

## Key Data Structures

| Symbol | Type | Purpose |
|--------|------|---------|
| `page_address_htable[]` | `struct page_address_slot[]` | Hash buckets mapping page → VA |
| `page_address_slot.lh` | `list_head` | Chain of `page_address_map` entries |
| `page_address_slot.lock` | `spinlock_t` | Per-bucket lock |

## Kconfig Dependencies

- `CONFIG_HIGHMEM`: Only meaningful when set (32-bit systems)
- Without `CONFIG_HIGHMEM`, this function compiles to nothing

## Cross-references

- [Phase overview](../README.md)
