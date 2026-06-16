# KASAN Deep Dive — Kernel Address Sanitizer

**Category**: Debugging and Profiling Tools  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
KASAN (Kernel Address Sanitizer): detects memory safety bugs in kernel
  Catches:
    - Use-after-free (accessing freed memory)
    - Heap out-of-bounds (read/write past end of allocation)
    - Stack out-of-bounds
    - Global variable out-of-bounds
    - Use-before-initialization (with CONFIG_KASAN_EXTRA_STACK_CHECK)
  
  Does NOT catch:
    - Integer overflows (use UBSAN for that)
    - Race conditions (use KCSAN for that)
    - Null pointer dereferences (covered by MMU faults)
  
Three KASAN modes:

1. Generic KASAN (software shadow memory):
   CONFIG_KASAN_GENERIC
   Uses: 1 shadow byte per 8 bytes of kernel memory
   Shadow mapping: every 8-byte kernel address → 1 shadow byte
   Overhead: 8× memory overhead + ~2× CPU overhead (instrumentation)
   Best for: comprehensive kernel development/testing

2. Software Tag-Based KASAN (KASAN_SW_TAGS):
   CONFIG_KASAN_SW_TAGS
   Uses: pointer tags in bits[63:56] (like MTE concept but software)
   ARM64: uses Top Byte Ignore (TBI) feature
   Overhead: ~15% CPU, pointer tag management per allocation
   Catches: use-after-free (tag mismatch)

3. Hardware Tag-Based KASAN (KASAN_HW_TAGS):
   CONFIG_KASAN_HW_TAGS
   Uses: ARM64 MTE hardware for tag checking
   Zero shadow memory overhead
   Overhead: ~0-3% (MTE is near-zero overhead)
   Requires: ARMv8.5-A MTE (FEAT_MTE2)
   Best for: production-like testing, performance-sensitive environments
```

---

## 2. Generic KASAN Shadow Memory

```
Shadow memory layout:
  Every 8 bytes of kernel memory (8-byte "cell") maps to 1 shadow byte
  
  Kernel virtual address space:
    [kernel_addr] → shadow_byte = *(addr >> 3) + KASAN_SHADOW_OFFSET
  
  KASAN_SHADOW_OFFSET (ARM64):
    Shadow base = KASAN_SHADOW_START
    For 48-bit VA: shadow area = [0xFFFF_F000_0000_0000, ...)
    KASAN_SHADOW_OFFSET chosen so that:
      kernel_addr >> 3 + KASAN_SHADOW_OFFSET = shadow_byte_addr
    
    Example:
      kernel_addr = 0xFFFF_8000_1234_5678
      shadow_addr = (0xFFFF_8000_1234_5678 >> 3) + KASAN_SHADOW_OFFSET
                  = 0x001F_FF00_0246_8ACF + KASAN_SHADOW_OFFSET

Shadow byte encoding (generic KASAN):
  Value  | Meaning
  -------|--------------------------------------------------
  0x00   | All 8 bytes VALID (accessible)
  0x01   | First 1 byte valid, bytes [1..7] poisoned
  0x02   | First 2 bytes valid
  ...    | ...
  0x07   | First 7 bytes valid, byte [7] poisoned
  0xF1   | KASAN_STACK_LEFT  (left redzone on stack)
  0xF2   | KASAN_STACK_MID   (stack redzone between vars)
  0xF3   | KASAN_STACK_RIGHT (right redzone on stack)
  0xF5   | KASAN_STACK_PARTIAL (partial stack frame)
  0xFA   | KASAN_KMALLOC_REDZONE (padding after kmalloc)
  0xFB   | KASAN_KMALLOC_FREE (freed kmalloc object — use-after-free)
  0xFC   | KASAN_ALLOCA_LEFT
  0xFD   | KASAN_ALLOCA_RIGHT
  0xFE   | KASAN_GLOBAL_REDZONE (after global variable)
  0xFF   | KASAN_PAGE_REDZONE (page allocator redzone) / freed page
  
  Most common patterns:
  0x00: completely valid → no error
  0xFB: KMALLOC_FREE → use-after-free bug caught!
  0xFA: past-end-of-allocation → heap overflow caught!

KASAN check instrumentation:
  GCC/Clang with -fsanitize=kernel-address:
  
  Before every load/store, compiler inserts:
    // For 8-byte load at addr:
    shadow = *((addr >> 3) + KASAN_SHADOW_OFFSET);
    if (shadow != 0) {
        if ((addr & 7) + access_size > shadow) {
            kasan_report(addr, size, is_write, return_address);
        }
    }
  
  This is ~2-4 additional instructions per memory access
  Unoptimized: 2× slowdown
  Production optimized (outline calls): ~1.3× slowdown
```

---

## 3. kasan_report() — Crash Output Format

```
Example KASAN report for use-after-free:

==================================================================
BUG: KASAN: use-after-free in my_driver_read+0x144/0x200 [my_driver]
Read of size 8 at addr ffff888012345678 by task systemd/1234
                 ↑ ACCESS TYPE   ↑ ADDRESS             ↑ TASK/PID

CPU: 3 PID: 1234 Comm: systemd Not tainted 6.6.0-arm64 #1
Hardware name: Neoverse N2 Server (DT)
Call trace:
 dump_backtrace+0x0/0x1e0
 show_stack+0x18/0x24
 dump_stack+0x10c/0x164
 kasan_report+0xf4/0x160        ← KASAN report function
 __asan_load8+0x78/0xb0         ← instrumented load
 my_driver_read+0x144/0x200     ← BUGGY ACCESS SITE

Allocated by task 1234:
 kasan_kmalloc+0xb8/0xc0        ← original allocation
 my_driver_alloc+0x48/0x60      ← allocation site
 driver_probe+0x1c/0x40

Freed by task 1234:
 kasan_kfree_large+0x30/0x50    ← free site
 my_driver_cleanup+0x34/0x78    ← freed here
 driver_remove+0x10/0x2c

The buggy address belongs to the object at ffff888012345600
 which belongs to the cache kmalloc-256 of size 256
The buggy address is located 120 bytes inside of
 256-byte region [ffff888012345600, ffff888012345700)
                  ↑ object start    ↑ object end

The buggy address ffff888012345678 is located in stack of task systemd/1234
 at offset 120 from the object

Memory state around the buggy address:
 ffff888012345600: fb fb fb fb fb fb fb fb   ← fb = KMALLOC_FREE (all freed)
 ffff888012345640: fb fb fb fb fb fb fb fb
>ffff888012345678: fb fb fb fb fb fb fb fb   ← ACCESS HERE (fb = freed!)
                          ^
 ffff8880123456c0: fb fb fb fb fb fb fb fb
 ffff888012345700: fc fc fc fc fc fc fc fc   ← fc = KMALLOC_PADDING

Reading the memory state:
  Each row: 16 shadow bytes = covers 128 bytes of kernel memory
  '>' marks the row containing the bug address
  '^' points to the exact shadow byte
  'fb' (0xfb) = KMALLOC_FREE = this memory was freed → use-after-free!
```

---

## 4. KASAN Hardware Tag-Based (KASAN_HW_TAGS)

```
Uses ARM64 MTE (Memory Tagging Extension) — FEAT_MTE2

Architecture:
  MTE: every 16-byte "granule" of heap has a 4-bit tag in Tag RAM
  Pointer: bits[59:56] contain the expected tag (Top Byte = ignored by MMU)
  On every load/store: hardware checks pointer tag == memory tag
  Mismatch: synchronous Tag Check Fault (SIGSEGV or async report)

KASAN_HW_TAGS implementation:
  kasan_alloc_pages() / kasan_kmalloc():
    1. Generate random 4-bit tag: irg(X0, X1) instruction
    2. Store tag in memory granule: stg instruction (tag store)
    3. Return tagged pointer: pointer.bits[59:56] = tag
    4. Redzone: adjacent granules tagged with different (wrong) tag
       → access past end → tag mismatch → fault

  kasan_kfree() / kasan_free_pages():
    1. Set memory tag to 0xF (reserved "freed" indicator):
       stzg [addr], nzreg  (store zero-to-granule with tag 0xF)
       OR: stg [addr], nzreg with tag = random_new != old_tag
    2. Tagged pointer returned to caller has old tag
    3. Any access via old (now-freed) pointer: tag mismatch → fault

Advantages over generic KASAN:
  - No shadow memory (saves 1/8 of memory)
  - No compiler instrumentation overhead (hardware checks every access)
  - Near-zero CPU overhead (~0-3% vs ~100-200% for generic)
  - Works in production! (unlike generic KASAN which is dev-only)

MTE mode for KASAN_HW_TAGS:
  Sync mode (default for KASAN):
    TCF = SYNC: fault raised synchronously on tag mismatch
    Useful: crash immediately at bug site (easy to debug)
  
  Async mode (for low overhead):
    TCF = ASYNC: mismatch recorded in TFSR_EL1, fault raised later
    Less accurate location, but even lower overhead

KASAN_HW_TAGS vs software MTE:
  Same hardware, different use:
  - KASAN_HW_TAGS: for KERNEL memory safety
  - MTE PROT_MTE: for USER SPACE memory safety
  Both use the same Tag RAM and IRG/STG/LDG instructions
```

---

## 5. KASAN in the Kernel Source

```c
// lib/kasan/kasan.h + lib/kasan/generic.c + lib/kasan/hw_tags.c

// Initialization (arch/arm64/mm/kasan_init.c):
kasan_init():
    // Map shadow memory:
    kasan_map_shadow();  // map 1/8 of kernel VA as shadow
    kasan_populate_shadow(start, end, 0);  // fill with 0x00 (valid)
    
    // Poison memory below TASK_SIZE (user space) shadow as inaccessible
    kasan_poison_shadow(0, KASAN_SHADOW_SIZE, KASAN_SHADOW_GAP);

// kmalloc with KASAN (mm/slab_common.c):
kmalloc(size, gfp):
    → __kmalloc_noprof(size, gfp)
    → slab allocator returns object
    → kasan_kmalloc(cachep, objp, size, gfp):
         Unpoisoned region: [objp, objp+size) = 0x00 (valid)
         Poison redzone: [objp+size, objp+cachep->size) = 0xFA (redzone)
         Return: objp (with tag for HW_TAGS mode)

// kfree with KASAN:
kfree(ptr):
    → kasan_kfree_large(ptr) or kasan_slab_free(cache, ptr, ...)
    → Poison entire object: [ptr, ptr+size) = 0xFB (freed)
    → add to quarantine (optional, with CONFIG_KASAN_QUARANTINE):
         Delay actual freeing to buddy/slab → catch delayed use-after-free
    → After quarantine: actually free

// Quarantine (generic KASAN only):
// Freed objects NOT immediately returned to slab
// Kept in quarantine_cache[] for a while (LRU eviction)
// Purpose: catch delayed use-after-free bugs
// Flushed by: quarantine_reduce() when quarantine gets too large
```

---

## 6. Interview Q&A

**Q1: What is the shadow memory ratio in generic KASAN and how is it addressed?**
Generic KASAN uses 1 shadow byte per 8 bytes of kernel memory — a 1:8 ratio. This means if the kernel maps 1GB of memory, KASAN needs 128MB of shadow memory. The shadow is mapped at a fixed offset (`KASAN_SHADOW_OFFSET`) so that the shadow address is computable with a single shift+add: `shadow = (kernel_addr >> 3) + KASAN_SHADOW_OFFSET`. On ARM64 with 48-bit VA: the shadow region consumes approximately 1/8 of the kernel VA space. This is why generic KASAN is only for development — the memory overhead makes it impractical for production.

**Q2: How does KASAN catch use-after-free?**
When memory is freed (`kfree()`), KASAN poisons the shadow bytes of the freed region with `0xFB` (KMALLOC_FREE). Any subsequent access to that memory: the compiler-inserted check reads the shadow byte → finds `0xFB` → calls `kasan_report()`. The report includes: the access type (read/write), the address, the allocation backtrace (where the memory was originally allocated), and the free backtrace (where it was freed). The quarantine mechanism extends detection: freed objects are kept in quarantine before returning to the slab → even if the original slab slot is reallocated, use-after-free through stale pointers is still caught.

**Q3: What makes KASAN_HW_TAGS suitable for near-production use, unlike generic KASAN?**
KASAN_HW_TAGS uses ARM64 MTE hardware: (1) **No shadow memory**: MTE tags live in dedicated Tag RAM (1/16 overhead, not software-managed 1/8). (2) **No compiler instrumentation**: every load/store is hardware-checked automatically — no 2× code bloat from inline shadow checks. (3) **Near-zero CPU overhead**: MTE tag checks run in parallel with memory access (< 1 cycle extra latency on modern ARM cores). (4) **Async mode**: errors reported out-of-band (via TFSR_EL1 and SIGSEGV at a safe point) rather than faulting inline. This makes KASAN_HW_TAGS viable for performance-sensitive testing and canary-fleet deployments where some overhead is acceptable but not the 100%+ slowdown of generic KASAN.

**Q4: What information does a KASAN report include and how do you read it?**
A KASAN report contains: (1) **Bug type**: use-after-free, out-of-bounds, etc. (2) **Access details**: read or write, size, virtual address. (3) **Current task**: task name and PID. (4) **Access backtrace**: full stack trace from the bug site back through the call chain. (5) **Allocation backtrace**: where the object was originally allocated (kasan_save_alloc_stack). (6) **Free backtrace**: where the object was freed (kasan_save_free_stack). (7) **Memory state map**: hex dump of shadow bytes around the bug address, annotated with the poisoning codes. The most useful fields: the access site + the free backtrace identifies the use-after-free pair.

**Q5: How does KASAN interact with the slab allocator's quarantine?**
With `CONFIG_KASAN_QUARANTINE=y` (generic KASAN): `kfree()` does NOT immediately return the object to the slab free list. Instead: the object is added to `quarantine_cache[]` (a per-CPU LIFO list). The memory is poisoned (`0xFB`) but physically stays in the slab. Any access via stale pointer during the quarantine period: shadow check fails → KASAN report. Eventually: `quarantine_reduce()` evicts old entries (LRU) and actually frees them to the slab. This extends the detection window for use-after-free significantly. Downside: slightly increases memory usage (quarantined = "committed but unavailable"). KASAN_HW_TAGS: no quarantine needed — the tag itself prevents access (any access via old pointer = wrong tag = hardware fault).

---

## 7. Quick Reference

| Shadow Value | Meaning |
|---|---|
| `0x00` | All 8 bytes valid (accessible) |
| `0x01-0x07` | First N bytes valid, rest poisoned |
| `0xFA` | Heap redzone (past end of kmalloc) |
| `0xFB` | Freed heap (use-after-free territory) |
| `0xFC` | Slab redzone / kmalloc padding |
| `0xFD` | Large alloc left redzone |
| `0xFE` | Global variable redzone |
| `0xF1/F2/F3` | Stack redzones |

| Mode | Memory Overhead | CPU Overhead | Production? |
|---|---|---|---|
| Generic KASAN | 1/8 (shadow RAM) | ~100-200% | No |
| SW Tag-Based | Pointer tags only | ~15% | Limited |
| HW Tag-Based (MTE) | Tag RAM (~1/16) | ~0-3% | Near-prod |
