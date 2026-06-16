# MESI / MOESI Coherency Protocols

## 1. MESI Protocol

MESI is the foundational cache coherency protocol. Each cache line is in one
of four states:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        MESI States                                   │
│                                                                      │
│  State       │ Description                    │ Properties            │
│ ─────────────┼────────────────────────────────┼──────────────────── │
│  M (Modified)│ Line modified by this core     │ Dirty (≠ memory)    │
│              │ Only copy in the system        │ Exclusive owner     │
│              │ MUST write back on eviction    │ No other cache has it│
│                                                                      │
│  E (Exclusive)│ Line matches memory           │ Clean (= memory)    │
│              │ Only this core has it          │ Can write without   │
│              │ No other cache has a copy      │ bus transaction     │
│                                                                      │
│  S (Shared)  │ Line matches memory            │ Clean (= memory)    │
│              │ Multiple caches may have it    │ Must invalidate     │
│              │ Read-only in this state        │ others before write │
│                                                                      │
│  I (Invalid) │ Line is not in this cache      │ Not present         │
│              │ (logically absent)             │ Must fetch on access│
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### MESI State Transition Diagram

```
                    ┌──────────────────────────────────────┐
                    │           MESI State Machine          │
                    │         (for one cache line)          │
                    └──────────────────────────────────────┘

                              BusRdX (snoop)
                    ┌────────────────────────────┐
                    │                             │
                    ▼                             │
              ┌───────────┐                 ┌────┴──────┐
     ┌───────▶│ Invalid   │                │ Modified  │
     │        │   (I)     │◀──Eviction─────│   (M)     │
     │        └─────┬─────┘                └────┬──────┘
     │              │                            │ ▲
     │              │ Read                       │ │
     │              │ Miss                       │ │ Local Write
     │              │                            │ │ (silent M→M)
     │              ▼                            │ │
     │ Snoop  ┌───────────┐    Local Write  ┌───┘ │
     │ Inv.   │ Exclusive │───────────────▶ │     │
     │        │   (E)     │                       │
     │        └─────┬─────┘                       │
     │              │                              │
     │              │ Snoop Read                   │
     │              │ (another core reads)         │
     │              ▼                              │
     │        ┌───────────┐    Local Write         │
     └────────│  Shared   │───(BusRdX/Inv)────────┘
              │   (S)     │   Invalidate others,
              └───────────┘   go to M

Legend:
  BusRd   = Another core reads this address
  BusRdX  = Another core wants to write (read-exclusive)
  Snoop   = Interconnect queries this cache
```

### MESI Transitions Table

```
┌──────────────┬───────────────┬──────────────┬────────────────────────┐
│ Current State│ Event          │ Next State   │ Action                 │
├──────────────┼───────────────┼──────────────┼────────────────────────┤
│ I            │ Local Read     │ E (if sole)  │ Fetch from memory      │
│              │                │ S (if shared)│ or from other cache    │
│ I            │ Local Write    │ M             │ Fetch + invalidate     │
│              │                │               │ others (BusRdX)        │
├──────────────┼───────────────┼──────────────┼────────────────────────┤
│ E            │ Local Read     │ E             │ Cache hit (no bus)     │
│ E            │ Local Write    │ M             │ Silent upgrade (no bus)│
│ E            │ Snoop Read     │ S             │ Supply data, share     │
│ E            │ Snoop Write    │ I             │ Invalidate self        │
├──────────────┼───────────────┼──────────────┼────────────────────────┤
│ S            │ Local Read     │ S             │ Cache hit (no bus)     │
│ S            │ Local Write    │ M             │ Invalidate others (Inv)│
│ S            │ Snoop Read     │ S             │ No action (still shared)│
│ S            │ Snoop Write    │ I             │ Invalidate self        │
├──────────────┼───────────────┼──────────────┼────────────────────────┤
│ M            │ Local Read     │ M             │ Cache hit (no bus)     │
│ M            │ Local Write    │ M             │ Cache hit (no bus)     │
│ M            │ Snoop Read     │ S             │ Write-back, supply data│
│ M            │ Snoop Write    │ I             │ Write-back, invalidate │
│ M            │ Eviction       │ I             │ Write-back dirty data  │
└──────────────┴───────────────┴──────────────┴────────────────────────┘
```

---

## 2. MOESI Protocol (Used in ARM)

ARM adds a fifth state: **Owned (O)**. MOESI avoids unnecessary write-backs.

```
┌─────────────────────────────────────────────────────────────────────┐
│                       MOESI States                                    │
│                                                                      │
│  M (Modified)  │ Only copy, dirty, exclusive write access            │
│  O (Owned)     │ Dirty, BUT shared — other caches have S copies      │
│                │ Owner is responsible for supplying data on snoop    │
│                │ Owner must write-back on eviction                   │
│  E (Exclusive) │ Only copy, clean (matches memory)                   │
│  S (Shared)    │ Clean copy, may be in multiple caches               │
│  I (Invalid)   │ Not present                                          │
│                                                                      │
│  Key difference from MESI:                                           │
│  In MESI: When M transitions to S (snoop read), it MUST write back  │
│  In MOESI: M transitions to O (no write-back!), supplies to snooper │
│  → Saves memory bandwidth!                                           │
└─────────────────────────────────────────────────────────────────────┘
```

### MOESI State Diagram

```
                        ┌─────────────┐
                        │  Modified   │◀── Local Write (from any state)
                        │    (M)      │
                        └──────┬──────┘
                               │ Snoop Read
                               ▼
                        ┌─────────────┐
               ┌───────▶│   Owned     │
               │        │    (O)      │  Dirty, but shared
               │        └──────┬──────┘
               │               │ Snoop Write (BusRdX)
               │               ▼
               │        ┌─────────────┐
               │        │  Invalid    │◀── Snoop Write / Eviction
               │        │    (I)      │
               │        └──────┬──────┘
               │               │ Read Miss (no other cache)
               │               ▼
               │        ┌─────────────┐
               │        │ Exclusive   │
               │        │    (E)      │  Clean, sole copy
               │        └──────┬──────┘
               │               │ Snoop Read
               │               ▼
               │        ┌─────────────┐
               └────────│  Shared     │  Clean, multiple copies
                        │    (S)      │
                        └─────────────┘

MOESI advantage example:
  
  MESI:
    Core 0 has M (dirty data = 99)
    Core 1 reads same address
    Core 0: M → S, MUST write-back 99 to memory (slow!)
    Core 1: Gets data from memory

  MOESI:
    Core 0 has M (dirty data = 99)
    Core 1 reads same address
    Core 0: M → O, supplies 99 directly to Core 1 (cache-to-cache!)
    Core 1: Gets S copy
    Memory: Still has OLD value (write-back deferred)
    → Saved one memory write!
```

---

## 3. Coherency Transactions

```
┌──────────────────────────────────────────────────────────────────┐
│  Transaction    │ Description                │ Initiated by      │
├──────────────────────────────────────────────────────────────────┤
│  ReadShared     │ Want to read, share OK     │ Cache miss (read) │
│  ReadUnique     │ Want exclusive (for write) │ Cache miss (write)│
│  CleanUnique    │ Upgrade S → M (no data)    │ Write hit on S    │
│  MakeUnique     │ Get ownership (invalidate) │ Write, don't need │
│                 │                             │ data (full overwrite)│
│  WriteBack      │ Evict dirty line to memory │ Cache eviction    │
│  WriteClean     │ Write clean data back      │ Cache clean op    │
│  Evict          │ Remove clean line (notify) │ Cache eviction    │
│  SnpShared      │ "Give me a shared copy"    │ Interconnect snoop│
│  SnpUnique      │ "Invalidate your copy"     │ Interconnect snoop│
│  SnpClean       │ "Clean your copy"          │ Interconnect snoop│
└──────────────────────────────────────────────────────────────────┘
```

---

## 4. Walk-Through Examples

### Example 1: Two Cores Reading Same Data

```
Initial state: addr 0x1000 = 42 in memory, not in any cache

Step 1: Core 0 reads 0x1000
  Core 0: I → E (exclusive, clean — only copy)
  Bus: ReadShared → memory supplies data (42)

Step 2: Core 1 reads 0x1000
  Interconnect snoops Core 0: "Do you have 0x1000?"
  Core 0: E → S (downgrade to shared)
  Core 1: I → S (gets shared copy)
  Both caches now have S(42)

Result:
  Core 0: S(42)    Core 1: S(42)    Memory: 42
```

### Example 2: Write After Read (Write Miss)

```
Start: Core 0 has S(42), Core 1 has S(42)

Step 1: Core 0 writes 99 to 0x1000
  Core 0 sends CleanUnique (or ReadUnique)
  Interconnect sends SnpUnique to Core 1
  Core 1: S → I (invalidated)
  Core 0: S → M(99) (now exclusive, modified)

Result:
  Core 0: M(99)    Core 1: I    Memory: 42 (stale, ok — Core 0 owns it)
```

### Example 3: Snoop on Modified (MOESI)

```
Start: Core 0 has M(99)

Step 1: Core 1 reads 0x1000
  Interconnect snoops Core 0: "Do you have 0x1000?"
  Core 0: M → O(99) (becomes owner, still dirty)
  Core 0 supplies data (99) directly to Core 1 (cache-to-cache transfer!)
  Core 1: I → S(99) (gets shared copy)

Result:
  Core 0: O(99)    Core 1: S(99)    Memory: 42 (still old — O is the source)
```

---

## 5. False Sharing

A performance problem caused by the coherency protocol:

```
┌──────────────────────────────────────────────────────────────┐
│  False Sharing                                                │
│                                                                │
│  Two cores access DIFFERENT variables that happen to be on    │
│  the SAME cache line (64 bytes):                               │
│                                                                │
│  struct {                                                      │
│      int core0_counter;  // Core 0 writes this                │
│      int core1_counter;  // Core 1 writes this                │
│  };                                                            │
│                                                                │
│  Both are in the same 64-byte cache line!                      │
│                                                                │
│  Core 0 writes core0_counter → invalidates Core 1's line      │
│  Core 1 writes core1_counter → invalidates Core 0's line      │
│  → Constant invalidation ping-pong (very slow!)                │
│                                                                │
│  Solution: Pad to cache line boundaries                        │
│  struct {                                                      │
│      int core0_counter;                                        │
│      char pad[60];  // Fill rest of 64-byte cache line        │
│      int core1_counter;  // Now on a DIFFERENT cache line     │
│      char pad2[60];                                            │
│  };                                                            │
│                                                                │
│  Or use __attribute__((aligned(64)))                           │
└──────────────────────────────────────────────────────────────┘
```

---

## 6. ARM Interconnect Coherency Solutions

```
┌──────────────┬──────────────────────────────────────────────────┐
│  Component   │  Description                                      │
├──────────────┼──────────────────────────────────────────────────┤
│  CCI-400/500 │  Cache Coherent Interconnect                     │
│              │  • 2-4 ACE ports (clusters) + ACE-Lite (GPU/IO) │
│              │  • Snoop filter for efficiency                    │
│              │  • Used in: big.LITTLE A53+A57 designs           │
│              │  • Protocol: AMBA 4 ACE                           │
│                                                                   │
│  CCN-502/512 │  Cache Coherent Network                          │
│              │  • Up to 12 clusters (48 cores)                   │
│              │  • Ring-based interconnect                         │
│              │  • L3 system cache (distributed)                  │
│              │  • Used in: server SoCs                            │
│              │  • Protocol: AMBA 4 ACE                           │
│                                                                   │
│  DSU         │  DynamIQ Shared Unit                              │
│              │  • Within-cluster coherency                       │
│              │  • Supports heterogeneous cores (big+LITTLE)     │
│              │  • Shared L3 cache                                │
│              │  • Snoop filter                                   │
│              │  • Protocol: Internal (exports ACE/CHI)           │
│                                                                   │
│  CMN-600/700 │  Coherent Mesh Network                           │
│              │  • Mesh topology (scalable to 256+ cores)        │
│              │  • System Level Cache (distributed slices)        │
│              │  • Snoop filter (directory-based)                 │
│              │  • Used in: Neoverse server platforms             │
│              │  • Protocol: AMBA 5 CHI                           │
└──────────────┴──────────────────────────────────────────────────┘
```

---

Next: [Cache Maintenance →](./04_Cache_Maintenance.md)
