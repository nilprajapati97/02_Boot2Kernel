# SLUB Debugging and MTE Integration

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Heap memory bugs are among the most common and dangerous:
  Use-After-Free (UAF): access memory after kfree() — most critical
  Heap Buffer Overflow: write past end of kmalloc'd buffer
  Double-Free: kfree() same object twice
  Uninitialized memory read: read before first write (info leak)

SLUB debugging tools:
  A. SLUB debug (kernel compile option + boot param): software-based
     Detects: UAF (poisoning), overflow (red zones), double-free
     Cost: ~20-50% performance overhead, extra memory per object
  
  B. KASAN (Kernel Address Sanitizer): shadow memory tracking
     Detects: all of above + out-of-bounds reads/writes
     Cost: 8× memory overhead for shadow, ~2× runtime overhead
  
  C. ARM64 MTE (Memory Tagging Extension): hardware-based
     Detects: UAF, overflow — IN HARDWARE (minimal overhead)
     Cost: ~1-3% performance overhead, 3% memory overhead (for tags)
     Available: ARMv8.5+ (Cortex-A510, Cortex-X2, Neoverse V2, etc.)

Why ARM64 has a hardware advantage:
  x86: no hardware tag support → must use KASAN (expensive shadow memory)
  ARM64 MTE: 4-bit tag per 16-byte granule
    Each pointer has 4-bit tag in bits [59:56]
    Memory granule has matching 4-bit tag (stored separately)
    Hardware checks on EVERY load/store: tag must match
    Mismatch → synchronous or asynchronous fault
    NO software overhead per access (pure hardware check)
```

---

## 2. SLUB Debug Mechanism

```
SLUB debug options (slub_debug=flags[,slabname]):
  
  F: Sanity checks (SLAB_CONSISTENCY_CHECKS)
    Check magic values on alloc/free
    Verify object is properly aligned
    Detect partial corruption
  
  Z: Red zones (SLAB_RED_ZONE)
    Add guard bytes before and after each object:
    [RED_LEFT_PAD][OBJECT][RED_RIGHT_PAD]
    On alloc: fill red zones with RED_ZONE_MAGIC = 0xbb
    On free: verify red zones are still 0xbb
    Detects: left/right overflow
    Cost: 2 × red_left_pad bytes per object (extra space)
  
  P: Poison (SLAB_POISON)
    After kfree(): fill object with POISON_FREE = 0x6b (letter 'k')
    On kmalloc: fill with POISON_INUSE = 0x5a before returning
    On next free: verify poison is still 0x6b (not modified)
    Detects: use-after-free (if UAF write happened, poison is corrupted)
    Cost: write entire object on every alloc+free (O(object_size))
  
  U: User tracking (SLAB_STORE_USER)
    Store allocation/free stack trace in each object's metadata
    Extra fields appended to object: caller_alloc, caller_free
    When UAF detected: can show BOTH allocation site AND free site
    Cost: 2 × sizeof(depot_stack_handle_t) extra per object
  
  T: Trace (SLAB_TRACE)
    Print call trace on every alloc and free
    Extremely verbose — only for debugging specific small caches

Example boot parameter:
  slub_debug=FZP,kmalloc-64  # Enable F+Z+P for kmalloc-64 only
  slub_debug=FZPU             # Enable all debug for all caches (very slow!)

Poison values:
  0x6b = 'k' = POISON_FREE   (free object marker)
  0x5a = 'Z' = POISON_INUSE  (just allocated)
  0xa5 = RED zone marker
  0xbb = RED_ZONE_MAGIC
  0xcc = object end marker (for detecting stack-based smashing)
  
  When UAF detected:
    pr_err("SLUB: Object 0x%p has been modified after being freed: ...")
    Dumps hex of corrupted object
    If SLAB_STORE_USER: shows alloc/free call traces
    Calls BUG() or panic depending on configuration
```

---

## 3. KASAN Integration

```
KASAN (CONFIG_KASAN): software shadow memory tracking for heap bugs

Hardware tag-based KASAN on ARM64 (CONFIG_KASAN_HW_TAGS):
  Uses ARM64 MTE hardware (ARMv8.5+)
  Much more efficient than software KASAN
  
  KASAN shadow memory model (software, for non-MTE ARM64):
    1 shadow byte per 8 bytes of kernel memory
    Shadow VA range: KASAN_SHADOW_START to KASAN_SHADOW_END
    On ARM64 (48-bit VA): shadow at specific VA range
    
    Shadow byte encoding:
      0: all 8 bytes accessible
      N (1-7): first N bytes accessible, rest poisoned
      0xFX: fully poisoned (negative values)
        0xFB: kmalloc red zone (right of object)
        0xF2: SLUB free zone
        0xF5: left red zone
        0xFC: kmalloc freed object
  
  KASAN check on every memory access (software KASAN):
    LDR X0, [X1]  becomes:
      MOV X2, X1 >> 3        // divide address by 8
      ADD X2, X2, SHADOW_OFFSET
      LDR W3, [X2]            // read shadow byte
      CBZ W3, .ok             // shadow 0: all bytes OK
      // check if access crosses poisoned byte:
      // ... complex check ...
      BL  kasan_report         // report error
    .ok:
      LDR X0, [X1]            // actual load
    
    Cost: ~1 extra load + branch per kernel memory access = ~2× overhead
  
  KASAN-reported bug output:
    BUG: KASAN: use-after-free in skb_put+0x88/0x90
    Read of size 4 at addr ffff000001234560 by task swapper/0
    
    Allocated by task 1234:
      kmalloc+0x40/0x80
      sk_alloc+0x48/0x1c0
      ...
    
    Freed by task 1234:
      kfree+0x20/0x60
      sk_free+0x30/0x50
      ...
```

---

## 4. ARM64 MTE for Heap Safety

```
MTE (Memory Tagging Extension): hardware-enforced memory safety on ARM64

Architecture:
  Physical memory: extra storage for 4-bit "color" tag per 16-byte granule
    (implemented in DRAM ECC bits or separate tag memory, SoC-specific)
  
  Pointer: bits [59:56] = 4-bit tag
    Tagged Pointer: 0xB500_0000_A234_5600 (tag=0xB, PA-like bits)
    Normal use: tag bits ignored in address translation (top-byte ignore/TBI)
  
  MTE check (hardware, automatic):
    When CPU executes: LDR X0, [X1]
      If MTE enabled: check that X1[59:56] == memory_tag(X1[47:0])
      Mismatch: generate Tag Check Fault
        SYNC: fault raised immediately (precise, for debugging)
        ASYNC: fault collected in register, reported later (for production)
  
  SLUB + MTE integration (CONFIG_KASAN_HW_TAGS):
    On kmem_cache_alloc():
      Generate random 4-bit tag (IRG instruction: Insert Random Tag)
      Set memory tags for all 16-byte granules of the object (STG instruction)
      Set pointer tag bits to match
      Return tagged pointer: kasan_tag_pointer(ptr, tag)
    
    On kfree():
      Assign NEW different random tag to freed object's memory
      (prevents use-after-free: old pointer has old tag, memory now has new tag)
      Set object memory tags to new tag
      The old pointer (with old tag) → tag mismatch → fault!
    
    ARM64 MTE instructions:
      IRG Xt, Xn:        Insert Random Tag (generates random tag into Xt[59:56])
      ADDG Xt, Xn, #i:   Add with Tag (increment tag, for arrays)
      SUBG Xt, Xn, #i:   Subtract with Tag
      STG [Xn], Xt:      Store Tag (sets 4-bit tag for memory at Xn from Xt)
      LDG Xt, [Xn]:      Load Tag (reads tag of memory at Xn into Xt[59:56])
      STZG [Xn], XZR:    Store Tag + Zero memory (set tag AND zero granule)

Performance comparison:
  Software KASAN (no MTE):  ~2× overhead
  Hardware KASAN (MTE SYNC): ~1-3% overhead  ← huge improvement!
  Hardware KASAN (MTE ASYNC): ~1% overhead   (used in production)
  
  ARM64 MTE ASYNC mode:
    Faults collected in TFSR_EL1/TFSRE0_EL1 registers
    Reported periodically (not immediately)
    Even lower overhead
    Trade-off: slightly delayed fault reporting (harder to pinpoint exact access)
```

---

## 5. Interview Questions & Answers

**Q1: KASAN detects a use-after-free in production. How does KASAN work, and what ARM64 features can reduce its overhead?**

Standard software KASAN works by maintaining a **shadow memory** — one byte per 8 bytes of kernel memory — that records whether memory is accessible or poisoned. On every load/store, the compiler inserts inline checks that look up the shadow byte and call `kasan_report()` on violation. This adds roughly 2× runtime overhead (one extra load + branch per memory access) and 8× memory overhead for shadow.

On ARM64 with MTE (ARMv8.5+), Linux can use **hardware tag-based KASAN** (`CONFIG_KASAN_HW_TAGS`):
1. On allocation: SLUB generates a random 4-bit tag, stores it in pointer bits [59:56], and writes it to the object's memory tag storage (via `STG`)
2. On free: assigns a NEW different random tag to the memory, invalidating old pointers
3. On every load/store: ARM64 hardware AUTOMATICALLY checks pointer tag == memory tag
4. Mismatch: Tag Check Fault (no compiler instrumentation needed)

Overhead: synchronous mode ~1-3% (vs 2× for software KASAN). Async mode: ~1%. No shadow memory (8× memory overhead eliminated). MTE hardware KASAN is suitable for production deployments on supported hardware.

**Q2: Explain the difference between SLAB_POISON and KASAN for detecting use-after-free.**

Both detect use-after-free but with different mechanisms:

**SLAB_POISON**:
- Fills freed object with `0x6b` pattern immediately on `kfree()`
- Checks pattern on next allocation from same slot
- Limitation: only detects if write occurred AND pattern was overwritten, AND detected at next allocation from that slot. If UAF READS occur (no write), POISON doesn't help. If the slot is allocated again before checking: no detection.
- No call stack captured for the UAF itself

**KASAN**:
- Maintains full shadow memory for every byte
- Poisons shadow for freed object immediately
- Detects on EVERY subsequent access to poisoned memory (read or write)
- Immediate detection (synchronous): the exact line/instruction causing UAF
- Stack trace captured: both allocation site AND free site AND UAF site
- Expensive: shadow memory + instrumentation overhead

For production: MTE-based KASAN (if ARMv8.5+) or SLAB_POISON (lower overhead, less precise). For debugging: full KASAN for complete coverage and exact call stacks.

---

## 6. Quick Reference

| Method | Detects UAF? | Detects Overflow? | Overhead | HW Required |
|---|---|---|---|---|
| SLUB POISON (P) | Write-only, deferred | NO (need Z flag) | ~20-50% | None |
| SLUB Red Zone (Z) | NO | YES (left+right) | ~5-10% | None |
| Software KASAN | YES (read+write) | YES | ~2× | None |
| HW KASAN (MTE) | YES (read+write) | YES (16B granule) | ~1-3% | ARMv8.5+ |

| ARM64 MTE Instruction | Purpose |
|---|---|
| IRG Xt, Xn | Insert Random Tag into Xt[59:56] |
| STG [Xn] | Store Tag to memory at Xn |
| LDG Xt, [Xn] | Load Tag from memory at Xn |
| STZG [Xn] | Store Tag + zero memory at Xn |
| ADDG Xt, Xn, #i | Add with tag offset |
