Let’s take one **concrete example** and walk it all the way through so the idea of *granule support* becomes very clear.

---

# ✅ Example: Kernel built with **4KB pages**

Assume:

```c
CONFIG_ARM64_4K_PAGES = y
```

This means:

* Kernel page size = **4KB**
* Translation granule = **4K**
* Kernel **expects CPU to support 4K page tables**

---

# 🔍 Step 1: What kernel selects (from your macro)

Because of `CONFIG_ARM64_4K_PAGES`, these macros get picked:

```c
#define ID_AA64MMFR0_EL1_TGRAN_SHIFT ID_AA64MMFR0_EL1_TGRAN4_SHIFT  // bits [31:28]
#define ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN  0x0
#define ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX  0x7
```

So kernel says:

👉 “I will check **TGRAN4 field** (bits 31–28)
👉 And I will accept values between **0x0 and 0x7**”

---

# 🔍 Step 2: CPU provides capability

At runtime, CPU has:

```c
ID_AA64MMFR0_EL1 = 0x...?
```

Let’s say actual register value is:

```c
ID_AA64MMFR0_EL1 = 0x00000000
```

Now extract bits `[31:28]`:

```c
tgran4 = (mmfr0 >> 28) & 0xf
        = 0x0
```

---

# 🔍 Step 3: Kernel checks support

Kernel performs:

```c
if (tgran4 < 0x0 || tgran4 > 0x7)
    fail;
else
    supported;
```

Here:

```
tgran4 = 0x0
```

So:

```
0x0 >= 0x0  ✔
0x0 <= 0x7  ✔
```

✅ Result: **4K granule is supported → boot continues**

---

# ❌ Failure example (important)

Now assume CPU gives:

```c
tgran4 = 0xf
```

Then:

```
0xf > 0x7  ❌
```

👉 Kernel concludes:

❗ “This CPU does NOT support 4KB granule”

So it jumps to:

```asm
__no_granule_support:
    wfe
    wfi
    loop forever
```

👉 Boot stops.

---

# 🧠 What this really means (conceptually)

The kernel is saying:

> “I am built to use 4KB pages.
> Can this CPU handle 4KB page tables?”

If:

* YES → proceed to enable MMU
* NO → stop boot (because page tables would be invalid)

---

# 🧩 Why this is critical

Page tables depend on granule size:

| Granule | Page size | Table structure  |
| ------- | --------- | ---------------- |
| 4K      | 4 KB      | 4-level tables   |
| 16K     | 16 KB     | different layout |
| 64K     | 64 KB     | fewer levels     |

👉 If kernel and CPU mismatch:

* Page table format becomes invalid
* MMU translation breaks
* System crashes instantly

So kernel must **verify before enabling MMU**

---

# 🔄 Same logic for other configs

### If kernel uses 16K pages:

* Extract bits `[23:20]` (TGRAN16)
* Accept values `0x1 → 0xf`

### If kernel uses 64K pages:

* Extract bits `[27:24]` (TGRAN64)
* Accept values `0x0 → 0x7`

---

# 🎯 One-line summary

👉 **Granule support check = “Does the CPU support the page size this kernel was compiled for?”**

---

If you want next step, I can show:

* how page table levels differ for 4K vs 64K
* or how Linux chooses VA/PA size based on granule 👈 very important for interviews/debugging

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The MMU in ARMv8-A is enabled by writing bit 0 (M) of SCTLR_EL1 to 1 via an MSR instruction followed by an ISB. The ISB is the critical barrier: it flushes the instruction pipeline so that all instructions fetched AFTER the ISB use the new memory system configuration. Before the MMU is enabled, the CPU operates in a flat physical address space. After the bit is set, the TLB, page-table walker, TTBR0/TTBR1, TCR_EL1, and MAIR_EL1 all become active simultaneously. There is no intermediate state.

### Kernel Perspective (Linux ARM64)
Linux enables the MMU in __enable_mmu (arch/arm64/kernel/head.S), called from __primary_switch. The sequence is:
  1. Write TTBR0_EL1 (identity map root).
  2. Write TTBR1_EL1 (kernel map root).
  3. ISB to synchronize TTBR writes.
  4. Write SCTLR_EL1 with M=1 (via set_sctlr_el1 macro).
  5. ISB to flush the pipeline.
  6. RET -- the very next instruction is fetched through the new MMU.
The identity map ensures that the physical address of the code after the RET is also mapped at the same VA (PA==VA), so no instruction-fetch fault occurs.

### Memory Perspective (ARMv8 Memory Model)
The moment SCTLR_EL1.M is written to 1 and the ISB completes, the ARMv8 memory model transitions from "flat PA" to "two-stage VA->PA via page tables". The identity map (stored in __idmap_text_start to __idmap_text_end, mapped in the .idmap.text section) covers the physical pages of the MMU-enable code so the VA==PA invariant holds during the critical window. Without the identity map, the instruction fetch for the RET after set_sctlr_el1 would target a VA that has no valid TLB entry, causing a translation fault with no exception handler installed yet.