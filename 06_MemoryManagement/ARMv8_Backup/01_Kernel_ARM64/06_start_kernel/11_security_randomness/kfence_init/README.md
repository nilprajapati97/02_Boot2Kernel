# `kfence_init()` — Kernel Electric Fence

## Purpose

Initializes KFENCE (Kernel Electric Fence), a sampling-based memory safety detector for the kernel heap. KFENCE detects use-after-free, out-of-bounds access, and invalid-free bugs in kernel code with very low overhead.

## Source File

`mm/kfence/core.c`

## The Problem: Heap Safety

Bugs in kernel heap allocation are severe:
- **Use-after-free**: Access memory after `kfree()` — can read stale data or be exploited for privilege escalation
- **Heap overflow**: Write past the end of an allocation — can corrupt adjacent objects

KASAN (Kernel Address SANitizer) catches these but with ~2x performance overhead. KFENCE provides ~5% overhead by using statistical sampling.

## How KFENCE Works

KFENCE maintains a pool of "KFENCE objects" — special allocations surrounded by guard pages:

```
Memory layout of a KFENCE object:

[Guard Page (unmapped)] [Object Allocation] [Guard Page (unmapped)]
     ↑                        ↑                     ↑
   Any access            Returned to              Any access
   here = PAGE FAULT      caller                here = PAGE FAULT
```

### Sampling

KFENCE intercepts `kmalloc()` allocations with a configurable probability (default: every 100 allocations):

```c
void *kmalloc(size_t size, gfp_t flags)
{
    if (should_alloc_kfence())  // 1-in-100 chance
        return kfence_alloc(size, flags);
    return slab_alloc(size, flags);
}
```

### Detection

When a KFENCE-allocated object is:
- **Freed**: The object is poisoned with a pattern (0xCC by default)
- **Accessed after free**: The guard page trap fires → kernel bug report
- **Written beyond bounds**: Guard page trap → kernel bug report

## Bug Report Example

```
==================================================================
BUG: KFENCE: use-after-free read in test_use_after_free+0x28/0x44

Use-after-free read at 0xffff8d3cffa0a000 (in kfence-#0):
 test_use_after_free+0x28/0x44
 kfence_test_init+0x388/0x3d0

kfence-#0 [0xffff8d3cffa0a000-0xffff8d3cffa0a007, size=8, cache=kmalloc-8] allocated by task 1:
 test_alloc+0x5c/0x164
 kfence_test_init+0x208/0x3d0

freed by task 1:
 test_free+0x20/0x3c
 kfence_test_init+0x2c4/0x3d0
==================================================================
```

## KFENCE Pool

```c
void __init kfence_init(void)
{
    if (!kfence_sample_interval)
        return;
    
    // Allocate the KFENCE object pool
    kfence_pool = alloc_pages_exact(KFENCE_POOL_SIZE, GFP_KERNEL);
    
    // Mark guard pages as unmapped
    kfence_protect_page(addr, false);  // Remove PTE
    
    // Start the KFENCE timer for periodic sampling
    setup_timer(&kfence_timer, toggle_allocation_gate, 0);
}
```

Default pool size: 255 objects × (1 page object + 2 guard pages) = 765 pages = ~3MB.

## Kconfig

- `CONFIG_KFENCE`: Enable KFENCE
- `kfence.sample_interval=N`: Sample every N allocations (default 100, 0 = disabled)

```bash
# Increase sampling for testing:
echo 1 > /sys/module/kfence/parameters/sample_interval
```

## Cross-references

- [Phase overview](../README.md)
- `random_init()`: [../random_init/README.md](../random_init/README.md) — KFENCE uses random patterns
