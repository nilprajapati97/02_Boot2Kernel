# 10.01 — Pointer Authentication (PAuth) and Memory Tagging (MTE)

> **ARM ARM Reference**: §D5.1.6 (PAC), §D5.6.2 (MTE / Tagged memory)

Two security features that touch the MMU / memory subsystem on modern arm64 (Apple, Pixel, server SoCs).

---

## 1. Pointer Authentication (FEAT_PAuth, ARMv8.3)

Adds a cryptographic **PAC (Pointer Authentication Code)** to the high bits of pointers. On dereference or branch, hardware checks the PAC; mismatch → fault.

### Keys (per-EL)

| Key | Use |
|---|---|
| `APIAKey` | Instr address, A (return address typically) |
| `APIBKey` | Instr address, B |
| `APDAKey` | Data address, A |
| `APDBKey` | Data address, B |
| `APGAKey` | Generic (any data) |

### Instructions

| Instr | Action |
|---|---|
| `PACIA Xd, Xm`  | Sign instr addr using key A and salt Xm |
| `AUTIA Xd, Xm`  | Verify and strip PAC |
| `PACIASP / AUTIASP` | Common form using SP as salt — used in function prologue/epilogue |
| `RETA{A,B}` | Authenticated return |
| `BLRA{A,B}` | Authenticated indirect branch |

### Use cases

- **Return-address signing** — defeats most ROP. Linux uses for kernel; userspace via compiler `-mbranch-protection=pac-ret+leaf`.
- **Forward-edge** — vtable pointer signing (`bti`+`PACIxA` combinations).
- **Data pointers** — limited use (signing structures' embedded pointers).

### Interaction with MMU

PAC bits occupy the top of the VA (above the 48/52-bit translation range). `TCR_EL1.{TBI0,TBI1}` (Top Byte Ignore) is essential — MMU must ignore the upper byte during translation so signed pointers can be dereferenced. With FEAT_LVA + 52-bit VA, layout shrinks PAC field; tradeoffs apply.

---

## 2. MTE — Memory Tagging Extension (FEAT_MTE, ARMv8.5)

Tags 16-byte memory granules with a **4-bit tag**. Pointers carry a 4-bit tag in bits[59:56] (above TBI region). Hardware checks `pointer_tag == memory_tag` on access; mismatch → tag-check fault.

### Two modes

- **Sync** — fault on the offending access (precise; used for debug).
- **Async** — accumulated check, reported via `TFSR_EL1` on next sync point (faster; used in production).

### Instructions

| Instr | Action |
|---|---|
| `IRG Xd, Xn, Xm` | Insert random tag into pointer |
| `STG Xt, [Xn]`   | Set tag of 16-byte granule |
| `LDG Xt, [Xn]`   | Load tag |
| `ADDG / SUBG` | Adjust tag in pointer |
| `STZG` | Set tag + zero |

### MAIR interaction

MTE requires Normal Cacheable, Inner+Outer Write-back attributes. Untagged Device memory is not tagged-checked.

### TCR controls

`TCR_EL1.TCMA{0,1}` (Tag Check Match All) — selects whether tag-checking applies for TTBR0/1 ranges. Tag-check faults generate DFSC = 0x11.

### Use cases

- **Hardware-accelerated ASAN** — find use-after-free / heap-overflow at near-zero overhead.
- **GWP-ASAN-style sampling** in production.
- **Chrome/V8** sandbox isolation.

Tag storage: hidden DRAM region (carve-out) holding 4 bits per 16 B granule (~3% memory overhead).

---

## 3. Diagram — pointer layout

```mermaid
flowchart LR
    Bits63["63:60 PAC"] -- TBI ignores --> Bits59["59:56 MTE tag"] --> Bits55["55 sign-ext"] --> Bits48["47:0 VA"]
```

---

## 4. Pitfalls

1. **PAC + JIT** — generating code that uses authenticated branches requires runtime PACIA on emitted targets.
2. **PAC across fork/exec** — kernel re-derives keys on exec to defeat key leaks; old signed pointers in stale state fault.
3. **TBI off** — without TBI, PAC bits cause spurious translation faults.
4. **MTE mixed with non-tagged maps** — must mmap with PROT_MTE; ordinary mmap won't check.
5. **MTE async fault attribution** — by the time TFSR fires, you may be many instructions past the bad access; sync mode for triage.
6. **Sharing tagged memory with non-MTE device** — DMA must use untagged pointer (clear top bits).

---

## 5. Interview Q&A

**Q1. What problem does PAC solve?**
Return-oriented programming and similar control-flow hijacks. Cryptographic check on pointers before use makes forged pointers fail.

**Q2. Why must TBI be enabled with PAuth?**
The PAC occupies the top VA bits; MMU must ignore them so the pointer still translates correctly.

**Q3. PAC keys — where stored?**
EL1 system registers (APIAKey_EL1 high/low pair, etc.); not directly readable from EL0. Kernel reseeds on exec.

**Q4. What's MTE?**
Memory Tagging Extension — 4-bit tag per 16-byte granule, checked against pointer tag at access. Catches OOB/UAF.

**Q5. Sync vs async MTE?**
Sync: faults precisely. Async: accumulates, reported at sync point — much faster.

**Q6. Memory overhead of MTE?**
~3% (4 bits per 128 bits of data) — stored in DRAM carve-out.

**Q7. Can MTE be used for security in production?**
Yes — Android Pixel uses MTE for memory-safety mitigation; lower overhead than ASAN/HWASAN.

**Q8. How does MTE interact with SMMU/DMA?**
DMA doesn't check tags; pointers must be untagged before passing to devices. Kernel handles this in DMA-API.

---

## 6. Cross-refs

- [03.06 Permissions](../03_Page_Tables_and_Translation/06_Permission_Checks_AP_UXN_PXN.md)
- [07.02 TTBR/TCR](../07_System_Registers_Quickref/02_TTBR0_TTBR1_TCR.md)
- [08.02 ESR decode](../08_Faults_and_Aborts/02_ESR_FAR_HPFAR_Decoding.md)
