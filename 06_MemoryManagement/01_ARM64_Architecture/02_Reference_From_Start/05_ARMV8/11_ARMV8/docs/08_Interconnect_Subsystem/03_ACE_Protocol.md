# ACE Protocol (AXI Coherency Extensions)

## 1. Why ACE?

Standard AXI has no concept of cache coherency. When multiple masters (CPU cores, GPU) have caches, they can hold stale copies of data. **ACE** extends AXI with snoop channels to maintain coherency.

```
The problem ACE solves:

  Core 0 cache: [addr=0x1000, data=0x42]    (Modified)
  Core 1 reads addr 0x1000 from memory:      gets stale data!
  
  With ACE:
  Core 1 read request → Interconnect snoops Core 0
  → Core 0 responds with latest data (0x42)
  → Core 1 gets correct, coherent data

ACE adds 3 channels to AXI's 5:
  ┌──────────────────────────────────────────────────────┐
  │ Standard AXI (5 channels):                            │
  │   AW, W, B    — Write address, data, response        │
  │   AR, R       — Read address, data                   │
  │                                                        │
  │ ACE additions (3 channels):                           │
  │   AC  — Snoop Address (interconnect → master)        │
  │   CR  — Snoop Response (master → interconnect)       │
  │   CD  — Snoop Data (master → interconnect)           │
  └──────────────────────────────────────────────────────┘
```

---

## 2. ACE Channel Signals

```
Snoop Address Channel (AC) — Interconnect to Master:
┌──────────────┬───────────────────────────────────────────────┐
│ ACADDR       │ Address to snoop                              │
│ ACSNOOP[3:0] │ Snoop type:                                   │
│              │   0000 = ReadOnce    0001 = ReadShared        │
│              │   0010 = ReadClean   0011 = ReadNotSharedDirty│
│              │   0100 = ReadUnique  0111 = CleanInvalid      │
│              │   1000 = CleanShared 1101 = MakeInvalid       │
│ ACPROT[2:0]  │ Protection (same as ARPROT)                   │
│ ACVALID      │ Valid                                          │
│ ACREADY      │ Ready                                          │
└──────────────┴───────────────────────────────────────────────┘

Snoop Response Channel (CR) — Master to Interconnect:
┌──────────────┬───────────────────────────────────────────────┐
│ CRRESP[4:0]  │ Response:                                     │
│              │   [0] DataTransfer — will send data on CD     │
│              │   [1] Error                                   │
│              │   [2] PassDirty — data is dirty, take it     │
│              │   [3] IsShared — line still shared by me     │
│              │   [4] WasUnique — I had the only copy        │
│ CRVALID      │ Valid                                          │
│ CRREADY      │ Ready                                          │
└──────────────┴───────────────────────────────────────────────┘

Snoop Data Channel (CD) — Master to Interconnect:
┌──────────────┬───────────────────────────────────────────────┐
│ CDDATA       │ Snoop data (cache line contents)              │
│ CDLAST       │ Last beat of snoop data                       │
│ CDVALID      │ Valid                                          │
│ CDREADY      │ Ready                                          │
└──────────────┴───────────────────────────────────────────────┘
```

---

## 3. ACE Transaction Types

```
ACE extends AR/AW channels with domain and snoop information:

ARDOMAIN / AWDOMAIN [1:0]:
  00 = Non-shareable   (no coherency needed)
  01 = Inner Shareable (coherent within cluster)
  10 = Outer Shareable (coherent across system)
  11 = System           (full system scope)

ARSNOOP[3:0] (Read transaction types):
┌────────────────────┬──────────────────────────────────────────┐
│ Transaction        │ Description                               │
├────────────────────┼──────────────────────────────────────────┤
│ ReadNoSnoop (0x0)  │ Non-coherent read (like plain AXI)       │
│ ReadOnce (0x0)     │ Read, don't cache (streaming/DMA)        │
│ ReadClean (0x2)    │ Read, get clean copy (for instruction    │
│                    │ cache fill — no dirty data)              │
│ ReadNotSharedDirty │ Read, can accept dirty but not if shared │
│ (0x3)              │ by others                                │
│ ReadShared (0x1)   │ Read, willing to share with others       │
│ ReadUnique (0x7)   │ Read with intent to modify (exclusive    │
│                    │ ownership — others must invalidate)      │
└────────────────────┴──────────────────────────────────────────┘

AWSNOOP[2:0] (Write transaction types):
┌────────────────────┬──────────────────────────────────────────┐
│ WriteNoSnoop (0x0) │ Non-coherent write                       │
│ WriteUnique (0x0)  │ Write, I have unique copy                │
│ WriteLineUnique    │ Write full cache line, get ownership     │
│ (0x1)              │                                          │
│ WriteBack (0x3)    │ Write dirty line back to memory          │
│ WriteClean (0x2)   │ Write clean line (flush without losing)  │
│ Evict (0x4)        │ Notify interconnect of eviction (clean)  │
│ WriteEvict (0x5)   │ Evict with data                          │
└────────────────────┴──────────────────────────────────────────┘
```

---

## 4. Snoop Transaction Walk-Through

```
Example: Core 0 has addr 0x1000 in Modified state
         Core 1 wants to read addr 0x1000 (ReadShared)

Step 1: Core 1 → Interconnect (AR channel)
  ARADDR = 0x1000
  ARSNOOP = ReadShared
  ARDOMAIN = Inner Shareable

Step 2: Interconnect snoops Core 0 (AC channel)
  ACADDR = 0x1000
  ACSNOOP = ReadShared

Step 3: Core 0 checks its cache
  Found: 0x1000 in Modified state!
  Must provide data and transition to Shared

Step 4: Core 0 → Interconnect (CR channel)
  CRRESP = DataTransfer=1, PassDirty=1, IsShared=1
  (I'll send data, it's dirty, I'll keep a shared copy)

Step 5: Core 0 → Interconnect (CD channel)
  CDDATA = [64 bytes of cache line data]
  CDLAST = 1
  Core 0 transitions: Modified → Shared

Step 6: Interconnect → Core 1 (R channel)
  RDATA = [cache line data from Core 0]
  Core 1 caches as Shared

Step 7: Interconnect writes dirty data to memory
  (since Core 0 passed dirty ownership)

Result: Both cores have Shared copy, memory is up-to-date
```

---

## 5. DVM — Distributed Virtual Memory Messages

```
ACE carries DVM (Distributed Virtual Memory) messages for
TLB maintenance across cores:

When one core executes TLBI (TLB Invalidate), ALL cores
that might have cached that translation must invalidate too.

DVM flow:
  1. Core 0 executes: TLBI VMALLE1  (invalidate all EL1 TLB)
  2. Core 0 sends DVM message via ACE snoop channel
  3. Interconnect broadcasts DVM to Core 1, Core 2, Core 3
  4. Each core invalidates matching TLB entries
  5. Each core sends DVM Complete acknowledgment
  6. When all cores acknowledge → Core 0's TLBI completes
  7. Core 0 can then execute DSB (wait for completion)

This is why TLBI + DSB can be expensive on many-core systems!
Each TLBI must wait for all cores to acknowledge.
```

---

## 6. ACE-Lite

```
ACE-Lite is a simplified version for masters that have
NO cache but need coherent access to memory:

  ACE-Lite masters can:
  ✓ Issue coherent reads (will get snooped data if available)
  ✓ Issue coherent writes
  ✗ Do NOT have snoop channels (AC/CR/CD)
  ✗ Cannot be snooped (they don't cache data)

  Use cases:
  • DMA controller — reads/writes coherent memory
  • GPU — accesses CPU-coherent buffers
  • Network controller — DMA to coherent memory
  • Accelerators — share data with CPU without cache flushes

  ┌───────────────────────────────────────────────────────────┐
  │  ACE (full) Masters     ACE-Lite Masters                   │
  │  ┌──────┐ ┌──────┐     ┌──────┐ ┌──────┐                 │
  │  │Core 0│ │Core 1│     │ DMA  │ │ GPU  │                 │
  │  │(ACE) │ │(ACE) │     │(Lite)│ │(Lite)│                 │
  │  └──┬───┘ └──┬───┘     └──┬───┘ └──┬───┘                 │
  │     │AC/CR/CD│              │        │                     │
  │     │   ↕    │              │ No snoop│                     │
  │  ┌──▼────────▼──────────────▼────────▼──┐                 │
  │  │        Coherent Interconnect (CCI)    │                 │
  │  └──────────────────────────────────────┘                 │
  └───────────────────────────────────────────────────────────┘
  
  Benefit: DMA/GPU get coherent data without CPU doing
  cache maintenance (DC CIVAC, etc.) — huge performance win!
```

---

## 7. ACE Ordering Model

```
ACE ordering guarantees:

┌──────────────────────────────────────────────────────────────┐
│ Ordering Rule                                                 │
├──────────────────────────────────────────────────────────────┤
│ Transactions from SAME master, SAME ID: strictly ordered    │
│ Transactions from SAME master, DIFF ID: unordered           │
│ Transactions from DIFF masters: unordered (need barriers)   │
│                                                               │
│ Coherent reads: see latest coherent write to same address   │
│ Snoops: completed before snoop response is sent              │
│ DVM: all targets process before originator completes         │
│                                                               │
│ Barriers affect ordering:                                    │
│   DMB → ensures ordering of memory accesses within domain   │
│   DSB → ensures completion of memory accesses + TLBIs       │
│                                                               │
│ Barriers on ACE:                                             │
│   Mapped to ARBAR/AWBAR signals                              │
│   ARBAR[1:0]: 00=Normal, 01=Memory barrier, 10=Sync barrier │
│   Interconnect ensures all prior transactions complete       │
│   before barrier response                                    │
└──────────────────────────────────────────────────────────────┘
```

---

Next: [CHI Protocol →](./04_CHI_Protocol.md) | Back to [Interconnect Overview](./README.md)
