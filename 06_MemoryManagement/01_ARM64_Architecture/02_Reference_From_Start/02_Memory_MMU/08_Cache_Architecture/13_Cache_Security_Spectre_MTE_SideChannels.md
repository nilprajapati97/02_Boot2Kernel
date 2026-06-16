# Cache in Security: Spectre, MTE, Cache Side-Channels

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Cache as a Side Channel

```
Cache side-channel attacks exploit the OBSERVABLE DIFFERENCE between:
  Cache HIT: fast (4–12 cycles for L1)
  Cache MISS: slow (100–300 cycles for DRAM)

An attacker can MEASURE the time of memory accesses:
  Fast access → the address was in cache (attacker's probe hit)
  Slow access → the address was NOT in cache (probe missed)
  
  Infer: if a SECRET operation caused a particular address to be cached,
  the attacker can detect it was accessed → SECRET LEAKED via timing!

Flush+Reload attack (classic):
  Attacker and victim share a physical page (shared library, pipe, etc.)
  
  1. Attacker flushes a cache line with DC CIVAC (or CLFLUSH on x86)
  2. Victim executes code that LOADS from address X (based on secret bit S)
     if (S == 0): access address A
     if (S == 1): access address B
  3. Attacker PROBES addresses A and B with timing measurements:
     if A is fast (cache hit): victim loaded A → S was 0
     if B is fast (cache hit): victim loaded B → S was 1
  → Secret bit S leaked!
  
Prime+Probe attack (no shared memory needed):
  Attacker fills entire cache sets with attacker-controlled data (PRIME)
  Victim executes secret-dependent code → evicts some attacker data
  Attacker measures its own lines again (PROBE):
  If attacker's line was evicted → victim accessed something in that cache set
  → Infers victim's memory access pattern → leaks secret
```

---

## 2. Spectre Variant 1 (BCB - Bounds Check Bypass)

```
CVE-2017-5753: Spectre Variant 1

Vulnerable code pattern:
  if (idx < array1_size) {      // bounds check
      val = array1[idx];         // safe access
      dummy = array2[val * 64]; // cache side channel!
  }
  
Exploitation:
  1. Attacker calls with idx = secret_idx (out-of-bounds, pointing to secret)
  2. CPU speculatively executes PAST the bounds check (predictor: "likely in bounds")
  3. Speculative: array1[secret_idx] loads secret byte into val
  4. Speculative: array2[val * 64] brings array2[secret_byte × 64] into CACHE
  5. Bounds check completes: idx is OUT OF BOUNDS → speculation squashed
     BUT: the cache state is NOT rolled back! array2[secret_byte×64] is still cached!
  6. Attacker probes array2 for each possible byte value:
     Finds which array2[X×64] is fast → X = secret_byte

ARM64 mitigations for Spectre Variant 1:
  array_index_nospec() macro (include/linux/nospec.h):
    Forces speculation to use an index of 0 (safe) rather than speculative out-of-bounds
    Uses conditional AND instruction to sanitize index:
    
    #define array_index_nospec(index, size)            \
    ({                                                  \
        unsigned long _i = (index);                    \
        unsigned long _s = (size);                     \
        _i &= array_index_mask_nospec(_i, _s);         \
        (typeof(index))_i;                             \
    })
    
    array_index_mask_nospec(i, s):
      Returns 0 if i >= s (out of bounds) or ~0 if in bounds
      Uses: SUB + SBFM (sign-bit manipulation) to avoid branches
      No branch → no branch predictor to mislead!
      
  Kernel syscall path: uses array_index_nospec where arrays indexed by user input
  Spectre v1 patches applied to: BPF verifier, eBPF JIT, slab allocator, etc.
```

---

## 3. Spectre Variant 2 (BTI - Branch Target Injection)

```
CVE-2017-5715: Spectre Variant 2

Exploit: poison indirect branch predictor (BTB - Branch Target Buffer)
  Attacker trains the BTB with a specific branch target
  Victim uses indirect branch (indirect call or return)
  CPU speculatively jumps to ATTACKER-CHOSEN address (BTB poisoning)
  Victim speculatively executes attacker-chosen code (gadget)
  Gadget: performs a cache-side-channel load → leaks secret
  
ARM64 Spectre v2 mitigations:
  
  1. BTI (Branch Target Identification) - ARMv8.5:
     FEAT_BTI: adds landing pad instructions (BTI, BTI C, BTI J, BTI JC)
     Indirect branches MUST land on a BTI instruction
     If not: trap to EL1 (SIGILL equivalent)
     Prevents attacker from redirecting execution to arbitrary gadgets
     
     PTE bit[GP] (Guarded Page): enables BTI enforcement for this page
     Linux: marks kernel .text as guarded (GP=1) if FEAT_BTI available
     User: mprotect(PROT_BTI) to enable on user pages
     
  2. CSV2 / BRAHSS (CPU_specific):
     Some ARM CPUs implement Spectre v2 mitigation in hardware
     cat /sys/devices/system/cpu/vulnerabilities/spectre_v2
     → "Mitigation: CSV2, BHB" or "Not affected"
     
  3. Retpoline (software):
     Indirect calls via a trampoline that confuses branch predictor
     Less common on ARM64 (architecture-specific hardware mitigations preferred)
     
  4. IBPB/IBRS equivalent on ARM64:
     FEAT_CSV2: prevents cross-context BTB speculation
     Equivalent to: EL1 cannot be affected by EL0 branch poisoning
```

---

## 4. Spectre Variant 3 (Meltdown) and ARM64

```
CVE-2017-5754: Meltdown

Exploit: user-mode code speculatively reads KERNEL memory
  (Kernel pages are mapped but NOT accessible from EL0 — AP[2:1]=0b00)
  
  On Intel CPUs: speculative load proceeds EVEN AFTER permission fault
  The loaded kernel data affects cache state → side channel leaks it
  
ARM64 status:
  Most ARM64 CPUs: NOT vulnerable to classic Meltdown
  ARM's microarchitecture: speculative execution stops at EL0 permission fault
  (The speculation is squashed BEFORE any observable cache side-effect)
  
  However: ARM published documentation of some variants:
    Some older ARM cores (Cortex-A8, A9, A15) may have Meltdown variants
    Check: cat /sys/devices/system/cpu/vulnerabilities/meltdown
    
  KPTI (Kernel Page Table Isolation) for ARM64:
    CONFIG_UNMAP_KERNEL_AT_EL0: only enabled for vulnerable CPUs
    Most ARM64: shows "Not affected" → KPTI not active
```

---

## 5. MTE (Memory Tagging Extension) and Cache

```
ARMv8.5 MTE: hardware tag checking for heap/stack safety

MTE architecture:
  Every 16-byte granule in physical memory has a 4-bit tag
  Tag stored in "tag RAM" — separate from data cache but associated with each line
  
  Tagged pointer:
    VA bits[59:56] = pointer tag (4 bits in the "top byte ignore" region)
    On every memory access: hardware checks pointer_tag == memory_tag
    Mismatch: generates a synchronous or asynchronous fault
    
  Cache and MTE:
    Cache lines: store 64 bytes of data + 16 bits of tag (for 4 granules per line)
    Cache coherency: tag is part of cache line → coherent with data
    Tag load/store instructions:
      STG: store tag to memory (sets memory tag)
      LDG: load tag from memory (reads memory tag for inspection)
      ST2G: store tag for 2 consecutive granules
      
  MTE cache maintenance:
    DC CIVAC: also cleans/invalidates the ASSOCIATED TAGS in tag RAM
    No separate tag maintenance instructions needed (tags are part of cache line)
    
  Linux MTE support (arch/arm64/):
    PROT_MTE flag for mmap/mprotect: enable MTE on this region
    alloc_pages(): can use MTE for heap pages (SLUB MTE support)
    MTE tags set by malloc() implementation (e.g., Android Scudo allocator)

Performance impact of MTE:
  Cache: minimal — tags stored alongside data, no extra cache miss
  Load/store: ~1% overhead (tag check is parallel with data access)
  Tag RAM: ~3% memory overhead (4 bits per 16 bytes = ~3% extra)
  
  Correctness benefit: catches use-after-free, buffer overflows in hardware
  Compared to ASAN: MTE = ~1% overhead vs ASAN = ~2× overhead
```

---

## 6. Mitigating Cache Side Channels in Linux

```
Linux kernel defenses:

1. PAGE_TABLE_ISOLATION (KPTI): Meltdown mitigation
   Kernel pages not mapped in user-mode page table
   Prevents speculative access to kernel cache lines from user mode

2. Retpoline/IBT: Spectre v2
   All indirect branches via retpoline trampoline
   ARM64: uses FEAT_BTI + CSV2 instead

3. Context speculation barrier:
   sb() macro (Speculation Barrier) in kernel:
   Emits: DSB SY + ISB (drain speculation pipeline)
   Used at: EL0→EL1 boundary, after array_index_nospec()
   Ensures: speculative execution cannot leak across this point

4. Flushing on context switch:
   FEAT_CSV2: hardware prevents cross-process Spectre v2 without flush
   Without CSV2: IBPB (Indirect Branch Predictor Barrier) equivalent:
     ARM64: HINT #14 (CSDB - Context Synchronization and Data Barrier)
     Prevents use of speculatively computed values in subsequent loads

5. BPF JIT hardening:
   eBPF programs: potentially attacker-controlled
   BPF_JIT_HARDEN: poison JIT constants to prevent Spectre v1 via BPF
   JIT_BLIND_CONSTANTS: randomize immediate values in JIT output

Linux /sys/devices/system/cpu/vulnerabilities/:
  spectre_v1: "Mitigation: usercopy/swapgs barriers and __user pointer sanitization"
  spectre_v2: "Mitigation: CSV2, BHB" (ARM64 hardware mitigations)
  meltdown:   "Not affected" (most ARM64 CPUs)
  spec_store_bypass: "Mitigation: Speculative Store Bypass disabled via prctl"
  srbds: "Not affected" (ARM64 specific)
```

---

## 7. Interview Questions & Answers

**Q1: Why does Spectre v1 not require kernel privileges to exploit, and how does array_index_nospec() prevent it?**

Spectre v1 exploits the **speculative execution** of a bounds-checked array access. When the CPU encounters `if (idx < size) val = array[idx]`, it makes a branch prediction and speculatively executes the body BEFORE the bounds check resolves. If an attacker provides an out-of-bounds `idx` (after training the branch predictor to always predict "in bounds"), the CPU speculatively loads from an out-of-bounds address — this could be kernel memory in the case of a syscall parameter. The speculative load pulls kernel data into a cache line. Even after the branch resolves and speculation is squashed, **the cache state change persists** (the cache line remains hot). The attacker then measures timing to determine which cache line was loaded → extracts the secret value.

No kernel privileges are needed: the attacker just makes a syscall with crafted arguments. The vulnerable code runs in the kernel (EL1), but the SIDE CHANNEL (timing measurement of cache lines) happens in user space (EL0).

`array_index_nospec(index, size)` prevents this by replacing the potentially-speculated index with a **speculatively-safe** value. It uses `array_index_mask_nospec()` which computes: if `index >= size`, return 0; else return `index` — using arithmetic that creates a DATA DEPENDENCY (not a branch). The mask is computed as `(index - size) >> 63` (sign bit): if `index >= size`, this is `0`; if `index < size`, this is `~0`. ANDing the index with this mask gives 0 for out-of-bounds access. Since the mask computation is sequential (no speculative path), the CPU cannot circumvent it: the speculative load always uses index=0 when out-of-bounds, which is a safe address.

---

## 8. Quick Reference

| Attack | Mechanism | ARM64 Status | Mitigation |
|---|---|---|---|
| Spectre v1 (BCB) | Bounds check bypass | Vulnerable | array_index_nospec |
| Spectre v2 (BTI) | Branch target injection | Partially affected | FEAT_BTI, CSV2 |
| Meltdown (v3) | Kernel mem from user mode | Most ARM64: NOT affected | KPTI (if needed) |
| Flush+Reload | Cache timing side-channel | All systems | Disable sharing |
| MeltdownPrime | L3 side-channel | Some ARM64 | FEAT_ETS2 |

| Defense | What it Does | Linux Config |
|---|---|---|
| KPTI | Remove kernel from user page table | CONFIG_UNMAP_KERNEL_AT_EL0 |
| BTI | Landing pad for indirect branches | CONFIG_ARM64_BTI_KERNEL |
| array_index_nospec | Safe speculative array index | In-code, used in kernel |
| CSDB barrier | Speculation barrier at EL0→EL1 | Automatic in entry.S |
| MTE | Hardware tag checking | CONFIG_ARM64_MTE |
