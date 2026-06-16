# ARM64 `set_my_cpu_offset()` — `tpidr_el1` / `tpidr_el2` Deep Dive

## Source Reference
- `arch/arm64/include/asm/percpu.h:15` — `set_my_cpu_offset()`
- `arch/arm64/include/asm/percpu.h:23` — `__hyp_my_cpu_offset()`
- `arch/arm64/include/asm/percpu.h:32` — `__kern_my_cpu_offset()`
- `arch/arm64/kernel/smp.c:456` — called from `smp_prepare_boot_cpu()`
- `arch/arm64/kernel/smp.c:203` — called from `secondary_start_kernel()`

---

## The `tpidr_el*` Register Family

ARM64 provides several Thread Pointer ID Registers at different exception levels:

| Register | Exception Level | Access | Linux Use |
|---|---|---|---|
| `tpidr_el0` | EL0 (user) + EL1 write | User R/W | User-space TLS pointer |
| `tpidr_el1` | EL1 only | Kernel R/W | Per-CPU offset (non-VHE) |
| `tpidr_el2` | EL2 only | Hypervisor R/W | Per-CPU offset (VHE) / KVM guest |
| `tpidr_el3` | EL3 only | Secure monitor | Firmware use |
| `tpidrro_el0` | EL0 read / EL1 write | User R-only | (some arch uses) |

For the Linux kernel:
- Non-VHE: uses `tpidr_el1`
- VHE (Virtualization Host Extensions, EL2): uses `tpidr_el2`

---

## `set_my_cpu_offset()` — Writing the Register

```c
/* arch/arm64/include/asm/percpu.h:15 */
static inline void set_my_cpu_offset(unsigned long off)
{
    asm volatile(
        ALTERNATIVE("msr tpidr_el1, %0",   /* default instruction */
                    "msr tpidr_el2, %0",   /* VHE replacement */
                    ARM64_HAS_VIRT_HOST_EXTN)
        :: "r" (off) : "memory"
    );
}
```

### How `ALTERNATIVE()` Works

`ALTERNATIVE(orig, alt, feature)` is an ARM64 macro defined in:
```c
/* arch/arm64/include/asm/alternative.h */
#define ALTERNATIVE(oldinstr, newinstr, feature) \
    ".pushsection .altinstructions, \"a\"\n"    \
    ".long 661b - .\n"                          \  /* offset to instruction */
    ".long 663f - .\n"                          \  /* offset to alt text */
    ".hword " __stringify(feature) "\n"         \  /* feature number */
    ".byte 663f - 662f\n"                       \  /* alt instruction size */
    ".byte 661b - 660b\n"                       \  /* orig instruction size */
    ".popsection\n"                             \
    ".pushsection .altinstr_replacement, \"ax\"\n" \
    "662:\n"                                    \
    newinstr "\n"                               \  /* the alternative */
    "663:\n"                                    \
    ".popsection\n"
```

The original instruction (`msr tpidr_el1, %0`) is in the kernel's `.text` section.
The alternative (`msr tpidr_el2, %0`) is in `.altinstr_replacement`.

At boot:
1. `apply_boot_alternatives()` scans the `.altinstructions` table
2. For each entry, checks if the feature (`ARM64_HAS_VIRT_HOST_EXTN`) is present
3. If yes: copies the alternative instruction over the original in-place
4. CPU instruction cache is invalidated for the modified range

After patching (on VHE systems):
```asm
; What the CPU actually executes after VHE patching:
msr  tpidr_el2, x0    ; (the "msr tpidr_el1" instruction was overwritten)
```

---

## `__kern_my_cpu_offset()` — Reading the Register

```c
/* arch/arm64/include/asm/percpu.h:32 */
static inline unsigned long __kern_my_cpu_offset(void)
{
    unsigned long off;
    asm volatile(
        ALTERNATIVE("mrs %0, tpidr_el1",    /* default */
                    "mrs %0, tpidr_el2",    /* VHE replacement */
                    ARM64_HAS_VIRT_HOST_EXTN)
        : "=r" (off)
        : "Q" (*(unsigned long *)NULL)      /* stack hazard marker */
    );
    return off;
}
#define __my_cpu_offset  __kern_my_cpu_offset()
```

### Why `ALTERNATIVE()` Needs to Patch Both MSR and MRS

Both the **write** (`set_my_cpu_offset`) and the **read** (`__kern_my_cpu_offset`)
must use the same register. If one uses `tpidr_el1` and the other uses `tpidr_el2`,
the per-CPU offset would be lost:

```
Without VHE patching (both use tpidr_el1):
  set_my_cpu_offset → msr tpidr_el1, x0  ✓ writes to el1
  __kern_my_cpu_offset → mrs x0, tpidr_el1  ✓ reads from el1

With VHE, both patched to tpidr_el2:
  set_my_cpu_offset → msr tpidr_el2, x0  ✓ writes to el2
  __kern_my_cpu_offset → mrs x0, tpidr_el2  ✓ reads from el2
```

---

## `__hyp_my_cpu_offset()` — Hypervisor EL2 Access

```c
/* arch/arm64/include/asm/percpu.h:23 */
static inline unsigned long __hyp_my_cpu_offset(void)
{
    /*
     * No "Q" constraint needed here because:
     * - We're in hypervisor (EL2) code
     * - Preemption is always disabled at EL2
     * - No preemption means no risk of CPU migration
     * - Therefore no need for the memory ordering constraint
     */
    return read_sysreg(tpidr_el2);
    /* Expands to: mrs x0, tpidr_el2 */
}
```

This is used by KVM hypervisor code (in `.hyp.text` section) to access its own
per-CPU data. At EL2, `tpidr_el2` is always the correct register regardless of VHE.

---

## VHE Architecture Detail

**VHE = Virtualization Host Extensions (ARMv8.1-A)**

```
Without VHE (traditional virtualization):
  Normal execution: EL1 (kernel)
  VM entry: EL1 → EL2 (trampoline) → EL1 (guest)
  Register ownership:
    tpidr_el1: belongs to whatever code runs at EL1
    tpidr_el2: belongs to hypervisor (EL2)

With VHE (Linux KVM host runs at EL2):
  Normal execution: EL2 (as if EL1, but with EL2 registers)
  VM entry: EL2 (host) → EL1 (guest)
  Register ownership:
    tpidr_el1: GUEST's register (saved/restored on VM entry/exit)
    tpidr_el2: HOST kernel's register → used for per-CPU offset
```

Detection:
```c
/* ARM64_HAS_VIRT_HOST_EXTN is set if: */
/* 1. CPU supports ARMv8.1-A VHE feature (ID_AA64MMFR1_EL1.VH != 0) */
/* 2. Kernel is running at EL2 (from bootloader/UEFI handoff) */

/* Checked in arch/arm64/kernel/cpufeature.c */
```

---

## MSR / MRS Instruction Encoding

### `msr tpidr_el1, Xn` (write)

```
MSR <sysreg>, Xn
Encoding: 1101 0101 0001 <op1> <CRn> <CRm> <op2> 1 Rt

tpidr_el1 system register: op0=3, op1=0, CRn=c13, CRm=c0, op2=4
Binary: 1101 0101 0001 1000 1101 0000 1001 Rt
```

### `mrs Xd, tpidr_el1` (read)

```
MRS Xd, <sysreg>
Encoding: 1101 0101 0011 <op1> <CRn> <CRm> <op2> 1 Rt

Same sysreg encoding, read direction bit set.
```

### `tpidr_el2` encoding

```
tpidr_el2: op0=3, op1=4, CRn=c13, CRm=c0, op2=2
msr tpidr_el2, Xn
mrs Xd, tpidr_el2
```

---

## Complete Assembly Sequence for `this_cpu_read(var)` on ARM64

```asm
; Source: this_cpu_read(my_var)
; Compiled (non-VHE, no preemption needed due to context):

; Step 1: Read per-CPU offset from tpidr_el1
mrs    x8, tpidr_el1                    ; x8 = __per_cpu_offset[current_cpu]

; Step 2: Add compile-time offset of 'my_var' in the template
; (Compiler knows this at compile time as a constant)
add    x8, x8, #<adrp_page_offset>      ; page part of template address
add    x8, x8, #<page_relative_offset>  ; within-page offset of var

; Step 3: Load the variable
ldr    w0, [x8]                         ; 32-bit load (int var example)

; Total: 3 instructions (1 + 1 or 2 + 1)
; The add may be a single instruction if offset fits in 12-bit immediate
```

For VHE (after `apply_boot_alternatives` patches `mrs tpidr_el1 → mrs tpidr_el2`):
```asm
mrs    x8, tpidr_el2                    ; patched instruction
add    x8, x8, #<offset>
ldr    w0, [x8]
; Still 3 instructions — identical structure
```

---

## ARM64 vs ARM32 Summary

| Aspect | ARM32 | ARM64 |
|---|---|---|
| Register | TPIDRPRW (CP15 c13) | `tpidr_el1` (or `tpidr_el2`) |
| Write instruction | `mcr p15, 0, Rn, c13, c0, 4` | `msr tpidr_el1, Xn` |
| Read instruction | `mrc p15, 0, Rd, c13, c0, 4` | `mrs Xd, tpidr_el1` |
| Instruction set | Thumb/ARM (32-bit ISA) | AArch64 (64-bit ISA) |
| VHE variant | N/A | `tpidr_el2` |
| Patching mechanism | `.alt.smp.init` (UP only) | `ALTERNATIVE()` (VHE capable) |
| Patch timing | At kernel boot, for UP systems | At kernel boot, for VHE systems |
| "Q" hazard | Yes | Yes |
| User space readable? | No (PRW = kernel-only) | No (EL1/EL2 register) |
| Banked per core? | Yes | Yes |
| Context switch save? | No | No |

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Default register on ARM64? | `tpidr_el1` |
| Register when VHE is active? | `tpidr_el2` |
| What is VHE? | ARMv8.1 feature where Linux KVM host runs at EL2 |
| Why switch to tpidr_el2 on VHE? | tpidr_el1 becomes the guest's register |
| What macro handles the switch? | `ALTERNATIVE("msr tpidr_el1", "msr tpidr_el2", ARM64_HAS_VIRT_HOST_EXTN)` |
| When is patching applied? | `apply_boot_alternatives()` in `setup_arch()` |
| What is `__hyp_my_cpu_offset`? | EL2 hypervisor code reads tpidr_el2 directly (no "Q" needed) |
| Why "Q" constraint on ARM64? | Prevents mrs from being hoisted above preceding memory writes |
| Is banked per core? | Yes — each core has its own tpidr_el1/tpidr_el2 |
