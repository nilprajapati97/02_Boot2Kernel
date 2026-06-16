# kasan_init() — Detailed Design

## 1. What Is KASAN?

KASAN (Kernel Address Sanitizer) is a dynamic memory safety tool for the Linux kernel. It detects:
- **Use-after-free**: Accessing memory after it has been freed
- **Heap buffer overflow**: Writing past the end of a kmalloc'd buffer
- **Stack buffer overflow**: Writing past a local variable
- **Global buffer overflow**: Writing past a global array

KASAN works by maintaining a **shadow memory** region — a compact representation that records whether each byte of kernel memory is accessible. Every memory access in kernel code is instrumented (by the compiler) to check the corresponding shadow memory byte before executing.

---

## 2. Shadow Memory Concept

```
Normal kernel memory (1 byte):
  [B][B][B][B][B][B][B][B]   ← 1 byte = 8 bits

Shadow memory (1 byte represents 8 kernel bytes):
  The shadow byte encodes how many of the 8 bytes are valid:
  Shadow = 0    → all 8 bytes valid
  Shadow = N    → first N bytes valid (1 ≤ N ≤ 7)
  Shadow < 0    → all 8 bytes INVALID (various error codes)

Shadow memory ratio: 1:8 (1 shadow byte per 8 kernel bytes)
For 1GB kernel address space: 128MB shadow memory needed
```

**Shadow error codes (negative values):**
```c
#define KASAN_FREE_PAGE         0xFF  /* page freed */
#define KASAN_PAGE_REDZONE      0xFE  /* page allocator redzone */
#define KASAN_KMALLOC_REDZONE   0xFC  /* kmalloc redzone */
#define KASAN_KMALLOC_FREE      0xFB  /* kmalloc freed */
#define KASAN_GLOBAL_REDZONE    0xF9  /* global variable redzone */
#define KASAN_STACK_LEFT        0xF1  /* stack left redzone */
#define KASAN_STACK_RIGHT       0xF3  /* stack right redzone */
```

---

## 3. Compiler Instrumentation

KASAN requires compiler support (`-fsanitize=kernel-address`). The compiler inserts checks before every load and store:

```c
/* Original kernel code: */
int *ptr = kmalloc(4, GFP_KERNEL);
*ptr = 42;

/* After KASAN instrumentation (conceptual): */
int *ptr = kmalloc(4, GFP_KERNEL);
/* Compiler inserts: */
void *shadow = (void *)((unsigned long)ptr >> 3) + KASAN_SHADOW_OFFSET;
if (*shadow != 0) {
    kasan_report(ptr, sizeof(*ptr), /*is_write=*/true, _RET_IP_);
}
*ptr = 42;
```

The `>> 3` is the 1:8 ratio. `KASAN_SHADOW_OFFSET` maps kernel addresses to shadow addresses.

---

## 4. kasan_init() Position in ARM32 Boot

```
setup_arch():
  ├── paging_init()          ← permanent page tables built
  │     └── bootmem_init()  ← struct page array and zones created
  └── kasan_init()           ← *** THIS FUNCTION ***
        │ KASAN depends on:
        │   - Permanent page tables (to map shadow memory)
        │   - struct page array (to handle shadow page allocation)
        ├── kasan_map_shadow()
        │     Map shadow memory region with read/write permissions
        └── kasan_populate_zero_shadow()
              Map unmapped shadow with read-only zero page
```

KASAN cannot run before `paging_init()` because it needs to:
1. Map a large shadow memory region (requires page table operations)
2. Allocate physical pages for shadow memory (requires memblock/struct page)

---

## 5. kasan_init() Source (ARM32) — arch/arm/mm/kasan_init.c

```c
void __init kasan_init(void)
{
    kasan_map_populate(KASAN_SHADOW_START, KASAN_SHADOW_END,
                       num_online_nodes());

    kasan_populate_zero_shadow((void *)KASAN_SHADOW_START,
                               kasan_mem_to_shadow((void *)MODULES_VADDR));

    for_each_mem_range(i, &start, &end) {
        void *shadow_start = kasan_mem_to_shadow((void *)(unsigned long)start);
        void *shadow_end   = kasan_mem_to_shadow((void *)(unsigned long)end);
        kasan_map_populate((unsigned long)shadow_start,
                           (unsigned long)shadow_end,
                           NUMA_NO_NODE);
    }

    kasan_populate_zero_shadow(kasan_mem_to_shadow((void *)arm_lowmem_limit),
                               (void *)KASAN_SHADOW_END);

    memset(kasan_early_shadow_page, KASAN_FREE_PAGE, PAGE_SIZE);
    init_task.kasan_depth = 0;
    pr_info("KernelAddressSanitizer initialized\n");
}
```

### Steps:

**1. Map shadow for kernel modules area**: The modules area (MODULES_VADDR to PAGE_OFFSET) needs shadow memory because kernel modules allocate from there.

**2. For each memblock region**: Map the actual shadow pages. Physical pages are allocated from memblock for the shadow memory.

**3. Zero shadow for unmapped areas**: The range from `arm_lowmem_limit` to `KASAN_SHADOW_END` doesn't correspond to real memory — it's mapped to a single shared zero page (read-only). Accesses to shadow of non-existent memory return 0 = "all valid", which is acceptable (these addresses aren't used anyway).

**4. Fill early shadow with KASAN_FREE_PAGE**: The initial shadow page (used before kasan_init) is filled with the "freed page" poison value, so any access via the early shadow is flagged as a use-after-free.

**5. init_task.kasan_depth = 0**: Each task has a `kasan_depth` counter. When > 0, KASAN checks are disabled (used for internal KASAN code to avoid recursive checks). Reset to 0 for the init task enables checks.

---

## 6. The KASAN Shadow Memory Layout

For ARM32 with PAGE_OFFSET = 0xC0000000:

```
Kernel VA space:    0xC0000000 – 0xFFFFFFFF (1GB kernel VA)
                    = 0x40000000 bytes

Shadow ratio: 1:8
Shadow size: 0x40000000 / 8 = 0x08000000 = 128MB

KASAN_SHADOW_OFFSET = KASAN_SHADOW_START - (0xC0000000 >> 3)
                    = shadow VA mapping formula

Shadow VA range: KASAN_SHADOW_START to KASAN_SHADOW_START + 128MB
```

The shadow is placed in the vmalloc region (between VMALLOC_START and VMALLOC_END). This is a significant use of the already-constrained vmalloc window on ARM32.

---

## 7. Performance Impact

KASAN adds significant overhead:
- **CPU overhead**: 2-3x slowdown (every memory access has a branch for shadow check)
- **Memory overhead**: 1/8 of kernel VA space for shadow memory (128MB for 1GB kernel VA)
- **Cache pressure**: Shadow memory accesses add cache misses

KASAN is a **debug tool** — enabled in `allmodconfig` or debug kernels, disabled in production:
```
CONFIG_KASAN=y         → enable KASAN
CONFIG_KASAN=n         → disable (default for production)
```

---

## 8. Interview Q&A

**Q1: Why must kasan_init() run after paging_init()?**
> KASAN needs to map a large shadow memory region (128MB on ARM32) into kernel virtual space. Mapping requires page table operations (`set_pte()`, `set_pmd()`) and physical page allocation for PTE pages. Both require the permanent page table infrastructure established by `paging_init()`. Additionally, KASAN's shadow memory allocation uses `memblock_alloc()` — still valid post-paging_init — to get physical pages for the shadow region. Before `paging_init()`, neither the permanent page tables nor the physical page allocator (in its memblock form) are fully set up.

**Q2: How does KASAN detect use-after-free without runtime overhead for every allocation?**
> KASAN uses "shadow poisoning": when `kfree()` is called, it writes a poison value (KASAN_FREE_PAGE or KASAN_KMALLOC_FREE) to the freed region's shadow bytes. When code later reads or writes that memory, the compiler-inserted instrumentation reads the shadow byte, finds the poison value, and calls `kasan_report()`. The overhead is paid on the access (check the shadow byte), not on the free. This is efficient because frees are rare compared to accesses. The poison byte also identifies the type of violation (freed page vs freed kmalloc vs stack redzone).

**Q3: What is the kasan_depth counter in task_struct?**
> `kasan_depth` is a per-task counter that disables KASAN checks when non-zero. It's used by KASAN's internal code that accesses memory without wanting recursive checks: when `kasan_report()` itself accesses memory (to print the report), it increments `kasan_depth` to prevent the shadow check from triggering again. Similarly, `memset()` calls in KASAN's own `kasan_poison_memory()` function disable checks. Functions that must access memory known to be invalid (like error handlers) can use `kasan_disable_current()` / `kasan_enable_current()` to bracket the access.

**Q4: How does KASAN's zero shadow page optimization work?**
> Large parts of the kernel VA space have no corresponding real memory. Mapping full shadow pages for these would waste memory. Instead, KASAN maps all unmapped VA regions to a single shared read-only zero page (`kasan_zero_page`). A shadow read from any unmapped kernel VA returns 0 = "all bytes valid". This is safe because unmapped kernel VAs are not used for allocations, so the "always valid" shadow value won't suppress real bugs. Only memory regions corresponding to actual memblock RAM get real shadow pages.
