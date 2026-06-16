# VA Tagging: TCR_EL1.TBI0 and TBI1 — Top Byte Ignore

**Category**: Virtual Address Space  
**Targeted**: ARM, Qualcomm, NVIDIA

---

## 1. Concept Foundation

**Top Byte Ignore (TBI)** is an ARM64 feature that allows software to store metadata in the top byte (bits [63:56]) of a virtual address pointer without causing a Translation Fault.

When TBI is enabled, the hardware ignores bits [63:56] of the VA when performing address translation. These 8 bits are treated as "don't care" by the MMU — they do not participate in the canonical form check or page table walk.

This enables:
1. **Pointer Authentication (PAC, ARMv8.3)**: Store a cryptographic signature in the top byte
2. **Memory Tagging Extension (MTE, ARMv8.5)**: Store a 4-bit color tag in bits [59:56]
3. **Software metadata**: Custom OS/runtime metadata in pointer upper bits
4. **hwasan (Hardware-Assisted Address Sanitizer)**: Embed shadow tag in pointer

Without TBI, accessing any address with non-zero bits [63:56] would cause a Translation Fault (non-canonical address), since for 48-bit VA, bits [63:48] must be all-zeros or all-ones.

---

## 2. TCR_EL1 TBI Fields

```
TCR_EL1.TBI0 (bit[37]):
  0 = Top byte NOT ignored for TTBR0 region (user space)
      Bits [63:56] must participate in canonical check
  1 = Top byte IS ignored for TTBR0 region
      Bits [63:56] ignored; VA[55:0] used for translation

TCR_EL1.TBI1 (bit[38]):
  0 = Top byte NOT ignored for TTBR1 region (kernel space)
  1 = Top byte IS ignored for TTBR1 region

TCR_EL1.TBID0 (bit[51], ARMv8.3):
  0 = TBI0 applies to both data and instruction fetches
  1 = TBI0 applies to data accesses only; instruction fetches use full VA
      (Prevents code injection via tagged pointers)

TCR_EL1.TBID1 (bit[52], ARMv8.3):
  Same as TBID0 but for TTBR1 region
```

---

## 3. How TBI Works: Address Translation with Tagged Pointer

### Without TBI (TBI0=0)

```
User pointer: 0x0042_1234_5678_9ABC
              ^^^^ Top byte = 0x42 → non-canonical for 48-bit VA → FAULT
```

### With TBI (TBI0=1)

```
User pointer: 0x0042_1234_5678_9ABC
              ^^^^  Top byte = 0x42 → IGNORED by MMU

MMU uses: 0x????_1234_5678_9ABC → bits[63:56] ignored
          The effective address for translation = 0x0000_1234_5678_9ABC
          (hardware uses bits[55:0])
```

The load/store succeeds if 0x0000_1234_5678_9ABC has a valid mapping.

---

## 4. Canonical Form with TBI

With TBI enabled, the canonical rule changes:

```
Without TBI (48-bit VA):
  Canonical: bits[63:48] all 0x0000 or 0xFFFF

With TBI0=1 (48-bit VA):
  Canonical check: bits[55:48] must match bit[47]
  (bits[63:56] are ignored — they are the "tag" field)
  
  Valid user address with TBI0=1:
    bits[55:48] = 0x00 (user)
    bits[63:56] = any value (the tag — ignored)
  
  Example: 0xAB00_1234_5678_9ABC
    bits[63:56] = 0xAB (tag, ignored)
    bits[55:48] = 0x00 (canonical for user space)
    Translation uses: 0x??00_1234_5678_9ABC
```

---

## 5. MTE (Memory Tagging Extension) and TBI

MTE is the primary production use case for TBI in Linux ARM64.

MTE uses bits [59:56] of the pointer (a 4-bit "color" tag) and a corresponding 4-bit tag stored in a special granule in memory. On every load/store, the hardware compares the pointer tag with the memory tag:
- Match: access proceeds normally
- Mismatch: fault (used to detect use-after-free, buffer overflow)

```
Tagged pointer format (MTE, user space):
Bit[63:60]  — Must match bits[55:48] canonical pattern
Bit[59:56]  — 4-bit Logical Tag (software metadata)
Bit[55:0]   — Virtual Address (translation uses these bits)

Example:
  0x0005_0000_0042_1000
    ^           = tag bits[59:56] = 0x5
    The memory at PA(0x0000_0042_1000) has allocation tag 0x5
    If pointer tag ≠ memory tag → MTE fault
```

```c
// Linux MTE configuration in TCR_EL1:
// TBI0=1: Top byte ignored for user space (allows MTE tags in pointers)
// TCMA0=0: Tag Check Memory Access — faults on tag mismatch

// arch/arm64/include/asm/pgtable-hwdef.h
#define TCR_TBI0   (UL(1) << 37)
#define TCR_TBI1   (UL(1) << 38)
#define TCR_TCMA0  (UL(1) << 57)
#define TCR_TCMA1  (UL(1) << 58)
```

---

## 6. PAC (Pointer Authentication) and TBI

ARMv8.3 PAC stores a cryptographic signature in the upper bits of a pointer:

```
Signed kernel return address (TTBR1 region):
Bits[63:56] — Pointer Authentication Code (PAC)
Bits[55:0]  — Actual address

With TBI1=1: hardware ignores bits[63:56] during translation
After authentication (AUTIA/AUTIB): PAC stripped, original address restored
```

PAC and MTE use different bit ranges (PAC uses more bits for the signature). TBI ensures neither feature's metadata causes a translation fault.

---

## 7. Linux Kernel TBI Configuration

```c
// arch/arm64/mm/proc.S
// Linux enables TBI0 for user space (PAC and MTE support):
#ifdef CONFIG_ARM64_PTR_AUTH
// TBI0=1 allows PAC in user pointers
tcr |= TCR_TBI0;
#endif

#ifdef CONFIG_ARM64_MTE
// MTE also needs TBI0
tcr |= TCR_TBI0;
// Enable tag checking:
// SCTLR_EL1.TCF0 field controls EL0 tag check fault mode
#endif

// TBID0=1: TBI only for data, not instructions
// Prevents code injection via tagged instruction pointers
tcr |= TCR_TBID0;
```

---

## 8. SCTLR_EL1.TCF0 — Tag Check Fault Mode (MTE)

MTE tag faults are configured separately from TBI:

```
SCTLR_EL1.TCF0 [1:0]:
  0b00 = Tag check faults have no effect (silent ignore)
  0b01 = Tag check faults cause synchronous Data Abort
  0b10 = Tag check faults are logged asynchronously (accumulate in TFSR_EL1)
  0b11 = Both synchronous + asynchronous reporting

Linux uses:
  SCTLR_EL1.TCF0 = 0b10 for asynchronous reporting (production)
  SCTLR_EL1.TCF0 = 0b01 for synchronous reporting (debug/sanitizer builds)
```

---

## 9. hwasan vs MTE

```
hwasan (software):
  Uses bits[63:56] to store a shadow tag (software-implemented)
  Every load/store includes a software check against shadow memory
  TBI0=1 needed to avoid faulting on tagged pointers
  ~2× slowdown

MTE (hardware):
  Uses bits[59:56] (4-bit tag)
  Hardware checks tag on every access — no software overhead
  ~1-5% performance impact
  Requires ARMv8.5 or later
  Linux MTE support: arch/arm64/kernel/mte.c
```

---

## 10. Interview Questions & Answers

**Q1: What is TBI and why is it needed for MTE?**

TBI (Top Byte Ignore) allows bits [63:56] of a user-space pointer to carry metadata without causing a Translation Fault. MTE stores a 4-bit color tag in bits [59:56] of every pointer. Without TBI, accessing `0x0005_0000_1234_5678` would fault because bits[63:48] ≠ 0x0000 (not canonical). With `TCR_EL1.TBI0=1`, bits [63:56] are ignored by the MMU, and translation proceeds using bits [55:0]. The hardware simultaneously reads the 4-bit memory tag from the physical memory granule and compares it to bits[59:56] of the pointer.

**Q2: Can TBI be enabled for kernel space (TTBR1)?**

Yes, `TCR_EL1.TBI1=1` enables TBI for kernel addresses. Linux does this for kernel pointer authentication (PAC for kernel return addresses). The kernel also sets `TCR_EL1.TBID1=1` so that only data accesses use TBI — instruction fetches always use the full address. This prevents an attacker from injecting a tagged function pointer that would bypass PAC authentication at the instruction fetch level.

**Q3: What is TBID and why does it matter for security?**

`TBID0`/`TBID1` restrict TBI to data accesses only (not instruction fetches). Without TBID, a tagged pointer could be used as a branch target — the tag would be ignored and execution would jump to the underlying address. This could bypass pointer authentication if the attacker can forge a tagged pointer. With `TBID=1`, instruction fetches must use a canonical address — the full bits[63:48] must be canonical — which prevents tagged pointers from being used as jump targets.

**Q4: How does Linux handle a MTE tag mismatch fault?**

With `SCTLR_EL1.TCF0=0b01` (synchronous), a tag mismatch generates a Synchronous Data Abort with `ESR_EL1.EC=0x25` (Data Abort from EL0) and a specific ISS indicating a tag check fault. Linux delivers SIGSEGV (signal 11) with `si_code=SEGV_MTESERR` to the process. The `si_addr` points to the faulting address with the pointer's tag in bits[59:56]. With asynchronous mode (`TCF0=0b10`), faults are accumulated in `TFSR_EL0`/`TFSR_EL1` and delivered lazily.

---

## 11. Quick Reference

| Field | Bit | Value 0 | Value 1 |
|---|---|---|---|
| TBI0 | TCR[37] | No TBI for user (TTBR0) | TBI enabled for user |
| TBI1 | TCR[38] | No TBI for kernel (TTBR1) | TBI enabled for kernel |
| TBID0 | TCR[51] | TBI for data+instructions | TBI data only (not instructions) |
| TBID1 | TCR[52] | TBI for data+instructions | TBI data only |
| TCMA0 | TCR[57] | Tag check applies | Tag check bypassed for user |
| TCMA1 | TCR[58] | Tag check applies | Tag check bypassed for kernel |

| Use Case | TBI Setting | Bit Range Used |
|---|---|---|
| MTE (user) | TBI0=1, TBID0=1 | Bits[59:56] = 4-bit tag |
| PAC (user) | TBI0=1 | Bits[63:56] = PAC code |
| hwasan | TBI0=1 | Bits[63:56] = 8-bit shadow tag |
| Normal pointer | TBI0=0 | No tagging |
