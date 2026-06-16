# MTE — Memory Tagging Extension Deep Dive

**Category**: Security Extensions  
**Platform**: ARM64 (AArch64) — ARMv8.5-A

---

## 1. Concept Foundation

```
MTE (Memory Tagging Extension) — introduced in ARMv8.5-A

Root problem: C/C++ memory safety bugs are the #1 source of CVEs:
  - Use-after-free: access memory after it has been freed
  - Heap buffer overflow: write past the end of an allocated region
  - Stack buffer overflow: corrupt adjacent stack variables
  - Use-of-uninitialized memory: read before any write

Previous approaches:
  AddressSanitizer (ASAN): ~2x slowdown, 2x memory overhead — not production-viable
  Safe C++ (bounds checking): requires language-level changes
  Guard pages: only catches large overflows (page granularity)

MTE design goal: ~1% overhead in ASYNC mode, hardware-enforced
  Granularity: 16 bytes (hardware "tag granule")
  Tag width: 4 bits → 16 possible values
  Storage: separate Tag RAM alongside main RAM (no extra address space)
  Pointer encoding: tag in unused VA bits[59:56]

Mental model:
  Every malloc():    assigns a random 4-bit color to the allocation
                     stamps that color into the allocation's Tag RAM
                     returns pointer with that color in bits[59:56]
  Every free():      changes the Tag RAM color (different from the freed pointer)
  Every load/store:  hardware compares pointer color vs Tag RAM color
                     MISMATCH → MTE fault (precise or accumulated)
```

---

## 2. ARM64 Hardware Detail

### 2.1 Tag Granule and Tag RAM Layout

```
Tag granule: 16 bytes
  Every 16-byte aligned, 16-byte chunk of memory has exactly ONE 4-bit tag
  Tag RAM: physically separate SRAM alongside normal DRAM
  
  For a 4KB page:
    Normal data:   4096 bytes
    Tag storage:   256 bytes (4096/16 = 256 granules × 4 bits = 128 bytes compressed)
    ARM stores tags as nibbles: 2 tags per byte → 128 bytes of tag storage per 4KB
    
    Example: malloc(32):
      VA range:  [0x7f000000, 0x7f000020)  (2 × 16-byte granules)
      Tag RAM:   [TAG_RAM_ADDR(0x7f000000)] = 0xAA (tag A for both granules)
      Pointer:   0xA_7f000000 (tag A in bits[59:56])

Tag bit location in ARM64 virtual address:
  63  59  55  47          0
  ┌───┬───┬───┬───────────┐
  │ S │TAG│ 0 │    VA     │
  └───┴───┴───┴───────────┘
   1   4   8      48 bits
  
  TAG = bits[59:56]: 4-bit logical tag embedded in pointer
  S   = bit[63]:     sign extension (0 = user, 1 = kernel)
  VA  = bits[47:0]:  virtual address
  Bits[55:48]:       must be all-zero (or all-one for kernel) — not used by MTE
  
  TBI (Top Byte Ignore): ARM64 TBI allows ignoring bits[63:56] for address computation
  MTE uses: bits[59:56] specifically for tag (TBI still active → VA computation ignores tag)
  
  TBI + MTE configuration:
    TCR_EL1.TBI0 = 1: ignore top byte for TTBR0 addresses (user)
    TCR_EL1.TBID0 = 0: DO check tag for data accesses (MTE enforcement)
    Note: TBI ignores bytes 56-63 for VA lookup, but MTE independently checks 59:56
```

### 2.2 MTE Instructions Reference

```
IRG Xd, Xn, Xm:  Insert Random Tag
  Purpose:  Generate a random tag and insert into pointer
  Xn:       base address (tag bits cleared on input)
  Xm:       tag exclusion mask (bits[15:0] = tags to AVOID)
  Xd:       result = Xn with random tag in bits[59:56] (not in exclusion mask)
  
  Example:
    IRG X0, X1, XZR   // X1 = 0x7f000000, XZR = no exclusion
                       // X0 = 0x_B_7f000000 (tag B randomly chosen)

STG Xt, [Xn, #offset]:  Store Allocation Tag
  Purpose:  Write the tag from Xt[59:56] to Tag RAM at [Xn + offset]
  Granule:  One 16-byte granule at [Xn + offset] (must be 16-byte aligned)
  Xt:       pointer whose tag[59:56] is the tag to store
  
  Example (stamp tag B to 32 bytes):
    STG X0, [X1]          // stamp tag B at [0x7f000000..0x7f00000f]
    STG X0, [X1, #16]     // stamp tag B at [0x7f000010..0x7f00001f]

LDG Xd, [Xn, #offset]:  Load Allocation Tag
  Purpose:  Read the tag from Tag RAM at [Xn + offset]
  Xd:       receives Xn with its bits[59:56] replaced by the Tag RAM tag
  
  Example:
    LDG X0, [X1]          // X0 = 0x_B_7f000000 (tag B read from Tag RAM)

ST2G Xt, [Xn, #offset]:  Store 2 Allocation Tags (32-byte region)
  Stamps tag from Xt[59:56] to TWO consecutive 16-byte granules
  More efficient than two STG instructions

STZG Xt, [Xn, #offset]:  Store Tag + Zero
  Stamps tag AND zeros 16 bytes of data in one operation
  Used in allocators: tag + zero-init simultaneously

STZ2G Xt, [Xn, #offset]: Store 2 Tags + Zero
  Stamps 2 tags and zeros 32 bytes in one operation

DC GVA, Xn:    Data Cache Zero + Tag (set allocation tags to zero)
DC GZVA, Xn:   Data Cache Zero to PoP + tag (zero data + set tags to 0)
```

### 2.3 MTE Exception Modes

```
SCTLR_EL1.TCF (Tag Check Fault):
  TCF[1:0] = 0b00: Tag check faults disabled (no checking)
  TCF[1:0] = 0b01: SYNC mode — fault on first tag mismatch, precise
  TCF[1:0] = 0b10: ASYNC mode — accumulate, imprecise
  TCF[1:0] = 0b11: ASYMM mode — stores=async, loads=sync

SCTLR_EL1.TCF0 (Tag Check Fault for EL0):
  Same encoding as TCF, but applies to EL0 (user space)
  
  Kernel sets TCF0 based on per-process prctl setting

TFSR_EL1 (Tag Fault Status Register at EL1):
  Used in ASYNC mode: accumulates fault information
  TFSR_EL1.TF0: tag check fault occurred in EL0
  TFSR_EL1.TF1: tag check fault occurred in EL1
  Software: reads TFSR_EL1 periodically to check for async faults
  
  Linux ASYNC mode path:
    Kernel: checks TFSR_EL1 on: schedule(), syscall return, signal delivery
    If TFSR_EL1.TF0 set: deliver SIGSEGV (or SIGBUS) to process
    Clear TFSR_EL1 after reporting

SYNC mode exception:
  Access fault → ESR_EL1.EC = 0x25 (Data Abort)
  ESR_EL1.ISS.TnD: Tag not Data (MTE fault indicator)
  FAR_EL1: exact faulting virtual address
  Linux: handle_tag_fault() → sends SIGSEGV(SEGV_MTEAERR) to process
```

### 2.4 Memory Attribute Requirements

```
For MTE to work, memory must be:
  MAIR_EL1: encoded as Normal memory (not Device)
  Page attribute: ATTRINDX[2:0] in PTE must point to Normal memory MAIR entry
  PTE MT bit: not set (not tagged → no MTE checking)
  
  New PTE field for MTE:
    Bit[59]: PBHA[3] / MT (Memory Tagging) in ARM64 page descriptor
    MT=0: not tagged (MTE checks disabled for this page even if TCF enabled)
    MT=1: tagged (MTE checks active for this page)
    
  mmap() with PROT_MTE (Linux):
    Sets MT=1 in all PTEs for the VMA
    Kernel: uses pte_mte() to OR in the MT bit
    
  malloc() with MTE:
    1. mmap(PROT_READ|PROT_WRITE|PROT_MTE): get MTE-tagged region
    2. For each allocation: IRG + STG to set allocation tag
    3. Return tagged pointer
    4. On free: new IRG + STG (change tag) to invalidate freed pointer
```

---

## 3. Linux Kernel Implementation

### 3.1 MTE Architecture Support

```
arch/arm64/include/asm/mte.h:
  mte_set_mem_tag_range(addr, size, tag): stamp tag to range
  mte_get_mem_tag(addr): read tag from Tag RAM
  mte_assign_mem_tag_range(addr, size): assign random tags
  mte_enable_kernel(): enable MTE for kernel space

arch/arm64/lib/mte.S:
  __mte_set_mem_tag_range: assembly loop using STG/ST2G/STZ2G
  __mte_copy_tags: copy tags from one region to another (for fork)
  __mte_zero_range: zero data + clear tags

arch/arm64/kernel/mte.c:
  mte_check_tfsr(): check TFSR_EL1 for async faults, report to process
  mte_update_sctlr_user(): update SCTLR_EL1.TCF0 for process
  copy_page_tags(): during fork → copy tags from parent page to child
  mte_sync_tags(): synchronize tags on page table changes

arch/arm64/kernel/process.c:
  mte_thread_switch(): on context switch
    Restores: SCTLR_EL1.TCF0 setting for new process
    If async fault pending (TFSR_EL1): deliver signal
```

### 3.2 User-Space MTE API

```c
// Enable MTE for process (called by glibc/allocator):
prctl(PR_SET_TAGGED_ADDR_CTRL,
      PR_TAGGED_ADDR_ENABLE    // allow tagged addresses
      | PR_MTE_TCF_SYNC        // or PR_MTE_TCF_ASYNC or PR_MTE_TCF_ASYMM
      | (0xffff << PR_MTE_TAG_MASK_SHIFT), // tag exclusion mask
      0, 0, 0);

// Allocate MTE-capable memory:
void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_MTE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
// PROT_MTE: sets MT=1 in PTEs for this VMA

// Tag the allocation (assembly or glibc wrapper):
asm volatile("irg %0, %1" : "=r"(tagged_ptr) : "r"(ptr));  // random tag
asm volatile("stg %0, [%0]" : : "r"(tagged_ptr));           // stamp tag

// Free: change tag to invalidate
asm volatile("irg %0, %1" : "=r"(new_tagged) : "r"(ptr));  // different tag
asm volatile("stg %0, [%0]" : : "r"(new_tagged));           // new tag stored
// Old tagged_ptr now has wrong tag → use-after-free detected by HW
```

### 3.3 KASAN MTE Integration

```c
// CONFIG_KASAN_HW_TAGS: uses MTE hardware for kernel heap tagging
// arch/arm64/include/asm/kasan.h

kasan_alloc_pages():
    // After allocating slab object:
    tag = kasan_random_tag();        // get 4-bit random tag
    tagged_addr = set_tag(addr, tag); // set bits[59:56]
    mte_set_mem_tag_range(addr, size, tag); // STG loop
    return tagged_addr;

kasan_free_pages():
    // When freeing slab object:
    new_tag = 0;    // or random different tag
    mte_set_mem_tag_range(addr, size, new_tag); // change tag
    // Future accesses with old tagged pointer → MTE fault → KASAN report
```

---

## 4. Hardware-Software Interaction

```
Full MTE allocation lifecycle:

1. malloc(64) call:
   ┌─────────────────────────────────────────────────────────┐
   │ glibc malloc → calls mmap/brk to get PROT_MTE memory   │
   │ Each heap chunk: IRG to get random tag T                │
   │ Loop: STG to stamp tag T into Tag RAM                   │
   │ Return: pointer with bits[59:56] = T                    │
   └─────────────────────────────────────────────────────────┘

2. Normal use:
   ┌─────────────────────────────────────────────────────────┐
   │ LDR X0, [X1]   X1 has tag T in bits[59:56]             │
   │ Hardware check: logical_tag(X1)[59:56] == Tag_RAM[X1]  │
   │ Both = T: ✓ pass, load proceeds                        │
   └─────────────────────────────────────────────────────────┘

3. free(ptr):
   ┌─────────────────────────────────────────────────────────┐
   │ glibc free → IRG to get new random tag T' (≠ T)        │
   │ Loop: STG to stamp tag T' into Tag RAM                  │
   │ Internal pointer saved with tag T' (for reuse)         │
   └─────────────────────────────────────────────────────────┘

4. Use-after-free attempt:
   ┌─────────────────────────────────────────────────────────┐
   │ LDR X0, [X1]   X1 still has old tag T in bits[59:56]   │
   │ Hardware check: logical_tag(X1) = T                    │
   │                 Tag_RAM[X1] = T' (changed by free())   │
   │ T ≠ T': MTE FAULT → SIGSEGV(SEGV_MTEAERR) delivered   │
   └─────────────────────────────────────────────────────────┘

Tag RAM access:
  Tag RAM is transparent to software (no explicit address for it)
  Only accessible via MTE instructions (STG, LDG, etc.)
  Physical location: alongside L1/L2/L3 cache tags or in DRAM controller
  Cache coherency: tags follow same coherency protocol as data
  Fork: copy_page_tags() copies both data AND tags to child pages
```

---

## 5. Interview Q&A

**Q1: How does MTE detect heap buffer overflows?**

Adjacent heap allocations get DIFFERENT random tags. When allocating 32 bytes, the `[32, 48)` byte range belongs to the NEXT allocation and has a different tag T2 in Tag RAM. If code writes to ptr[32] (1 byte past end): the pointer tag is T1, but Tag RAM at that address is T2 → T1 ≠ T2 → MTE fault. Overflow caught at the exact overflowing instruction.

**Q2: What is the difference between SYNC and ASYNC MTE modes?**

SYNC (`TCF=0b01`): Every tag mismatch causes an immediate synchronous exception. Precise: `FAR_EL1` contains the exact faulting address. Overhead: ~5% (every fault takes exception path). Used for: debugging, catching bugs precisely.

ASYNC (`TCF=0b10`): Mismatches are accumulated in `TFSR_EL1.TF0`. No immediate exception. Linux checks `TFSR_EL1` at scheduling points and syscall return. Overhead: ~1-2% (no per-fault exception). Used for: production deployments (still catches bugs, just less precisely). Address of the fault is NOT available in async mode.

**Q3: Why does free() change the tag instead of zeroing it?**

The tag `0x0` (zero) is a valid tag value. If free() set the tag to 0, any untagged pointer (with tag 0) would incorrectly pass the check on freed memory. Instead, free() uses `IRG` to get a NEW random tag T' (≠ T, using exclusion mask). This new tag is stamped into Tag RAM. The old freed pointer (with tag T) will always mismatch → detected. The allocator reuses the memory internally with tag T' → no false positives.

**Q4: Can MTE protect kernel memory as well as userspace?**

Yes. `CONFIG_KASAN_HW_TAGS` uses MTE for kernel slab allocations. `SCTLR_EL1.TCF` controls kernel-level tag checking. `kasan_alloc_pages()` stamps tags with `mte_set_mem_tag_range()`. Slab UAF, slab overflow → MTE fault in kernel → `kasan_report()`. The same hardware is used for both kernel and userspace, just controlled by different `TCF`/`TCF0` bits.

**Q5: What is the performance overhead of MTE for heap allocations?**

Main overheads: (1) `IRG` instruction: ~1 cycle per allocation. (2) `STG`/`ST2G` loop: 1 STG per 16 bytes = ~N/16 extra cycles for a size-N allocation. (3) SYNC fault handling when a bug fires: ~thousands of cycles (but this is only on bugs). (4) Tag RAM bandwidth: minimal (separate path). Overall: ~1-2% in ASYNC mode for typical workloads (Android system, Chrome). SYNC mode: ~5%. Production Android (Android 12+): ships with ASYNC MTE enabled on ARMv8.5 hardware.

---

## 6. Pitfalls & Gotchas

1. **Tag granule is 16 bytes, not 1 byte**: If you allocate 10 bytes, bytes 10-15 in the same granule have the same tag as bytes 0-9. A 6-byte overflow within the same 16-byte granule is NOT detected by MTE (it is by ASAN).

2. **Non-MTE memory doesn't check tags**: Only pages with `PROT_MTE` (MT bit = 1 in PTE) are checked. `mmap()` without `PROT_MTE` → no tag checking on that memory, even if TCF is enabled.

3. **Fork must copy tags**: During `fork()`, parent pages are COW-copied to child. `copy_page_tags()` must copy the Tag RAM as well. If you copy just data and not tags: child accesses trigger MTE faults (wrong tags). Linux handles this in `copy_page_with_type()`.

4. **ASYNC mode doesn't give fault address**: `TFSR_EL1` says "a fault happened" but not where. For debugging: switch to SYNC mode. Production: ASYNC gives better perf at cost of imprecise reporting.

5. **Tag 0 is not "null"**: All 16 values (0-15) are valid allocation tags. The exclusion mask lets allocators avoid specific tags to reduce false negatives (e.g., exclude tag 0 so that tag-zero pointers are never confused with tagged allocations).

---

## 7. Quick Reference

| Instruction | Purpose | Operands |
|---|---|---|
| `IRG Xd, Xn, Xm` | Insert random tag | Xm = exclusion mask |
| `STG Xt, [Xn]` | Store 1 tag (16B) | Xt[59:56] = tag to store |
| `ST2G Xt, [Xn]` | Store 2 tags (32B) | Faster than 2× STG |
| `STZG Xt, [Xn]` | Store tag + zero 16B | Init + tag in one op |
| `LDG Xd, [Xn]` | Load tag into Xd[59:56] | Read from Tag RAM |

| MTE Mode | `TCF` Value | Fault Handling | Overhead |
|---|---|---|---|
| Disabled | 0b00 | No checking | 0% |
| Synchronous | 0b01 | Immediate fault, precise | ~5% |
| Asynchronous | 0b10 | Accumulated in TFSR_EL1 | ~1-2% |
| Asymmetric | 0b11 | Store=async, Load=sync | ~2-3% |

| Kernel Config | Purpose |
|---|---|
| `CONFIG_ARM64_MTE` | Enable MTE platform support |
| `CONFIG_KASAN_HW_TAGS` | Use MTE for kernel KASAN |
| `CONFIG_ARM64_MTE_COMP_PERF_WORKAROUND` | Errata workarounds for early MTE HW |
