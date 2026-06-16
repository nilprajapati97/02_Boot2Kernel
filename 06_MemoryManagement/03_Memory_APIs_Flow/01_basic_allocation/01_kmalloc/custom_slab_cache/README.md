# Custom Slab Cache Implementation

## Overview
This module demonstrates creating and using custom slab caches in the Linux kernel for efficient memory allocation of frequently used objects.

## Key Concepts

### Slab Cache Creation
```c
struct kmem_cache *kmem_cache_create(
    const char *name,           // Cache name
    size_t size,                // Object size
    size_t align,               // Alignment (0 for default)
    unsigned long flags,        // Cache flags
    void (*ctor)(void *)        // Constructor function
);
```

### Flags
- `SLAB_HWCACHE_ALIGN` - Align objects to cache line
- `SLAB_POISON` - Fill with poison pattern for debugging
- `SLAB_RED_ZONE` - Add red zones for overflow detection
- `SLAB_PANIC` - Panic if allocation fails

### Operations
- `kmem_cache_alloc()` - Allocate object from cache
- `kmem_cache_free()` - Free object back to cache
- `kmem_cache_destroy()` - Destroy cache

## Build & Test

```bash
# Build module
make

# Load module
sudo insmod custom_slab.ko

# Check logs
dmesg | tail -20

# View cache info
cat /proc/slabinfo | grep my_cache

# Unload module
sudo rmmod custom_slab

# Clean build
make clean
```

## Expected Output
```
Creating custom slab cache
Constructor called for object at <addr>
Cache created successfully
Allocated obj1: id=1, name=Object-1
Allocated obj2: id=2, name=Object-2
Allocated obj3: id=3, name=Object-3
Objects freed back to cache
```

## Advantages
- Fast allocation/deallocation
- Reduced fragmentation
- Cache-line alignment
- Object reuse
- Constructor initialization
