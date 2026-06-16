Let's map a concrete user-space address: **VA = `0x0040_1ABC`** (a typical `.text` address in a user process).

---

## Worked Example: VA `0x0040_1ABC` → PA

### Given Setup
```
TTBCR.N  = 1          (2GB/2GB split)
TTBR0    = 0x6000_4000 (user L1 table at physical 0x6000_4000)
DACR     = 0x0000_0005 (Domain 0 = Client, Domain 1 = No Access)
CPSR.M   = User mode
ASID     = 7          (current process)
```

---

### Step 1 — TTBR Selection

```
VA = 0x0040_1ABC
VA[31] = 0  →  VA < 0x80000000  →  use TTBR0
```

TTBR0 base = `0x6000_4000`

---

### Step 2 — TLB Lookup

```
TLB key = { ASID=7, VA[31:12] = 0x00401 }
```

Assume **TLB MISS** → page table walk begins.

---

### Step 3 — Extract Bit Fields from VA

```
VA = 0x0040_1ABC
   = 0000 0000 0100 0000 0001 1010 1011 1100
          ───────────  ────────  ────────────
            [31:20]     [19:12]    [11:0]
            L1 index    L2 index   Page offset

L1 index   = VA[31:20] = 0x004  = 4
L2 index   = VA[19:12] = 0x01   = 1
Page offset= VA[11:0]  = 0xABC
```

---

### Step 4 — Compute & Read L1 Entry

```
L1 entry address = TTBR0[31:14] | L1_index << 2
                 = 0x6000_4000  | (4 << 2)
                 = 0x6000_4000  | 0x10
                 = 0x6000_4010
```

Read 32-bit word at physical `0x6000_4010`:

```
L1 descriptor = 0x6100_1001
              = 0110 0001 0000 0000 0001 0000 0000 0001
                                                    ──
                                                   [1:0] = 0b01  →  Page Table entry
```

Decode the Page Table entry:
```
bits [31:10] = L2 table base address = 0x6100_1000  (1KB aligned)
bits [8:5]   = Domain = 0
bits [1:0]   = 0b01 (Page Table)
```

---

### Step 5 — Domain Check

```
Domain field = 0
DACR[1:0]   = 0b01  →  Client  →  AP bits WILL be checked
```

---

### Step 6 — Compute & Read L2 Entry

```
L2 entry address = L2_base | L2_index << 2
                 = 0x6100_1000 | (1 << 2)
                 = 0x6100_1004
```

Read 32-bit word at physical `0x6100_1004`:

```
L2 descriptor = 0x8040_045E
              = 1000 0000 0100 0000 0000 0100 0101 1110
                ────────────────────  ─────────────  ──
                     [31:12]              attrs       [1:0]
                  PA base = 0x80400       AP,TEX,etc  0b10 → Small Page
```

Decode:
```
bits [31:12] = PA base = 0x8040_0000
bits [9]     = APX = 0
bits [5:4]   = AP  = 0b11  →  APX:AP = 0b011  →  User RW  ✓
bits [8:6]   = TEX = 0b001
bits [3]     = C   = 1
bits [2]     = B   = 1      →  Normal WB-WA (RAM)
bits [11]    = nG  = 1      →  Process-specific (ASID tagged)
bits [1:0]   = 0b10         →  Small Page (4KB)
```

---

### Step 7 — AP Check

```
Current mode = User
APX:AP = 0b011  →  AP=11  →  User RW  ✓  (no fault)
XN bit = 0      →  Execute allowed  ✓
```

---

### Step 8 — Form Physical Address

```
PA = descriptor[31:12]  |  VA[11:0]
   =    0x8040_0000     |   0xABC
   =    0x8040_0ABC
```

---

### Visual Bit Concatenation

```
VA:  0x0040_1ABC
     ┌──────────────┬──────────┬────────────┐
     │  0x004 (L1)  │ 0x01(L2) │ 0xABC (off)│
     └──────────────┴──────────┴────────────┘
                                      │
                    L1[4] → L2 table @ 0x6100_1000
                    L2[1] → PA base  = 0x8040_0000
                                      │
                                      ▼
PA:  0x8040_0ABC
     ┌──────────────────────┬────────────┐
     │   0x80400 (PA base)  │ 0xABC (off)│
     └──────────────────────┴────────────┘
```

---

### Step 9 — TLB Fill & Memory Access

The hardware now:
1. **Loads the TLB** with `{ ASID=7, VA[31:12]=0x00401 → PA=0x80400, attrs: WB-WA, User-RW }`
2. **Issues the bus transaction** to PA `0x8040_0ABC` with **Normal, Write-Back** cache attributes
3. **Future accesses** to any address in `0x00401_000–0x00401_FFF` hit the TLB and skip the walk entirely

---

### Summary Table

| Step | Operation | Value |
|---|---|---|
| Input VA | — | `0x0040_1ABC` |
| TTBR selected | VA[31]=0 → TTBR0 | `0x6000_4000` |
| L1 index | VA[31:20] | `4` |
| L1 entry addr | `0x6000_4000 + (4×4)` | `0x6000_4010` |
| L1 descriptor | read from L1 | `0x6100_1001` (Page Table) |
| L2 base | desc[31:10] | `0x6100_1000` |
| L2 index | VA[19:12] | `1` |
| L2 entry addr | `0x6100_1000 + (1×4)` | `0x6100_1004` |
| L2 descriptor | read from L2 | `0x8040_045E` (Small Page) |
| PA base | desc[31:12] | `0x8040_0000` |
| Page offset | VA[11:0] | `0xABC` |
| **Final PA** | PA base + offset | **`0x8040_0ABC`** |
| Memory type | TEX=001,C=1,B=1 | Normal WB-WA |
| Permission | APX:AP=011, User mode | Read/Write allowed |