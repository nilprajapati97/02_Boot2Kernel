# CHI Protocol (Coherent Hub Interface)

## 1. Why CHI?

ACE (AXI-based coherency) uses a crossbar topology — every port connects to every other port. This doesn't scale beyond ~8-12 ports. **CHI** replaces ACE for scalable, many-core SoCs with a mesh or ring interconnect.

```
ACE limitation:
  ┌─────────────────────────────────────────────────────┐
  │  ACE Crossbar (CCI-500):                             │
  │  • Every master connected to every slave             │
  │  • O(N²) wiring — doesn't scale past ~10 ports      │
  │  • Snoop broadcast to ALL masters (expensive)       │
  │  • Suitable for: 4-8 core mobile SoCs               │
  └─────────────────────────────────────────────────────┘

CHI solution:
  ┌─────────────────────────────────────────────────────┐
  │  CHI Mesh (CMN-700):                                 │
  │  • Nodes connected via mesh topology                 │
  │  • O(N) wiring — scales to 256+ nodes               │
  │  • Snoop filter/directory — targeted snoops          │
  │  • Suitable for: server, HPC, data center SoCs      │
  └─────────────────────────────────────────────────────┘
```

---

## 2. CHI Node Types

```
CHI uses a node-based architecture:

┌──────────────────────────────────────────────────────────────────┐
│                                                                    │
│  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────────┐  ┌──────────────┐  │
│  │ RN-F │  │ RN-F │  │ RN-I │  │ RN-D     │  │ HN-F         │  │
│  │Core 0│  │Core 1│  │ DMA  │  │ (RN-I w/ │  │ (Home Node   │  │
│  │      │  │      │  │      │  │  DVM)    │  │  Full)       │  │
│  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───────┘  └──┬───────────┘  │
│     │         │         │         │              │                │
│  ┌──▼─────────▼─────────▼─────────▼──────────────▼───────┐      │
│  │                CHI Mesh Interconnect                     │      │
│  │                (CMN-600 / CMN-700)                       │      │
│  └──┬──────────────────────────┬──────────────┬──────────┘      │
│     │                          │              │                  │
│  ┌──▼───────────┐  ┌──────────▼──┐  ┌───────▼───────┐          │
│  │ SN-F         │  │ MN          │  │ HN-I          │          │
│  │ (Slave Node  │  │ (Misc Node) │  │ (Home Node    │          │
│  │  Full — DDR) │  │             │  │  I/O)         │          │
│  └──────────────┘  └─────────────┘  └───────────────┘          │
│                                                                    │
└──────────────────────────────────────────────────────────────────┘

Node Types:
┌──────────┬────────────────────────────────────────────────────────┐
│ Node     │ Description                                            │
├──────────┼────────────────────────────────────────────────────────┤
│ RN-F     │ Request Node Full — CPU core with cache (can be       │
│          │ snooped, full coherency)                               │
├──────────┼────────────────────────────────────────────────────────┤
│ RN-I     │ Request Node I/O — I/O master without cache (DMA,    │
│          │ GPU). Like ACE-Lite.                                   │
├──────────┼────────────────────────────────────────────────────────┤
│ RN-D     │ Request Node with DVM — like RN-I but participates   │
│          │ in DVM (TLB maintenance). E.g., SMMU.                │
├──────────┼────────────────────────────────────────────────────────┤
│ HN-F     │ Home Node Full — coherency point. Contains Snoop     │
│          │ Filter / Directory. Manages coherency for an          │
│          │ address range. Routes requests, sends snoops.         │
├──────────┼────────────────────────────────────────────────────────┤
│ HN-I     │ Home Node I/O — for non-coherent I/O regions.       │
│          │ Routes to device memory without coherency.            │
├──────────┼────────────────────────────────────────────────────────┤
│ SN-F     │ Slave Node Full — memory controller (DDR, HBM).     │
│          │ Responds to data requests from HN.                    │
├──────────┼────────────────────────────────────────────────────────┤
│ MN       │ Miscellaneous Node — DVM processing, system control.│
└──────────┴────────────────────────────────────────────────────────┘
```

---

## 3. CHI Channels (Flits)

```
CHI uses message-based communication (flits = flow control units):

┌──────────┬──────────────────────────────────────────────────────┐
│ Channel  │ Description                                          │
├──────────┼──────────────────────────────────────────────────────┤
│ REQ      │ Request: Read, Write, Snoop, Maintenance             │
│          │ RN → HN: "I want to read/write address X"           │
├──────────┼──────────────────────────────────────────────────────┤
│ SNP      │ Snoop: HN → RN-F (cache lookup request)             │
│          │ HN → RN: "Do you have address X in your cache?"     │
├──────────┼──────────────────────────────────────────────────────┤
│ DAT      │ Data: carries cache line data (64 bytes)             │
│          │ Can flow: SN → HN → RN, or RN → RN (direct)        │
├──────────┼──────────────────────────────────────────────────────┤
│ RSP      │ Response: completion, acknowledgment                 │
│          │ "Transaction complete" / "Snoop result"              │
└──────────┴──────────────────────────────────────────────────────┘

Flit (Flow Control Unit):
  Each message is a "flit" — a fixed-size packet on the mesh:
  
  REQ flit: ~90 bits (address, opcode, TxnID, SrcID, TgtID, ...)
  SNP flit: ~70 bits (address, opcode, TxnID, SrcID, ...)
  DAT flit: ~640 bits (256-bit data + metadata)
  RSP flit: ~50 bits (opcode, TxnID, status)

  Unlike AXI (channel-based, back-to-back signals),
  CHI uses packet/flit-based communication through the mesh.
```

---

## 4. CHI Transaction Flow

```
Example: Core 0 (RN-F) reads address 0x1000, currently in
         Core 1's cache (Modified state)

  Step 1: Core 0 → HN-F (REQ channel)
    Opcode: ReadShared
    Address: 0x1000
    TxnID: 42
    SrcID: Core 0 node ID

  Step 2: HN-F looks up Snoop Filter/Directory
    Directory says: "Core 1 has 0x1000 in Modified state"
    → Targeted snoop (NOT broadcast!)

  Step 3: HN-F → Core 1 (SNP channel)
    Opcode: SnpShared
    Address: 0x1000
    
  Step 4: Core 1 checks cache
    Found: 0x1000 Modified
    → Must provide data, transition to Shared

  Step 5: Core 1 → Core 0 (DAT channel) [Direct Data Transfer!]
    Opcode: SnpRespData_SC (Snoop Response with Data, Shared Clean)
    Data: [64 bytes]
    Note: Data goes DIRECTLY from Core 1 to Core 0
          (does NOT go through HN-F — reduces latency!)

  Step 6: Core 1 → HN-F (RSP channel)
    "I provided data, transitioned to Shared"

  Step 7: HN-F updates Snoop Filter:
    Core 0: Shared, Core 1: Shared

  Step 8: Core 0 has the data, transaction complete!

CHI optimization: Direct Data Transfer (DDT)
  In ACE: all data goes through the interconnect
  In CHI: data can go directly from one RN to another
  → Reduces latency and interconnect congestion
```

---

## 5. Snoop Filter / Directory

```
The Snoop Filter (in HN-F) is a critical CHI component:

Purpose: Track which caches hold which addresses
  → Send snoops ONLY to caches that have the data
  → Avoid expensive broadcast snoops

Snoop Filter entry:
  ┌────────────────────────────────────────────────────┐
  │  Address Tag  │ Core 0 │ Core 1 │ Core 2 │ Core 3 │
  │               │ State  │ State  │ State  │ State  │
  ├───────────────┼────────┼────────┼────────┼────────┤
  │  0x1000_xxxx  │   I    │   M    │   I    │   I    │
  │  0x2000_xxxx  │   S    │   S    │   I    │   S    │
  │  0x3000_xxxx  │   I    │   I    │   E    │   I    │
  └───────────────┴────────┴────────┴────────┴────────┘

  Reading: Core 0 requests 0x1000
    SF lookup: Only Core 1 has it (Modified)
    → Snoop ONLY Core 1 (not Core 2, 3)

Without Snoop Filter (broadcast):
  Every read → snoop ALL cores → O(N) snoop traffic
  With 64 cores → 63 snoops per read!

With Snoop Filter (directory):
  Read → lookup directory → snoop only holders (usually 1-2)
  With 64 cores → 1-2 snoops per read!

  SF size: typically tracks last 32K-256K cache lines
  SF eviction: if SF is full, must "back-invalidate" a cache
  line from a core to make room in the SF
```

---

## 6. CHI Cache States (Extended MOESI)

```
CHI uses a more detailed cache state model than MESI/MOESI:

┌───────┬───────────────────────────────────────────────────────┐
│ State │ Description                                           │
├───────┼───────────────────────────────────────────────────────┤
│  I    │ Invalid — line not in cache                           │
│  SC   │ Shared Clean — shared copy, memory is up-to-date    │
│  UC   │ Unique Clean — only copy, memory is up-to-date      │
│       │ (can silently write → UD without snoop)               │
│  SD   │ Shared Dirty — shared, but I'm responsible for      │
│       │ writing back to memory (like MOESI Owned)            │
│  UD   │ Unique Dirty — only copy, dirty (must write back)   │
│       │ (equivalent to MESI Modified)                        │
│  UDP  │ Unique Dirty Partial — partially valid line (store  │
│       │ without full line read, ARMv8's FEAT_MTE2)          │
└───────┴───────────────────────────────────────────────────────┘

State transitions:
  Read miss:
    I → SC  (shared read)
    I → UC  (unique read, no other sharers)
  
  Write miss:
    I → UD  (ReadUnique + write)
  
  Write hit:
    SC → UD (need MakeUnique first — invalidate others)
    UC → UD (silent upgrade — no snoop needed!)
  
  Eviction:
    SC → I  (clean evict — just drop)
    UD → I  (dirty evict — must WriteBack to memory)
    SD → I  (dirty evict — must WriteBack)
    UC → I  (clean evict — Evict notification to HN)

UC → UD is a KEY optimization:
  If you have Unique Clean, you can write without any
  coherency transaction! Just write locally.
  (In MESI 'E' state you can silently go to 'M')
```

---

## 7. CHI Mesh Topology

```
CMN-700 Mesh example (4x4):

  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐
  │ RN-F │──│ HN-F │──│ RN-F │──│ HN-I │
  │Core 0│  │  SF  │  │Core 1│  │ I/O  │
  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘
     │         │         │         │
  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐
  │ HN-F │──│ XP   │──│ HN-F │──│ RN-I │
  │  SF  │  │(cross│  │  SF  │  │ GPU  │
  └──┬───┘  │point)│  └──┬───┘  └──┬───┘
     │      └──┬───┘     │         │
  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐
  │ RN-F │──│ HN-F │──│ RN-F │──│ SN-F │
  │Core 2│  │  SF  │  │Core 3│  │ DDR0 │
  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘
     │         │         │         │
  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐  ┌──▼───┐
  │ RN-I │──│ HN-F │──│ SN-F │──│ MN   │
  │ DMA  │  │  SF  │  │ DDR1 │  │      │
  └──────┘  └──────┘  └──────┘  └──────┘

  XP = Cross Point (mesh router node)
  Each node connects to 4 neighbors (N/S/E/W)
  Packets routed via X-Y routing:
    First go horizontal (X), then vertical (Y)
  
  Scalability:
    CMN-600: up to 8x8 mesh (64 nodes)
    CMN-700: up to 12x12 mesh (144 XPs, 256 nodes)
    CMN-S3:  up to 12x12 mesh, CHI Issue F
```

---

## 8. CHI vs ACE Comparison

```
┌──────────────────┬────────────────┬───────────────────────────┐
│ Feature          │ ACE            │ CHI                       │
├──────────────────┼────────────────┼───────────────────────────┤
│ Topology         │ Crossbar/bus   │ Mesh/ring                 │
│ Scalability      │ 4-10 ports     │ 32-256+ nodes             │
│ Protocol         │ Channel-based  │ Flit/packet-based         │
│ Snooping         │ Broadcast      │ Directory + targeted      │
│ Data transfer    │ Through interco│ Direct core-to-core       │
│ Cache states     │ MOESI          │ Extended (UC/SC/UD/SD)    │
│ Interconnect IP  │ CCI-400/500    │ CMN-600/700/S3            │
│ Target SoCs      │ Mobile         │ Server, HPC, data center  │
│ Bandwidth        │ ~20 GB/s       │ ~100+ GB/s                │
│ Latency          │ Lower (simpler)│ Slightly higher (routing) │
│ Complexity       │ Medium         │ High                      │
│ Memory ctrl      │ External       │ SN-F integrated in mesh   │
└──────────────────┴────────────────┴───────────────────────────┘
```

---

Next: Back to [Interconnect Overview](./README.md) | Continue to [Power Management →](../09_Power_Management/)
