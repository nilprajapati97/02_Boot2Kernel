# `__primary_switched` — First Code in Virtual Kernel Address Space

**File**: `arch/arm64/kernel/head.S`
**Section**: `.text` (standard kernel text — NOT `.idmap.text`)
**Called from**: `__primary_switch` via `br x8` (indirect branch to virtual address)
**Entry state**: MMU ON, virtual address space fully active, `swapper_pg_dir` in TTBR1
**Exits to**: `start_kernel()` — no return

---

## Purpose

`__primary_switched` is the **first function that executes at the kernel's
permanent virtual address**. Every line of code before this ran from an
identity-mapped physical address. This function performs the final
bootstrapping that bridges the raw hardware state left by `__primary_switch`
to the C runtime expected by `start_kernel`:

1. Binds the hardware CPU to `init_task` (the first `task_struct`)
2. Installs the exception vector table into `VBAR_EL1`
3. Anchors the unwinder: marks the bottom of the boot stack
4. Saves the FDT pointer globally as `__fdt_pointer`
5. Computes and stores `kimage_voffset` (virt − phys delta for the entire image)
6. Commits the boot CPU mode to `__boot_cpu_mode[]`
7. Optionally initialises KASAN shadow memory
8. Promotes to VHE (Virtual Host Extension) if the CPU supports it
9. Calls `start_kernel` — the kernel never returns here

---

## Register State at Entry

| Register | Value | Source |
|----------|-------|--------|
| `x0`     | `__pa(KERNEL_START)` (physical base of kernel image) | loaded by `__primary_switch` before `br x8` |
| `x20`    | CPU boot mode (`BOOT_CPU_MODE_EL1` or `EL2` + flags) | preserved from `init_kernel_el` |
| `x21`    | FDT physical address | preserved from `preserve_boot_args` |
| `sp`     | `early_init_stack` (virtual, post-MMU) | reset by `__primary_switch` |
| `x29`    | `xzr` | reset by `__primary_switch` |

---

## Complete Call Flow

```
__primary_switch
  br x8  →  __primary_switched          [First instruction at virtual address]
│
├── [1] init_cpu_task (macro)            Bind CPU to init_task
│     adr_l  x4, init_task              Load virtual addr of init_task
│     │
│     ├── msr sp_el0, x4                SP_EL0 = init_task ptr
│     │     ARM64 uses SP_EL0 as the    "current" pointer at EL1.
│     │     current_thread_info() and   current macro both read SP_EL0.
│     │
│     ├── ldr tmp1, [x4, #TSK_STACK]    Load init_task.stack pointer
│     │     add sp, tmp1, #THREAD_SIZE  SP = top of init_task's kernel stack
│     │     sub sp, sp, #PT_REGS_SIZE   Reserve pt_regs space at stack top
│     │     Stack layout after this:
│     │       [ pt_regs region ]  ← sp (PT_REGS_SIZE bytes reserved)
│     │       ...
│     │       [ bottom of stack ] ← init_task.stack (low address)
│     │
│     ├── stp xzr, xzr, [sp, #S_STACKFRAME]
│     │     Write {fp=0, lr=0} at the STACKFRAME slot in pt_regs.
│     │     Zero FP+LR signals the unwinder: "frame record present,
│     │     but no caller — stop here."
│     │
│     ├── mov tmp1, #FRAME_META_TYPE_FINAL
│     │     str tmp1, [sp, #S_STACKFRAME_TYPE]
│     │     Mark this frame as FINAL (type=1).
│     │     The unwinder terminates successfully at FRAME_META_TYPE_FINAL.
│     │     Without this, a panic backtrace could walk off the bottom of
│     │     the boot stack into unmapped memory.
│     │
│     ├── add x29, sp, #S_STACKFRAME    x29 (FP) points at this frame record
│     │     This is the canonical bottom frame for all call chains that
│     │     trace back through start_kernel → __primary_switched.
│     │
│     ├── scs_load_current              Load Shadow Call Stack base for init_task
│     │     (if CONFIG_SHADOW_CALL_STACK)
│     │     Sets x18 = init_task's SCS base
│     │
│     └── set_this_cpu_offset           Install per-CPU offset for CPU0
│           adr_l tmp1, __per_cpu_offset
│           ldr   w_tmp2, [x4, #TSK_TI_CPU]   w_tmp2 = init_task.cpu = 0
│           ldr   tmp1, [tmp1, tmp2, lsl #3]   per_cpu_offset[0]
│           msr   tpidr_el1, tmp1              install into TPIDR_EL1
│           Per-CPU variables now accessible via this_cpu_ptr() for CPU0.
│
├── [2] Install exception vector table
│     adr_l  x8, vectors              Virtual address of vectors[] table
│     msr    vbar_el1, x8             Write VBAR_EL1
│     isb                             Instruction sync: VBAR takes effect now
│     │
│     Before this instruction, ANY exception or interrupt would vector to
│     whatever VBAR_EL1 contained from __cpu_setup (undefined/garbage for
│     EL1 boot path). After isb, all exceptions route to the proper kernel
│     vector table. This must happen BEFORE any C code that could fault.
│
├── [3] Establish calling frame (save LR for stack unwinder)
│     stp x29, x30, [sp, #-16]!      Push {FP, LR} — standard prologue
│     mov x29, sp                    Update FP to point at new frame
│     │
│     x30 = return address from `bl __primary_switched` (never used,
│     since bl start_kernel() at the end never returns).
│     But saving LR is required so panic() backtrace shows the frame.
│
├── [4] Save FDT pointer globally
│     str_l  x21, __fdt_pointer, x5  __fdt_pointer = x21 (FDT phys addr)
│     │
│     `__fdt_pointer` is declared as:
│       extern phys_addr_t __fdt_pointer;  (arch/arm64/kernel/setup.c)
│     It is the canonical location queried by setup_arch() → unflatten_device_tree().
│     x21 has held the FDT physical address since preserve_boot_args().
│
├── [5] Compute and store kimage_voffset
│     adrp   x4, _text               x4 = virtual address of _text
│     sub    x4, x4, x0              x4 = VA(_text) - PA(KERNEL_START)
│     str_l  x4, kimage_voffset, x5  kimage_voffset = VA - PA delta
│     │
│     x0 = __pa(KERNEL_START) = physical address of kernel start (from br x8 setup).
│     _text is defined in the linker script at the kernel virtual base.
│     Because KASLR may have shifted the virtual load address, this delta
│     (kimage_voffset) is computed dynamically, not at link time.
│     │
│     Used by:
│       __kimg_to_phys(addr) = addr - kimage_voffset    (virt → phys)
│       __phys_to_kimg(x)    = x    + kimage_voffset    (phys → virt)
│     │
│     kimage_voffset is `__ro_after_init` — written once here, then
│     permanently read-only for the lifetime of the kernel.
│
├── [6] Commit CPU boot mode to __boot_cpu_mode[]
│     mov  x0, x20
│     bl   set_cpu_boot_mode_flag
│         │
│         ├── adr_l x1, __boot_cpu_mode    &__boot_cpu_mode[0]
│         ├── cmp   w0, #BOOT_CPU_MODE_EL2
│         ├── b.ne  1f                       if EL1 → write to [0]
│         │   add   x1, x1, #4              if EL2 → write to [1]
│         └── str   w0, [x1]
│         │
│         __boot_cpu_mode[2] is a u32 pair:
│           [0] = mode written by primary CPU  (this call)
│           [1] = mode written by secondary CPUs (set_cpu_boot_mode_flag
│                 also called from __secondary_switched)
│         │
│         Both must equal BOOT_CPU_MODE_EL2 for is_hyp_mode_available()
│         to return true. A mismatch detected by is_hyp_mode_mismatched()
│         causes a kernel warning and disables KVM.
│
├── [7] KASAN early init  [CONFIG_KASAN_GENERIC || CONFIG_KASAN_SW_TAGS]
│     bl   kasan_early_init
│     │
│     Maps the KASAN shadow region covering the current kernel VA space.
│     KASAN requires its shadow to be mapped before any instrumented C code
│     runs — which is everything in start_kernel().
│     Without this call, the first instrumented memory access would fault
│     on an unmapped shadow address.
│
├── [8] Finalise EL2 / promote to VHE
│     mov  x0, x20
│     bl   finalise_el2              arch/arm64/kernel/hyp-stub.S
│         │
│         ├── cmp w0, #BOOT_CPU_MODE_EL2   Did we boot at EL2?
│         │   b.ne 1f → ret               No: nothing to do (EL1 boot)
│         │
│         ├── mrs x0, CurrentEL            Are we still at EL1?
│         │   b.ne 1f → ret               No: already at EL2 (shouldn't happen)
│         │
│         ├── mov x0, #HVC_FINALISE_EL2
│         │   hvc #0                      Trap into hyp-stub
│         │       │
│         │       └── __finalise_el2 (in hyp-stub):
│         │               │
│         │               ├── finalise_el2_state
│         │               │     Configure remaining EL2 control registers
│         │               │
│         │               ├── Check sctlr_el2.M == 0 (MMU must be off at EL2)
│         │               │     If on → return HVC_STUB_ERR (nVHE path)
│         │               │
│         │               ├── Check CPU supports VHE (ID_AA64MMFR1.VH)
│         │               │     If not → return HVC_STUB_ERR (nVHE path)
│         │               │
│         │               ├── Check arm64_sw_feature_override HVHE bit
│         │               │     If force-nVHE → return HVC_STUB_ERR
│         │               │
│         │               ├── [VHE path]
│         │               │     msr HCR_EL2, HCR_HOST_VHE_FLAGS
│         │               │     Transfer sp_el1, tpidr_el1 → EL2
│         │               │     Copy CPACR, VBAR EL12 → EL1 aliases
│         │               │     Transfer TCR/TTBR0/TTBR1/MAIR/TCR2/PIR
│         │               │       (MM state from EL1 context → EL2 context)
│         │               │     Patch spsr_el1 to return to EL2h mode
│         │               │     b enter_vhe
│         │               │         │
│         │               │         ├── tlbi vmalle1; dsb nsh; isb
│         │               │         ├── set_sctlr_el1 (SCTLR_EL12)
│         │               │         │     Enable MMU at EL2 using EL1's tables
│         │               │         ├── Disable EL1 S1 MMU (SCTLR_EL12 = MMU_OFF)
│         │               │         └── eret → returns to EL2 (VHE mode!)
│         │               │
│         │               └── [nVHE path] → eret returns HVC_STUB_ERR → ret
│         │
│         After this call, the kernel may be running at EL2 (VHE) or EL1 (nVHE).
│         With VHE: EL2 and EL1 share the same translation regime.
│         KVM requires VHE for the "host" hypervisor model (pKVM/nVHE uses
│         the stub only for init then hands off to KVM).
│
├── [9] Restore frame pointer (epilogue pairing for step [3])
│     ldp  x29, x30, [sp], #16       Pop {FP, LR}
│
└── [10] Enter the C kernel
      bl   start_kernel              arch/arm64/kernel/... → init/main.c
      ASM_BUG()                      Unreachable — start_kernel() never returns
```

---

## Deep Dive: `init_cpu_task` and the Task Stack Layout

### Why `SP_EL0` holds `current`

ARM64 Linux uses `SP_EL0` as the "current task pointer" register.
At EL1, `SP_EL0` is normally the user-space stack pointer.
But the kernel uses it as a dedicated per-CPU "current" register because:

- It is saved/restored automatically on exception entry/exit via `kernel_entry`/`kernel_exit`
- It is never used by EL1 code directly (EL1 uses `SP_EL1` or `SP`)
- Avoids consuming a general-purpose callee-saved register for `current`

The `current` macro in C compiles to `get_current()` which reads `SP_EL0`.

### Stack Layout After `init_cpu_task`

```
init_task.stack (low address)
│
│   [ THREAD_SIZE = 16KB (if PAGE_SIZE=4KB) ]
│
├── bottom guard page (if VMAP_STACK)
│
├── actual kernel stack (grows downward)
│   ...
│   sp_after_init_cpu_task
│     ├── [S_STACKFRAME+8] lr = 0            \  FINAL frame record
│     ├── [S_STACKFRAME]   fp = 0            /  {fp=0,lr=0} = bottom sentinel
│     ├── [S_STACKFRAME_TYPE] type = 1  (FRAME_META_TYPE_FINAL)
│     └── pt_regs base (S_X0)
│
└── sp = pt_regs base  (PT_REGS_SIZE bytes reserved at top)
```

The `{fp=0, lr=0}` sentinel at `S_STACKFRAME` is the terminator that the
ARM64 unwinder (`arch/arm64/kernel/stacktrace.c`) recognises as
"successfully reached the bottom — stop unwinding". Without it, a panic
backtrace would attempt to follow the zero FP, detect it as a corrupt frame,
and print a spurious "corrupted stack" error.

---

## Deep Dive: `kimage_voffset` — The Kernel VA/PA Bridge

```
Physical address space:
  0x0000_0008_0000_0000  ← typical physical load address (64GiB)
      _text (phys) = PA(KERNEL_START)

Virtual address space:
  0xFFFF_8000_1000_0000  ← KIMAGE_VADDR + KASLR offset
      _text (virt)

kimage_voffset = _text(virt) - _text(phys)
             = 0xFFFF_8000_1000_0000 - 0x0000_0008_0000_0000
             = 0xFFFF_7FF8_1000_0000  (an arbitrary example)
```

This delta is used everywhere the kernel needs to convert between the virtual
address it knows (from symbols, `adrp`, etc.) and the physical address needed
for hardware registers (TTBR, DMA, kexec, crash dump):

```c
// arch/arm64/include/asm/memory.h
#define __kimg_to_phys(addr)   ((addr) - kimage_voffset)
#define __phys_to_kimg(x)      ((unsigned long)((x) + kimage_voffset))
```

`kimage_voffset` is declared `__ro_after_init`, meaning the page table
protection is upgraded to read-only after `mark_rodata_ro()` runs during
`start_kernel`. This prevents any kernel or module code from accidentally
(or maliciously) altering the offset after boot.

---

## Deep Dive: `set_cpu_boot_mode_flag` — The EL Consensus Array

```
__boot_cpu_mode[2]:

  [0]  = written by primary CPU  (this function, __primary_switched)
  [1]  = written by secondary CPUs (from __secondary_switched)

Both must match for is_hyp_mode_available() to be true.
```

The rationale for a two-element array is the **bootloader conformance check**:
the ARM GIC and boot protocol require all CPUs to enter the kernel in the
same exception level. If CPU0 boots at EL2 but CPU1 boots at EL1 (a
non-conformant bootloader), `is_hyp_mode_mismatched()` returns true and the
kernel:

1. Prints a warning: `"KASLR disabled due to lack of seed"`
2. Disables KVM: `kvm_arm_init()` checks `is_hyp_mode_available()`
3. Prevents any EL2 access from secondary CPUs

The flag set here (primary CPU writing `[0]`) is compared against secondary
CPU writes (to `[1]`) after each secondary CPU comes online in
`secondary_start_kernel()`.

---

## Deep Dive: `finalise_el2` and the VHE Transition

### Why promote to VHE here (not earlier)?

VHE (Virtual Host Extension, ARMv8.1+) allows the host OS to run at EL2
instead of EL1. Benefits:

| Aspect | nVHE (EL1) | VHE (EL2) |
|--------|------------|-----------|
| KVM guest entry cost | Need EL1→EL2 switch | Already at EL2 |
| Page table | Separate EL2 tables | Shares EL1 tables |
| System registers | Different EL1/EL2 names | EL2 aliases EL1 regs |
| Linux kernel impact | Normal | Kernel runs at EL2 |

The promotion cannot happen in `__cpu_setup` or `__enable_mmu` because:
- VHE transition requires copying the entire MM state (TCR, TTBR0, TTBR1, MAIR)
  from EL1 registers to their EL2 counterparts
- This requires the kernel page tables (`swapper_pg_dir`) to already be built
  and active in TTBR1 — which only happens in `__pi_early_map_kernel`
- The hyp-stub's `enter_vhe` re-enables the MMU at EL2 using the same
  page tables that are now live for EL1 execution

After `finalise_el2` returns on VHE hardware, the CPU is at EL2.
All `mrs/msr` accesses to EL1 system registers transparently access
the EL2 equivalents (e.g., `sctlr_el1` → `sctlr_el2`). The kernel source
code is unchanged — the hardware aliasing is invisible to C code.

### nVHE path (no VHE capable CPU, or HVHE override disabled)

`finalise_el2` returns normally via `ret` without any `hvc`. The CPU stays
at EL1. KVM uses nVHE mode (separate EL2 world) for virtualisation.

---

## Callee-Saved Register Lifecycle: End of Boot Path

```
Register  Set by                   Last used in              Freed by
─────────────────────────────────────────────────────────────────────────────
x19       record_mmu_state()       init_kernel_el()          ← never freed,
           (MMU state flag)                                    overwritten by
                                                               start_kernel()

x20       init_kernel_el()         set_cpu_boot_mode_flag()  ← this function
           (CPU boot mode)          finalise_el2()

x21       preserve_boot_args()     str_l __fdt_pointer       ← this function
           (FDT phys addr)          (step 4 above)
```

After `__primary_switched` calls `start_kernel`, the boot-variable protocol
is complete. `x20` and `x21` have been consumed. The only remaining consumer
of boot state is `__fdt_pointer` and `kimage_voffset`, both now in globally
accessible memory.

---

## Key Design Decisions

| Decision | Reason |
|----------|--------|
| VBAR_EL1 installed before any C code | Any fault in `init_cpu_task` or later would corrupt the CPU without valid exception vectors — no safe recovery |
| `init_cpu_task` uses `SP_EL0` as `current` pointer | Avoids consuming a callee-saved GPR; the register is naturally saved/restored on every exception entry/exit |
| FINAL frame record at `pt_regs.stackframe` | Gives the unwinder a clean termination point; prevents spurious "corrupted stack" panic messages |
| `kimage_voffset` is `__ro_after_init` | Written once here, then write-protected by `mark_rodata_ro()`; prevents in-kernel or module corruption of the VA/PA bridge |
| `__boot_cpu_mode[2]` two-element array | Enables a post-boot consistency check: primary writes `[0]`, secondaries write `[1]`; mismatch → KVM disabled |
| `finalise_el2` called after virtual mappings are live | VHE transition copies TCR/TTBR/MAIR from EL1 to EL2 — requires swapper_pg_dir to already be active |
| `kasan_early_init` before `start_kernel` | All C code in `start_kernel` is KASAN-instrumented; shadow must be mapped before the first instrumented access |
| `ASM_BUG()` after `bl start_kernel` | Documents and enforces the contract that `start_kernel` never returns; generates a `BRK` instruction to catch hypothetical future regressions |
