

## Complete VA → PA Translation on ARM32

---

### Step 0 — The Starting Point: Hardware Registers

Before any translation happens, three CP15 registers govern everything:

| Register | Role |
|---|---|
| `TTBCR` | Decides which TTBR to use, and where the split is |
| `TTBR0` | Page table base for user space |
| `TTBR1` | Page table base for kernel space |
| `DACR` | Domain access control — a gate before AP bits are even checked |
| `CONTEXTIDR[7:0]` | Current ASID — used for TLB tagging |
| `SCTLR.M` | If this bit is 0, the entire translation below is bypassed; VA = PA |

---

### Step 1 — TTBR Selection via TTBCR.N

The `TTBCR.N` field (bits [2:0]) defines the split:

```
TTBCR.N = 1  →  VA[31] = 1  → TTBR1 (kernel)
               VA[31] = 0  → TTBR0 (user)
```

More precisely, if `VA[31:32-N]` are all 1s, TTBR1 is used; otherwise TTBR0 is used. Linux typically sets `N=1`, giving a clean 2GB/2GB split at `0x80000000`.

```
VA >= 0x80000000  →  TTBR1  (kernel, never changes across context switches)
VA <  0x80000000  →  TTBR0  (user, reloaded on every context switch)
```

---

### Step 2 — TLB Lookup First (Hardware does this before any walk)

The hardware checks the TLB **before** walking page tables:

```
TLB key  =  { ASID (from CONTEXTIDR[7:0]), VA[31:12] }

         ┌─ HIT  →  skip Steps 3–7, go straight to Step 8
         └─ MISS →  continue with page table walk
```

Global entries (kernel mappings with `nG=0`) match regardless of ASID, so the kernel TLB is never stale across process switches.

---

### Step 3 — Compute L1 Table Entry Address

```
VA bit layout:
 31        20 19      12 11          0
┌───────────┬──────────┬─────────────┐
│  L1 Index │ L2 Index │ Page Offset │
│  12 bits  │  8 bits  │  12 bits    │
└───────────┴──────────┴─────────────┘
```

```
L1 entry address = TTBR[31:14] | VA[31:20] << 2
```

The L1 table is 4096 entries × 4 bytes = **16 KB**, aligned to 16 KB. `TTBR[31:14]` is the base; `VA[31:20]` is the index (12 bits), shifted left 2 to get the byte offset.

---

### Step 4 — Read & Decode the L1 Descriptor

The MMU fetches the 32-bit word at that address. The bottom 2 bits tell you what you have:

| `[1:0]` | Type | Action |
|---|---|---|
| `0b00` | **Fault** | Translation fault immediately |
| `0b01` | **Page Table** | Points to an L2 table → continue to Step 5 |
| `0b10` | **Section (1 MB)** | PA resolved here → jump to Step 7a |
| `0b10` + `[18]=1` | **Supersection (16 MB)** | PA resolved here → jump to Step 7b |

---

### Step 5 — Domain Check (happens for both paths)

Before using any descriptor, the hardware:

1. Reads the **Domain** field from the descriptor (`bits[8:5]`)
2. Indexes into `DACR` (2 bits per domain × 16 domains = 32-bit register)
3. Evaluates:

```
DACR[domain*2+1 : domain*2]:
  0b00 → No Access  → Domain fault (abort), game over
  0b01 → Client     → AP bits are checked (Step 6)
  0b11 → Manager    → AP bits BYPASSED, full access granted
```

Linux sets kernel domain to Manager (`0b11`) and user domain to Client (`0b01`).

---

### Step 6 — Access Permission Check (Client domains only)

For Client domains, the hardware evaluates `APX:AP[1:0]` (3 bits total) against the current privilege level (`CPSR.M`):

```
APX=0, AP=01 → Kernel RW / User: fault
APX=0, AP=10 → Kernel RW / User RO
APX=0, AP=11 → Kernel RW / User RW
APX=1, AP=01 → Kernel RO / User: fault
APX=1, AP=10 → Kernel RO / User RO
```

If `SCTLR.AFE=1` (simplified mode), only `AP[2:1]` are checked.

Additionally:
- **XN bit** checked on instruction fetch — if set, a Permission fault fires
- **PXN bit** checked for privileged instruction fetch

---

### Step 7 — Physical Address Formation

#### Path A: Section entry (1 MB page)

```
PA = descriptor[31:20] | VA[19:0]
```

```
 descriptor bits:  31        20
                  ┌───────────┐
                  │  PA base  │  (1MB aligned)
                  └───────────┘
                                19         0
                               ┌────────────┐
                  +  VA offset │            │
                               └────────────┘
```

#### Path B: L2 Page Table (4 KB or 64 KB pages)

First, compute the L2 entry address:

```
L2 table base = descriptor[31:10]   (1 KB aligned, from L1 entry)
L2 entry addr = L2_base | VA[19:12] << 2
```

Read the L2 descriptor and decode its `[1:0]`:

| `[1:0]` | Type |
|---|---|
| `0b00` | Fault |
| `0b01` | Large page (64 KB) |
| `0b1x` | Small page (4 KB) ← most common |

**Small page (4 KB):**
```
PA = descriptor[31:12] | VA[11:0]
```

**Large page (64 KB):**
```
PA = descriptor[31:16] | VA[15:0]
```
(The same L2 descriptor is replicated across 16 consecutive L2 entries to match the 4 KB walk granularity.)

---

### Step 8 — Memory Attribute Delivery to Cache/Bus

The final PA isn't sent bare to the bus. The descriptor's **TEX, C, B** bits are decoded into a memory type that controls caching:

```
TEX=001, C=1, B=1 → Normal, Write-Back Write-Allocate  (RAM)
TEX=000, C=0, B=1 → Shared Device                      (MMIO)
TEX=000, C=0, B=0 → Strongly-Ordered                   (legacy devices)
```

The **S (Shareable)** bit tells the bus interconnect whether this access must be visible to other CPUs (critical for SMP coherency).

---

### Complete Flow Diagram

```
VA (32-bit)
    │
    ▼
[TTBCR.N] ──── select TTBR0 or TTBR1
    │
    ▼
[TLB lookup: (ASID, VA[31:12])]
    │
    ├── HIT ──────────────────────────────────┐
    │                                         │
    └── MISS                                  │
         │                                    │
         ▼                                    │
[L1 index = VA[31:20]]                        │
[L1 addr  = TTBR[31:14] | L1_idx << 2]       │
         │                                    │
         ▼                                    │
[Read L1 descriptor]                          │
         │                                    │
         ├─ [1:0]=0b00 → Translation Fault    │
         ├─ [1:0]=0b10 → Section/Supersection │
         │       PA = desc[31:20] | VA[19:0]  │
         │                                    │
         └─ [1:0]=0b01 → Page Table           │
                  │                           │
                  ▼                           │
         [Domain check via DACR]              │
                  │                           │
         [L2 addr = desc[31:10] | VA[19:12]<<2]
                  │
                  ▼
         [Read L2 descriptor]
                  │
                  ├─ Small: PA = desc[31:12] | VA[11:0]
                  └─ Large: PA = desc[31:16] | VA[15:0]
                  │
                  ▼
         [AP/APX permission check]
                  │
                  ▼
    ◄─────────────┘
Physical Address + Memory Attributes
(TEX,C,B,S) → Cache/Bus Controller
```

---

### Key Invariants to Remember

1. **SCTLR.M=0** → MMU off, VA=PA always, no translation at all.
2. **DACR Manager** → AP bits completely ignored; domain manager gets unconditional access.
3. **TLB always checked first** — the page table walk is the *slow path* only on a miss.
4. **nG=0 (Global)** entries survive ASID changes — kernel never needs TLB flush on context switch.
5. **Section entries** resolve in one memory access (L1 only); small pages need two (L1 + L2).
6. The **physical address bus width** can exceed 32 bits on LPAE-capable cores (Supersection entries encode `PA[39:32]`), but standard ARM32 with classical MMU is 32-bit PA.