# Memory Subsystem — Questions & Answers

---

## Q1. [L1] Explain the ARMv8 virtual memory system. What is virtual addressing and why do we need it?

**Answer:**

```
Virtual memory provides each process an ILLUSION of having its
own private, contiguous address space, while physical memory is
shared, fragmented, and limited.

Without virtual memory:
  Process A: uses address 0x1000 → physical 0x1000
  Process B: uses address 0x1000 → CONFLICT! Same physical!
  Must manually relocate programs, no protection, no isolation.

With virtual memory:
  Process A: VA 0x1000 → MMU → PA 0x50000
  Process B: VA 0x1000 → MMU → PA 0x80000
  Different physical addresses! Processes are isolated.

  ┌──────────────────────────────────────────────────────────┐
  │           Virtual Address Space (per-process)            │
  │  ┌──────────────────┐                                   │
  │  │   User Space      │ VA: 0x0000_0000_0000_0000        │
  │  │   (TTBR0_EL1)    │     to 0x0000_FFFF_FFFF_FFFF     │
  │  │   Code, Data,    │     (lower 48 bits, configurable) │
  │  │   Stack, Heap    │                                    │
  │  ├──────────────────┤                                   │
  │  │   Kernel Hole    │ VA: 0x0001...to 0xFFFE...         │
  │  │   (unmapped)      │ Fault if accessed                │
  │  ├──────────────────┤                                   │
  │  │   Kernel Space   │ VA: 0xFFFF_0000_0000_0000         │
  │  │   (TTBR1_EL1)   │     to 0xFFFF_FFFF_FFFF_FFFF     │
  │  │   Kernel code,   │     (upper 48 bits)               │
  │  │   modules, vmalloc│                                  │
  │  └──────────────────┘                                   │
  └──────────────────────────────────────────────────────────┘

  Two TTBR registers:
    TTBR0_EL1: translates addresses with upper bits = 0 (user)
    TTBR1_EL1: translates addresses with upper bits = 1 (kernel)
    
    On context switch: only TTBR0 changes (new process page tables)
    TTBR1 stays the same (kernel is mapped identically in all processes)

Benefits of virtual memory:
  1. Process isolation: one process can't access another's memory
  2. Memory protection: read/write/execute permissions per page
  3. Demand paging: pages loaded from disk only when accessed
  4. Memory overcommit: allocate more VA than physical RAM
  5. Shared libraries: same physical pages, different VAs
  6. Copy-on-Write: fork() shares pages until written
  7. Memory-mapped I/O: hardware registers at known VAs
```

---

## Q2. [L2] Describe the 4-level page table structure in ARMv8 with 4KB granule. Walk through a VA→PA translation.

**Answer:**

```
With 48-bit VA and 4KB page granule:

64-bit Virtual Address decomposition:
┌────────────────────────────────────────────────────────────────┐
│ [63:48]  │ [47:39]  │ [38:30]  │ [29:21]  │ [20:12]  │[11:0]│
│ 16 bits  │ 9 bits   │ 9 bits   │ 9 bits   │ 9 bits   │12 bit│
│ Sign ext │ L0 Index │ L1 Index │ L2 Index │ L3 Index │Offset│
│ (unused) │ 512 ent  │ 512 ent  │ 512 ent  │ 512 ent  │ 4KB  │
└────────────────────────────────────────────────────────────────┘

Each page table = 512 entries × 8 bytes = 4KB (fits in one page!)

Translation walk:
  Step 1: TTBR0_EL1 contains base address of L0 table
          L0 entry = TTBR0_EL1[base] + L0_Index * 8
          Read L0 entry → contains L1 table base address

  Step 2: L1 entry = L1_table[L1_Index]
          Read L1 entry → could be:
            a) Table descriptor → points to L2 table
            b) Block descriptor → 1GB huge page (done!)

  Step 3: L2 entry = L2_table[L2_Index]
          Read L2 entry → could be:
            a) Table descriptor → points to L3 table
            b) Block descriptor → 2MB huge page (done!)

  Step 4: L3 entry = L3_table[L3_Index]
          Read L3 entry → Page descriptor → 4KB page
          PA = L3.OutputAddress + Offset[11:0]

  ┌─────────┐     ┌─────────┐     ┌─────────┐     ┌─────────┐
  │ L0 Table│     │ L1 Table│     │ L2 Table│     │ L3 Table│
  │         │     │         │     │         │     │         │
  │ [  0  ] │     │ [  0  ] │     │ [  0  ] │     │ [  0  ] │
  │ [  1  ] │     │ [  1  ] │     │ [  1  ] │     │ [  1  ] │
  │ [ ... ] │     │ [ ... ] │     │ [ ... ] │     │ [ ... ] │
  │ [L0idx]─┼────>│ [L1idx]─┼────>│ [L2idx]─┼────>│ [L3idx]─┼─> PA
  │ [ ... ] │     │ [ ... ] │     │ [ ... ] │     │ [ ... ] │
  │ [ 511 ] │     │ [ 511 ] │     │ [ 511 ] │     │ [ 511 ] │
  └─────────┘     └─────────┘     └─────────┘     └─────────┘
    TTBR0↑

Page table entry format (64-bit descriptor):
  ┌───────────────────────────────────────────────────────────┐
  │ [63:52] │ [51:48] │ [47:12]         │ [11:2]  │ [1:0]   │
  │ Upper   │ Res0/   │ Output Address  │ Lower   │ Type    │
  │ Attrs   │ PBHA    │ (PA of next lvl)│ Attrs   │ Valid   │
  └───────────────────────────────────────────────────────────┘
  
  [1:0] = 0b11: Valid table/page descriptor
  [1:0] = 0b01: Valid block descriptor (L1=1GB, L2=2MB)
  [1:0] = 0b00: Invalid (fault on access)

  Lower attributes (L3 page):
    [2]    : MemAttr index → MAIR lookup (see Q4)
    [7:6]  : AP (Access Permission)
              00 = EL1 R/W, EL0 No access
              01 = EL1 R/W, EL0 R/W
              10 = EL1 R/O, EL0 No access
              11 = EL1 R/O, EL0 R/O
    [8]    : SH (Shareability): 00=Non, 10=Outer, 11=Inner
    [10]   : AF (Access Flag): 0=never accessed, 1=accessed
    [53]   : PXN: Privileged Execute Never
    [54]   : UXN/XN: User/Unprivileged Execute Never
    [55]   : DBM: Dirty Bit Modifier (ARMv8.1)

Cost: worst case = 4 memory accesses for one translation
  → That's why TLBs are critical!
```

---

## Q3. [L2] What is TCR_EL1 and how does it configure the translation regime?

**Answer:**

```
TCR_EL1 = Translation Control Register for EL1
It configures EVERYTHING about how the MMU translates addresses.

Key fields:
┌──────────┬────────────────────────────────────────────────────────┐
│ Field    │ Purpose                                                │
├──────────┼────────────────────────────────────────────────────────┤
│ T0SZ     │ Size of TTBR0 region: VA bits = 64 - T0SZ            │
│ [5:0]    │ T0SZ=16 → 48-bit VA (256 TB user space)             │
│          │ T0SZ=25 → 39-bit VA (512 GB user space)             │
│          │ T0SZ=39 → 25-bit VA (32 MB user space — tiny!)      │
├──────────┼────────────────────────────────────────────────────────┤
│ T1SZ     │ Size of TTBR1 region: same encoding for kernel       │
│ [21:16]  │ T1SZ=16 → 48-bit kernel VA space                    │
├──────────┼────────────────────────────────────────────────────────┤
│ TG0      │ TTBR0 granule size:                                   │
│ [15:14]  │ 00=4KB, 01=64KB, 10=16KB                            │
├──────────┼────────────────────────────────────────────────────────┤
│ TG1      │ TTBR1 granule size:                                   │
│ [31:30]  │ 01=16KB, 10=4KB, 11=64KB (different encoding!)      │
├──────────┼────────────────────────────────────────────────────────┤
│ SH0/SH1  │ Shareability of page table walks:                    │
│          │ 00=Non, 10=Outer, 11=Inner shareable                │
├──────────┼────────────────────────────────────────────────────────┤
│ ORGN0/   │ Outer cacheability of page table walks:              │
│ IRGN0    │ 00=NonCache, 01=WB-WA, 10=WT, 11=WB-noWA           │
├──────────┼────────────────────────────────────────────────────────┤
│ EPD0/EPD1│ Translation disable: 1=disable TTBR0/TTBR1           │
│          │ EPD1=1: no kernel mapping (used during early boot)   │
├──────────┼────────────────────────────────────────────────────────┤
│ IPS      │ Intermediate Physical Address Size:                   │
│ [34:32]  │ 000=32-bit (4GB), 010=40-bit (1TB),                │
│          │ 101=48-bit (256TB), 110=52-bit (4PB)                 │
├──────────┼────────────────────────────────────────────────────────┤
│ A1       │ ASID select: 0=TTBR0 ASID, 1=TTBR1 ASID            │
├──────────┼────────────────────────────────────────────────────────┤
│ AS       │ ASID size: 0=8-bit (256), 1=16-bit (65536)          │
├──────────┼────────────────────────────────────────────────────────┤
│ TBI0/TBI1│ Top Byte Ignore: 1=ignore VA[63:56]                  │
│          │ Enables: pointer tagging (MTE, PAC use top byte)    │
├──────────┼────────────────────────────────────────────────────────┤
│ HA/HD    │ Hardware management of Access/Dirty flags (ARMv8.1)  │
│          │ HA=1: HW sets AF automatically                      │
│          │ HD=1: HW sets dirty bit automatically               │
└──────────┴────────────────────────────────────────────────────────┘

Linux kernel default configuration:
  T0SZ = 16 (48-bit user VA)  OR  T0SZ = 25 (39-bit, less common)
  T1SZ = 16 (48-bit kernel VA)
  TG0/TG1 = 4KB granule
  IPS = match ID_AA64MMFR0_EL1.PARange
  AS = 1 (16-bit ASID, commonly)
  TBI0 = 1 (top byte ignore for user — enables MTE/HWASan)
  HA = 1, HD = 1 (if available — reduces page fault overhead)
```

---

## Q4. [L2] What is MAIR_EL1? Explain memory types and attribute indices.

**Answer:**

```
MAIR_EL1 = Memory Attribute Indirection Register

Page table entries DON'T directly specify memory type.
Instead, they contain a 3-bit AttrIndx that indexes into MAIR_EL1.
MAIR_EL1 defines up to 8 memory attribute configurations.

MAIR_EL1 is a 64-bit register with 8 × 8-bit attribute fields:
  ┌────────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
  │ Attr7  │ Attr6  │ Attr5  │ Attr4  │ Attr3  │ Attr2  │ Attr1  │ Attr0  │
  │[63:56] │[55:48] │[47:40] │[39:32] │[31:24] │[23:16] │[15:8]  │ [7:0]  │
  └────────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┘

Each 8-bit attribute field encodes:
  Upper 4 bits: Outer attribute
  Lower 4 bits: Inner attribute
  
  0x00 = Device-nGnRnE (strongly-ordered device memory)
  0x04 = Device-nGnRE  (device, allows Early write ack)
  0x08 = Device-nGRE   (device, allows Reordering + Early ack)
  0x0C = Device-GRE    (device, allows Gathering + Reordering)
  0x44 = Normal Non-cacheable
  0xBB = Normal Write-Through, R/W Allocate
  0xFF = Normal Write-Back, R/W Allocate (best performance!)

Memory types explained:
┌──────────────────────────────────────────────────────────────┐
│ Device-nGnRnE (0x00):                                       │
│   • No Gathering: each access becomes separate bus txn      │
│   • No Reordering: strict program order                     │
│   • No Early write acknowledgement                          │
│   • Use for: interrupt controller, timer, MMIO that triggers│
│     side effects on every access                            │
│                                                              │
│ Device-nGnRE (0x04):                                         │
│   • Like nGnRnE but write can be acknowledged early         │
│   • Common for most MMIO peripherals                        │
│                                                              │
│ Normal Non-cacheable (0x44):                                 │
│   • Data goes directly to memory (bypasses cache)           │
│   • Use for: DMA buffers (NB: could also use cacheable      │
│     + cache maintenance depending on DMA coherency)         │
│                                                              │
│ Normal Write-Back (0xFF):                                    │
│   • Data cached, written back on eviction                   │
│   • Both read and write allocate into cache                 │
│   • Use for: all normal memory (code, data, stack, heap)    │
│   • Best performance for CPU-intensive workloads            │
│                                                              │
│ Normal Write-Through (0xBB):                                 │
│   • Data cached AND written to memory simultaneously        │
│   • Use for: shared memory where coherency complexity is    │
│     not desired (rare in practice)                          │
└──────────────────────────────────────────────────────────────┘

Linux kernel MAIR setup (arch/arm64/include/asm/memory.h):
  MAIR_EL1 = 0x000000000044FF00
  Attr0 (idx 0) = 0x00 → Device-nGnRnE (MMIO)
  Attr1 (idx 1) = 0xFF → Normal WB (cacheable memory)
  Attr2 (idx 2) = 0x44 → Normal Non-cacheable
  
  Page table entry: AttrIndx = 1 → use Attr1 → WB cacheable
```

---

## Q5. [L3] Explain the ARM memory model (ARM memory ordering rules). How does it differ from x86 TSO?

**Answer:**

```
ARM uses a WEAKLY ORDERED memory model — loads and stores can be
observed by other cores in a different order than program order.

ARM ordering rules for Normal memory:
  1. Load-Load: can be reordered (A loads before B, another core
     may see B's result before A's)
  2. Load-Store: can be reordered
  3. Store-Load: can be reordered (the classic!)
  4. Store-Store: can be reordered (!)
  
  Only DEPENDENCIES are guaranteed:
    LDR X0, [X1]
    LDR X2, [X0]   // Depends on X0 → ordered after first LDR
    // Address dependency: guaranteed ordered

x86 TSO (Total Store Order):
  1. Load-Load: ORDERED (never reordered)
  2. Load-Store: ORDERED
  3. Store-Load: can be reordered (store buffer)
  4. Store-Store: ORDERED (never reordered)
  
  x86 is MUCH stronger — only store→load can reorder.
  On ARM, EVERYTHING can reorder (without barriers).

Comparison:
┌────────────────┬────────────┬──────────────┐
│ Reorder Type   │ ARM (weak) │ x86 (TSO)    │
├────────────────┼────────────┼──────────────┤
│ Load → Load    │ Allowed    │ Forbidden    │
│ Load → Store   │ Allowed    │ Forbidden    │
│ Store → Store  │ Allowed    │ Forbidden    │
│ Store → Load   │ Allowed    │ Allowed      │
│ Dependent loads│ Ordered    │ Ordered      │
└────────────────┴────────────┴──────────────┘

Why ARM chose weak ordering:
  • More freedom for hardware optimization
  • OoO core can issue loads earlier, hide latency
  • Store buffer can merge/reorder stores
  • Better performance on ARM (but harder to program)
  
  x86 pays for TSO with hardware:
  • Larger store buffer with ordering checks
  • Store → Load ordering requires fencing in store buffer
  • ~10-20% area/power overhead estimated

Classic bug (Dekker's algorithm breaks on ARM):
  Thread 1:           Thread 2:
  STR #1, [flag1]     STR #1, [flag2]
  LDR X0, [flag2]     LDR X0, [flag1]
  
  On ARM, both threads can read 0! (both stores reordered after loads)
  On x86, at least one sees 1 (TSO prevents this)
  
  Fix on ARM:
  Thread 1:               Thread 2:
  STR #1, [flag1]         STR #1, [flag2]
  DMB ISH                 DMB ISH
  LDR X0, [flag2]         LDR X0, [flag1]

Multi-copy atomicity (ARMv8):
  ARM guarantees "other-multi-copy atomic":
  Once a store is visible to ANY other core, it's visible to ALL.
  This simplifies reasoning about concurrent code.
  (ARM32 did NOT have this guarantee — much harder!)
```

---

## Q6. [L2] What is ASID (Address Space Identifier)? How does it avoid TLB flushes on context switch?

**Answer:**

```
Problem without ASID:
  Process A: VA 0x1000 → PA 0x5000 (TLB entry)
  Context switch to Process B
  Process B: VA 0x1000 → PA 0x9000 (different mapping!)
  
  Without ASID: must flush ALL TLB entries on context switch
  (otherwise Process B hits Process A's stale TLB entry → WRONG PA)
  
  TLB flush cost: ~1000 cycles + subsequent TLB misses →
  context switch overhead explodes on high-context-switch workloads

With ASID:
  Process A (ASID=5): VA 0x1000, ASID=5 → PA 0x5000
  Process B (ASID=7): VA 0x1000, ASID=7 → PA 0x9000
  
  Both entries coexist in TLB!
  TLB lookup: match VA AND ASID → correct translation
  Context switch: change TTBR0_EL1 (includes ASID) → no flush!

ASID in ARMv8:
  TTBR0_EL1[63:48] = ASID (16 bits if TCR_EL1.AS=1)
  Or TTBR0_EL1[55:48] = ASID (8 bits if TCR_EL1.AS=0)
  
  8-bit ASID: 256 concurrent processes without TLB flush
  16-bit ASID: 65536 concurrent processes without TLB flush
  
  When ASIDs run out:
    Linux: allocates ASIDs from a bitmap
    When all 256/65536 used → "ASID rollover":
    1. Flush ALL TLB entries (TLBI VMALLE1IS)
    2. Reset ASID bitmap
    3. Assign fresh ASIDs to active processes
    4. Inactive processes get new ASID on next schedule

Global pages (nG bit):
  Page table entry bit [11] = nG (non-Global)
  nG=0: Global page → TLB entry matches ALL ASIDs
        Used for: kernel mappings (same in all processes)
  nG=1: Non-global → TLB entry tagged with ASID
        Used for: user-space mappings

  ┌───────────────────────────────────────────────────────┐
  │ TLB Entry:                                           │
  │  [VA Tag] [ASID] [VMID] [PA] [Attrs] [Global?]     │
  │                                                      │
  │ Lookup match:                                        │
  │  Global: VA match → HIT (ignore ASID)               │
  │  Non-global: VA match AND ASID match → HIT          │
  └───────────────────────────────────────────────────────┘
```

---

## Q7. [L1] What is a page fault? Walk through what happens when a process accesses unmapped memory.

**Answer:**

```
A page fault occurs when the MMU cannot translate a virtual address
to a physical address. This generates a Data Abort (EC=0x24/0x25)
or Instruction Abort (EC=0x20/0x21) exception.

Types of page faults:
┌─────────────────────────────────────────────────────────────────┐
│ 1. Translation Fault (DFSC=0x04-0x07):                         │
│    Page table entry is invalid (bit [0]=0)                     │
│    → Page not mapped. Linux checks VMAs:                        │
│      a) VA in valid VMA → demand paging (allocate + map page)  │
│      b) VA not in any VMA → SIGSEGV (segfault)                │
│                                                                  │
│ 2. Permission Fault (DFSC=0x0D-0x0F):                          │
│    Page exists but access violates AP bits                     │
│    → EL0 writing to read-only page                             │
│    → Kernel writing to user page (PAN fault)                   │
│    → Examples: copy-on-write trigger, stack guard page         │
│                                                                  │
│ 3. Access Flag Fault (DFSC=0x09-0x0B):                         │
│    Page exists but AF bit = 0 (never accessed)                 │
│    → Used for tracking page access patterns (aging)            │
│    → Linux: if HW access flag mgmt (HA=1), never happens      │
│                                                                  │
│ 4. Alignment Fault:                                             │
│    Unaligned exclusive or stack pointer access                 │
└─────────────────────────────────────────────────────────────────┘

Complete page fault flow (demand paging):

  User process (EL0):
    LDR X0, [X1]          // X1 = VA 0x7000_1000 (never accessed)
      │
  MMU: walk page tables → L3 entry is invalid (0x0)
       → Data Abort exception!
      │
  Hardware: 
    SPSR_EL1 = PSTATE
    ELR_EL1 = PC of LDR instruction
    ESR_EL1.EC = 0x24 (Data Abort from lower EL)
    ESR_EL1.ISS.DFSC = 0x07 (Translation fault, Level 3)
    ESR_EL1.ISS.WnR = 0 (read)
    FAR_EL1 = 0x7000_1000 (faulting address)
    PC = VBAR_EL1 + 0x400
      │
  Linux kernel (EL1):
    1. Read ESR_EL1 → Translation fault from EL0
    2. Read FAR_EL1 → 0x7000_1000
    3. Find VMA: find_vma(current->mm, 0x7000_1000)
       → Found: VMA for heap region, permissions = R/W
    4. Allocate physical page: alloc_pages(GFP_USER, 0)
       → Got page at PA 0x1234_5000
    5. Map in page tables: set L3 entry for VA 0x7000_1000
       → L3[index] = PA 0x1234_5000 | Valid | AF | AP(R/W) | ...
    6. Return from exception: ERET
      │
  User process (EL0):
    CPU retries LDR X0, [X1]
    MMU: walk page tables → L3 entry valid → PA 0x1234_5000
    Load succeeds! Process never knew the fault happened.
```

---

## Q8. [L3] Explain Copy-on-Write (COW). How do page tables and permission faults implement it?

**Answer:**

```
Copy-on-Write allows fork() to be fast by sharing memory UNTIL
either process writes to it. Implemented via page table permissions.

fork() without COW (old way):
  • Copy ALL parent's memory to child → SLOW (100s of MB)
  • Child may exec() immediately → wasted copy!

fork() with COW:
  1. Share ALL parent's pages with child
  2. Mark shared pages as READ-ONLY in BOTH processes
  3. On write: permission fault → kernel copies just that page
  
  Step-by-step:
  
  BEFORE fork():
  ┌─────────────────────────────────────────────────────┐
  │ Parent (ASID 5):                                    │
  │   VA 0x1000 → PA 0xA000, AP=R/W, refcount=1       │
  │   VA 0x2000 → PA 0xB000, AP=R/W, refcount=1       │
  └─────────────────────────────────────────────────────┘
  
  AFTER fork():
  ┌─────────────────────────────────────────────────────┐
  │ Parent (ASID 5):                                    │
  │   VA 0x1000 → PA 0xA000, AP=R/O ←── changed!      │
  │   VA 0x2000 → PA 0xB000, AP=R/O ←── changed!      │
  │                                                     │
  │ Child (ASID 7):                                    │
  │   VA 0x1000 → PA 0xA000, AP=R/O (same PA!)        │
  │   VA 0x2000 → PA 0xB000, AP=R/O (same PA!)        │
  │                                                     │
  │ PA 0xA000 refcount=2, PA 0xB000 refcount=2         │
  └─────────────────────────────────────────────────────┘
  
  Child writes to VA 0x1000:
  1. STR X0, [0x1000] → MMU → Permission Fault!
     (page is R/O but process wants to write)
  2. ESR_EL1.ISS.WnR = 1 (write), DFSC = 0x0F (Permission L3)
  3. Linux COW handler:
     a. Check: is this a COW page? (VMA says R/W, PTE says R/O)
     b. Yes → refcount > 1, need to copy
     c. Allocate new page PA 0xC000
     d. Copy contents: 0xA000 → 0xC000 (4KB memcpy)
     e. Update child's PTE: VA 0x1000 → PA 0xC000, AP=R/W
     f. Decrement 0xA000 refcount: 2→1
     g. If refcount == 1: parent can restore R/W on its PTE
  4. ERET → retry STR → succeeds (now R/W)
  
  AFTER COW fault:
  ┌─────────────────────────────────────────────────────┐
  │ Parent: VA 0x1000 → PA 0xA000, AP=R/W, refcount=1 │
  │ Child:  VA 0x1000 → PA 0xC000, AP=R/W, refcount=1 │
  │ Both:   VA 0x2000 → PA 0xB000, AP=R/O, refcount=2 │
  │         (still shared — neither wrote to it yet)    │
  └─────────────────────────────────────────────────────┘
```

---

## Q9. [L2] What is the TLB structure in ARMv8? Explain TLBI instructions and their variants.

**Answer:**

```
TLB Invalidation instructions (TLBI) are critical for correctness
when page tables change. Wrong TLB entries → CATASTROPHIC bugs.

TLBI instruction format:
  TLBI <type><level>{IS|OS}, {Xt}
  
  type: what to invalidate
  level: which exception level's translations
  IS: Inner Shareable (broadcast to all cores in cache domain)
  OS: Outer Shareable (all agents including GPUs, DMA)
  Xt: register containing VA/ASID/VMID

Key TLBI variants:
┌───────────────────────────┬──────────────────────────────────┐
│ Instruction               │ What it invalidates               │
├───────────────────────────┼──────────────────────────────────┤
│ TLBI VMALLE1IS            │ ALL EL1&0 entries, inner share  │
│                           │ (entire address space, all ASIDs)│
│                           │ Heavy! Used at ASID rollover    │
├───────────────────────────┼──────────────────────────────────┤
│ TLBI VALE1IS, X0          │ By VA, Last level, EL1          │
│                           │ X0 = VA[55:12]:ASID             │
│                           │ Used when unmapping one page     │
├───────────────────────────┼──────────────────────────────────┤
│ TLBI VAE1IS, X0           │ By VA, ALL levels, EL1          │
│                           │ Invalidates walk caches too     │
│                           │ Needed when changing intermediate│
│                           │ page table entries               │
├───────────────────────────┼──────────────────────────────────┤
│ TLBI ASIDE1IS, X0         │ By ASID, EL1                    │
│                           │ All entries for one ASID         │
│                           │ Used when destroying mm_struct  │
├───────────────────────────┼──────────────────────────────────┤
│ TLBI IPAS2E1IS, X0        │ By IPA, Stage 2, EL1           │
│                           │ Used by hypervisor when changing │
│                           │ Stage-2 mappings for a VM       │
├───────────────────────────┼──────────────────────────────────┤
│ TLBI ALLE2IS              │ All EL2 entries                  │
│                           │ Used by hypervisor               │
├───────────────────────────┼──────────────────────────────────┤
│ TLBI ALLE3                │ All EL3 entries (no IS needed — │
│                           │ EL3 runs on one core typically) │
└───────────────────────────┴──────────────────────────────────┘

Correct sequence for page table update:
  // 1. Modify page table entry
  STR XZR, [page_table_entry]         // Invalidate PTE
  
  // 2. Invalidate stale TLB entry
  TLBI VALE1IS, X0                     // Broadcast to all cores
  
  // 3. Wait for TLBI to complete on all cores
  DSB ISH                              // Data Sync Barrier
  
  // 4. Ensure subsequent instructions use new translation
  ISB                                  // Pipeline flush
  
  CRITICAL: forgetting DSB after TLBI → other cores may still
  use stale TLB entries → memory corruption, security hole!

TLBI performance:
  Local (no IS): ~10-50 cycles
  IS broadcast: ~100-500 cycles (must reach all cores)
  All entries flush: ~1000+ cycles
  
  Linux batches TLBIs: collects dirty ranges, flushes once
  (mmu_gather framework)
```

---

## Q10. [L3] What are huge pages (block mappings) in ARMv8? When and why would you use them?

**Answer:**

```
ARMv8 supports three page sizes with block mappings at different
levels:

With 4KB granule:
  L3 page:  4KB   (standard page)
  L2 block: 2MB   (512 × 4KB)
  L1 block: 1GB   (512 × 2MB)

With 16KB granule:
  L3 page:  16KB
  L2 block: 32MB
  L1 block: 64GB  (if supported)

With 64KB granule:
  L3 page:  64KB
  L2 block: 512MB

How block mapping works:
  Instead of L2 entry pointing to L3 table:
  L2 entry IS the final translation (descriptor type = block)
  
  Normal 4KB page walk: L0 → L1 → L2 → L3 (4 lookups)
  2MB block walk:       L0 → L1 → L2 (3 lookups — skip L3!)
  1GB block walk:       L0 → L1 (2 lookups — skip L2 & L3!)

Benefits:
┌──────────────────────────────────────────────────────────────┐
│ 1. Fewer TLB entries needed:                                 │
│    1 TLB entry covers 2MB (vs 512 entries for 4KB pages)    │
│    → Dramatically reduces TLB misses for large data sets    │
│                                                              │
│ 2. Fewer page table walks:                                   │
│    Skip one level of page table → faster walks (and fewer   │
│    memory accesses per walk)                                 │
│                                                              │
│ 3. Less page table memory:                                   │
│    No L3 tables needed for 2MB blocks → saves 4KB per 2MB  │
│                                                              │
│ Performance impact (database workload):                      │
│    4KB pages: TLB miss rate ~5%, IPC 1.8                    │
│    2MB pages: TLB miss rate ~0.3%, IPC 2.4  (+33%!)        │
│    1GB pages: TLB miss rate ~0.01%, IPC 2.5                 │
└──────────────────────────────────────────────────────────────┘

Linux huge pages:
  Transparent Huge Pages (THP):
    echo always > /sys/kernel/mm/transparent_hugepage/enabled
    Kernel automatically uses 2MB pages when possible
    
  Explicit hugetlbfs:
    echo 1024 > /proc/sys/vm/nr_hugepages  // Reserve 1024 × 2MB
    mount -t hugetlbfs none /mnt/huge
    mmap(NULL, 2*1024*1024, PROT_READ|PROT_WRITE,
         MAP_PRIVATE|MAP_HUGETLB, -1, 0);
    
  1GB pages (for databases, VMs):
    Boot param: hugepagesz=1G hugepages=16
    → Reserves 16 GB as 1GB pages at boot time
    Must be reserved early (before memory fragmentation)

When NOT to use huge pages:
  • Small working sets (waste memory — internal fragmentation)
  • Many small processes (fork+COW → COW copies 2MB per fault!)
  • Memory-constrained systems (cannot reclaim partial huge page)
```

---

## Q11. [L2] What is the difference between Inner Shareable, Outer Shareable, and Non-shareable memory?

**Answer:**

```
Shareability domains define WHICH observers (cores, GPUs, DMA
engines) must see a coherent view of memory.

┌─────────────────────────────────────────────────────────────────┐
│                        System                                    │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                 Outer Shareable Domain                     │  │
│  │  ┌─────────────────────────────────────────────────┐     │  │
│  │  │           Inner Shareable Domain                  │     │  │
│  │  │                                                    │     │  │
│  │  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐            │     │  │
│  │  │  │Core 0│ │Core 1│ │Core 2│ │Core 3│            │     │  │
│  │  │  └──────┘ └──────┘ └──────┘ └──────┘            │     │  │
│  │  │  (all CPU cores in the cache-coherent cluster)   │     │  │
│  │  │                                                    │     │  │
│  │  └────────────────────────────────────────────────────┘     │  │
│  │                                                              │  │
│  │  ┌──────┐  ┌──────┐  ┌──────┐                              │  │
│  │  │ GPU  │  │ DMA  │  │ NPU  │                              │  │
│  │  └──────┘  └──────┘  └──────┘                              │  │
│  │  (masters that share memory but may not be cache-coherent) │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  ┌──────────────────┐                                              │
│  │ Non-shareable     │                                              │
│  │ (core-private,   │                                              │
│  │  not broadcast)  │                                              │
│  └──────────────────┘                                              │
└─────────────────────────────────────────────────────────────────────┘

Configuration:
  Page table entry SH bits [9:8]:
    00 = Non-shareable (core accesses only, no cache maintenance
         broadcast; used for core-private data)
    10 = Outer Shareable (visible to all masters including GPU/DMA
         cache maintenance and barrier broadcasts reach all)
    11 = Inner Shareable (visible to all coherent cores
         DMB ISH / DSB ISH scope)

  Barrier scope:
    DMB ISH:  orders memory as seen by all Inner Shareable observers
    DMB OSH:  orders memory as seen by all Outer Shareable observers
    DMB SY:   orders memory as seen by Full System
    DMB NSH:  orders memory only for the local core (rarely used)

Practical impact:
  • Most memory: Inner Shareable (CPU cores are coherent)
  • DMA buffers: Outer Shareable (DMA engine needs to see)
  • MMIO: Device memory (shareability N/A, handled by device type)
  
  Linux: kernel memory = Inner Shareable (11)
         dma_alloc_coherent = Outer Shareable (10)
```

---

## Q12. [L2] What is the MMU table walker? How does it work and what can go wrong?

**Answer:**

```
The MMU Table Walker is HARDWARE that automatically reads page
tables from memory when there's a TLB miss.

Table Walker operation:
  1. TLB miss: VA not in µTLB or main TLB
  2. Table Walker activates (does not stall entire core on OoO)
  3. Reads TTBR0_EL1 or TTBR1_EL1 (based on VA upper bits)
  4. Computes L0 table address: TTBR + L0_index * 8
  5. Issues LOAD to memory for L0 descriptor
     → May hit in L1/L2 cache (PTE caching!)
  6. Reads L0 descriptor → extracts L1 table address
  7. Issues LOAD for L1 descriptor
  8. Reads L1 → L2 table address
  9. Issues LOAD for L2 descriptor
  10. Reads L2 → L3 table address (or block mapping)
  11. Issues LOAD for L3 descriptor
  12. Extracts PA + attributes → fills TLB entry

  Total: 4 memory accesses for 4-level walk
  Latency: 15 cycles (all in L1) to 400+ cycles (page tables in DRAM)

Hardware features:
┌────────────────────────────────────────────────────────────────┐
│ Multiple walkers: Cortex-A78 has 2-4 concurrent table walkers │
│   → Multiple TLB misses can be resolved in PARALLEL           │
│                                                                │
│ PTE caching: Page table entries are cached in L1/L2          │
│   → L2/L1 table entries often hit in cache                   │
│   → Effective walk time: 1-2 cache misses (not 4)           │
│                                                                │
│ Walk cache: Some cores cache intermediate translations        │
│   (L0/L1/L2 entries) separately from the data cache          │
│   → Skip first 1-2 levels of walk                           │
│                                                                │
│ Speculative walks: Walker can start on predicted addresses   │
│   → Pre-fill TLB before CPU actually accesses the VA        │
└────────────────────────────────────────────────────────────────┘

What can go wrong:
  1. Translation fault: invalid PTE → Data Abort exception
  2. Permission fault: valid PTE but wrong permissions
  3. Access flag fault: AF=0 → need to set it
  4. External abort during walk: memory error reading PTE
     → generates SError or synchronous external abort
  5. Walk cache vs TLB coherency: after changing PTEs,
     must TLBI + DSB to ensure walker reads new PTEs
  
  Bug scenario:
    Thread A: updates PTE for VA 0x1000 (changes PA)
    Thread A: forgets TLBI
    Thread B: accesses VA 0x1000 → gets OLD PA from TLB!
    → Thread B reads/writes WRONG physical memory!
    → Memory corruption, security vulnerability
```

---

## Q13. [L1] What is demand paging vs. pre-paging? How does Linux implement each?

**Answer:**

```
Demand Paging:
  Pages are only allocated and mapped when FIRST ACCESSED.
  malloc(1GB) → NO physical memory allocated!
  Only when you read/write a page: page fault → allocate page.
  
  malloc(1GB) → mmap(MAP_PRIVATE|MAP_ANONYMOUS, 1GB)
    → VMA created (virtual range recorded)
    → PTEs all invalid (no physical pages)
    → Physical memory used: 0
  
  *ptr = 42;  // Write to first page
    → Page fault (translation fault)
    → Kernel: allocate 1 page (4KB), map it, zero-fill
    → Physical memory used: 4KB
  
  ptr[4096] = 42;  // Write to second page
    → Another page fault → allocate another 4KB
    → Physical memory used: 8KB
  
  Benefits:
    • Fast malloc() (just VMA, no page allocation)
    • Programs often allocate more than they use
    • Only use physical memory for actually-touched pages
  
  Drawbacks:
    • Page fault overhead per first access (~1-5 µs per fault)
    • Unpredictable latency spikes (fault during real-time work)

Pre-paging (Prefault/Readahead):
  Allocate and map pages BEFORE they are accessed.
  
  Linux implements:
  1. READ-AHEAD (file pages):
     Reading page N of a file → kernel also reads pages N+1..N+31
     into page cache (readahead window)
     → Subsequent reads hit page cache → no fault
     
  2. FAULT-AROUND (anonymous pages, CONFIG_FAULT_AROUND_BYTES):
     When faulting in 1 page, also map nearby pages that are
     already in page cache → reduce future faults
     Default: 64KB (16 pages around the fault)
     
  3. MAP_POPULATE (mmap flag):
     mmap(..., MAP_POPULATE, ...) → immediately fault in ALL pages
     Use for: latency-sensitive apps (databases, RT)
     
  4. mlock()/mlockall():
     Lock pages in RAM → prevents paging AND pre-faults them
     Use for: real-time applications, crypto key storage
     
  5. madvise(MADV_WILLNEED):
     Hint to kernel: "I'll need these pages soon"
     Kernel starts readahead asynchronously
```

---

## Q14. [L3] What is the Kernel Virtual Address layout on ARM64 Linux? Explain linear map, vmalloc, modules, etc.

**Answer:**

```
ARM64 Linux kernel virtual address space layout (48-bit VA):

  ┌──────────────────────────────────────────────────────────────┐
  │ Address Range                │ Size   │ Purpose               │
  ├──────────────────────────────┼────────┼───────────────────────┤
  │ 0x0000_0000_0000_0000       │        │ User space             │
  │     to                      │ 256 TB │ (per-process,          │
  │ 0x0000_FFFF_FFFF_FFFF       │        │  TTBR0_EL1)           │
  ├──────────────────────────────┼────────┼───────────────────────┤
  │ 0x0001 to 0xFFFE...         │  huge  │ Hole (faults if       │
  │ (gap between user/kernel)   │        │  accessed)             │
  ├──────────────────────────────┼────────┼───────────────────────┤
  │ 0xFFFF_0000_0000_0000       │ 256 TB │ Kernel space           │
  │     to                      │        │ (TTBR1_EL1)           │
  │ 0xFFFF_FFFF_FFFF_FFFF       │        │                       │
  ├──────────────────────────────┼────────┼───────────────────────┤
  │ Kernel space breakdown:                                       │
  │                                                               │
  │ FFFF_0000_0000_0000         │ 128 GB │ Modules region         │
  │   to FFFF_0000_1FFF_FFFF   │        │ (loadable kernel mods)│
  │                              │        │ BL range from kernel  │
  ├──────────────────────────────┼────────┼───────────────────────┤
  │ FFFF_0000_2000_0000         │ ~125TB │ vmalloc / ioremap     │
  │   to FFFF_7BFF_BFFF_FFFF   │        │ (VA-contiguous,       │
  │                              │        │  PA-scattered)        │
  ├──────────────────────────────┼────────┼───────────────────────┤
  │ FFFF_8000_0000_0000         │ 128 TB │ Linear Map            │
  │   to FFFF_FFFF_FFFF_FFFF   │        │ (VA = PA + offset)    │
  │                              │        │ ALL physical memory   │
  │                              │        │ mapped here 1:1       │
  └──────────────────────────────┴────────┴───────────────────────┘

Linear Map (most important for kernel engineers):
  • Maps ALL physical RAM at a fixed offset:
    VA = PA + PAGE_OFFSET
    PA = VA - PAGE_OFFSET
    PAGE_OFFSET = 0xFFFF_8000_0000_0000 (typical)
  
  • __pa(va) and __va(pa) macros convert between them
  • Used by: kmalloc, page allocator, slab allocator
  • Uses 2MB or 1GB block mappings for efficiency!
  • virt_to_phys() / phys_to_virt() work only in linear map

vmalloc region:
  • For virtually contiguous, physically scattered allocations
  • vmalloc(4*1024*1024) → 4MB contiguous VA, but may be
    1024 non-contiguous 4KB physical pages
  • Also used for ioremap() (mapping MMIO into kernel VA)
  • Slower than kmalloc (TLB pollution — separate page table entries)

Modules region:
  • 128 GB near kernel text
  • BL instruction has ±128 MB range
  • Module code must be within BL range of kernel functions
  • KASLR may randomize module load address within this region

KASLR (Kernel Address Space Layout Randomization):
  • Kernel text randomized at boot (physical and virtual)
  • Linear map offset randomized
  • Makes ROP/JOP attacks harder
```

---

## Q15. [L2] What is PAN (Privileged Access Never) and UAO (User Access Override)?

**Answer:**

```
PAN (Privileged Access Never) — ARMv8.1:
  PROBLEM: Kernel bugs can accidentally dereference USER pointers
  directly. If kernel reads/writes user memory without proper
  checks, it creates security vulnerabilities (info leak, write
  primitive for exploits).
  
  SOLUTION: When PSTATE.PAN = 1, ANY kernel access to user-mapped
  memory causes a PERMISSION FAULT, even though EL1 normally has
  full access.
  
  ┌───────────────────────────────────────────────────────────┐
  │ Without PAN:                                              │
  │   Kernel EL1: LDR X0, [user_ptr]  → succeeds silently!  │
  │   Attacker controls user_ptr → kernel reads arbitrary    │
  │   user-controlled data (TOCTOU, confused deputy)         │
  │                                                           │
  │ With PAN (PSTATE.PAN = 1):                               │
  │   Kernel EL1: LDR X0, [user_ptr]  → PERMISSION FAULT!   │
  │   Forces kernel to use copy_from_user() / get_user()     │
  │   which temporarily disable PAN, validate address,       │
  │   and re-enable PAN.                                     │
  └───────────────────────────────────────────────────────────┘
  
  Kernel copy_from_user() implementation:
    MSR PAN, #0           // Disable PAN (allow user access)
    LDTR X0, [user_addr]  // Load with EL0 privilege check
    MSR PAN, #1           // Re-enable PAN immediately
  
  LDTR/STTR: Load/Store with user privilege — checks EL0
  permissions even when executing at EL1. Double protection!

UAO (User Access Override) — ARMv8.2:
  PROBLEM: LDTR/STTR always use EL0 permissions. But sometimes
  the kernel uses copy_from_user() on KERNEL addresses
  (e.g., set_fs(KERNEL_DS) in old Linux).
  
  SOLUTION: When PSTATE.UAO = 1, LDTR/STTR use EL1 permissions
  instead of EL0 permissions.
  
  → Allows copy_from_user() to work correctly for both user
    and kernel addresses without changing the instruction.
  → Linux 5.10+ removed set_fs() entirely, making UAO less critical
    but it's still used for compatibility.
```

---

## Q16. [L3] Explain 52-bit virtual and physical address extensions (LVA/LPA). Why were they added?

**Answer:**

```
Standard ARMv8: 48-bit VA (256 TB) and 48-bit PA (256 TB)
ARMv8.2-LVA: 52-bit VA (4 PB user or kernel space)
ARMv8.2-LPA: 52-bit PA (4 PB physical address space)

Why needed:
  • Server systems with >256 TB physical RAM (not yet, but
    architecture must plan ahead)
  • Applications with huge virtual address spaces
    (e.g., sparse hash tables, huge mmap regions)
  • Not all SoCs implement this — it's optional

How 52-bit PA works:
  Page table descriptors only have bits [47:12] for output address.
  Where do bits [51:48] go?
  
  → They're stored in previously-reserved bits of the PTE:
    L1 block: bits [15:12] of PTE hold OA[51:48]
    L2 block: bits [15:12] of PTE hold OA[51:48]
    L3 page:  bits [15:12] of PTE hold OA[51:48]
  
  Only 64KB granule supports 52-bit output addresses.
  4KB and 16KB granules: still limited to 48-bit PA.

How 52-bit VA works:
  With 64KB granule:
    VA = 52 bits → need L0 table (additional level)
    Or: uses 3-level walk with expanded index
  
  TCR_EL1.T0SZ = 12 → 64 - 12 = 52-bit VA space
  TCR_EL1.DS = 1 → enable 52-bit descriptor format

Linux support:
  CONFIG_ARM64_PA_BITS_52=y
  CONFIG_ARM64_VA_BITS_52=y
  
  Linux detects at boot whether CPU supports it.
  Falls back to 48-bit if not.
  Linear map adjusted for larger physical address space.
  
  Practical: as of 2024, no commercial system needs >48-bit PA.
  But architecture readiness ensures forward compatibility.
```

---

## Q17. [L2] How does the kernel handle DMA memory on ARM64? What is CMA, dma_alloc_coherent, and IOMMU?

**Answer:**

```
DMA (Direct Memory Access) engines access physical memory directly
without CPU involvement. This creates challenges:

  Problem 1: DMA engine uses PHYSICAL addresses
    CPU: VA 0xFFFF_8000_0001_0000 → PA 0x0001_0000
    DMA: needs PA 0x0001_0000 (doesn't know about VA)
    → Kernel must provide physical address to DMA

  Problem 2: Cache coherency
    CPU writes data to cache → DMA reads from DRAM → stale data!
    DMA writes to DRAM → CPU reads cache → stale data!
    → Must synchronize caches with DMA

Solutions:

1. dma_alloc_coherent():
   Allocates physically contiguous, non-cacheable memory
   CPU and DMA always see same data (no cache issues)
   Performance hit: uncached memory is SLOW for CPU
   
   void *buf = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
   // buf = kernel VA (uncached)
   // dma_addr = physical/bus address for DMA engine

2. Streaming DMA (dma_map_single/dma_map_sg):
   Uses CACHEABLE memory + explicit sync
   Better performance for large transfers
   
   dma_addr_t dma = dma_map_single(dev, buf, size, DMA_TO_DEVICE);
   // Flushes cache (clean) for DMA_TO_DEVICE
   // Invalidates cache for DMA_FROM_DEVICE
   ... DMA transfers ...
   dma_unmap_single(dev, dma, size, DMA_TO_DEVICE);

3. CMA (Contiguous Memory Allocator):
   DMA engines often need LARGE contiguous physical regions
   (e.g., video frame buffer = 4MB contiguous)
   After boot, physical memory is fragmented → can't alloc 4MB!
   
   CMA: reserves a physical region at boot
   Normal pages: CMA region used for movable pages
   DMA request: migrate movable pages out → free contiguous block
   
   Boot param: cma=64M (reserve 64 MB for CMA)

4. IOMMU / SMMU (System MMU):
   An MMU for DMA engines! Translates DMA addresses → PA
   
   ┌──────┐    ┌───────┐    ┌──────┐    ┌──────┐
   │ DMA  │───▶│ SMMU  │───▶│ Bus  │───▶│ DRAM │
   │Engine│    │(IOMMU)│    │      │    │      │
   └──────┘    └───────┘    └──────┘    └──────┘
              DMA addr→PA
   
   Benefits:
   • DMA can use scattered pages (SMMU makes them contiguous)
   • Device isolation: one device can't access another's memory
   • Virtualization: guest VM's DMA addresses → host PA
   • No need for physically contiguous memory!
```

---

## Q18. [L1] What is memory-mapped I/O (MMIO)? How does ARM handle it differently from normal memory?

**Answer:**

```
MMIO maps hardware device registers into the physical address space.
CPU accesses device registers using normal load/store instructions
to specific physical addresses.

Example — UART register access:
  UART base address: PA 0x0900_0000
  UART data register: offset 0x00
  UART status register: offset 0x18
  
  // Write 'A' to UART
  LDR X0, =0x0900_0000      // Device base address
  MOV W1, #'A'
  STR W1, [X0]               // Write to data register
  
  // Read status
  LDR W2, [X0, #0x18]       // Read status register

MMIO vs Normal Memory — CRITICAL differences:
┌───────────────────────────────────────────────────────────────┐
│ Property        │ Normal Memory      │ MMIO (Device Memory)  │
├─────────────────┼────────────────────┼───────────────────────┤
│ Caching         │ Cacheable (WB/WT)  │ NEVER cached          │
│ Reordering      │ Can be reordered   │ Strict order (nGnRnE) │
│ Gathering       │ Allowed            │ Not allowed (each     │
│                 │                    │ access hits device)    │
│ Side effects    │ None               │ YES! Reading a reg    │
│                 │                    │ may clear interrupt   │
│ Write buffering │ Allowed            │ Not allowed (nRnE)    │
│ Speculative     │ Can prefetch       │ NEVER speculate       │
│ Multiple reads  │ May return cached  │ Each read hits device │
│ Access width    │ Flexible           │ Must match register   │
│                 │                    │ width exactly         │
└─────────────────┴────────────────────┴───────────────────────┘

Mapping MMIO in Linux kernel:
  void __iomem *base = ioremap(0x0900_0000, 0x1000);
  // Maps as Device-nGnRnE memory type (MAIR index 0)
  
  // Access via accessor functions (include barriers):
  u32 val = readl(base + 0x18);      // Read 32-bit
  writel(0x41, base);                  // Write 32-bit
  
  // readl/writel include DSB + access ordering
  // Raw versions (no barrier): readl_relaxed / writel_relaxed

Why NOT use LDR/STR directly on __iomem?
  1. Compiler may optimize away "redundant" reads
     (doesn't know reads have side effects)
  2. CPU may reorder or merge accesses
  3. readl/writel use volatile + barrier → correct behavior
```

---

## Q19. [L3] Explain memory tagging (MTE) in ARMv8.5. How does it detect use-after-free and buffer overflow?

**Answer:**

```
MTE (Memory Tagging Extension) adds a 4-bit TAG to EVERY 16 bytes
of physical memory and to the top bits of pointers. On every
access, the tags are compared — mismatch = BUG detected!

Architecture:
  ┌───────────────────────────────────────────────────────────┐
  │ Pointer: [63:60] = Key[3:0] + [59:56] = TAG[3:0]        │
  │          [55:0]  = Virtual Address                        │
  │                                                           │
  │ Memory:  Each 16-byte aligned granule has a 4-bit tag    │
  │          stored in separate tag memory (not in RAM data) │
  │                                                           │
  │ Access check:                                             │
  │   LDR X0, [X1]                                           │
  │   → Extract tag from X1[59:56]                           │
  │   → Read tag from memory tag store for address X1        │
  │   → Compare: pointer tag == memory tag?                  │
  │     YES → access proceeds                                │
  │     NO  → Tag Check Fault (synchronous or async)         │
  └───────────────────────────────────────────────────────────┘

Detecting use-after-free:
  1. malloc(32): allocator sets memory tag = 5 (random)
     Returns pointer with tag 5: 0x0500_0000_1234_5000
  2. Application uses pointer → tags match (5==5) ✓
  3. free(ptr): allocator changes memory tag to 9 (different!)
     Pointer still has tag 5 (dangling pointer)
  4. Application uses stale pointer:
     Pointer tag=5, Memory tag=9 → MISMATCH → FAULT!

Detecting buffer overflow:
  1. malloc(32) → tag=3, covers bytes [0..31] of allocation
     Adjacent allocation → tag=7, covers bytes [32..63]
  2. ptr[33] → pointer tag=3, but byte 33 has memory tag=7
     → MISMATCH → FAULT!
  
  ┌────────────────────────────────────────────────────────┐
  │    Allocation A (tag=3)     │  Allocation B (tag=7)   │
  │   [0  ...  15] [16 ... 31] │ [32 ... 47] [48 ... 63] │
  │    tag=3         tag=3      │  tag=7        tag=7     │
  │                              ▲                        │
  │                              │ ptr[33] with tag=3     │
  │                              │ Memory tag=7           │
  │                              │ MISMATCH! Bug caught!  │
  └────────────────────────────────────────────────────────┘

MTE instructions:
  IRG Xd, Xn, Xm       // Insert Random tag into pointer
  ADDG Xd, Xn, #imm, #tag  // Add offset and set tag
  SUBG Xd, Xn, #imm, #tag
  GMI Xd, Xn, Xm       // Tag Mask Insert
  STG [Xn]              // Store Allocation Tag (set memory tag)
  LDG Xt, [Xn]          // Load Allocation Tag
  STGM Xt, [Xn]         // Store Tag Multiple (clear region)
  
MTE modes:
  Synchronous: tag check fault → immediate exception (precise, slow)
  Asynchronous: tag mismatch recorded in TFSR_EL1, checked later
                (faster, but imprecise — harder to debug)
  
  SCTLR_EL1.TCF0: control MTE behavior for EL0
  SCTLR_EL1.TCF1: control MTE behavior for EL1
  
  Android: uses MTE in async mode for security, sync for debugging.
  Linux: CONFIG_ARM64_MTE=y, PROT_MTE flag in mmap()
```

---

## Q20. [L2] What is Pointer Authentication (PAC)? How does it protect return addresses?

**Answer:**

```
PAC (ARMv8.3) uses a cryptographic MAC (message authentication
code) embedded in unused pointer bits to detect tampering.

How it works:
  ┌──────────────────────────────────────────────────────────┐
  │ Normal 64-bit pointer:                                   │
  │ [63:56] = unused (TBI) or sign extension                │
  │ [55:48] = unused (for 48-bit VA)                        │
  │ [47:0]  = virtual address                               │
  │                                                          │
  │ PAC adds to unused bits:                                 │
  │ [63:48] = PAC signature (crypto hash of address+context)│
  │ [47:0]  = virtual address (unchanged)                   │
  └──────────────────────────────────────────────────────────┘

  Signing: PACIA X30, SP
    1. Takes X30 (return address) + SP (context/modifier)
    2. Uses secret key (APIAKey_EL1) stored in system register
    3. Computes: PAC = QARMA(address, context, key)
    4. Inserts PAC into upper bits of X30

  Verification: AUTIA X30, SP
    1. Extracts PAC from X30 upper bits
    2. Recomputes: expected = QARMA(address, context, key)
    3. Compares: PAC == expected?
       YES → strips PAC, restores original pointer → use it
       NO  → corrupts pointer so any use causes fault

Function prologue/epilogue with PAC:
  func:
    PACIASP                // Sign LR (X30) with SP as context
    STP X29, X30, [SP, #-16]!
    // ... function body ...
    LDP X29, X30, [SP], #16
    AUTIASP                // Verify LR — if tampered, corrupts it
    RET                    // If LR was modified → fault on RET!

Attack scenario:
  Without PAC:
    Stack buffer overflow → overwrite return address → ROP chain
    RET jumps to attacker-controlled address
    
  With PAC:
    Stack buffer overflow → overwrites return address
    But attacker doesn't know the key → can't forge valid PAC
    AUTIASP → PAC mismatch → pointer corrupted → fault!
    ROP chain is broken.

Keys (5 key pairs):
  APIAKey: Instruction address A key (for PACIA/AUTIA)
  APIBKey: Instruction address B key (for PACIB/AUTIB)
  APDAKey: Data address A key (for PACDA/AUTDA)
  APDBKey: Data address B key
  APGAKey: Generic authentication (PACGA — for data integrity)
  
  Keys are per-process (Linux randomizes at exec())
  Stored in EL1 system registers → user can't read them
  
  Linux: gcc -mbranch-protection=standard
  → Compiler auto-inserts PACIASP/AUTIASP in every function
```

---

Back to [Question & Answers Index](./README.md)
