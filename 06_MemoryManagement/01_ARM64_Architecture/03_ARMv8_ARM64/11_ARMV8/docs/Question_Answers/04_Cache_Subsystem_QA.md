# Cache Subsystem — Questions & Answers

---

## Q1. [L1] Describe the cache hierarchy in a typical ARMv8 system. What are L1, L2, and L3 caches?

**Answer:**

```
Modern ARM systems have a 2-3 level cache hierarchy:

  ┌─────────────────────────────────────────────────────────────┐
  │ Core 0               │ Core 1               │ Core 2/3     │
  │  ┌────────────────┐  │  ┌────────────────┐  │              │
  │  │ L1I   │ L1D    │  │  │ L1I   │ L1D    │  │  ...         │
  │  │ 32-64K│ 32-64K │  │  │ 32-64K│ 32-64K │  │              │
  │  │ 4-way │ 4-way  │  │  │ 4-way │ 4-way  │  │              │
  │  └───────┴────────┘  │  └───────┴────────┘  │              │
  │  ┌────────────────┐  │  ┌────────────────┐  │              │
  │  │  L2 (unified)  │  │  │  L2 (unified)  │  │              │
  │  │  256KB - 1MB   │  │  │  256KB - 1MB   │  │              │
  │  │  8-way          │  │  │  8-way          │  │              │
  │  └────────┬───────┘  │  └────────┬───────┘  │              │
  │           │          │           │          │              │
  ├───────────┴──────────┴───────────┴──────────┴──────────────┤
  │                    L3 / System Cache                        │
  │                    2MB - 64MB                               │
  │                    16-way, shared by all cores             │
  │                    Inclusive or exclusive (design choice)   │
  └────────────────────────────────────────────────────────────┘

Cache characteristics by core:
┌──────────────┬────────┬────────┬────────┬─────────┬──────────┐
│ Core         │ L1I    │ L1D    │ L2     │ L3      │ Line Size│
├──────────────┼────────┼────────┼────────┼─────────┼──────────┤
│ Cortex-A55   │ 16-64K │ 16-64K │ 64-256K│ Shared  │ 64 bytes │
│ Cortex-A78   │ 64KB   │ 64KB   │ 256-512│ 4-8 MB  │ 64 bytes │
│ Cortex-X2    │ 64KB   │ 64KB   │ 512K-1M│ 4-16 MB │ 64 bytes │
│ Cortex-X4    │ 64KB   │ 64KB   │ 1-2 MB │ 4-16 MB │ 64 bytes │
│ Neoverse N2  │ 64KB   │ 64KB   │ 1 MB   │ 32-64MB │ 64 bytes │
│ Neoverse V2  │ 64KB   │ 64KB   │ 2 MB   │ 32-64MB │ 64 bytes │
└──────────────┴────────┴────────┴────────┴─────────┴──────────┘

Cache line = 64 bytes on ALL modern ARM cores
  This is the unit of transfer between cache levels and memory.
  Loading 1 byte from DRAM → entire 64-byte line fetched into L1.

Latency (cycles):
  L1 hit:    4 cycles (Cortex-A78)
  L2 hit:    ~10-12 cycles
  L3 hit:    ~30-40 cycles
  DRAM miss: ~100-200+ cycles
```

---

## Q2. [L2] Explain cache associativity. What is direct-mapped, set-associative, and fully-associative?

**Answer:**

```
Cache associativity determines WHERE a cache line can be placed
based on its address. It's a fundamental design trade-off.

Direct-Mapped (1-way):
  Each address maps to EXACTLY ONE cache line position
  Index = (Address / LineSize) % NumLines
  
  Address 0x000: → Line 0
  Address 0x040: → Line 1
  Address 0x1000: → Line 0 (CONFLICT!)
  
  Pros: Simple, fast lookup (1 comparison)
  Cons: Conflict misses — two addresses fighting for same line
  Used: Almost never in modern ARM (too many conflicts)

Fully Associative:
  Any address can go in ANY cache line position
  Must compare EVERY tag on lookup → expensive
  
  Pros: No conflict misses (best hit rate)
  Cons: Requires N comparators (one per line) → power-hungry
  Used: µTLB, small buffers (32-48 entries)

N-Way Set Associative (the sweet spot):
  Cache divided into SETS, each SET has N WAYS
  Address maps to one SET, can go in any WAY within that set
  
  Example: 64KB, 4-way, 64-byte lines:
    64KB / 64B = 1024 lines total
    1024 / 4 ways = 256 sets
    
    Address breakdown:
    [63:16] = Tag (compared for hit/miss)
    [15:6]  = Set index (10 bits → 256 sets)
    [5:0]   = Offset within line (6 bits → 64 bytes)
    
    ┌─────────────────────────────────────────────┐
    │ Set  │ Way 0    │ Way 1    │ Way 2 │ Way 3 │
    ├──────┼──────────┼──────────┼───────┼───────┤
    │  0   │ [tag|d]  │ [tag|d]  │ [t|d] │ [t|d] │
    │  1   │ [tag|d]  │ [tag|d]  │ [t|d] │ [t|d] │
    │  ... │  ...     │  ...     │  ...  │  ...  │
    │  255 │ [tag|d]  │ [tag|d]  │ [t|d] │ [t|d] │
    └──────┴──────────┴──────────┴───────┴───────┘
    
    Lookup: compute set index → compare tag in all 4 ways
    → Only 4 comparisons (parallel!) → fast + reasonable hit rate

ARM cache associativity:
  L1 I/D cache: typically 4-way set associative
  L2 cache: 8-way set associative
  L3/System cache: 8-16 way set associative
  
  Higher associativity = fewer conflicts but more power
  L3 uses higher associativity because it's accessed less often
  L1 uses lower (4-way) because it must be accessed EVERY cycle
```

---

## Q3. [L2] What is cache coherency? Why is it needed in multi-core systems?

**Answer:**

```
Cache coherency ensures that ALL cores see a CONSISTENT view of
memory, even though each core has its own private L1/L2 caches.

The problem:
  Core 0: reads X=5 from memory (cached in Core 0's L1)
  Core 1: writes X=10 to memory (cached in Core 1's L1)
  Core 0: reads X again → gets 5 from its L1 (STALE!)
  
  Without coherency: Core 0 never sees Core 1's update!
  → Data corruption, crashes, impossible to share data between cores.

Solution: Hardware Cache Coherency Protocol
  Cores communicate via the interconnect to keep caches synchronized.
  
  When Core 1 writes X:
  1. Core 1 sends "I'm going to write X" to interconnect
  2. Interconnect SNOOPS all other cores: "Do you have X?"
  3. Core 0: "Yes, I have X in my L1"
  4. Core 0: INVALIDATES its copy of X (marks line Invalid)
  5. Core 1: proceeds with write (has exclusive ownership)
  6. Core 0: next read of X → cache miss → gets updated value
  
  This happens in HARDWARE, transparent to software.
  (But software needs barriers to control WHEN it sees updates.)

ARMv8 coherency mechanisms:
  • MESI/MOESI protocol state machines in each cache controller
  • Snoop Control Unit (SCU) in cluster
  • Interconnect: CCI, CCN, CMN, DSU — handles inter-cluster coherency
  • Cache lines have state bits: Modified, Exclusive, Shared, Invalid
  
  Shareability attribute determines which caches participate:
    Inner Shareable: all cores in coherent domain (standard)
    Outer Shareable: all masters including GPU/DMA (if hw-coherent)
    Non-shareable: no coherency tracking (private data only!)

Cost of coherency:
  • Hardware area: 10-20% of interconnect
  • Latency: snoop + response = 10-50 cycles per coherent miss
  • Power: coherency traffic on every shared write
  • Scalability: O(N) snoops for N cores → directory protocols
    (CHI uses directory/snoop filter to avoid broadcast snoops)
```

---

## Q4. [L3] Explain MESI and MOESI protocols in detail. Show state transitions with examples.

**Answer:**

```
MESI = 4 states for each cache line:

  M (Modified):  This cache has ONLY copy, it's DIRTY (changed)
                 Memory is STALE. Must write back before eviction.
                 Can read/write freely.
  
  E (Exclusive): This cache has ONLY copy, it's CLEAN (matches memory)
                 No other cache has it.
                 Can read freely. Write → M (silent, no bus traffic!)
  
  S (Shared):    Multiple caches may have this line (all CLEAN)
                 Can read freely.
                 Write → must invalidate others first → M
  
  I (Invalid):   Line is not in this cache (or has been invalidated)
                 Read → must fetch from memory/other cache

State transitions:

  ┌────────────────────────────────────────────────────────────────┐
  │              Local CPU         │    Snoop (from other core)   │
  ├────────────────────────────────┼──────────────────────────────┤
  │ I → E: Read miss, no sharers  │                              │
  │ I → S: Read miss, others have │                              │
  │ I → M: Write miss (RFO)       │                              │
  │ E → M: Local write (silent!)  │ E → S: Snoop read from other│
  │        No bus transaction!     │ E → I: Snoop write from other│
  │ S → M: Local write + invalidate│ S → I: Snoop write from other│
  │ M → S: Snoop read (write back)│ M → I: Snoop write (flush)  │
  │ M → I: Eviction (write back)  │                              │
  └────────────────────────────────┴──────────────────────────────┘

Example scenario:
  Initial: X not in any cache (all Invalid)
  
  Step 1: Core 0 reads X
    Core 0: I → E  (only reader, exclusive)
    
  Step 2: Core 1 reads X
    Snoop: Core 0 has X in E
    Core 0: E → S  (downgrade to shared)
    Core 1: I → S  (gets shared copy)
    
  Step 3: Core 0 writes X=42
    Core 0: S → M  (needs exclusive → sends invalidate)
    Core 1: S → I  (invalidated by Core 0)
    Core 0 now owns X exclusively, modified
    
  Step 4: Core 1 reads X
    Snoop: Core 0 has X in M
    Core 0: M → S  (write back dirty data, share)
    Core 1: I → S  (gets updated value 42)

MOESI adds Owned state:
  O (Owned): This cache has a DIRTY copy, but others have SHARED
             copies too. This cache is responsible for supplying
             data on snoops (memory is still stale).
             
  Why? In MESI: M→S requires writeback to memory first.
  In MOESI: M→O allows sharing the dirty line WITHOUT writing
  to memory → saves memory bandwidth!
  
  M → O: when another core reads (avoids writeback)
  O → M: when shared copies are invalidated
  O → I: on eviction (MUST write back — it's still dirty!)
  
  ARM uses MOESI-like protocol in practice to save bus bandwidth.
```

---

## Q5. [L2] What is the Snoop Control Unit (SCU)? How does it work within a cluster?

**Answer:**

```
The SCU maintains coherency WITHIN a cluster (between cores that
share an L2/L3 cache).

In DynamIQ / DSU (DynamIQ Shared Unit):
  ┌─────────────────────────────────────────────────────────────┐
  │                     DSU (Cluster)                           │
  │  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐                  │
  │  │Core 0│  │Core 1│  │Core 2│  │Core 3│                  │
  │  │L1I/D │  │L1I/D │  │L1I/D │  │L1I/D │                  │
  │  │L2    │  │L2    │  │L2    │  │L2    │                  │
  │  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘                  │
  │     │         │         │         │                        │
  │  ┌──┴─────────┴─────────┴─────────┴──┐                    │
  │  │     SCU (Snoop Control Unit)       │                    │
  │  │  • Duplicate tag RAM for all L1/L2 │                    │
  │  │  • Snoop filter (knows who has what)│                   │
  │  │  • Direct data transfer between L2s│                    │
  │  └──────────────┬────────────────────┘                    │
  │                 │                                          │
  │  ┌──────────────┴───────────────────┐                     │
  │  │       L3 / System Level Cache    │                     │
  │  │       (Shared across cluster)    │                     │
  │  └─────────────────────────────────┘                     │
  └─────────────────────────────────────────────────────────────┘

SCU operations:
  1. SNOOP FILTERING:
     SCU maintains a TAG DIRECTORY (duplicate tags from L1/L2)
     When Core 0 reads X, SCU checks:
     "Does any other core have X?" → look up tag directory
     If yes: send snoop only to THAT core (targeted snoop)
     If no: no snoop needed → go to L3/memory
     → Avoids broadcast snoops → saves power and bandwidth

  2. DIRECT DATA TRANSFER:
     Core 0 has X in Modified state
     Core 1 wants X
     SCU: forwards data DIRECTLY from Core 0's L2 to Core 1's L2
     → Skips L3 and memory → much faster (~10 cycles vs ~40)

  3. COHERENCY STATE TRACKING:
     SCU tracks MESI/MOESI state for each line in each core
     Manages transitions: exclusive→shared, modified→shared, etc.

Inter-cluster coherency:
  Between clusters: handled by the INTERCONNECT (CCI/CCN/CMN)
  Snoop requests travel over the interconnect
  Much more expensive than intra-cluster (50-100+ cycles)
  
  This is why scheduler affinity matters:
  Threads sharing data should run on SAME cluster (fast snoops)
  rather than cross-cluster (expensive interconnect traffic)
```

---

## Q6. [L1] What is a cache miss? Explain compulsory, capacity, and conflict misses.

**Answer:**

```
Three types of cache misses (the "3 C's"):

1. COMPULSORY (Cold) Miss:
   First-ever access to a cache line — data was never in cache.
   CANNOT be avoided (must load from memory at least once).
   
   Can be REDUCED by:
   • Software prefetch (PRFM) → brings data before first use
   • Hardware prefetcher → detects patterns, pre-loads
   
   Example:
     int arr[1000]; // First access to arr[0] → compulsory miss
     
2. CAPACITY Miss:
   Cache is too small to hold entire working set.
   Line was evicted because cache was full.
   
   Working set = 1 MB, L1 D-cache = 64 KB → capacity misses!
   
   Can be REDUCED by:
   • Larger cache (hardware solution)
   • Loop tiling / blocking (reduce working set per loop)
   • Data structure layout (pack hot fields together)
   
   Example:
     // Matrix multiply — naive:
     for (i=0; i<N; i++)
       for (j=0; j<N; j++)
         for (k=0; k<N; k++)
           C[i][j] += A[i][k] * B[k][j]; // B column → capacity miss
     
     // Tiled (fits in cache):
     for (ii=0; ii<N; ii+=TILE)
       for (jj=0; jj<N; jj+=TILE)
         for (kk=0; kk<N; kk+=TILE)
           // Process TILE×TILE blocks → fit in L1!

3. CONFLICT Miss:
   Two addresses map to the same cache set, evicting each other.
   Even though cache has free space in OTHER sets.
   
   4-way 64KB cache: addresses separated by 16 KB map to same set
   → If 5 addresses hit same set, one is evicted (only 4 ways!)
   
   Can be REDUCED by:
   • Higher associativity (hardware)
   • Change data alignment / padding (software)
   • Avoid power-of-2 strides (common conflict source!)
   
   Example:
     // Array stride = cache size → all access same set:
     int a[4096]; // 4096 * 4 = 16 KB
     for (i = 0; i < 4096; i += 1024) // stride = 4KB = set size
       sum += a[i]; // All map to set 0 → conflict misses!

4th C (often added): COHERENCE Miss
   Miss caused by another core invalidating a shared line
   (not your fault — the other core wrote to shared data)
   
   Can be REDUCED by:
   • Per-core data (avoid sharing)
   • Cache line padding (prevent false sharing)
```

---

## Q7. [L3] What is false sharing? How does it cause performance problems and how do you fix it?

**Answer:**

```
False sharing occurs when TWO cores access DIFFERENT variables
that happen to be in the SAME cache line. Each write invalidates
the ENTIRE line for the other core, even though they're accessing
different data.

Example:
  struct counters {
      uint64_t core0_count;   // offset 0
      uint64_t core1_count;   // offset 8
  } __attribute__((packed));
  
  Both variables fit in ONE 64-byte cache line!
  
  Core 0: core0_count++   → writes to cache line → MODIFIED
  Core 1: core1_count++   → needs cache line → INVALIDATE Core 0!
  Core 0: core0_count++   → needs cache line → INVALIDATE Core 1!
  ... ping-pong forever ...
  
  Performance impact:
  Without false sharing: increment = ~1 cycle (L1 hit)
  With false sharing: increment = ~50-100 cycles (cross-core snoop)
  → 50-100x SLOWER!!!

  ┌────────── Cache Line (64 bytes) ──────────────────────┐
  │ core0_count │ core1_count │     padding/other data     │
  │ (Core 0)    │ (Core 1)    │                            │
  │  ← writes    → writes      │                            │
  │                                                        │
  │ Both in same line → FALSE SHARING!                    │
  └────────────────────────────────────────────────────────┘

Detection:
  perf c2c record ./program    // Cache-to-cache transfer recording
  perf c2c report              // Shows shared cache lines
  
  PMU event: L1D_CACHE_REFILL with high rate on specific addresses
  + Data address sampling (SPE) to identify which addresses

Fix: PAD to separate cache lines

  struct counters {
      uint64_t core0_count;
      uint8_t  _pad0[56];     // Pad to 64 bytes
      uint64_t core1_count;
      uint8_t  _pad1[56];     // Pad to 64 bytes
  };
  
  Or use compiler attributes:
  struct counters {
      alignas(64) uint64_t core0_count;  // Own cache line
      alignas(64) uint64_t core1_count;  // Own cache line
  };
  
  Linux kernel uses ____cacheline_aligned_in_smp:
  struct per_cpu_data {
      unsigned long counter;
  } ____cacheline_aligned_in_smp;

Trade-off: wastes memory (56 bytes of padding per variable)
  but NECESSARY for high-performance multi-threaded code.
```

---

## Q8. [L2] Explain cache maintenance operations in ARMv8. What are DC and IC instructions?

**Answer:**

```
ARMv8 provides Data Cache (DC) and Instruction Cache (IC)
maintenance instructions for explicit cache management.

Data Cache (DC) operations:
┌─────────────────────────────────────────────────────────────────┐
│ Instruction       │ Operation                                   │
├───────────────────┼─────────────────────────────────────────────┤
│ DC CIVAC, Xn      │ Clean + Invalidate by VA to PoC           │
│                   │ Write dirty data to memory, then invalidate│
│                   │ Used: before DMA read from this address    │
├───────────────────┼─────────────────────────────────────────────┤
│ DC CVAC, Xn       │ Clean by VA to PoC                        │
│                   │ Write dirty data to memory (keep in cache) │
│                   │ Used: ensure DMA sees latest data          │
├───────────────────┼─────────────────────────────────────────────┤
│ DC CVAU, Xn       │ Clean by VA to PoU                        │
│                   │ Clean to Point of Unification              │
│                   │ Used: self-modifying code (I-cache sync)   │
├───────────────────┼─────────────────────────────────────────────┤
│ DC IVAC, Xn       │ Invalidate by VA to PoC                   │
│                   │ Discard cache line (warning: loses dirty!) │
│                   │ Used: after DMA write (CPU cache is stale) │
│                   │ EL1+ only (dangerous at EL0!)              │
├───────────────────┼─────────────────────────────────────────────┤
│ DC ZVA, Xn        │ Zero by VA                                 │
│                   │ Zero entire cache line (allocate + zero)   │
│                   │ Used: fast memset/bzero (avoids read-for-  │
│                   │ ownership, just writes zeros)              │
├───────────────────┼─────────────────────────────────────────────┤
│ DC CSW/CISW       │ Clean / Clean+Invalidate by Set/Way       │
│                   │ Used during power-down (flush all caches)  │
│                   │ Not available at EL0                       │
└───────────────────┴─────────────────────────────────────────────┘

Instruction Cache (IC) operations:
┌─────────────────────────────────────────────────────────────────┐
│ IC IVAU, Xn       │ Invalidate by VA to PoU                    │
│                   │ Used after modifying code in memory        │
│ IC IALLU          │ Invalidate ALL (entire I-cache)            │
│ IC IALLUIS        │ Invalidate ALL, Inner Shareable            │
└─────────────────────────────────────────────────────────────────┘

PoC and PoU explained:
  PoC (Point of Coherency):
    The point where ALL observers (all cores, DMA, GPU) see the
    same copy of data. Usually = last-level cache or memory.
    
  PoU (Point of Unification):
    The point where instruction and data caches are unified.
    Usually = L2 (unified) cache.
    
  Example: self-modifying code / JIT:
    1. Write new instructions to memory (D-cache path)
    2. DC CVAU, addr    // Clean D-cache to PoU (L2)
    3. DSB ISH          // Ensure clean completes
    4. IC IVAU, addr    // Invalidate I-cache for that address
    5. DSB ISH          // Ensure invalidation completes
    6. ISB              // Flush pipeline → fetch new instructions
```

---

## Q9. [L1] What is write-back vs. write-through cache? Which does ARM use and why?

**Answer:**

```
Write-Back (WB):
  On store: write ONLY to cache, mark line "dirty"
  Write to memory LATER (when evicted or explicitly cleaned)
  
  Pros:
  • Fast writes (L1 latency, ~1-4 cycles)
  • Reduces memory bus traffic (many writes absorbed by cache)
  • Multiple writes to same line → only one memory write
  
  Cons:
  • Complexity: must track dirty lines
  • Eviction: dirty line must be written back (extra latency)
  • Coherency: other agents see stale memory until writeback

Write-Through (WT):
  On store: write to BOTH cache AND memory simultaneously
  Cache always clean (never dirty)
  
  Pros:
  • Simpler: no dirty tracking needed
  • Memory always up-to-date (good for coherency with non-coherent agents)
  
  Cons:
  • SLOW writes (must wait for memory, or at least write buffer)
  • High memory bus traffic (every write goes to memory)
  • Performance much worse for write-heavy workloads

ARM uses Write-Back for almost everything:
  MAIR attribute 0xFF = Write-Back, Read-Allocate, Write-Allocate
  This is the DEFAULT for all normal memory in Linux.
  
  Write-Through used only for specific scenarios:
  • Some shared memory regions with non-coherent agents
  • Debug scenarios
  
  Write-Allocate vs No-Write-Allocate:
    Write-Allocate: on write miss, bring line into cache, then write
    No-Write-Allocate: on write miss, write directly to memory
    
    ARM default: both Read-Allocate and Write-Allocate (0xFF)
    → Best for most workloads

  ┌──────────────────────────────────────────────────────────┐
  │ Write-Back with Write-Allocate (ARM default):           │
  │                                                          │
  │ Write hit:  → write to cache, mark dirty               │
  │ Write miss: → allocate line, write to cache, mark dirty │
  │ Eviction:   → dirty? write back to memory              │
  │              → clean? just discard                      │
  │                                                          │
  │ Read hit:   → return from cache                        │
  │ Read miss:  → allocate line, fill from memory          │
  └──────────────────────────────────────────────────────────┘
```

---

## Q10. [L2] What is a cache eviction policy? Explain LRU and pseudo-LRU as used in ARM caches.

**Answer:**

```
When a cache set is full and a new line must be loaded, the eviction
policy decides WHICH existing line to replace.

LRU (Least Recently Used):
  Evict the line that hasn't been accessed for the longest time.
  Optimal in theory (uses recency as predictor of future use).
  
  For 4-way cache: need to track access order of 4 lines
  True LRU needs: log2(4!) = ~4.58 → 5 bits per set
  For 8-way: log2(8!) = ~15.3 → 16 bits per set
  → Gets expensive for high associativity

Problem with true LRU:
  4-way set, access pattern: A B C D E A B C D E ...
  (5 unique lines cycling through 4-way cache)
  LRU: ALWAYS evicts the line you need next → 0% hit rate!
  ("LRU thrashing" — working set = cache size + 1)

Pseudo-LRU (PLRU):
  ARM caches use variants of Pseudo-LRU.
  Much cheaper to implement (~N-1 bits for N-way).
  
  Tree-based PLRU (common in ARM):
  ┌──────────────────────────────────────────────────┐
  │ 4-way set with 3 tree bits:                     │
  │                                                  │
  │         [b0]                                     │
  │        /    \                                    │
  │     [b1]    [b2]                                │
  │     /  \    /  \                                 │
  │   W0   W1  W2   W3                             │
  │                                                  │
  │ On access to Way 2:                             │
  │   b0 = left (not right)                         │
  │   b2 = left (not right side of b2)              │
  │                                                  │
  │ On eviction: follow bits → find victim          │
  │   b0=left → go right → b2                      │
  │   b2=left → go right → Way 3 (victim!)         │
  └──────────────────────────────────────────────────┘
  
  Not perfectly LRU but close enough (~97% accuracy).
  Cost: 3 bits per set (vs 5 for true LRU in 4-way).

Other policies ARM cores may use:
  • Random replacement: simple, avoids LRU pathologies
    (Cortex-A55 can be configured for random or PLRU)
  • RRIP (Re-Reference Interval Prediction): 
    tracks "reuse distance" — better for scan-resistant behavior
    (used in some L3 caches)
```

---

## Q11. [L3] Explain the DynamIQ Shared Unit (DSU) and how it manages the shared L3 cache.

**Answer:**

```
DSU (DynamIQ Shared Unit) is ARM's cluster-level cache management
block used in Cortex-A7x, A5xx, X-series cores.

DSU Architecture:
  ┌─────────────────────────────────────────────────────────────┐
  │                      DSU-110 / DSU-120                      │
  │                                                             │
  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  │
  │  │Core 0│ │Core 1│ │Core 2│ │Core 3│ │Core 4│ │Core 5│  │
  │  │A55   │ │A55   │ │A55   │ │A55   │ │A78   │ │X2    │  │
  │  │L2    │ │L2    │ │L2    │ │L2    │ │L2    │ │L2    │  │
  │  └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘  │
  │     └────────┴────────┴────────┴────────┴────────┘        │
  │                        │                                    │
  │  ┌────────────────────┴────────────────────────────┐      │
  │  │           Snoop Filter / Tag Directory          │      │
  │  │  Tracks which core caches which line            │      │
  │  │  Targeted snoops (not broadcast)                │      │
  │  └────────────────────┬────────────────────────────┘      │
  │                       │                                    │
  │  ┌────────────────────┴────────────────────────────┐      │
  │  │             Shared L3 Cache (2-16 MB)            │      │
  │  │  • Inclusive or exclusive (configurable)        │      │
  │  │  • 16-way set associative                       │      │
  │  │  • Can be partitioned (MPAM)                    │      │
  │  │  • Power gating per slice                       │      │
  │  └────────────────────┬────────────────────────────┘      │
  │                       │                                    │
  │                   ACE / CHI port                           │
  │               (to system interconnect)                    │
  └─────────────────────────────────────────────────────────────┘

Key DSU features:
  1. HETEROGENEOUS cores: mix big/LITTLE/X in ONE cluster
     (DynamIQ replaced separate big/LITTLE clusters)
  
  2. Snoop filter: directory-based coherency within cluster
     Track 32K-64K tags → targeted snoops, not broadcast
     
  3. L3 cache configuration:
     • Size: 2-16 MB (implementation choice)
     • Inclusive: L3 contains copy of all L1/L2 lines
       → Snoop filter uses L3 tags (less area for separate filter)
       → But wasted capacity (L3 duplicates L1/L2 data)
     • Exclusive: L3 does NOT contain L1/L2 lines
       → More effective capacity
       → Needs separate snoop filter (more area)
  
  4. Power management:
     • Per-core power domains (turn off individual cores)
     • L3 cache slicing with power gating
     • Retention state (keep L3 data, clock-gate logic)
     
  5. L3 partitioning (MPAM/QoS):
     Assign L3 cache ways to specific cores or tasks
     Prevent noisy neighbor from evicting your data
     e.g., Core 5 (big) gets 12 ways, Cores 0-3 (LITTLE) get 4 ways
```

---

## Q12. [L2] What is the difference between inclusive and exclusive cache hierarchies?

**Answer:**

```
Inclusive Cache:
  L3 contains a SUPERSET of all L1+L2 contents.
  Every line in L1/L2 is also in L3.
  
  ┌────────────────────────────────────────────────────────┐
  │ Properties:                                            │
  │  • Snoop filter built into L3 tags (saves area)       │
  │  • Eviction from L3 → MUST also evict from L1/L2     │
  │    (back-invalidation)                                │
  │  • Effective capacity = L3 size (L1/L2 is "included") │
  │  • Wasted: L3 stores copies of data in L1/L2          │
  │                                                        │
  │ Example: 64KB L1 + 512KB L2 + 4MB L3 (inclusive)      │
  │ Effective capacity = 4MB (not 4MB + 512KB + 64KB)     │
  │                                                        │
  │ Advantage: snoop management is simple                 │
  │   "Is line X in any core?" → check L3 tag             │
  │   If not in L3 → guaranteed not in any L1/L2         │
  │                                                        │
  │ Used by: some ARM implementations, Intel (historically)│
  └────────────────────────────────────────────────────────┘

Exclusive Cache:
  L3 does NOT contain copies of L1/L2 data.
  Evicted from L2 → goes to L3. Evicted from L3 → goes to memory.
  Data exists in exactly one level at a time.
  
  ┌────────────────────────────────────────────────────────┐
  │ Properties:                                            │
  │  • Effective capacity = L1 + L2 + L3 (maximum use!)  │
  │  • No back-invalidation needed                        │
  │  • BUT: needs separate snoop filter / directory       │
  │    (can't use L3 tags for snooping)                  │
  │  • Eviction path: L1→L2→L3→memory (line migration)  │
  │                                                        │
  │ Example: 64KB L1 + 512KB L2 + 4MB L3 (exclusive)     │
  │ Effective capacity = ~4.5MB                            │
  │                                                        │
  │ Advantage: better effective capacity                  │
  │ Disadvantage: more complex, need separate snoop filter │
  │                                                        │
  │ Used by: AMD (historically), some ARM SoCs            │
  └────────────────────────────────────────────────────────┘

Non-inclusive, Non-exclusive (NINE):
  L3 may or may not contain L1/L2 data.
  Most flexible — L3 keeps "useful" data, doesn't force inclusion.
  
  ARM DSU typically uses non-inclusive policy:
  • Allocate into L3 on L2 eviction (victim cache behavior)
  • Don't back-invalidate (no forced L1/L2 eviction)
  • Use separate snoop filter for coherency
  Best of both worlds but most complex to implement.
```

---

## Q13. [L2] How does cache coloring / page coloring work? When is it used?

**Answer:**

```
Cache coloring is a SOFTWARE technique to control which cache sets
physical pages map to, reducing conflict misses.

The problem:
  In a set-associative cache, physical address bits determine the
  SET index. Two pages whose PA set-index bits are the same will
  compete for the same cache sets → conflict misses.
  
  64KB L1 D-cache, 4-way, 64B line:
    256 sets, set index = PA[15:6]
    Page size = 4KB → PA[11:0] = page offset
    Set index bits: PA[15:12] come from the PAGE FRAME NUMBER
    → Different page frames can map to same sets!

Cache coloring:
  "Color" = the set index bits from the page frame number
  
  With 4KB pages and 64KB 4-way L1:
    Colors = 64KB / (4 × 4KB) = 4 colors
    Color = PA[13:12] (2 bits → 4 colors)
  
  ┌─────────────────────────────────────────────────────┐
  │ Physical Address:                                   │
  │ [...] [Page Frame Number] [Page Offset]            │
  │       [color bits][...]   [11:0]                   │
  │       PA[13:12]                                     │
  │                                                     │
  │ Color 0: PA[13:12] = 00 → sets 0-63               │
  │ Color 1: PA[13:12] = 01 → sets 64-127             │
  │ Color 2: PA[13:12] = 10 → sets 128-191            │
  │ Color 3: PA[13:12] = 11 → sets 192-255            │
  └─────────────────────────────────────────────────────┘

  Technique: when allocating pages, choose pages with DIFFERENT
  colors for data that will be accessed together.
  
  Process A's code: color 0 + color 1
  Process A's data: color 2 + color 3
  → No conflict misses between code and data!

Use cases:
  • Real-time systems: guarantee cache partition between tasks
  • Database: ensure hot data doesn't collide in cache
  • Virtualization: prevent VM cache interference
  • MPAM (Memory Partitioning and Monitoring) in ARMv8.4
    provides HARDWARE support for this (assigns cache portions)

Linux: does NOT do page coloring by default
  (relies on cache being large enough + associativity)
  Some RTOS kernels and hypervisors implement it.
```

---

## Q14. [L1] What is the cache line size and why is 64 bytes used universally in ARM?

**Answer:**

```
Cache line (also called cache block) is the fundamental unit of
data transfer between cache levels and memory.

ARM uses 64 bytes universally (CTR_EL0.DminLine / IminLine).

Why 64 bytes?
  ┌──────────────────────────────────────────────────────────┐
  │ Trade-off: line size vs. performance                     │
  │                                                          │
  │ Too small (e.g., 16 bytes):                             │
  │  • More tag overhead (~38% of cache = tags)             │
  │  • Can't exploit spatial locality effectively           │
  │  • Memory bus underutilized (DDR burst = 64 bytes!)    │
  │  • More cache misses for sequential access              │
  │                                                          │
  │ Too large (e.g., 256 bytes):                            │
  │  • Wasted bandwidth (load 256B but use 8B → 97% waste) │
  │  • Fewer total lines in cache → more capacity misses   │
  │  • Higher false sharing probability                    │
  │  • Longer cache fill time                              │
  │                                                          │
  │ 64 bytes = sweet spot:                                  │
  │  • Matches DDR4/5 burst length (8 × 8B = 64B)         │
  │  • Reasonable tag overhead (~6% of cache)              │
  │  • Good spatial locality exploitation                  │
  │  • Manageable false sharing risk                       │
  │  • 16 × 4-byte words per line (common data sizes)     │
  └──────────────────────────────────────────────────────────┘

Reading CTR_EL0 to determine line size:
  MRS X0, CTR_EL0
  
  DminLine [19:16]: log2(minimum D-cache line size in words)
    Value 4 → 2^4 = 16 words × 4 bytes = 64 bytes
  IminLine [3:0]: log2(minimum I-cache line size in words)
    Value 4 → 64 bytes
  
  Software MUST read CTR_EL0 for portable cache maintenance
  (never hardcode 64 — future cores could change it)
```

---

## Q15. [L3] What is cache partitioning (MPAM)? How does ARMv8.4 enable QoS for caches?

**Answer:**

```
MPAM = Memory System Resource Partitioning and Monitoring (ARMv8.4)
Provides hardware support for partitioning caches, memory bandwidth,
and other resources among tasks, VMs, or processes.

Problem without MPAM:
  VM A (database): needs large L3 cache for hot data
  VM B (batch job): scans huge array, pollutes L3 cache
  → VM A's hot data evicted by VM B's streaming data
  → VM A performance drops 3x → "noisy neighbor" problem

MPAM solution:
  Assign each VM/task a PARTITION ID (PARTID)
  Configure how much cache each PARTID can use.
  
  ┌───────────────────────────────────────────────────────────┐
  │ MPAM registers:                                          │
  │                                                          │
  │ MPAMIDR_EL1: reports MPAM capabilities                  │
  │ MPAM0_EL1: PARTID + PMG for EL0                        │
  │ MPAM1_EL1: PARTID + PMG for EL1                        │
  │ MPAM2_EL2: PARTID + PMG for EL2                        │
  │                                                          │
  │ Cache-level MPAM controls (memory-mapped):              │
  │ CPOR (Cache Portion): which cache ways each PARTID gets │
  │ CCAP_CTL: capacity control (max cache proportion)       │
  │ CMCR: cache monitoring counter/register                 │
  └───────────────────────────────────────────────────────────┘

Cache way partitioning example:
  L3 cache: 16 ways
  
  ┌────────────────────────────────────────────────────────┐
  │ Way: 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 │
  │      ─────────────  ──────────── ─────────────────── │
  │      PARTID 0       PARTID 1     PARTID 2           │
  │      (VM A: DB)     (VM B: batch)(OS/hypervisor)    │
  │      4 ways (25%)   4 ways (25%) 8 ways (50%)       │
  └────────────────────────────────────────────────────────┘
  
  VM B's batch job can ONLY fill ways 4-7
  → VM A's data in ways 0-3 is PROTECTED from eviction
  → Predictable performance!

MPAM monitoring:
  Track per-PARTID:
  • Cache occupancy (how many lines does this PARTID use?)
  • Memory bandwidth consumption
  • L3 miss rate per partition
  
  Hypervisor/OS can monitor and adjust partitions dynamically.

Linux support:
  resctrl filesystem (similar to Intel RDT):
  /sys/fs/resctrl/
    schemata          # Define partition allocations
    tasks             # Assign tasks to groups
    mon_data/         # Monitoring data

Alternative to MPAM (pre-ARMv8.4):
  Cache coloring (software-only, coarser control)
  Application-level: madvise, cgroups memory limits
```

---

Back to [Question & Answers Index](./README.md)
