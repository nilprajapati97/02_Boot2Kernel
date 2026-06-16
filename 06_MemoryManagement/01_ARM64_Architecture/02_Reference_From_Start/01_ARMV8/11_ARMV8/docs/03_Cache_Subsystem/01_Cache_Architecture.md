# Cache Architecture

## 1. Cache Basics

A cache stores copies of frequently used data from main memory. When the CPU
needs data, it checks the cache first (fast) before going to DRAM (slow).

```
Terminology:
  Cache Hit:   Data found in cache → fast access (~1-5 cycles)
  Cache Miss:  Data NOT in cache → must fetch from lower level/DRAM
  Cache Line:  Unit of data in cache (64 bytes on ARM, typically)
  Tag:         Identifier to match cache lines to memory addresses
  Set:         Group of cache lines that share the same index
  Way:         One slot within a set (set-associative cache)
  Hit Rate:    Percentage of accesses that hit in cache (target: >95%)
```

---

## 2. Cache Organization

### Direct-Mapped Cache

```
Each memory address maps to EXACTLY one cache location:

  Address: [  TAG  |  INDEX  |  OFFSET  ]
                      │
                      ▼
  ┌─────┬──────────────┬────────────────────────┐
  │Valid│     Tag       │        Data (64B)       │  ← Only ONE slot
  └─────┴──────────────┴────────────────────────┘

  Problem: Two addresses with same INDEX evict each other
  → High conflict miss rate
```

### Set-Associative Cache (Used in ARM)

```
N-way set associative: each index has N slots (ways)

  Example: 4-way set associative L1 cache (32 KB, 64B lines)

  Address decomposition:
  ┌──────────────────┬───────────┬──────────┐
  │      TAG         │  INDEX/SET│  OFFSET  │
  │   (remaining)    │  (7 bits) │  (6 bits)│
  │                   │  128 sets │  64 bytes│
  └──────────────────┴───────────┴──────────┘

  32 KB = 128 sets × 4 ways × 64 bytes/line

  Set 0:  ┌──────┬──────┬──────┬──────┐
          │ Way 0│ Way 1│ Way 2│ Way 3│
          │Tag|D │Tag|D │Tag|D │Tag|D │
          └──────┴──────┴──────┴──────┘
  Set 1:  ┌──────┬──────┬──────┬──────┐
          │ Way 0│ Way 1│ Way 2│ Way 3│
          └──────┴──────┴──────┴──────┘
  ...
  Set 127:┌──────┬──────┬──────┬──────┐
          │ Way 0│ Way 1│ Way 2│ Way 3│
          └──────┴──────┴──────┴──────┘

  Lookup: Hash address → select SET → compare TAG against all 4 ways
  If hit: return data[OFFSET]
  If miss: fetch from next level, evict one way (LRU/random)
```

---

## 3. ARM Cache Hierarchy Details

### L1 Instruction Cache (L1I)

```
┌───────────────────────────────────────────────────────────────┐
│  L1 Instruction Cache                                         │
│                                                                │
│  • Size: 32-64 KB (typical)                                   │
│  • Line size: 64 bytes                                        │
│  • Associativity: 4-way (A53/A55) or 4-way (A78)            │
│  • Access latency: 1-2 cycles (pipelined with fetch)          │
│  • Features:                                                   │
│    - VIPT (Virtually Indexed, Physically Tagged)               │
│    - Read-only (instruction fetch)                             │
│    - Parity or ECC protection                                  │
│    - Fills from L2 on miss                                     │
│  • Coherency: NOT snooped (no writes from other cores)        │
│    - Must use IC IALLU / IC IVAU for coherency                │
└───────────────────────────────────────────────────────────────┘
```

### L1 Data Cache (L1D)

```
┌───────────────────────────────────────────────────────────────┐
│  L1 Data Cache                                                │
│                                                                │
│  • Size: 32-64 KB (typical)                                   │
│  • Line size: 64 bytes                                        │
│  • Associativity: 4-way (A53) or 4-way (A78)                │
│  • Access latency: 3-5 cycles                                  │
│  • Write policy: Write-Back, Write-Allocate                    │
│  • Features:                                                   │
│    - VIPT (Virtually Indexed, Physically Tagged)               │
│    - Read & Write                                              │
│    - ECC protection                                            │
│    - Participates in coherency (snooped)                       │
│    - Hardware prefetcher (next-line, stride, stream)           │
│  • Coherency: Maintained via snoop protocol                    │
└───────────────────────────────────────────────────────────────┘
```

### L2 Cache

```
┌───────────────────────────────────────────────────────────────┐
│  L2 Cache (Unified — instructions + data)                     │
│                                                                │
│  • Size: 128 KB – 1 MB (per core or per pair)                 │
│  • Line size: 64 bytes                                        │
│  • Associativity: 8-16 way                                    │
│  • Access latency: 8-20 cycles                                 │
│  • Write policy: Write-Back, Write-Allocate                    │
│  • Organization:                                               │
│    - Cortex-A53/A55: private per core (128-256 KB)            │
│    - Cortex-A72/A78: private per core (256 KB-1 MB)           │
│  • PIPT (Physically Indexed, Physically Tagged)                │
│  • Inclusive or exclusive of L1                                │
│  • ECC/parity protection                                      │
│  • Hardware prefetcher                                         │
└───────────────────────────────────────────────────────────────┘
```

### L3 / System Level Cache (SLC)

```
┌───────────────────────────────────────────────────────────────┐
│  L3 / System Level Cache                                      │
│                                                                │
│  • Size: 1 MB – 32 MB+ (shared across all cores)             │
│  • Line size: 64 bytes                                        │
│  • Associativity: 16-way                                      │
│  • Access latency: 20-50 cycles                                │
│  • Part of the interconnect (CCI/CCN/CMN/DSU)                 │
│  • System cache (may also serve GPU, DMA)                     │
│  • Slice-based (distributed across interconnect)               │
│  • Snoop filter to reduce coherency traffic                    │
└───────────────────────────────────────────────────────────────┘
```

---

## 4. VIPT vs PIPT vs VIVT

```
┌──────────────────────────────────────────────────────────────────┐
│  Cache Tagging Model        Description                          │
├──────────────────────────────────────────────────────────────────┤
│                                                                   │
│  VIVT (Virtual Index,      • Indexed and tagged with VA          │
│        Virtual Tag)         • No TLB lookup needed for hit       │
│                              • Aliasing problems (same PA, diff VA)│
│                              • Must flush on context switch       │
│                              • NOT used in modern ARM cores       │
│                                                                   │
│  VIPT (Virtual Index,      • Indexed with VA bits (fast: no TLB)│
│        Physical Tag)        • Tagged with PA (accurate)          │
│                              • TLB lookup in parallel with index  │
│                              • Aliasing possible if index > page  │
│                              • Used for L1 caches in ARM          │
│                              • Works with ASID for context switch │
│                                                                   │
│  PIPT (Physical Index,     • Both index and tag from PA          │
│        Physical Tag)        • No aliasing (PA is unique)          │
│                              • Requires TLB first (slower)        │
│                              • Used for L2 and L3 caches          │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘

Why L1 is VIPT:
  The VA is available immediately (no TLB wait)
  Index the cache with VA while TLB translates in parallel
  By the time tag comparison happens, PA from TLB is ready

  ┌──────────┐    ┌───────────┐
  │    VA    │───▶│  Cache    │  Index with VA[11:6]
  │          │    │  Index    │  
  └──────────┘    └─────┬─────┘  
       │                │ Read all ways
       │         ┌──────▼──────┐
       │         │  Way 0 Tag  │
       │         │  Way 1 Tag  │  Compare against PA tag
       │         │  Way 2 Tag  │
       │         │  Way 3 Tag  │
       │         └──────┬──────┘
       │                │
  ┌────▼────┐    ┌──────▼──────┐
  │   TLB   │───▶│ Tag Compare │  PA from TLB arrives in time
  │  VA→PA  │    │  (hit/miss) │  
  └─────────┘    └─────────────┘
```

---

## 5. Cache Policies

### Replacement Policies

```
When all ways in a set are occupied, which one to evict?

  LRU (Least Recently Used):
    Evict the way that hasn't been accessed for longest
    → Best hit rate but complex hardware (track access order)
    → Used in most ARM L1 caches (pseudo-LRU approx.)

  Random:
    Evict a random way
    → Simple hardware, surprisingly good performance
    → Used in some L2 configurations

  PLRU (Pseudo-LRU):
    Tree-based approximation of LRU
    → Good balance of hardware cost and hit rate
    → Most common in ARM cores

  Example: 4-way pseudo-LRU tree
       ┌───┐
       │ 0 │ = Left side more recently used
       └─┬─┘
      ┌──┴──┐
    ┌─┴─┐ ┌─┴─┐
    │ 0 │ │ 1 │
    └─┬─┘ └─┬─┘
   ┌──┴─┐ ┌─┴──┐
   W0  W1  W2  W3  ← Evict candidate: follow "least recent" path
```

### Write Policies

```
Write-Back (WB) — Used for Normal memory:
  ┌──────────────────────────────────────────────────┐
  │  Store X0, [addr]                                 │
  │                                                    │
  │  1. Write to cache ONLY (mark line as "dirty")    │
  │  2. Memory is NOT updated yet                      │
  │  3. When line is evicted: write dirty data to RAM  │
  │                                                    │
  │  Advantage: Fewer memory writes, higher performance│
  │  Disadvantage: Data in cache ≠ data in RAM         │
  │               (coherency protocol handles this)    │
  └──────────────────────────────────────────────────┘

Write-Through (WT) — Sometimes used for shared regions:
  ┌──────────────────────────────────────────────────┐
  │  Store X0, [addr]                                 │
  │                                                    │
  │  1. Write to cache AND memory simultaneously      │
  │  2. Memory always has up-to-date data              │
  │                                                    │
  │  Advantage: Simpler coherency                      │
  │  Disadvantage: More memory bus traffic              │
  └──────────────────────────────────────────────────┘

Write-Allocate vs No-Write-Allocate:
  Write-Allocate: On write miss, allocate a cache line, then write
  No-Write-Allocate: On write miss, write directly to next level
```

---

## 6. Cache Identification Registers

```
CTR_EL0 — Cache Type Register:
  Bits [3:0]   — IminLine:  log2(I-cache line size / 4)
  Bits [19:16] — DminLine:  log2(D-cache line size / 4)
  Bits [27:24] — CWG:       Cache Writeback Granule
  Bits [23:20] — ERG:       Exclusives Reservation Granule
  Bit [28]     — DIC:       I-cache invalidation not required for data-to-instr coherence
  Bit [29]     — IDC:       D-cache clean not required for data-to-instr coherence

CLIDR_EL1 — Cache Level ID Register:
  Tells how many cache levels exist and their type

CSSELR_EL1 — Cache Size Selection Register:
  Select which cache level to query

CCSIDR_EL1 — Cache Size ID Register:
  Returns details (sets, ways, line size) for selected cache level

Example: Reading cache info
  MRS X0, CTR_EL0           // Get cache type
  UBFX X1, X0, #16, #4      // Extract DminLine
  MOV X2, #4
  LSL X2, X2, X1            // Data cache line size in bytes
```

---

## 7. Hardware Prefetching

ARM cores include hardware prefetchers that predict future memory accesses:

```
┌──────────────────────────────────────────────────────────────┐
│  Prefetcher Type      Description                            │
├──────────────────────────────────────────────────────────────┤
│  Next-Line            Prefetch the next cache line           │
│                        (spatial locality)                    │
│                                                              │
│  Stride               Detect regular access patterns         │
│                        (e.g., array[0], array[16], array[32])│
│                        → Prefetch array[48]                  │
│                                                              │
│  Stream               Detect sequential streams              │
│                        (forward or backward)                 │
│                        → Prefetch several lines ahead        │
│                                                              │
│  Software hint        PRFM PLDL1KEEP, [X0, #64]             │
│                        Hint to prefetch into L1 cache        │
└──────────────────────────────────────────────────────────────┘

Software prefetch instructions:
  PRFM PLDL1KEEP, [X0]      // Prefetch to L1 for load, keep
  PRFM PLDL1STRM, [X0]      // Prefetch to L1, streaming
  PRFM PLDL2KEEP, [X0]      // Prefetch to L2
  PRFM PSTL1KEEP, [X0]      // Prefetch to L1 for store
```

---

Next: [Cache Coherency →](./02_Cache_Coherency.md)
