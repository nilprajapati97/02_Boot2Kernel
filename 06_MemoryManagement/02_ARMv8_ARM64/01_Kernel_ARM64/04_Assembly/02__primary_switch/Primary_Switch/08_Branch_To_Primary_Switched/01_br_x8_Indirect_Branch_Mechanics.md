# `br x8` — ARM64 Indirect Branch Mechanics

**Instruction:** `br x8` — Branch to Register  
**Context:** Final instruction in `__primary_switch` that transfers execution to kernel VA space

---

## 0. Instruction Encoding

```
br x8

ARM64 encoding:
[31:25] = 1101011 0  (unconditional branch to register, opc=00)
[24:21] = 0000       (Z=0, not branch and link; A=0, M=0)
[20:16] = 11111      (op2 = 0b11111)
[15:10] = 000000     (op3)
[9:5]   = 01000      (Rn = 8, register x8)
[4:0]   = 00000      (op4 = 0)

Full encoding: 0xD61F0100
```

Semantics:
```
PC = x8    (unconditionally jump to address in x8)
```

No link register modification. No condition codes. No return capability.

---

## 1. Direct (`b`) vs Indirect (`br`) Branch

| Property | `b target` | `br x8` |
|---|---|---|
| Target encoding | 26-bit signed offset in instruction | Register x8 |
| Target range | PC ± 128MB | Full 64-bit address space |
| BTI required | No (static branch) | Yes, if BTI enabled (`BTI j`/`BTI jc`) |
| Branch prediction | BHB (Branch History Buffer) | IBP (Indirect Branch Predictor) |
| Spectre risk | Spectre-BHB | Spectre v2 (IBPB/IBRS mitigations) |
| Target visible in binary | Yes (offset) | No (only register at runtime) |

---

## 2. ARM64 Branch Predictor for Indirect Branches

Modern ARM64 processors (Cortex-A78, Neoverse N2, etc.) implement an
**Indirect Branch Predictor (IBP)** that predicts the target of `br`, `blr`,
and `ret` instructions.

The IBP is typically a **BTB (Branch Target Buffer)**:
```
BTB entry:
  key:   PC of the br instruction
  value: predicted target VA

Lookup:
  On instruction fetch of br x8 at PC=0x4000_1238:
  1. Look up PC=0x4000_1238 in BTB
  2. If hit: speculatively fetch instructions at BTB.target
  3. Execute br x8 → actual target = x8
  4. If BTB.target == x8: no misprediction penalty
  5. If BTB.target != x8: pipeline flush + refetch at x8
```

In early boot, the BTB is empty (cold). The `br x8` will suffer a BTB miss:
- CPU stalls waiting for x8 to be computed and the branch to resolve
- No speculative execution of `__primary_switched` ahead of time
- Penalty: ~15–20 cycles (branch misprediction cost on high-end Cortex cores)

This is negligible during boot (happens once).

---

## 3. The BTI (Branch Target Identification) Interaction

With `CONFIG_ARM64_BTI_KERNEL=y`, the CPU enforces that `br x8` can only land
at a `BTI` landing pad instruction.

**`BTI` instruction encodings:**

```asm
hint    #32   // BTI - basic BTI (no indirect branch allowed to land here)
hint    #34   // BTI c - allow indirect call (blr)
hint    #36   // BTI j - allow indirect jump (br)
hint    #38   // BTI jc - allow both indirect jump and call
```

`__primary_switched` must start with `BTI jc` or `BTI j` since it's reached via
`br x8` (indirect jump, not call):

```asm
SYM_FUNC_START_LOCAL(__primary_switched)
    // With BTI_KERNEL, SYM_FUNC_START expands to include:
    hint    #36    // BTI j
    ...
```

**CPU enforcement mechanism:**

The CPU maintains a **BTYPE** (Branch Type) state in PSTATE:
```
PSTATE.BTYPE:
  00: No BTI state (normal execution)
  01: Indirect call target expected (blr)
  10: Indirect jump target expected (br)  ← State after br x8
  11: Both indirect call and jump expected
```

When `br x8` executes, PSTATE.BTYPE is set to 0b10.
The next instruction at the target must be a `BTI j` or `BTI jc`.
If it is not (or is any other instruction), a `BranchTarget` exception fires.

---

## 4. CPU State at `br x8` Execution

Hardware state immediately before `br x8`:

```
PC:           0x00000000_4000_1238   (PA in identity map)
x8:           0xFFFF_8000_xxxx_xxxx  (KASLR-adjusted VA of __primary_switched)
SCTLR_EL1:   MMU_ON | C=1 | ...     (MMU and caches enabled)
TTBR0_EL1:   PA of idmap_pg_dir     (identity map still active)
TTBR1_EL1:   PA of swapper_pg_dir   (kernel map active)
SP:           VA of early_init_stack top (just set in __primary_switch)
x29 (fp):    0 (cleared by __primary_switch)
x20:          BOOT_CPU_MODE value
x21:          PA of FDT
DAIF:         F=1,I=1,A=1,D=1 (all exceptions masked)
```

After `br x8`:
```
PC:           0xFFFF_8000_xxxx_xxxx  (now in kernel VA space!)
x8:           0xFFFF_8000_xxxx_xxxx  (unchanged, holds previous target VA)
...all other registers unchanged...
```

The SINGLE change is the PC. Everything else is preserved. The CPU's instruction
fetch unit now asks: "Fetch instruction at VA `0xFFFF_8000_xxxx_xxxx`."

---

## 5. The First VA Instruction Fetch

The CPU performs a TLB lookup for `VA = 0xFFFF_8000_xxxx_xxxx`:

```
Step 1: TLB check
   VA[47] = 1 → TTBR1_EL1 selected = PA(swapper_pg_dir)
   TLB miss (cold cache in early boot — no previous TTBR1 accesses)

Step 2: Page Table Walk (PTW)
   Walk swapper_pg_dir:
   PGD[VA[47:39]] → PUD entry
   PUD[VA[38:30]] → PMD entry  (or PUD block)
   PMD[VA[29:21]] → 2MB block descriptor  ← Hit! (set by __pi_early_map_kernel)
   PA = block_base_PA + VA[20:0]

Step 3: Permissions check
   Block descriptor has AF=1 (Access Flag), PXN=0 (Privileged Execute Not)
   Kernel text is mapped with execute permission → fetch allowed

Step 4: Cache check
   Cache is enabled (C=1 in SCTLR_EL1 now)
   Cache miss (cold) → fetch from DRAM
   Instruction bytes cached for next fetch

Step 5: Instruction returned
   First instruction of __primary_switched is fetched and decoded
```

---

## 6. Spectre v2 in Context

Spectre v2 exploits the IBP to cause the CPU to speculatively execute code at
an attacker-controlled address when an indirect branch executes.

**At boot time (`br x8` in early boot), Spectre v2 is irrelevant because:**
1. No user processes are running
2. No cross-process attacker can have trained the IBP
3. The IBPB (Indirect Branch Predictor Barrier) — a specialized ISB-like
   instruction — would be used on context switches if needed

**When does Spectre v2 matter for `br` instructions?**
- User→kernel transitions (syscall `svc`)
- After returning from a user process, the IBP could contain attacker-trained
  entries. A subsequent kernel `br` instruction might speculatively execute
  at an attacker-chosen address.
- Mitigated by `IBPB` at context switch (or `IBRS` for always-on protection)

---

## 7. Return Stack Buffer (RSB) and the Missing `ret`

`br x8` does NOT push a return address onto the hardware **Return Stack Buffer
(RSB)** (ARM64: RAS — Return Address Stack). The RSB tracks `bl`/`blr`
instructions and predicts `ret` targets.

Since `br x8` (not `blr x8`) is used:
- No RSB entry is created
- `__primary_switched` cannot be `ret`'d out of via this path
- `start_kernel` is called with `bl start_kernel` from within `__primary_switched`
  → that DOES push an RSB entry

The use of `br` (not `blr`) correctly signals: this is a one-way jump, no return.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The BR Xn instruction in ARMv8-A is an unconditional indirect branch: it sets PC = Xn. Unlike B (immediate) or BL (branch-with-link), BR does not use the PC-relative encoding and can reach any 64-bit address. The CPU's branch predictor can predict indirect branches using the indirect branch predictor (IBP) or indirect branch target buffer. After the MMU enable, the very first BR to a kernel VA address triggers a TLB miss (if the kernel VA is not yet in the TLB), which initiates a page-table walk using TTBR1_EL1. The walk succeeds if the kernel page tables have been correctly set up by __pi_early_map_kernel.

### Kernel Perspective (Linux ARM64)
In __primary_switch, the sequence:
  ldr x8, =primary_switched   // load VA of target function
  br  x8                       // jump to kernel VA
This is the point-of-no-return: the CPU jumps from the identity-mapped PA region to the kernel VA. After BR x8, x30 (link register) still holds the identity-mapped return address from the original caller, but it is never used again. The CPU is now fully in kernel VA space. Spectre-v2 mitigations require that indirect branches be protected: on patched kernels, BR x8 may be replaced by a retpoline or enhanced IBRS sequence.

### Memory Perspective (ARMv8 Memory Model)
The BR x8 instruction that transitions the CPU from PA to VA is a memory system event: it changes the instruction fetch address from an identity-mapped PA (low VA) to a high kernel VA. At this point, TTBR0_EL1 still points to the identity map (it will be cleared later when init_task's mm is set to NULL). TTBR1_EL1 covers the target VA. The TLB may not yet have an entry for the target VA; the first access causes a hardware page-table walk. If the page-table walk finds a valid entry (Normal, Execute permission, correct PA), the CPU populates the TLB and continues. This walk is the first use of the kernel page tables built by __pi_early_map_kernel.