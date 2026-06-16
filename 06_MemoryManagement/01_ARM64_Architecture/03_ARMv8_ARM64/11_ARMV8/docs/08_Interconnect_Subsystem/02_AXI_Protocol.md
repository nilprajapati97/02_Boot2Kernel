# AXI Protocol (AMBA AXI4)

## 1. Overview

AXI (Advanced eXtensible Interface) is the workhorse protocol of modern ARM SoCs. It connects CPUs, GPUs, DMA engines, and memory controllers.

```
AXI key features:
  • 5 independent channels (read/write can happen simultaneously)
  • Multiple outstanding transactions (don't wait for response)
  • Burst transfers (up to 256 beats)
  • Out-of-order responses (with transaction IDs)
  • Multiple clock domains (with async bridges)
  • QoS (Quality of Service) support
```

---

## 2. Five AXI Channels

```
AXI separates read and write into independent channels:

  ┌──────────────────────────────────────────────────────────────┐
  │                                                               │
  │  Master                                        Slave          │
  │  (CPU/DMA)                                    (Memory/Periph)│
  │                                                               │
  │  ──── Write Address Channel (AW) ─────────────────►          │
  │       (AWADDR, AWLEN, AWSIZE, AWBURST, AWID...)              │
  │                                                               │
  │  ──── Write Data Channel (W) ─────────────────────►          │
  │       (WDATA, WSTRB, WLAST)                                  │
  │                                                               │
  │  ◄──── Write Response Channel (B) ────────────────           │
  │       (BRESP, BID)                                            │
  │                                                               │
  │  ──── Read Address Channel (AR) ──────────────────►          │
  │       (ARADDR, ARLEN, ARSIZE, ARBURST, ARID...)              │
  │                                                               │
  │  ◄──── Read Data Channel (R) ─────────────────────           │
  │       (RDATA, RRESP, RLAST, RID)                              │
  │                                                               │
  └──────────────────────────────────────────────────────────────┘

  Write = 3 channels: AW (address) + W (data) + B (response)
  Read  = 2 channels: AR (address) + R (data+response)
  
  All channels are INDEPENDENT — reads and writes happen in parallel
```

---

## 3. AXI Handshake Protocol

```
Every channel uses the SAME 2-signal handshake:

  VALID: Source asserts when data/address is available
  READY: Destination asserts when it can accept

  Transfer occurs on clock edge where VALID=1 AND READY=1

  Three valid scenarios:

  1. VALID before READY:
     Clock:  ─┐  ┌──┐  ┌──┐  ┌──┐  ┌──
              └──┘  └──┘  └──┘  └──┘
     VALID:  ───┐              ┌───
                └──────────────┘
     READY:  ─────────┐  ┌────────
                       └──┘
     Data:   ───XXXX──DATA──XXXX───
                          ↑ Transfer here (both high)

  2. READY before VALID:
     READY:  ──┐                   ┌───
               └───────────────────┘
     VALID:  ─────────┐  ┌────────
                       └──┘
              Transfer ───↑

  3. VALID and READY simultaneously:
     VALID:  ──┐  ┌───
               └──┘
     READY:  ──┐  ┌───
               └──┘
              ↑ Transfer

  RULE: Master must NOT wait for READY before asserting VALID
        (prevents deadlock)
  RULE: Slave CAN wait for VALID before asserting READY
```

---

## 4. AXI Signals (Complete)

```
Write Address Channel (AW):
┌──────────────┬──────┬─────────────────────────────────────────┐
│ Signal       │ Width│ Description                              │
├──────────────┼──────┼─────────────────────────────────────────┤
│ AWID         │ var  │ Transaction ID (for out-of-order)       │
│ AWADDR       │ 32/64│ Start address                           │
│ AWLEN[7:0]   │ 8    │ Burst length: 0=1 beat, 255=256 beats  │
│ AWSIZE[2:0]  │ 3    │ Beat size: 0=1B,1=2B,2=4B,3=8B,...7=128B│
│ AWBURST[1:0] │ 2    │ Burst type: 00=FIXED,01=INCR,10=WRAP   │
│ AWLOCK       │ 1    │ Exclusive access (for atomics)          │
│ AWCACHE[3:0] │ 4    │ Cache attributes                       │
│ AWPROT[2:0]  │ 3    │ Protection: [0]priv,[1]nonsec,[2]instr │
│ AWQOS[3:0]   │ 4    │ Quality of Service                     │
│ AWREGION[3:0]│ 4    │ Region identifier                      │
│ AWVALID      │ 1    │ Address valid                           │
│ AWREADY      │ 1    │ Slave ready                             │
└──────────────┴──────┴─────────────────────────────────────────┘

Write Data Channel (W):
┌──────────────┬──────┬─────────────────────────────────────────┐
│ WDATA        │ var  │ Write data (32/64/128/256/512/1024 bits)│
│ WSTRB        │ var  │ Write strobes (1 bit per byte)          │
│              │      │ WSTRB[n]=1 means byte n is valid       │
│ WLAST        │ 1    │ Last beat of burst (1=last)             │
│ WVALID       │ 1    │ Data valid                              │
│ WREADY       │ 1    │ Slave ready for data                   │
└──────────────┴──────┴─────────────────────────────────────────┘

Write Response Channel (B):
┌──────────────┬──────┬─────────────────────────────────────────┐
│ BID          │ var  │ Transaction ID (matches AWID)           │
│ BRESP[1:0]   │ 2    │ Response: 00=OKAY,01=EXOKAY,           │
│              │      │ 10=SLVERR, 11=DECERR                   │
│ BVALID       │ 1    │ Response valid                          │
│ BREADY       │ 1    │ Master ready for response              │
└──────────────┴──────┴─────────────────────────────────────────┘

Read Address Channel (AR): Same as AW but with AR prefix
Read Data Channel (R):    RDATA + RID + RRESP + RLAST + handshake
```

---

## 5. Burst Types

```
AXI supports three burst types:

1. FIXED (AWBURST=00):
   All beats access the SAME address
   Use: FIFO access (writing to a peripheral data register)
   
   Beat:  1      2      3      4
   Addr:  0x100  0x100  0x100  0x100  (same address)

2. INCR (AWBURST=01):
   Address increments by transfer size each beat
   Use: Sequential memory access (most common)
   
   Beat:  1      2      3      4
   Addr:  0x100  0x108  0x110  0x118  (incrementing by 8)

3. WRAP (AWBURST=10):
   Address wraps at boundary (aligned to burst size)
   Use: Cache line fill (critical word first)
   
   4-beat wrap burst, 8 bytes/beat, start at 0x118:
   Beat:  1      2      3      4
   Addr:  0x118  0x100  0x108  0x110  (wraps at 0x120 boundary)
   
   The cache gets data from the critical word first,
   then wraps to fill the rest of the line.

AWLEN and AWSIZE define burst:
  Total bytes = (AWLEN + 1) × 2^AWSIZE
  
  Example: Cache line fill (64 bytes, 128-bit bus)
    AWLEN  = 3  (4 beats)
    AWSIZE = 4  (16 bytes per beat = 128-bit bus)
    AWBURST = WRAP
    Total = 4 × 16 = 64 bytes = one cache line
```

---

## 6. Transaction IDs and Ordering

```
AXI uses IDs to support multiple outstanding transactions:

  Master issues multiple reads without waiting:
  
  Time 1: AR channel: ARID=0, ARADDR=0x1000 → Read A
  Time 2: AR channel: ARID=1, ARADDR=0x2000 → Read B  
  Time 3: AR channel: ARID=0, ARADDR=0x3000 → Read C
  
  Slave can respond out of order (by ID):
  Time 5: R channel: RID=1, RDATA=...  → Response for Read B
  Time 6: R channel: RID=0, RDATA=...  → Response for Read A
  Time 7: R channel: RID=0, RDATA=...  → Response for Read C

Ordering rules:
  ┌─────────────────────────────────────────────────────────────┐
  │ Rule                                                         │
  ├─────────────────────────────────────────────────────────────┤
  │ Same ID:     responses must be IN ORDER                     │
  │ Different ID: responses can be OUT OF ORDER                │
  │ Read after Read (same ID):  ordered                        │
  │ Write after Write (same ID): ordered                       │
  │ Read vs Write (same ID):    ordered                        │
  │ Read vs Write (diff ID):    NO ordering guarantee          │
  └─────────────────────────────────────────────────────────────┘

  CPU uses same ID for accesses that need ordering.
  DMA engine uses different IDs for independent channels.
  This allows maximum concurrency while preserving correctness.
```

---

## 7. AXI Write Data Interleaving (AXI3 only)

```
AXI3 allowed write data from different transactions to be interleaved:

  AW: [ID=0, addr=A] [ID=1, addr=B]
  W:  [ID=0, data0] [ID=1, data0] [ID=0, data1] [ID=1, data1]
      ← interleaved write data beats

AXI4 REMOVED write interleaving (too complex, rarely used).
AXI4: All write data beats for one transaction must be contiguous.
```

---

## 8. AXI Exclusive Access (Atomics)

```
Exclusive access implements atomic read-modify-write for ARMv8:

  LDXR X0, [X1]     →  AXI exclusive read  (ARLOCK=1)
  STXR W2, X0, [X1] →  AXI exclusive write (AWLOCK=1)

  Flow:
  1. CPU sends exclusive read (ARLOCK=1, address=A)
  2. Slave monitors address A for this master
  3. CPU modifies value locally
  4. CPU sends exclusive write (AWLOCK=1, address=A)
  5. Slave checks: was A accessed by another master?
     → No:  BRESP=EXOKAY (exclusive write succeeded)
     → Yes: BRESP=OKAY (exclusive write failed — retry)
  6. CPU checks STXR result (W2):
     → 0 = success
     → 1 = fail → branch back to LDXR

  The interconnect/slave must implement an "exclusive monitor"
  to track exclusive access reservations per master.
```

---

## 9. AXI4-Lite

```
Simplified AXI for simple peripherals (register access):

AXI4-Lite restrictions:
  • No bursts (AWLEN/ARLEN always 0 — single transfers)
  • No write strobes for sub-word access (all bytes valid)
  • Fixed data width (32 or 64 bits)
  • No IDs (no out-of-order, no interleaving)
  • No QoS, no regions, no user signals
  
  Use: Simple MMIO register access to peripherals
  Benefit: Much simpler hardware implementation than full AXI
  
  Example: CPU reads a 32-bit status register
    AR: ARADDR=0xFF00_0010, ARVALID=1
    R:  RDATA=0x0000_0001, RRESP=OKAY, RVALID=1
```

---

## 10. AXI4-Stream

```
AXI-Stream is a point-to-point streaming protocol (NO addresses):

  ┌──────────┐   TDATA, TVALID, TREADY   ┌──────────┐
  │  Source   │──────────────────────────►│  Sink    │
  │ (Camera,  │   TLAST (end-of-packet)   │ (Codec,  │
  │  DMA Rd)  │   TKEEP (byte enables)    │  Display)│
  └──────────┘                            └──────────┘

  No addresses — just a data pipe
  Uses VALID/READY handshake (same as AXI)
  TLAST marks end of a "packet" or frame
  
  Use cases:
  • Video pipeline: camera → ISP → encoder → display
  • Audio streaming
  • Network data path
  • DSP data flow
  • DMA scatter/gather (memory → stream → memory)
```

---

Next: [ACE Protocol →](./03_ACE_Protocol.md) | Back to [Interconnect Overview](./README.md)
