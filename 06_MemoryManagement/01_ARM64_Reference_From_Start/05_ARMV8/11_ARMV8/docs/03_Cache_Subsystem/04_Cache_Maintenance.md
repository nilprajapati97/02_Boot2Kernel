# Cache Maintenance Operations

## 1. Why Cache Maintenance?

While hardware handles coherency between CPU caches automatically, software
must manage cache coherency in certain situations:

```
When cache maintenance is needed:
  1. DMA operations (non-coherent devices read/write memory)
  2. Self-modifying code / JIT compilation
  3. Loading new code (I-cache doesn't snoop D-cache)
  4. Changing memory attributes (cached ↔ uncached)
  5. Power management (cache may lose contents)
  6. Security (clearing sensitive data from cache)
```

---

## 2. Cache Operation Types

```
┌──────────────────────────────────────────────────────────────────┐
│  Operation      │ Description                                    │
├──────────────────────────────────────────────────────────────────┤
│  Invalidate     │ Mark cache line as Invalid (discard)           │
│  (INV)          │ → Data in cache is LOST                       │
│                 │ → Next access goes to memory                   │
│                 │ → Use: before DMA read (device → memory → CPU)│
│                                                                   │
│  Clean          │ Write dirty data to next level/memory          │
│  (CLEAN)        │ → Line stays in cache (now clean)              │
│                 │ → Use: before DMA write (CPU → memory → device)│
│                                                                   │
│  Clean &        │ Clean first, then invalidate                   │
│  Invalidate     │ → Dirty data written to memory, then discarded│
│  (CLEAN+INV)    │ → Use: before changing memory mapping         │
│                 │ → Most conservative option                     │
│                                                                   │
│  Zero           │ Allocate cache line filled with zeros          │
│  (DC ZVA)       │ → Efficient memory zeroing (no read needed)    │
│                 │ → Use: clearing pages, calloc                   │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. Data Cache Operations

```
Syntax: DC <op>, Xt    (Xt holds virtual address)

┌──────────────────────────────────────────────────────────────────┐
│  Instruction      │ Operation        │ Point      │ Use case     │
├──────────────────────────────────────────────────────────────────┤
│  DC IVAC, X0      │ Invalidate       │ PoC        │ DMA inbound  │
│  DC ISW, X0       │ Invalidate       │ Set/Way    │ Boot/powrmgt │
│  DC CIVAC, X0     │ Clean+Invalidate │ PoC        │ DMA+remap    │
│  DC CISW, X0      │ Clean+Invalidate │ Set/Way    │ Boot/powrmgt │
│  DC CVAC, X0      │ Clean            │ PoC        │ DMA outbound │
│  DC CVAU, X0      │ Clean            │ PoU        │ Code loading │
│  DC CSW, X0       │ Clean            │ Set/Way    │ Power save   │
│  DC ZVA, X0       │ Zero             │ —          │ memset(0)    │
│  DC CVAP, X0      │ Clean            │ Persistence│ NVM (v8.2)   │
│  DC CVADP, X0     │ Clean            │ Deep Pers. │ NVM (v8.5)   │
└──────────────────────────────────────────────────────────────────┘

PoC = Point of Coherency (all observers see the data — usually main memory)
PoU = Point of Unification (I-cache and D-cache see same data)
Set/Way = Address by cache set+way number (not virtual address)
```

---

## 4. Instruction Cache Operations

```
Syntax: IC <op>{, Xt}

┌──────────────────────────────────────────────────────────────────┐
│  Instruction      │ Operation        │ Scope                    │
├──────────────────────────────────────────────────────────────────┤
│  IC IALLU         │ Invalidate ALL   │ Local core, all levels   │
│  IC IALLUIS       │ Invalidate ALL   │ Inner Shareable (all     │
│                   │                   │ cores in IS domain)      │
│  IC IVAU, X0      │ Invalidate by VA │ PoU                      │
└──────────────────────────────────────────────────────────────────┘

I-cache is read-only → no clean needed, only invalidate
```

---

## 5. Common Cache Maintenance Sequences

### Self-Modifying Code / JIT Compilation

```
When you write code into memory (JIT compiler, module loader):

  1. Write the new code to memory (via D-cache):
     STR X0, [code_addr]         // Store instruction bytes
     STR X1, [code_addr + 4]     // Store more instructions

  2. Clean D-cache to PoU (so I-cache can see it):
     DC CVAU, X2                 // Clean data cache to PoU
     DSB ISH                     // Wait for clean to complete

  3. Invalidate I-cache:
     IC IVAU, X2                 // Invalidate I-cache at this VA
     DSB ISH                     // Wait for invalidation
     ISB                         // Flush pipeline

  If CTR_EL0.DIC = 1: Step 3 not needed (hardware I-cache coherent)
  If CTR_EL0.IDC = 1: Step 2 not needed (D-cache clean not needed)
```

### DMA: Device Reading from Memory (Outbound DMA)

```
CPU writes data → DMA device reads it from memory:

  Problem: CPU writes may be in D-cache, not yet in memory
  Solution: Clean D-cache before starting DMA

  // CPU writes data to buffer
  STR X0, [dma_buf]
  STR X1, [dma_buf + 8]
  ...
  
  // Clean the buffer from D-cache to PoC (memory)
  loop:
    DC CVAC, X2              // Clean one cache line
    ADD X2, X2, #64          // Next cache line (64 bytes)
    CMP X2, X3               // Until end of buffer
    B.LT loop
  
  DSB SY                     // Wait for all cleans to complete
  
  // Now start DMA — device will read correct data from memory
```

### DMA: Device Writing to Memory (Inbound DMA)

```
DMA device writes data → CPU reads it:

  Problem: D-cache may have stale data at those addresses
  Solution: Invalidate D-cache after DMA completes

  // Before DMA: Invalidate (ensure no stale cache lines will be read)
  loop:
    DC IVAC, X2              // Invalidate one cache line
    ADD X2, X2, #64
    CMP X2, X3
    B.LT loop
  DSB SY
  
  // Start DMA — device writes to memory
  // Wait for DMA completion...
  
  // After DMA: Invalidate again (ensure CPU reads from memory)
  loop2:
    DC IVAC, X2
    ADD X2, X2, #64
    CMP X2, X3
    B.LT loop2
  DSB SY
  
  // Now CPU reads correct freshly-DMA'd data
  LDR X0, [dma_buf]

  NOTE: If DMA buffer was clean+invalidated before DMA, only one
  invalidation (before) is needed. The double-invalidation pattern
  handles the case where cache might speculatively fetch during DMA.
```

### Set/Way Operations (Boot & Power Management)

```
Cleaning ALL caches by Set/Way (used during power-down):

  // Read cache geometry
  MRS X0, CLIDR_EL1          // Cache Level ID Register
  MRS X1, CCSIDR_EL1         // Cache Size ID (after selecting level)

  // Iterate over all sets and ways
  for each cache_level:
    MSR CSSELR_EL1, level    // Select cache level
    ISB
    MRS X0, CCSIDR_EL1       // Get geometry (sets, ways, line size)
    
    for each way:
      for each set:
        // Encode set/way in X1
        DC CSW, X1            // Clean by Set/Way
        // or DC CISW, X1     // Clean + Invalidate by Set/Way

  DSB SY                     // Wait for all operations

  WARNING: Set/Way operations are DANGEROUS in concurrent systems!
  They don't interact with coherency → only use at boot or power-down
  when other cores are not running.
```

---

## 6. Cache Maintenance and Barriers

```
Rules:
  1. Cache maintenance operations are ordered like stores
  2. DSB after cache ops ensures completion
  3. ISB after I-cache ops ensures pipeline sees new instructions

  DC CVAC, X0     // Clean D-cache
  DSB ISH          // Wait for clean to reach PoC across inner shareable
  
  IC IVAU, X0     // Invalidate I-cache
  DSB ISH          // Wait for invalidation to propagate
  ISB              // Flush pipeline — fetch new instructions
  
  TLBI VAE1IS, X0 // Invalidate TLB
  DSB ISH          // Wait for TLB invalidation
  ISB              // Ensure new translations used
```

---

## 7. DC ZVA — Data Cache Zero by Virtual Address

A unique ARM optimization for zeroing memory:

```
DC ZVA, X0   // Zero a block of memory (cache-line sized)

  • Allocates a cache line and fills it with zeros
  • Does NOT read from memory first (saves bandwidth!)
  • Block size: read DCZID_EL0 register for size (typically 64 bytes)
  
  Example: Fast memset(buf, 0, 4096):
    MOV X0, buf_addr
    ADD X1, X0, #4096
  loop:
    DC ZVA, X0
    ADD X0, X0, #64          // Advance by cache line size
    CMP X0, X1
    B.LT loop

  Performance: ~4x faster than regular STR-based zeroing
  (avoids read-for-ownership on write miss)
```

---

Next: Back to [Cache Subsystem Overview](./README.md) | Continue to [Interrupt Subsystem →](../04_Interrupt_Subsystem/)
