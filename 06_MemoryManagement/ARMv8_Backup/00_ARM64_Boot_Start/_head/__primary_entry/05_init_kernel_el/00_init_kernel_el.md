# `init_kernel_el` — ARM64 Exception Level Initialization

**File**: `arch/arm64/kernel/head.S`
**Called from**: `primary_entry` (after `__pi_create_init_idmap` + cache maintenance)
**Also called from**: `secondary_holding_pen`, `secondary_entry`

---

## Purpose

When the bootloader hands control to the kernel, it may be running at either
**EL1** (normal world) or **EL2** (hypervisor level). The kernel does not know
in advance which level it starts at.

`init_kernel_el` does three things:
1. Detects current exception level (EL1 or EL2)
2. Programs a **clean, kernel-defined CPU state** into SCTLR/PSTATE registers
3. Uses `eret` to return back to the caller — atomically committing the new
   CPU state on exit

---

## Call Site in `primary_entry`

```asm
1:  mov  x0, x19          // pass MMU-on/off state (from record_mmu_state)
    bl   init_kernel_el   // configure exception level → w0=cpu_boot_mode
    mov  x20, x0          // save boot mode for entire boot path
```

- `x19` (MMU state) is forwarded as the argument — EL2 path uses it to
  decide whether to clean HYP code before disabling the MMU
- `x20` carries the returned boot mode all the way through to `__primary_switched`

---

## The `eret` Return Trick

Normal functions return with `ret` (branch to `lr`). Here `eret` is used
because it does **two things atomically**:

| Field       | Normal `ret`        | `eret` here                        |
|-------------|---------------------|------------------------------------|
| PC          | `lr`                | `ELR_ELx` (pre-loaded with `lr`)  |
| PSTATE      | unchanged           | Loaded from `SPSR_ELx`            |
| Exception level | unchanged       | Can change (EL2 → EL1)           |

This is the **only way** to simultaneously:
- Return to the original caller address
- Set all PSTATE flags (mask interrupts, select stack pointer)
- Drop exception level from EL2 to EL1 (EL2 path only)

A plain `ret` would return but leave PSTATE in whatever state the bootloader
left it — which is unsafe and unpredictable.

---

## Call Flow

```
primary_entry
│
├── mov x0, x19               (x0 = MMU was ON/OFF)
│
└── bl init_kernel_el
        │
        ├── Read CurrentEL
        │
        ├── [EL1 path] ──────────────────────────────────────────────────┐
        │   │                                                            │
        │   ├── msr SCTLR_EL1 = INIT_SCTLR_EL1_MMU_OFF                   │
        │   │     Configures known-safe CPU control bits:                │
        │   │       M  = 0   MMU off                                     │
        │   │       C  = 0   D-cache off                                 │
        │   │       I  = 1   I-cache on                                  │
        │   │       SA = 1   Stack alignment check on                    │
        │   │       EE = 0   Little-endian data accesses                 │
        │   │     isb: flush pipeline, new SCTLR takes effect            │
        │   │                                                            │
        │   ├── msr SPSR_EL1 = INIT_PSTATE_EL1                           │
        │   │     Sets processor state to be restored on eret:           │
        │   │       DAIF = 1111  (all interrupts/aborts masked)          │
        │   │       SP   = SP_EL1 (EL1 dedicated stack pointer)          │
        │   │       EL   = 1     (remain at EL1 after eret)              │
        │   │                                                            │
        │   ├── msr ELR_EL1 = lr                                         │
        │   │     Load return address into exception link register       │
        │   │     eret will branch here (= instruction after bl)         │
        │   │                                                            │
        │   ├── mov w0, #BOOT_CPU_MODE_EL1  (0xe11)                      │
        │   │                                                            │
        │   └── eret ───────────────────────────────────────────────────►│
        │           Atomically:                                          │
        │             PC     ← ELR_EL1  (original lr, return addr)       │
        │             PSTATE ← SPSR_EL1 (INIT_PSTATE_EL1)                │
        │           Returns to primary_entry, w0 = BOOT_CPU_MODE_EL1     │
        │◄───────────────────────────────────────────────────────────────┘
        │
        └── [EL2 path] ───────────────────────────────────────────────┐
        │                                                             │
        ├── msr ELR_EL2 = lr                                          │
        │     Save return address FIRST — before any bl clobbers lr   │
        │                                                             │
        ├── [If MMU was ON at EL2] (x0 != 0)                          │
        │     dcache_clean_poc(__hyp_idmap_text_start, __hyp_text_end)│
        │     → Clean HYP code region to Point-of-Coherency           │
        │     → Required before turning MMU off: ensures HYP code     │
        │       is visible to instruction fetch after MMU disabled    │
        │     msr SCTLR_EL2 = INIT_SCTLR_EL2_MMU_OFF                  │
        │     isb → MMU now off at EL2                                │
        │                                                             │
        ├── init_el2_hcr (HCR_HOST_NVHE_FLAGS | HCR_ATA)              │
        │     Configure Hypervisor Control Register:                  │
        │       RW  = 1   EL1 executes in AArch64 (not AArch32)       │
        │       HCD = 1   HVC instruction disabled from EL1           │
        │       ATA = 1   Allocation Tag Access allowed               │
        │       E2H = ?   Set if CPU advertises VHE-only operation    │
        │                                                             │
        ├── init_el2_state                                            │
        │     Initialize all EL2 system registers to safe defaults:   │
        │       CPTR_EL2   (coprocessor/FP trap control)              │
        │       CNTHCTL_EL2 (counter/timer access control)            │
        │       MDCR_EL2   (debug control)                            │
        │       + others per el2_setup.h                              │
        │                                                             │
        ├── msr VBAR_EL2 = __hyp_stub_vectors                         │
        │     Install minimal hypervisor exception vectors            │
        │     Handles HVC calls from EL1 during early boot            │
        │     (e.g., finalise_el2 later uses HVC to configure KVM)    │
        │                                                             │
        ├── Check HCR_EL2.E2H bit (VHE mode detection)                │
        │   │                                                         │
        │   ├── [VHE mode: E2H = 1] ─────────────────────────────     │
        │   │     msr SCTLR_EL12 = INIT_SCTLR_EL1_MMU_OFF             │
        │   │     x2 = BOOT_CPU_FLAG_E2H  (bit 32)                    │
        │   │     VHE: EL1 and EL2 share register bank                │
        │   │     SCTLR_EL12 is the VHE alias for SCTLR_EL1           │
        │   │                                                         │
        │   └── [nVHE mode: E2H = 0] ────────────────────────────     │
        │         msr SCTLR_EL1 = INIT_SCTLR_EL1_MMU_OFF              │
        │         x2 = 0                                              │
        │         Standard separate EL1/EL2 register banks            │
        │                                                             │
        ├── msr SPSR_EL2 = INIT_PSTATE_EL1                            │
        │     On eret: drop to EL1, all interrupts masked             │
        │                                                             │
        ├── mov w0, #BOOT_CPU_MODE_EL2  (0xe12)                       │
        │   orr x0, x0, x2   (OR in BOOT_CPU_FLAG_E2H if VHE)         │
        │                                                             │
        └── eret ───────────────────────────────────────────────────► │
                Atomically:                                           │
                  PC     ← ELR_EL2  (original lr = return addr)       │
                  PSTATE ← SPSR_EL2 (INIT_PSTATE_EL1 → now at EL1)    │
                Returns to primary_entry, w0 = BOOT_CPU_MODE_EL2      │
                CPU has dropped from EL2 → EL1                        │
        ◄─────────────────────────────────────────────────────────────┘
│
└── mov x20, x0   (save boot mode for rest of boot)
```

---

## Return Values

| Value                               | Meaning                                  |
|-------------------------------------|------------------------------------------|
| `BOOT_CPU_MODE_EL1` = `0xe11`      | Bootloader started kernel at EL1         |
| `BOOT_CPU_MODE_EL2` = `0xe12`      | Bootloader started kernel at EL2 (nVHE) |
| `BOOT_CPU_MODE_EL2 \| BOOT_CPU_FLAG_E2H` | EL2 with VHE (bit 32 set)         |

> Note: The `BOOT_CPU_FLAG_E2H` flag (bit 32) is **NOT** stored in
> `__boot_cpu_mode[]` — it is only used transiently during boot by
> `finalise_el2` to decide VHE vs nVHE KVM mode.

---

## Where `x20` (Boot Mode) Is Used Downstream

```
x20 = BOOT_CPU_MODE_EL1 / BOOT_CPU_MODE_EL2 [| BOOT_CPU_FLAG_E2H]
│
├── __primary_switch
│     mov x0, x20 → passed to __pi_early_map_kernel
│
├── __primary_switched
│     bl set_cpu_boot_mode_flag  → writes to __boot_cpu_mode[]
│     bl finalise_el2            → decides VHE/nVHE for KVM
│
└── start_kernel → setup_arch
      is_hyp_mode_available()    → queries __boot_cpu_mode[]
      KVM init path              → determines hypervisor capability
```

---

## VHE vs nVHE — Why It Matters

| Mode  | HCR_EL2.E2H | Description                                      |
|-------|-------------|--------------------------------------------------|
| nVHE  | 0           | Classic split: EL2 = hypervisor, EL1 = OS       |
| VHE   | 1           | EL2 runs host OS directly; EL1/EL2 share regs   |

VHE (Virtualization Host Extensions, ARMv8.1+) eliminates the EL2→EL1
context switch overhead when the host kernel runs at EL2. The kernel
detects this here and passes the flag forward so `finalise_el2` can
make the final decision.

---

## Key Design Decisions

| Decision | Reason |
|----------|--------|
| `eret` instead of `ret` | Only instruction that atomically changes PC + PSTATE + EL |
| Save `lr` into `ELR_EL2` before any `bl` | Any branch-with-link would clobber `lr`; ELR is sysreg, safe |
| Clean HYP code before MMU disable (EL2 path) | If HYP code was cached, turning off MMU without cleaning risks stale I-cache fetches |
| `isb` after SCTLR write | Pipeline must be flushed before new MMU/cache state takes effect |
| All interrupts masked in INIT_PSTATE | Boot path must not be interrupted; no exception handlers installed yet |
