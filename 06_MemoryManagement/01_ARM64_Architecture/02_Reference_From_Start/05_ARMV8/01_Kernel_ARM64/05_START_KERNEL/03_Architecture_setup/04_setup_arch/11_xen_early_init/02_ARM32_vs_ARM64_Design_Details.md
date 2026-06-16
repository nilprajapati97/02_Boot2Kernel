# xen_early_init() — ARM32 vs ARM64 Design Details

## 1. Function Presence and Location

| | ARM32 | ARM64 |
|--|-------|-------|
| Function | `xen_early_init()` | `xen_early_init()` |
| Source file | `arch/arm/xen/enlighten.c` | `arch/arm64/xen/enlighten.c` |
| Called from | `setup_arch()` in `arch/arm/kernel/setup.c` | `setup_arch()` in `arch/arm64/kernel/setup.c` |
| CONFIG guard | `CONFIG_XEN` | `CONFIG_XEN` |

Both ARM32 and ARM64 call `xen_early_init()`. The function name and purpose are identical; the implementation differences reflect architectural changes.

---

## 2. ARM32 Xen Specifics

### Exception Level Model (ARMv7-A with VE)

```
EL0  — User space (applications)
EL1  — Linux kernel (supervisor)
EL2  — Xen hypervisor (hyp mode)
```

ARM32 calls the levels differently:
- **SVC mode** = EL1 equivalent (supervisor, where Linux runs)
- **HYP mode** = EL2 equivalent (hypervisor, where Xen runs)
- **USR mode** = EL0 equivalent

On ARM32, HYP mode is optional (requires `CONFIG_ARM_VIRT_EXT`). Systems without VE cannot run Xen.

### ARM32 Xen Detection

```c
/* arch/arm/xen/enlighten.c */
void __init xen_early_init(void)
{
    xen_domain_type = XEN_NATIVE;

    if (!acpi_disabled)
        return;

    /* FDT-based detection only */
    if (of_find_compatible_node(NULL, NULL, "xen,xen")) {
        if (!xen_guest_init())
            pr_warn("Xen found in DT but init failed\n");
    }
}
```

ARM32 Xen guests are always FDT-based (no ACPI ARM32 Xen support).

### ARM32 Hypercall Mechanism

ARM32 uses `hvc #0` instruction to enter Xen EL2. Arguments are passed in registers:
```
r12 = hypercall number
r0-r5 = arguments
return in r0
```

---

## 3. ARM64 Xen Specifics

### Exception Level Model (ARMv8)

```
EL0  — User space (AArch64 / AArch32 compat)
EL1  — Linux kernel (AArch64)
EL2  — Xen hypervisor (AArch64)
EL3  — Secure monitor (ATF — ARM Trusted Firmware)
```

ARM64 always has EL2 available (mandatory in ARMv8 profile A). EL3 (Secure World) is also defined — providing a full security hierarchy.

### ARM64 Xen Detection

```c
/* arch/arm64/xen/enlighten.c */
void __init xen_early_init(void)
{
    xen_domain_type = XEN_NATIVE;

    /* ARM64 Xen supports both FDT (embedded) and ACPI (server) */
    if (acpi_disabled) {
        /* FDT path */
        if (of_find_compatible_node(NULL, NULL, "xen,xen")) {
            if (!xen_guest_init())
                pr_warn("Xen initialization failed\n");
        }
    } else {
        /* ACPI path: check for Xen signature in RSDP or MADT */
        /* ARM64 servers may boot Xen with ACPI (not FDT) */
    }
}
```

ARM64 adds the ACPI detection path for server deployments.

### ARM64 Hypercall Mechanism

ARM64 uses `hvc #0` with 64-bit registers:
```
x16 = hypercall number (or in some calling conventions, x0)
x0-x5 = arguments
return in x0
```

---

## 4. Stage-2 MMU: ARM32 vs ARM64

Xen's core mechanism is the stage-2 MMU (Second Stage Address Translation):

| Feature | ARM32 (LPAE + VE) | ARM64 (ARMv8) |
|---------|-------------------|---------------|
| Stage-1 VA → IPA | Guest (EL1) page tables | Guest (EL1) page tables |
| Stage-2 IPA → PA | Xen (EL2) LPAE page tables | Xen (EL2) ARMv8 page tables |
| VTTBR | VTTBR (64-bit, 40-bit PA) | VTTBR_EL2 (64-bit, 48/52-bit PA) |
| IPA size | Up to 40 bits | Up to 48 bits (52 with LPA) |
| Page sizes | 4KB, 2MB, 1GB | 4KB, 16KB, 64KB, 2MB, 1GB |
| VTCR | VTCR (controls IPA size) | VTCR_EL2 |

ARM64's stage-2 is more capable: supports 4 levels of page tables (vs 3 for ARM32 LPAE), larger IPA, and more granule sizes.

---

## 5. Grant Table Memory Map Comparison

### ARM32 Grant Table Setup

Grant table is mapped via `early_ioremap()` using the fixmap. On ARM32:
- Fixmap has limited slots (NR_FIX_BTMAPS entries)
- Grant table size is typically 4-16 pages
- Mapped at a fixed virtual address in the fixmap window

### ARM64 Grant Table Setup

ARM64 has a much larger fixmap region and larger virtual address space, so grant table mapping is less constrained. ARM64 also supports larger grant tables for high-memory guests.

---

## 6. Xen PV Clock (Stolen Time)

Xen provides a **paravirtual clock** — a shared memory page that Xen writes with the current time. The guest reads this instead of querying hardware timers, which would be expensive (timer registers may be emulated). 

| Feature | ARM32 Xen | ARM64 Xen |
|---------|-----------|-----------|
| PV clock | `xen_setup_timer()` — maps shared info page | Same, via `HYPERVISOR_vcpu_op` |
| Hardware timer | ARM arch timer (if not virtualized) | ARM arch timer (virtual timer at EL1) |
| Virtual arch timer | EL2 traps EL1 CNTV access | Transparent via EL2 trap |
| Stolen time | `HYPERVISOR_vcpu_op(VCPUOP_register_runstate_memory_area)` | Same |

On ARM64, the virtual counter (`CNTV_CTval_EL0`) is directly accessible to EL1 without trapping to EL2. Xen injects a virtual timer interrupt when needed. This is more efficient than ARM32 where every timer register access could trap.

---

## 7. KASLR Interaction (ARM64 Only)

On ARM64 with KASLR, Xen must know the kernel load address to properly set up stage-2 page tables:

```
ARM64 KASLR boot sequence:
1. Xen starts the guest
2. Xen provides randomized physical load address to the guest via FDT
3. ARM64 head.S reads load address and computes KASLR offset
4. parse_early_param() processes nokaslr if present
5. xen_early_init() detects Xen
6. Xen already set up stage-2 for the randomized address
```

ARM32 does not have KASLR, so there's no interaction.

---

## 8. Comparison Table: xen_early_init() ARM32 vs ARM64

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| Function present | Yes (`arch/arm/xen/enlighten.c`) | Yes (`arch/arm64/xen/enlighten.c`) |
| HYP/EL2 requirement | Optional (requires VE extension) | Mandatory (always present) |
| Hypercall instruction | `hvc #0` (32-bit registers) | `hvc #0` (64-bit registers) |
| Detection method | FDT only (`xen,xen` node) | FDT + ACPI |
| Stage-2 MMU | LPAE (40-bit PA, 3-level) | ARMv8 (48-bit PA, 4-level) |
| KASLR interaction | None (no KASLR) | Xen provides randomized base |
| PV clock | `xen_setup_timer()` | Same mechanism |
| ACPI Xen support | No | Yes (server platforms) |
| Secure Monitor (EL3) | Optional | Supported (ATF at EL3) |
| `xen_domain_type` set | `XEN_HVM_DOMAIN` (ARM has no PV mode) | `XEN_HVM_DOMAIN` |
