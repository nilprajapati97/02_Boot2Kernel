# ARM64 `tpidr_el1` / `tpidr_el2` System Register Deep Dive

## Register Identity

| Property | `tpidr_el1` | `tpidr_el2` |
|---|---|---|
| Full name | Thread Pointer / ID Register, EL1 | Thread Pointer / ID Register, EL2 |
| System register encoding | `3, 0, c13, c0, 1` | `3, 4, c13, c0, 2` |
| Access from EL0 | No | No |
| Access from EL1 | Read/Write | No (EL1 cannot access) |
| Access from EL2 | Read/Write | Read/Write |
| Width | 64-bit | 64-bit |
| Linux per-CPU use | Non-VHE kernel | VHE kernel (EL2 host) |

---

## The Full `tpidr_*` Register Family (ARM64)

| Register | System Reg Encoding | Access Privilege | Purpose |
|---|---|---|---|
| `tpidr_el0` | `3, 3, c13, c0, 2` | EL0 R/W, EL1 R/W | User-space TLS (glibc `set_tid_address`) |
| `tpidrro_el0` | `3, 3, c13, c0, 3` | EL0 R-only, EL1 R/W | User-space read-only TLS (alternate TLS ABI) |
| `tpidr_el1` | `3, 0, c13, c0, 1` | EL1 R/W | Kernel per-CPU offset (non-VHE) |
| `tpidr_el2` | `3, 4, c13, c0, 2` | EL2 R/W | Hypervisor per-CPU offset (VHE) |
| `tpidr_el3` | `3, 6, c13, c0, 2` | EL3 R/W | Secure monitor / firmware use |

---

## System Register Encoding Format

ARM64 system registers are accessed via `MRS`/`MSR` using a 5-field encoding:

```
MRS Xd, <sysreg>
MSR <sysreg>, Xn

Encoding: op0 : op1 : CRn : CRm : op2

tpidr_el1:  op0=3, op1=0, CRn=c13, CRm=c0, op2=1
tpidr_el2:  op0=3, op1=4, CRn=c13, CRm=c0, op2=2

ARM64 MRS instruction bit encoding:
  [31:20] = 1101 0101 0011  (fixed for MRS)
  [19:19] = 1               (L bit: 1=read, 0=write for MSR)
  [18:14] = op1
  [13:10] = CRn
  [9:5]   = CRm : op2
  [4:0]   = Rt  (destination register)
```

---

## Write/Read Instructions

### `tpidr_el1`

```asm
; Write:
msr   tpidr_el1, x0      ; x0 = per-CPU offset to store

; Read:
mrs   x0, tpidr_el1      ; x0 = per-CPU offset for this CPU
```

### `tpidr_el2`

```asm
; Write (requires EL2 privilege):
msr   tpidr_el2, x0

; Read (requires EL2 privilege):
mrs   x0, tpidr_el2
```

---

## Exception Level Isolation

ARM64's exception level model isolates registers strictly:

```
EL3 (Secure Monitor / TF-A):
  Can access: tpidr_el3, tpidr_el2, tpidr_el1, tpidr_el0

EL2 (Hypervisor):
  Can access: tpidr_el2, tpidr_el1 (via EL1 simulation), tpidr_el0
  Cannot access: tpidr_el3 (secure)

EL1 (Kernel):
  Can access: tpidr_el1, tpidr_el0
  Cannot access: tpidr_el2, tpidr_el3

EL0 (User space):
  Can access: tpidr_el0 (read/write), tpidrro_el0 (read-only)
  Cannot access: tpidr_el1, tpidr_el2, tpidr_el3
```

---

## VHE: Why Linux KVM Uses `tpidr_el2`

### Non-VHE Architecture (ARMv8.0-A)

```
CPU running Linux (EL1):
  tpidr_el1 = Linux per-CPU offset (belongs to Linux)
  tpidr_el2 = (unused or KVM hypervisor use)

VM entry (EL2 trampoline):
  Save tpidr_el1 = Linux per-CPU offset to memory
  Load tpidr_el1 = Guest OS per-CPU offset  ← guest takes over el1 register
  Enter EL1 guest

VM exit:
  Save tpidr_el1 = Guest per-CPU offset
  Restore tpidr_el1 = Linux per-CPU offset  ← Linux gets its register back
```

**Problem:** Every VM entry/exit requires save/restore of `tpidr_el1`.

### VHE Architecture (ARMv8.1-A with VHE)

```
CPU running Linux KVM host (at EL2 with VHE):
  tpidr_el2 = Linux per-CPU offset (host kernel uses el2 register)
  tpidr_el1 = Reserved for VM guests

VM entry (direct EL2 → EL1):
  Save tpidr_el1 = (might be unused or previous guest)
  Load tpidr_el1 = Guest per-CPU offset
  Enter EL1 guest

VM exit:
  tpidr_el2 still holds Linux per-CPU offset!  ← no save/restore needed
```

**Benefit:** Linux's per-CPU pointer (`tpidr_el2`) survives VM entry/exit unchanged.
The guest's `tpidr_el1` is the only register that needs save/restore.

---

## VHE Detection and Register Selection in Linux

```c
/* arch/arm64/include/asm/percpu.h:15 */
static inline void set_my_cpu_offset(unsigned long off)
{
    asm volatile(
        ALTERNATIVE("msr tpidr_el1, %0",   /* EL1 kernel (non-VHE) */
                    "msr tpidr_el2, %0",   /* EL2 kernel (VHE)    */
                    ARM64_HAS_VIRT_HOST_EXTN)
        :: "r" (off) : "memory"
    );
}
```

```c
/* Feature detection at boot: */
/* arch/arm64/kernel/cpufeature.c */
static bool has_vhe(const struct arm64_cpu_capabilities *entry, int scope)
{
    if (!is_kernel_in_hyp_mode())
        return false;
    /* ARMv8.1+ and EL2 available: VHE is active */
    return cpus_have_const_cap(ARM64_HAS_VIRT_HOST_EXTN);
}
```

---

## KVM Guest/Host Register Partition

When Linux KVM runs with VHE (`is_kernel_in_hyp_mode() == true`):

```
Register        | Host Linux (EL2) | Guest (EL1)
----------------|------------------|------------------
tpidr_el2       | Per-CPU offset   | NOT VISIBLE to guest
tpidr_el1       | Guest TLS        | Guest per-CPU (KVM saves/restores)
tpidr_el0       | User TLS         | Guest user TLS (KVM saves/restores)
tpidrro_el0     | User ro TLS      | Guest user ro TLS
```

KVM `vcpu_put()`/`vcpu_load()` saves and restores guest register state including
`tpidr_el1` (and `tpidr_el0`, `tpidrro_el0`) on every VM entry/exit.

---

## ARM64 vs ARM32 Register Comparison

| Aspect | ARM32 TPIDRPRW | ARM64 tpidr_el1 | ARM64 tpidr_el2 |
|---|---|---|---|
| Architecture | ARMv6+ | ARMv8.0-A | ARMv8.1-A (VHE) |
| Width | 32-bit | 64-bit | 64-bit |
| Privilege | PL1 (kernel) | EL1 | EL2 |
| User readable | No | No | No |
| Instruction (write) | `mcr p15,0,Rn,c13,c0,4` | `msr tpidr_el1, Xn` | `msr tpidr_el2, Xn` |
| Instruction (read) | `mrc p15,0,Rd,c13,c0,4` | `mrs Xd, tpidr_el1` | `mrs Xd, tpidr_el2` |
| Per-core? | Yes (banked) | Yes (banked) | Yes (banked) |
| Patching mechanism | `.alt.smp.init` | `ALTERNATIVE()` | `ALTERNATIVE()` |
| Analog of | — | TPIDRPRW | TPIDRPRW (when VHE) |

---

## ISB Requirement After Writing

On ARM64, an `ISB` (Instruction Synchronization Barrier) is NOT needed immediately
after `msr tpidr_el1, x0` for subsequent `mrs x0, tpidr_el1` reads on the SAME CPU.
The architecture guarantees that system register writes are immediately visible to
subsequent reads on the same processor.

However, when setting up per-CPU registers before enabling the MMU or before cache
maintenance operations, ordering may still be needed:

```asm
msr   tpidr_el1, x0   ; write per-CPU offset
isb                    ; only needed if subsequent code relies on instruction
                       ; refetching (e.g., after patching code with alternatives)
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Default register for Linux per-CPU on ARM64? | `tpidr_el1` |
| Register when VHE is active? | `tpidr_el2` |
| What is VHE? | ARMv8.1-A feature: KVM host runs at EL2, not EL1 |
| Why tpidr_el2 for VHE? | tpidr_el1 must be free for guest OS use during VM entry/exit |
| Can EL1 kernel read tpidr_el2? | No — requires EL2 privilege |
| User-space TLS register? | `tpidr_el0` (not `tpidr_el1`) |
| System register encoding for tpidr_el1? | op0=3, op1=0, CRn=c13, CRm=c0, op2=1 |
| Is ISB needed after writing tpidr_el1? | Not for same-CPU reads; may be needed for cross-CPU ordering |
| What does KVM save/restore? | tpidr_el1 (guest) on VM entry/exit; tpidr_el2 stays as host per-CPU |
| Banked per-core? | Yes — each physical CPU core has its own register set |
