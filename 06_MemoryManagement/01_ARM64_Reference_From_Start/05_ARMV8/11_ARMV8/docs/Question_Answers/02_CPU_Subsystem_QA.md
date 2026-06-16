# CPU Subsystem — Questions & Answers

---

## Q1. [L1] Describe the ARMv8 AArch64 pipeline stages. What is the typical pipeline depth of a Cortex-A class core?

**Answer:**

```
A modern ARM core doesn't use a simple textbook 5-stage pipeline.
Real designs are deep, superscalar, and out-of-order:

Simple textbook pipeline (understanding the concept):
  ┌────────┬────────┬────────┬────────┬────────┐
  │ Fetch  │ Decode │Execute │ Memory │ Write  │
  │  (IF)  │  (ID)  │  (EX)  │  (MEM) │ Back   │
  └────────┴────────┴────────┴────────┴────────┘

Real Cortex-A76/A78 pipeline (~13 stages):
  ┌─────────────────────────────────────────────────────────────┐
  │ Front-end (In-Order):                                       │
  │  F1 → F2 → F3 → F4    (4 stages: Fetch + Branch Predict)  │
  │  D1 → D2               (2 stages: Decode, up to 4-wide)   │
  │  Rn                    (1 stage: Rename / Register Alloc)  │
  │  Dp                    (1 stage: Dispatch to issue queues) │
  ├─────────────────────────────────────────────────────────────┤
  │ Back-end (Out-of-Order):                                    │
  │  Issue → Execute → Write-back (variable depth per unit)    │
  │  • Integer ALU: 1 cycle                                    │
  │  • Integer MUL: 3 cycles                                   │
  │  • FP/NEON:     2-4 cycles                                 │
  │  • Load:        4 cycles (L1 hit)                          │
  │  • Branch:      1 cycle (misprediction penalty ~11 cycles) │
  ├─────────────────────────────────────────────────────────────┤
  │ Commit (In-Order):                                          │
  │  Retire → Commit (2 stages: reorder buffer drain)          │
  └─────────────────────────────────────────────────────────────┘

Pipeline depths by core:
  Cortex-A53 (in-order):     8 stages, 2-wide
  Cortex-A55 (in-order):     8 stages, 2-wide (improved A53)
  Cortex-A72 (OoO):         ~15 stages, 3-wide decode
  Cortex-A76 (OoO):         ~13 stages, 4-wide decode
  Cortex-A78 (OoO):         ~12 stages, 4-wide decode
  Cortex-X1/X2 (OoO):       ~11 stages, 5-wide decode
  Cortex-X4 (OoO):          ~10 stages, 6-wide decode
  Neoverse N2/V2 (server):  ~11 stages, 5/6-wide

Key insight: deeper pipeline = higher clock frequency but longer
branch misprediction penalty. Wider decode = more IPC but more
power. ARM balances these for mobile (power) vs server (perf).
```

---

## Q2. [L2] What is out-of-order (OoO) execution? Explain the Tomasulo-style algorithm ARM uses and why it matters.

**Answer:**

```
Out-of-order execution allows the CPU to execute instructions in
a different order than they appear in the program, as long as
DATA DEPENDENCIES are respected.

Why?
  In-order problem:
    LDR X0, [X1]           // Cache miss — 100+ cycles
    ADD X2, X0, #1          // Depends on X0 — STALLED
    MUL X3, X4, X5          // Independent — but stalled too!
    
  With OoO:
    LDR X0, [X1]           // Cache miss — issued to memory
    MUL X3, X4, X5          // Independent → EXECUTE NOW
    ADD X5, X6, X7          // Independent → EXECUTE NOW
    ADD X2, X0, #1          // Still waiting for X0
    // When LDR completes, ADD X2 executes

Tomasulo-style OoO in ARM cores:

  1. FETCH: Fetch instructions from I-cache (up to 4-6 per cycle)
  
  2. DECODE: Decode into µ-ops (micro-operations)
     Most A64 instructions → 1 µ-op
     Some complex ones → 2-3 µ-ops (e.g., LDP → 2 loads)
  
  3. RENAME: Map architectural registers (X0-X30) to physical
     registers (128-256 physical registers)
     Purpose: eliminate false dependencies (WAR, WAW)
     
     Example of false dependency eliminated:
       ADD X0, X1, X2    // Writes X0
       MUL X0, X3, X4    // Also writes X0 (WAW on X0!)
       
     After rename:
       ADD P47, P12, P33  // Writes P47 (was X0)
       MUL P48, P22, P19  // Writes P48 (also X0) — no conflict!
  
  4. DISPATCH: Place µ-ops into issue queues (reservation stations)
     Separate queues for: Integer, FP/SIMD, Load, Store, Branch
  
  5. ISSUE: When ALL operands are ready, issue µ-op to execute
     Can issue 6-8 µ-ops per cycle (superscalar)
     Oldest-ready-first policy (fairness + forward progress)
  
  6. EXECUTE: Functional units compute results
     Results broadcast on Common Data Bus (CDB)
     Waiting µ-ops in issue queue "wake up" when their operand
     appears on CDB
  
  7. REORDER BUFFER (ROB): Tracks original program order
     Each µ-op gets an ROB entry with: instruction, result,
     "completed" flag, exception info
     ROB size: ~128-256 entries (Cortex-A76: ~128, X3: ~256+)
  
  8. COMMIT/RETIRE: When oldest ROB entry is complete:
     • Write result to architectural register file
     • If exception: flush everything after this instruction
     • Retire up to 4-8 instructions per cycle
     • This is IN-ORDER (maintains precise exceptions)

  ┌─────────────────────────────────────────────────────────┐
  │ Fetch → Decode → Rename → Dispatch                     │
  │          │                    │                         │
  │          │              ┌─────┴──────────┐             │
  │          │              │  Issue Queues   │             │
  │          │              │ ┌──┐ ┌──┐ ┌──┐ │             │
  │          │              │ │IQ│ │FQ│ │LQ│ │             │
  │          │              │ └──┘ └──┘ └──┘ │             │
  │          │              └──┬─────┬────┬──┘             │
  │          │              ┌──▼──┐┌─▼─┐┌─▼──┐            │
  │          │              │ ALU ││FPU ││LSU │            │
  │          │              └──┬──┘└─┬─┘└─┬──┘            │
  │          │                 └─────┴────┘                │
  │          │                      │                      │
  │          │              ┌───────▼────────┐             │
  │          │              │  Reorder Buffer │             │
  │          │              └───────┬────────┘             │
  │          │                      │ Commit               │
  │          │              ┌───────▼────────┐             │
  │          │              │ Arch Reg File   │             │
  │          │              └────────────────┘             │
  └─────────────────────────────────────────────────────────┘
```

---

## Q3. [L2] What is branch prediction? Explain TAGE, BTB, and RAS in the context of ARMv8.

**Answer:**

```
Branch prediction is critical because:
  • A branch misprediction flushes 11+ pipeline stages
  • ~20% of instructions are branches
  • 99%+ accuracy needed for good performance

Three components work together:

1. DIRECTION PREDICTOR (TAGE):
   Predicts: taken or not-taken for conditional branches
   
   TAGE = TAgged GEometric history length predictor
   Uses MULTIPLE history tables with geometrically increasing
   history lengths:
   
   ┌────────────────────────────────────────────────────────┐
   │ Table │ History Length │ Entries │ Purpose              │
   ├───────┼───────────────┼─────────┼──────────────────────┤
   │ T0    │ 0 (bimodal)  │ 4096   │ Default prediction    │
   │ T1    │ 8 branches   │ 2048   │ Short patterns        │
   │ T2    │ 16 branches  │ 2048   │ Medium patterns       │
   │ T3    │ 32 branches  │ 1024   │ Longer correlations   │
   │ T4    │ 64 branches  │ 1024   │ Long loop patterns    │
   │ T5    │ 128 branches │ 512    │ Complex behaviors     │
   └───────┴───────────────┴─────────┴──────────────────────┘
   
   How: hash PC + branch history → index into each table
   Pick the LONGEST matching table (most specific prediction)
   
   Cortex-A76+: achieves >97% accuracy on typical workloads

2. BRANCH TARGET BUFFER (BTB):
   Predicts: WHERE does the branch go? (target address)
   
   ┌──────────────────────────────────────────────────────┐
   │ BTB Entry:                                           │
   │   [Tag (PC hash)] [Target Address] [Type] [Valid]   │
   │                                                      │
   │ Types: direct branch, indirect branch, call, return │
   │                                                      │
   │ Sizes: 512-8192 entries (varies by core)            │
   │ Cortex-A78: 8K entry BTB                            │
   │ Cortex-X2: 12K+ entry BTB                           │
   └──────────────────────────────────────────────────────┘
   
   For INDIRECT branches (BR X16 — function pointers, vtables):
   Uses an indirect target predictor backed by a pattern table
   (harder to predict — many possible targets)

3. RETURN ADDRESS STACK (RAS):
   Predicts: return address for RET instruction
   
   ┌──────────────────────────────────────────────────────┐
   │ BL handler    → PUSH return address onto RAS        │
   │ ...           │                                      │
   │ RET           → POP from RAS (predicted target)     │
   │                                                      │
   │ Depth: typically 16-32 entries                      │
   │ Overflow: wraps around (deep recursion = mispredicts)│
   │ Very accurate: ~100% unless stack overflow or        │
   │ longjmp/exception breaks the call/return pairing    │
   └──────────────────────────────────────────────────────┘

Misprediction recovery:
  When a branch resolves differently from prediction:
  1. Flush all instructions AFTER the branch in pipeline & ROB
  2. Restore register rename map to checkpoint
  3. Redirect fetch to correct target
  4. Lost cycles: pipeline depth (11-15 cycles)
  5. This is why prediction accuracy is so important
```

---

## Q4. [L3] What is speculative execution? How can it lead to vulnerabilities like Spectre?

**Answer:**

```
Speculative execution = CPU executes instructions BEFORE knowing
if they should execute (after a branch prediction, before the
branch resolves).

Normal case (prediction correct):
  CMP X0, #10
  B.GT skip           // Predicted: not-taken
  LDR X1, [X2]        // Speculatively executed
  ADD X3, X1, #1      // Speculatively executed
skip:
  // Branch resolved: prediction was RIGHT
  // Speculative results are committed → no wasted work

Mispredict case (prediction wrong):
  CMP X0, #10
  B.GT skip           // Predicted: not-taken
  LDR X1, [X2]        // Speculatively executed → SQUASHED
  ADD X3, X1, #1      // Speculatively executed → SQUASHED
skip:
  // Branch resolved: prediction was WRONG (X0 > 10)
  // Pipeline flush, discard speculative results
  // Architectural state UNCHANGED — correct!
  // BUT... microarchitectural state IS changed:
  //   - Cache now has data from [X2] (speculative load)
  //   - TLB has translation for X2's address
  //   - These are SIDE CHANNELS

Spectre v1 (Bounds Check Bypass):
  // Imagine kernel code:
  if (index < array_size) {          // Branch predicted taken
      value = array[index];           // Speculative: reads OOB!
      temp = probe[value * 4096];    // Encodes secret in cache
  }
  
  // Attacker:
  // 1. Train branch predictor with valid index (in bounds)
  // 2. Supply out-of-bounds index
  // 3. CPU speculatively reads array[evil_index] → secret byte
  // 4. secret byte → probe[secret * 4096] cached
  // 5. Measure timing: which probe[] line is fast?
  //    That reveals the secret byte!
  // 6. Even though speculative results are squashed,
  //    the CACHE STATE remains (covert channel)

ARM mitigations:
  • CSDB barrier (Consumption of Speculative Data Barrier)
    Prevents speculative load results from being used in
    subsequent data-dependent operations
    
  • SSBS (Speculative Store Bypass Safe, ARMv8.5)
    PSTATE.SSBS: controls speculative store bypass per-thread
    
  • CSV2 (Cache Speculation Variant 2)
    Branch prediction indexed by VMID/ASID
    → one VM can't poison another's predictor
    
  • CSV3: guarantees speculative reads don't cross privilege
    
  • Linux: uses CSDB in array_index_nospec():
    AND index, index, mask    // mask = 0 if OOB
    CSDB                      // barrier
    LDR value, [array, index] // safe: index masked to 0 if OOB
```

---

## Q5. [L2] Explain register renaming and the physical register file. How many physical registers does a typical ARM core have?

**Answer:**

```
AArch64 has 31 architectural registers (X0-X30), but a modern
OoO core has 128-300+ PHYSICAL registers.

Why? To eliminate FALSE DEPENDENCIES:

  Write-After-Write (WAW):
    ADD X5, X0, X1     // Writes X5
    MUL X5, X2, X3     // Also writes X5 — must wait? NO!
    
  Write-After-Read (WAR):
    ADD X0, X5, X1     // Reads X5
    SUB X5, X2, X3     // Writes X5 — must wait for ADD to read? NO!
    
  These are NOT true data dependencies — they just reuse the
  same architectural register name.

Register Rename Table (RAT):
  Maps architectural → physical registers
  
  Before rename:
    ADD X5, X0, X1     (reads X0, X1; writes X5)
    MUL X5, X2, X3     (writes X5 — same arch reg!)
    ADD X6, X5, X4     (reads X5 — which X5?)
    
  After rename:
    ADD P42, P10, P11  (X5 mapped to P42)
    MUL P43, P12, P13  (X5 remapped to P43 — different phys reg!)
    ADD P44, P43, P14  (reads P43 = latest X5 = MUL result) ✓
    
  Now ADD and MUL can execute in parallel — no false dependency!

Physical register file sizes (approximate):
  Cortex-A72:  ~128 integer + ~128 FP/SIMD physical registers
  Cortex-A76:  ~160 integer + ~128 FP/SIMD
  Cortex-A78:  ~160 integer + ~128 FP/SIMD
  Cortex-X2:   ~192 integer + ~192 FP/SIMD
  Cortex-X4:   ~256 integer + ~256 FP/SIMD
  Neoverse V2: ~256 integer + ~256 FP/SIMD

  More physical registers → more in-flight instructions →
  better ability to hide memory latency →
  but more power (bigger register files = more transistors + wires)

Free list:
  • Unused physical registers sit in a "free list"
  • On rename: allocate from free list, update RAT
  • On retire/commit: old mapping's physical register returns to free list
  • If free list empty: stall decode (can't rename)
```

---

## Q6. [L1] What is the A64 instruction set? How does it differ from A32/T32?

**Answer:**

```
A64 is the AArch64 instruction set — completely new, not backward
compatible with ARMv7's A32/T32.

Key characteristics:
┌────────────────────────────────────────────────────────────────┐
│ Property      │ A64 (AArch64)    │ A32 (ARM)   │ T32 (Thumb) │
├───────────────┼──────────────────┼─────────────┼─────────────┤
│ Encoding      │ Fixed 32-bit     │ Fixed 32-bit│ 16/32-bit   │
│ Registers     │ 31 GPRs (X0-X30)│ 16 (R0-R15) │ 16 (R0-R15) │
│ PC access     │ Not a GPR        │ R15 = PC    │ R15 = PC    │
│ Condition     │ Per-instruction  │ Conditional │ IT block    │
│   execution   │ (CSEL, CSINC)   │ on every instr│             │
│ Load/Store    │ LDP/STP (pair)  │ LDM/STM     │ LDM/STM     │
│ Address modes │ Fewer, simpler  │ Many, complex│ Subset      │
│ Multiply      │ MUL, MADD, SMULL│ MLA, UMULL  │ Similar     │
│ Division      │ SDIV, UDIV (HW) │ Optional HW │ Optional    │
│ Bit manip     │ BFM, UBFX, etc. │ Similar      │ Similar     │
│ Immediates    │ MOVZ/MOVK chain │ Barrel shift │ Limited     │
│ System        │ MSR/MRS         │ MCR/MRC p15 │ MCR/MRC     │
│ Alignment     │ SP must be 16B  │ 4-byte stack│ 4-byte      │
└───────────────┴──────────────────┴─────────────┴─────────────┘

Major A64 improvements:
  1. More registers: 31 vs 16 → fewer spills to stack
  2. No conditional execution on every instruction (reduces
     decode complexity, enables wider decode)
  3. PC not a GPR → simpler pipeline, no "trick" instructions
  4. LDP/STP for register pairs → efficient prologue/epilogue:
       STP X29, X30, [SP, #-16]!  // Push FP and LR
       LDP X29, X30, [SP], #16    // Pop FP and LR
  5. CSEL/CSINC replaces conditional move:
       CMP X0, #0
       CSEL X1, X2, X3, EQ   // X1 = (X0==0) ? X2 : X3
  6. MADD replaces MLA with explicit accumulator:
       MADD X0, X1, X2, X3   // X0 = X1*X2 + X3
```

---

## Q7. [L3] Explain the Load/Store Unit (LSU) in detail. How does it handle store buffers, load forwarding, and memory ordering?

**Answer:**

```
The LSU is one of the most complex parts of an OoO ARM core.
It must handle: load/store execution, address translation,
cache access, store buffering, and memory ordering — all while
maintaining correctness and maximizing throughput.

LSU Architecture:
  ┌───────────────────────────────────────────────────────────┐
  │                    Load/Store Unit                        │
  │  ┌─────────────┐    ┌──────────────┐                    │
  │  │ Load Queue   │    │ Store Queue   │                    │
  │  │ (LQ)         │    │ (SQ)          │                    │
  │  │ ~64 entries  │    │ ~48 entries   │                    │
  │  └──────┬───────┘    └───────┬───────┘                    │
  │         │                    │                            │
  │  ┌──────▼────────────────────▼───────┐                   │
  │  │        Address Generation          │                   │
  │  │   Base + Offset / Index calc      │                   │
  │  └──────────────┬────────────────────┘                   │
  │                 │                                         │
  │  ┌──────────────▼────────────────────┐                   │
  │  │         TLB Lookup (µTLB)         │                   │
  │  │  VA → PA translation              │                   │
  │  │  Check permissions, cacheability  │                   │
  │  └──────────────┬────────────────────┘                   │
  │                 │                                         │
  │  ┌──────────────▼─────────────────────┐                  │
  │  │         L1 Data Cache Access        │                  │
  │  │  Tag match + data read/write       │                  │
  │  └────────────────────────────────────┘                  │
  └───────────────────────────────────────────────────────────┘

Store Buffer:
  Stores are NOT written to cache immediately.
  They go through a STORE BUFFER:
  
  1. Store executes: address + data go into Store Queue (SQ)
  2. Store becomes "committed" when it retires from ROB
  3. Only committed stores write to L1 D-cache (in-order!)
  4. Uncommitted stores may be squashed (misprediction)
  
  Why? Ordering: ARM memory model requires stores to become
  visible to other cores in program order for certain
  access types. The store buffer ensures this.

Store-to-Load Forwarding:
  When a load's address matches a pending store in the SQ:
  
  STR X0, [X1]         // In store buffer, not yet in cache
  ... 
  LDR X2, [X1]         // Same address! → forward from SQ
  
  Rules:
  • Full overlap: forward directly (1 cycle penalty vs cache)
  • Partial overlap: may stall or forward + merge
  • No match: go to L1 cache
  
  This is CRITICAL for performance:
  Function prologue stores X29/X30, epilogue loads them back.
  Without forwarding: ~4 cycle L1 latency.
  With forwarding: ~1 cycle (data hasn't left the core).

Memory Ordering (ARM is weakly ordered):
  The LSU must enforce ordering rules:
  
  • Normal loads can reorder with other loads (unless LDAR)
  • Normal stores can reorder with other stores (unless STLR)  
  • Loads can pass earlier stores (if different address)
  • LDAR (Load-Acquire): all subsequent memory ops after this
  • STLR (Store-Release): all previous memory ops before this
  • DMB: Data Memory Barrier — enforces order at the LSU
  
  The LSU tracks potential ordering violations:
  If a speculative load executed before an earlier store,
  and the store address turns out to match → FLUSH the load
  and all dependent instructions (memory ordering violation).
```

---

## Q8. [L2] What is the difference between in-order and out-of-order cores? Give examples and use cases.

**Answer:**

```
┌────────────────────┬──────────────────────┬─────────────────────┐
│ Property           │ In-Order             │ Out-of-Order (OoO)  │
├────────────────────┼──────────────────────┼─────────────────────┤
│ Execution order    │ Program order        │ Data dependency order│
│ Stalls             │ On ANY hazard        │ Only true dependency│
│ Hardware complexity│ Simple               │ Complex             │
│ Power              │ Low                  │ High                │
│ Area (transistors) │ Small                │ Large (3-5x)        │
│ IPC                │ ~0.5-1.0             │ ~2.0-4.0+           │
│ Frequency          │ Can be high          │ Similar              │
│ L1 miss penalty    │ Full stall           │ Execute other instrs│
│ Branch mispredict  │ Moderate penalty     │ Higher penalty       │
│ Memory latency     │ Cannot hide          │ Can hide (OoO)      │
├────────────────────┼──────────────────────┼─────────────────────┤
│ ARM in-order cores │ Cortex-A53, A55, A510│                     │
│ ARM OoO cores      │                      │ A72-A78, X1-X4, N2 │
│ ARM use case       │ LITTLE cores (mobile)│ big cores (mobile)  │
│                    │ Always-on efficiency │ Performance bursts  │
└────────────────────┴──────────────────────┴─────────────────────┘

big.LITTLE / DynamIQ strategy:
  ┌─────────────────────────────────────────────────┐
  │ Cluster 0: LITTLE (A55 × 4)                    │
  │   • Handle background tasks, idle screen       │
  │   • 500 MHz-1.8 GHz, ~30 mW per core          │
  │   • Email, music, notifications                │
  │                                                 │
  │ Cluster 1: big (A78 × 4)                       │
  │   • Handle UI, gaming, camera processing       │
  │   • 2.0-3.0 GHz, ~500 mW per core             │
  │   • Burst performance when needed              │
  │                                                 │
  │ Cluster 2: X core (X4 × 1)                    │
  │   • Peak single-thread performance             │
  │   • 3.0-3.5 GHz, ~1 W                         │
  │   • Browser, app launch latency                │
  └─────────────────────────────────────────────────┘

Why not all OoO?
  Power is proportional to: Frequency × Voltage² × Activity
  OoO has ~3-5x more transistors → more activity factor →
  more leakage current even when idle.
  A55 at 1 GHz ≈ 50 mW. A78 at 1 GHz ≈ 150 mW.
  For "check notification" workload: A55 is 3x more efficient.
```

---

## Q9. [L1] What are the special instructions for synchronization in ARMv8? (DMB, DSB, ISB)

**Answer:**

```
Three barrier instructions control ordering of memory operations
and instruction execution:

DMB — Data Memory Barrier:
  Ensures that all memory accesses before DMB are observed
  by the specified shareability domain BEFORE any memory
  accesses after DMB.
  
  DMB ISH   — Inner Shareable (all cores in cluster)
  DMB OSH   — Outer Shareable (all cores + devices)
  DMB SY    — Full System
  DMB ISHLD — Inner Shareable, loads only
  DMB ISHST — Inner Shareable, stores only
  
  Use case: spinlock release
    STR X0, [X1]           // Write shared data
    DMB ISH                // Ensure data visible before flag
    STR XZR, [X2]          // Release lock

DSB — Data Synchronization Barrier:
  Stronger than DMB. Ensures:
  1. All memory accesses before DSB are COMPLETE
  2. All cache/TLB maintenance before DSB is COMPLETE
  3. No instruction after DSB executes until DSB completes
  
  DSB ISH, DSB OSH, DSB SY (same domain options)
  
  Use case: TLB invalidation
    TLBI VMALLE1IS         // Invalidate TLB entries
    DSB ISH                // Wait until TLB invalidation complete
    ISB                    // Flush pipeline → new translations used

ISB — Instruction Synchronization Barrier:
  Flushes the pipeline. Ensures:
  1. All instructions after ISB are fetched FRESH
  2. System register changes take effect
  3. New page table mappings are used for fetch
  
  Use case: system register change
    MSR SCTLR_EL1, X0     // Change system control register
    ISB                    // Pipeline flush — next instruction
                           // sees the new SCTLR value

Common interview mistakes:
  • Using DMB when DSB is needed (TLB maintenance)
  • Forgetting ISB after MSR
  • Not specifying domain (DMB SY is overkill, DMB ISH is enough
    for inter-core sync within a shared-memory system)

  Ordering strength: ISB > DSB > DMB
  DMB: orders memory OP relative to each other
  DSB: completes all prior memory ops + cache ops
  ISB: context synchronization (pipeline flush)
```

---

## Q10. [L2] Explain LDAR / STLR (Load-Acquire / Store-Release) semantics. How do they relate to C11 memory ordering?

**Answer:**

```
ARMv8 has HARDWARE support for acquire/release semantics:

LDAR (Load-Acquire Register):
  Reads memory AND establishes a barrier:
  → All memory accesses AFTER LDAR in program order are ordered
    AFTER the LDAR.
  → The load itself is ordered after all previous LDAR/STLR.
  
  Think of it as: "From this point forward, everything happens
  after I read this value"

STLR (Store-Release Register):
  Writes memory AND establishes a barrier:
  → All memory accesses BEFORE STLR in program order are ordered
    BEFORE the STLR.
  
  Think of it as: "Everything before this store is visible to
  anyone who reads it"

Spinlock example:
  Lock acquire:
    LDAXR W0, [X1]        // Load-Acquire Exclusive
    CBNZ  W0, spin        // Already locked?
    STXR  W2, W3, [X1]    // Store Exclusive (try to take lock)
    CBNZ  W2, retry        // Failed? Retry
    // LDAR semantics: all subsequent accesses see data
    // protected by the lock

  Lock release:
    STLR WZR, [X1]        // Store-Release: release lock
    // All prior writes to shared data are visible to
    // any core that acquires the lock (via LDAR)

C11 memory ordering mapping on ARM64:
  ┌───────────────────────────┬────────────────────────────────┐
  │ C11 ordering              │ ARM64 implementation           │
  ├───────────────────────────┼────────────────────────────────┤
  │ memory_order_relaxed      │ LDR / STR (normal load/store) │
  │ memory_order_acquire      │ LDAR                          │
  │ memory_order_release      │ STLR                          │
  │ memory_order_acq_rel      │ LDAR for load, STLR for store │
  │ memory_order_seq_cst      │ LDAR + STLR (on ARM, same as │
  │                           │  acq_rel due to multi-copy    │
  │                           │  atomicity from ARMv8)        │
  │ atomic_thread_fence(acq)  │ DMB ISHLD                     │
  │ atomic_thread_fence(rel)  │ DMB ISH                       │
  │ atomic_thread_fence(s_c)  │ DMB ISH                       │
  └───────────────────────────┴────────────────────────────────┘

LSE equivalents (ARMv8.1):
  CAS   → CASA (acquire), CASL (release), CASAL (both)
  LDADD → LDADDA, LDADDL, LDADDAL
  SWP   → SWPA, SWPL, SWPAL

Performance: LDAR/STLR are cheaper than DMB barriers because
they only order relative to themselves, not ALL memory operations.
```

---

## Q11. [L3] What is the Reorder Buffer (ROB) and how does it enable precise exceptions in OoO cores?

**Answer:**

```
The ROB is the CENTRAL structure in an OoO processor that allows:
1. Instructions to EXECUTE out of order
2. But COMMIT (retire) in program order
3. Precise exceptions: when exception occurs, all prior instructions
   are committed, all subsequent are discarded

ROB Entry structure:
  ┌───────────────────────────────────────────────────────┐
  │ ROB Entry:                                            │
  │  [Sequence#] [Instruction] [Dest PhysReg] [Result]   │
  │  [Completed?] [Exception?] [ExceptionType]           │
  │  [Speculative?] [BranchMispredicted?]                │
  └───────────────────────────────────────────────────────┘

How it works:
  1. DISPATCH: instruction → ROB tail (in program order)
  2. EXECUTE: out of order; when done, mark "completed" + store result
  3. RETIRE HEAD: check oldest entry:
     a. If completed + no exception → commit (write arch reg), free entry
     b. If completed + exception → flush everything after, take exception
     c. If not completed → STALL retirement (wait)
  
  Example:
  ┌────┬────────────────────┬──────┬──────────┬───────────┐
  │ #  │ Instruction        │ Done │ Result   │ Exception │
  ├────┼────────────────────┼──────┼──────────┼───────────┤
  │ 1  │ ADD X0, X1, X2     │ Yes  │ 42       │ No        │ ← HEAD
  │ 2  │ LDR X3, [X4]       │ No   │ pending  │ -         │ (cache miss)
  │ 3  │ MUL X5, X6, X7     │ Yes  │ 100      │ No        │ (done OoO!)
  │ 4  │ STR X0, [X8]       │ Yes  │ -        │ No        │ (done OoO!)
  │ 5  │ DIV X9, X1, XZR    │ Yes  │ -        │ YES       │ (div by 0!)
  └────┴────────────────────┴──────┴──────────┴───────────┘
  
  Retirement sequence:
  Cycle 1: Retire #1 (commit X0=42) ✓
  Cycle 2: #2 not done → STALL (even though #3,#4,#5 are done)
  Cycle N: #2 completes → Retire #2 ✓, then #3 ✓, then #4 ✓
  Cycle N+1: #5 has exception! 
    → Flush everything from #5 onward
    → ELR_EL1 = PC of DIV instruction (precise!)
    → Take exception

Why precise exceptions matter:
  • Debugger shows EXACT instruction that faulted
  • Page fault handler knows which load/store needs a page
  • Can retry: fix fault, ERET, re-execute same instruction
  • Imprecise exceptions are MUCH harder to debug/handle

ROB sizes (bigger = more ILP extraction):
  Cortex-A72:  ~128 entries
  Cortex-A76:  ~128 entries
  Cortex-A78:  ~160 entries
  Cortex-X2:   ~224 entries
  Cortex-X4:   ~288 entries
  Neoverse V2: ~256 entries
```

---

## Q12. [L2] What is the Micro-TLB and how does it differ from the main TLB in the CPU pipeline?

**Answer:**

```
ARM cores typically have a 2-level TLB hierarchy:

  ┌──────────────────────────────────────────────────────────┐
  │ Level   │ Name    │ Entries  │ Latency │ Associativity  │
  ├─────────┼─────────┼──────────┼─────────┼────────────────┤
  │ µTLB    │ micro-  │ 32-48   │ 0-1 cyc │ Fully assoc.   │
  │ (L1 TLB)│ TLB     │ per I/D │         │                │
  ├─────────┼─────────┼──────────┼─────────┼────────────────┤
  │ Main TLB│ L2 TLB  │ 512-4096│ 3-7 cyc │ 4-8 way set    │
  │ (unified)│        │ unified │         │ associative    │
  └─────────┴─────────┴──────────┴─────────┴────────────────┘

µTLB characteristics:
  • SEPARATE for instruction and data
  • Accessed EVERY cycle (part of the pipeline)
  • Fully associative → any entry can hold any translation
  • Very small → low power, low latency
  • Handles vast majority of translations (~99% hit rate)
  
Main TLB (L2 TLB):
  • UNIFIED (instruction + data share it)
  • Only accessed on µTLB miss
  • Larger, set-associative (lower power than fully associative)
  • May support multiple page sizes (4KB, 2MB, 1GB pages)
  • Some designs have separate entries for different page sizes
  
Miss path:
  1. Address → µTLB lookup
  2. Hit? → Done (0-1 cycle, pipelined)
  3. Miss → Main TLB lookup (3-7 cycles)
  4. Hit? → Refill µTLB, done
  5. Miss → TABLE WALK UNIT activates
     a. Walk page tables in memory (L1 cache → L2 → DRAM)
     b. Could take 10-100+ cycles (especially if page table
        entries miss all caches)
     c. Fill Main TLB + µTLB with result
     d. If translation invalid → Data Abort exception

  Cortex-A78 TLB:
    I-µTLB: 48 entries, fully associative
    D-µTLB: 48 entries, fully associative
    Main TLB: 1280 entries, 5-way set associative
    Table walk: 2 concurrent walks
    
  Cortex-X3:
    I-µTLB: 48 entries
    D-µTLB: 48 entries
    Main TLB: 2048 entries
    Table walk: 4 concurrent walks (more parallelism)
```

---

## Q13. [L1] What is the difference between BL (Branch with Link) and BR (Branch to Register)?

**Answer:**

```
BL (Branch with Link):
  BL label
  • Saves return address in X30 (LR): X30 = PC + 4
  • Branches to label (PC-relative, ±128 MB range)
  • Used for: function calls
  • Direct: target is encoded in the instruction
  
  Example:
    BL printf           // X30 = return address, PC = printf
    // ... printf returns here via RET

BR (Branch to Register):
  BR Xn
  • Branches to address in register Xn
  • Does NOT save return address (no link)
  • Used for: jump tables, function pointers, tail calls
  • Indirect: target is in a register (runtime-determined)
  
  Example:
    LDR X16, [X0, X1, LSL #3]  // Load function pointer
    BR X16                       // Jump to it (tail call)

BLR (Branch with Link to Register):
  BLR Xn
  • Saves X30 = PC + 4 (like BL)
  • Branches to address in Xn (like BR)
  • Used for: calling function pointers, virtual methods
  
  Example:
    LDR X16, [X0, #vtable_offset]  // Load vtable entry
    BLR X16                          // Call virtual method

RET:
  RET {Xn}    // Default: RET X30 (same as BR X30, but...)
  • Functionally same as BR X30
  • BUT: hints the branch predictor that this is a RETURN
  • Uses Return Address Stack (RAS) for prediction
  • Much higher prediction accuracy than generic BR
  
  Why not just BR X30?
    BR X30 → CPU uses indirect branch predictor (BTB)
    RET    → CPU uses RAS (almost 100% accurate)
    Using BR X30 instead of RET → misprediction penalty!

Summary:
  BL label    → Call known function (direct)
  BLR Xn      → Call function pointer (indirect)
  BR Xn       → Jump (tail call, computed goto)
  RET         → Return from function (use RAS predictor)
```

---

## Q14. [L3] What is the Memory Prefetch Unit? How do hardware and software prefetch work in ARMv8?

**Answer:**

```
Prefetch = bringing data into cache BEFORE the CPU needs it,
to hide memory latency (100+ cycles for DRAM access).

HARDWARE Prefetch:
  The L1/L2 prefetcher monitors access patterns and automatically
  fetches ahead:
  
  Stream detection:
    Load [A], [A+64], [A+128]... → sequential stream detected
    Prefetcher fetches [A+192], [A+256] into L2/L1
    
  Stride detection:
    Load [A], [A+256], [A+512]... → stride-256 pattern
    Prefetcher fetches [A+768] ahead
    
  Cortex-A78 hardware prefetchers:
  ┌────────────────────────────────────────────────────────┐
  │ Level  │ Prefetchers                                   │
  ├────────┼──────────────────────────────────────────────┤
  │ L1     │ • Next-line prefetch (immediate +1 line)    │
  │        │ • Stride prefetch (up to 2 strides)         │
  │ L2     │ • Stream prefetch (up to 4 streams)         │
  │        │ • Stride prefetch (longer patterns)         │
  │        │ • Temporal prefetch (recently evicted)       │
  │ L3     │ • Regional prefetch (spatial correlation)    │
  └────────┴──────────────────────────────────────────────┘

SOFTWARE Prefetch (PRFM instruction):
  PRFM PLDL1KEEP, [X0, #256]   // Prefetch for load, L1, temporal
  PRFM PLDL2KEEP, [X0, #512]   // Prefetch for load, L2, temporal
  PRFM PSTL1STRM, [X0, #256]   // Prefetch for store, L1, streaming
  
  Encoding:
    PLD = Prefetch for Load
    PST = Prefetch for Store (allocates in write-allocate cache)
    PLI = Prefetch for Instruction execution
    
    L1/L2/L3 = target cache level
    KEEP = temporal (likely reused → keep in cache)
    STRM = streaming (use once → don't pollute cache)

  Practical usage:
    // Process array with software prefetch
    loop:
      PRFM PLDL1KEEP, [X0, #256]  // Prefetch 4 cache lines ahead
      LDR  X1, [X0]
      // ... process X1 ...
      ADD  X0, X0, #64
      CMP  X0, X2
      B.LT loop

  When to use software prefetch:
  ✓ Pointer chasing (linked list traversal — HW can't predict)
  ✓ Irregular access patterns (hash table probing)
  ✓ Known distance ahead (large array processing)
  
  When NOT to:
  ✗ Sequential access (HW prefetcher handles it)
  ✗ Tiny working sets (all in cache already)
  ✗ Too aggressive → evicts useful data (cache pollution)
```

---

## Q15. [L2] How does ARM handle unaligned memory access in AArch64?

**Answer:**

```
AArch64 supports unaligned access for most normal memory loads/stores
but NOT for all instruction types:

  Allowed (unaligned):
    LDR/STR with normal memory (even to non-aligned addresses)
    LDRH/STRH (halfword)
    LDUR/STUR (pre/post indexed forms)
    Performance: may be slower (spans 2 cache lines)
    
  NOT allowed (must be aligned):
    LDP/STP (load/store pair) — must be naturally aligned
    LDXR/STXR (exclusive) — must be naturally aligned
    LDAR/STLR (acquire/release) — must be naturally aligned
    PRFM (prefetch) — aligned to cache line
    LSE atomics (CAS, LDADD) — naturally aligned
  
  Control: SCTLR_EL1.A bit
    A=1: ALL unaligned accesses generate Alignment Fault
    A=0: Allow unaligned access for normal loads/stores
    
  SP alignment:
    SP MUST always be 16-byte aligned at EL1 (SCTLR_EL1.SA=1)
    SP MUST always be 16-byte aligned at EL0 (SCTLR_EL1.SA0=1)
    Misaligned SP → immediate SP Alignment Fault
    This catches stack corruption bugs early

Performance impact of unaligned access:
  Within same cache line: ~same as aligned (1 cycle)
  Spanning 2 cache lines: ~2x latency (2 cache accesses)
  Spanning 2 pages: page table walk × 2 (very expensive!)
  
  Compilers align data structures by default.
  __attribute__((packed)) in C can cause unaligned access.
  ARM recommends: align all data to natural boundary.
```

---

## Q16. [L2] What is the Performance Monitoring Unit (PMU) in ARMv8? Name important events you'd monitor.

**Answer:**

```
PMU allows counting hardware events to profile and optimize code.

Architecture:
  PMCR_EL0:    PMU Control Register (enable, reset, divide)
  PMEVCNTR<n>: Event counter n (6-31 counters, implementation-defined)
  PMEVTYPER<n>: Event type selector for counter n
  PMCCNTR_EL0: Cycle counter (64-bit, counts every cycle)
  PMCNTENSET:  Counter enable set (bitmask)
  PMOVSSET:    Overflow status (for generating interrupts)
  PMUSERENR_EL0: User Enable Register (allowing EL0 access)

Key events to monitor:
┌────────────────────────────┬──────────────────────────────────┐
│ Event                      │ Why Monitor It                    │
├────────────────────────────┼──────────────────────────────────┤
│ CPU_CYCLES                 │ Total cycles (baseline)          │
│ INST_RETIRED               │ Instructions retired → IPC calc  │
│ L1I_CACHE_REFILL           │ I-cache misses (code too large?) │
│ L1D_CACHE_REFILL           │ D-cache misses (data locality?)  │
│ L2D_CACHE_REFILL           │ L2 misses → going to L3/DRAM    │
│ BR_MIS_PRED                │ Branch mispredictions           │
│ BR_PRED                    │ Total branch predictions         │
│ INST_SPEC                  │ Speculatively executed instrs    │
│ MEM_ACCESS                 │ Total memory accesses           │
│ BUS_ACCESS                 │ External bus transactions        │
│ LL_CACHE_MISS              │ Last-level cache misses (→DRAM) │
│ DTLB_WALK                  │ D-TLB table walks (expensive!)  │
│ ITLB_WALK                  │ I-TLB table walks               │
│ STALL_FRONTEND             │ Cycles stalled, front-end       │
│ STALL_BACKEND              │ Cycles stalled, back-end        │
│ EXC_TAKEN                  │ Exceptions taken (IRQ/SVC rate) │
└────────────────────────────┴──────────────────────────────────┘

Linux perf usage:
  perf stat -e cycles,instructions,cache-misses,branch-misses ./app
  
  IPC (Instructions Per Cycle):
    IPC = INST_RETIRED / CPU_CYCLES
    IPC < 1: memory-bound (lots of stalls)
    IPC > 2: compute-efficient
    IPC > 3: excellent (rare outside SIMD workloads)
    
  Cache miss rate:
    L1 miss rate = L1D_CACHE_REFILL / MEM_ACCESS
    Target: < 5% for L1, < 1% for L2
    
  Branch prediction accuracy:
    Accuracy = 1 - (BR_MIS_PRED / BR_PRED)
    Target: > 97%

PMU overflow interrupt:
  Configure counter to overflow at threshold → generates IRQ
  IRQ handler records PC → sampling profiler (perf record)
  This is how perf builds flame graphs from hardware events.
```

---

## Q17. [L1] What are Condition Codes and how do conditional operations work in AArch64?

**Answer:**

```
AArch64 condition codes are stored in PSTATE {N, Z, C, V}:

  N = Negative: result bit [63] or [31] is 1
  Z = Zero: result is zero
  C = Carry: unsigned overflow / borrow
  V = oVerflow: signed overflow

Only specific instructions SET flags:
  ADDS, SUBS, ANDS, CMP, CMN, TST, CCMP
  (Note: ADD, SUB, AND do NOT set flags — need the 'S' suffix)

Condition codes:
┌──────┬──────────────────────────────────────────────────────┐
│ Code │ Meaning                        │ Flags               │
├──────┼────────────────────────────────┼─────────────────────┤
│ EQ   │ Equal / zero                   │ Z=1                 │
│ NE   │ Not equal / non-zero           │ Z=0                 │
│ CS/HS│ Carry set / unsigned >=        │ C=1                 │
│ CC/LO│ Carry clear / unsigned <       │ C=0                 │
│ MI   │ Minus / negative               │ N=1                 │
│ PL   │ Plus / positive or zero        │ N=0                 │
│ VS   │ Overflow set                   │ V=1                 │
│ VC   │ Overflow clear                 │ V=0                 │
│ HI   │ Unsigned higher                │ C=1 && Z=0          │
│ LS   │ Unsigned lower or same         │ C=0 || Z=1          │
│ GE   │ Signed >=                      │ N==V                │
│ LT   │ Signed <                       │ N!=V                │
│ GT   │ Signed >                       │ Z=0 && N==V         │
│ LE   │ Signed <=                      │ Z=1 || N!=V         │
│ AL   │ Always (default)               │ any                 │
│ NV   │ Never (reserved, behaves as AL)│ any                 │
└──────┴────────────────────────────────┴─────────────────────┘

Conditional operations in AArch64 (NO conditional execution!):
  Unlike ARMv7 where nearly every instruction could be conditional
  (ADDEQ, LDRNE), AArch64 uses:
  
  Conditional branches: B.EQ, B.NE, B.LT, etc.
  Conditional select:   CSEL, CSINC, CSINV, CSNEG
  Conditional compare:  CCMP, CCMN
  
  CSEL X0, X1, X2, EQ    // X0 = (Z==1) ? X1 : X2
  CSINC X0, X1, X2, NE   // X0 = (Z==0) ? X1 : X2+1
  CSET X0, EQ             // X0 = (Z==1) ? 1 : 0  (alias)
  
  CCMP (very powerful — chains conditions):
    CMP X0, #5
    CCMP X1, #10, #0, EQ   // If EQ: compare X1 vs 10
                            // If NE: set flags to #0 (NZCV=0000)
    B.EQ both_true          // Branch if X0==5 AND X1==10
```

---

## Q18. [L3] How does the CPU handle multi-cycle instructions and pipeline interlocks?

**Answer:**

```
Not all instructions complete in 1 cycle. The CPU must handle
variable latency without stalling the entire pipeline:

Instruction latencies (typical A78):
  ┌──────────────────────────────┬──────────┬────────────────┐
  │ Instruction                  │ Latency  │ Throughput     │
  ├──────────────────────────────┼──────────┼────────────────┤
  │ ADD, SUB, AND, ORR          │ 1 cycle  │ 1/cycle        │
  │ MUL (32-bit)                │ 3 cycles │ 1/cycle        │
  │ MUL (64-bit)                │ 4 cycles │ 1/cycle        │
  │ SDIV/UDIV (32-bit)          │ 7-12 cyc │ varies         │
  │ SDIV/UDIV (64-bit)          │ 9-20 cyc │ varies         │
  │ FADD (FP64)                 │ 2 cycles │ 1/cycle        │
  │ FMUL (FP64)                 │ 3 cycles │ 1/cycle        │
  │ FDIV (FP64)                 │ 7-15 cyc │ 1/7 cycles     │
  │ FSQRT (FP64)                │ 8-18 cyc │ 1/8 cycles     │
  │ LDR (L1 hit)                │ 4 cycles │ 2/cycle        │
  │ LDR (L2 hit)                │ 10 cyc   │ varies         │
  │ LDR (L3 hit)                │ 30-40 cyc│ varies         │
  │ LDR (DRAM)                  │ 100+ cyc │ varies         │
  └──────────────────────────────┴──────────┴────────────────┘

In-order core handling:
  Pipeline interlock hardware detects when a result isn't ready:
  
  MUL X0, X1, X2    // Takes 3 cycles, result in cycle 3
  ADD X3, X0, X4    // Needs X0 → stall 2 cycles (interlock)
  
  Interlock inserts "pipeline bubbles" (NOPs):
  MUL X0 | [bubble] | [bubble] | ADD X3 ← uses X0 result
  
  Compiler can schedule to avoid stalls:
  MUL X0, X1, X2    // Start multiply
  ADD X5, X6, X7    // Independent work (fills bubble)
  SUB X8, X9, X10   // More independent work
  ADD X3, X0, X4    // Now X0 is ready — no stall!

Out-of-order core handling:
  No interlocks needed — the issue queue handles it:
  
  MUL X0, X1, X2    // Issued to multiply unit
  ADD X3, X0, X4    // Sits in issue queue, waiting for X0
  SUB X5, X6, X7    // Independent → issued immediately
  ORR X8, X9, X10   // Independent → issued immediately
  // When MUL finishes, X0 result broadcast on CDB
  // ADD wakes up and issues next cycle

Division (fully pipelined):
  SDIV is iterative — takes variable cycles depending on operand
  values. The divide unit is typically NOT pipelined:
  • While dividing, no other SDIV can start (throughput = latency)
  • Other functional units continue working — only divide unit busy
  • Compiler avoids division when possible (use shifts, multiply
    by reciprocal for constant divisors)
```

---

## Q19. [L2] Explain the concept of "micro-ops fusion" and "macro-ops fusion" in ARM cores.

**Answer:**

```
These are optimization techniques to increase effective throughput:

MACRO-OP FUSION (front-end):
  Combines TWO separate instructions into ONE µ-op during decode.
  Saves decode bandwidth and execution resources.
  
  Fused pairs on Cortex-A76+:
  ┌─────────────────────────────────────────────────────────┐
  │ Instructions              │ Fused into                  │
  ├───────────────────────────┼─────────────────────────────┤
  │ CMP + B.cond              │ Single compare-and-branch  │
  │ CMN + B.cond              │ Single compare-and-branch  │
  │ ADDS + B.cond             │ Fused add-compare-branch   │
  │ SUBS + B.cond             │ Fused sub-compare-branch   │
  │ AND (flag-setting) + B.EQ │ Fused test-and-branch      │
  └───────────────────────────┴─────────────────────────────┘
  
  Example:
    CMP X0, #10        ─┐
    B.EQ target         ─┘ Fused into 1 µ-op
    // 4-wide decoder effectively executes 5 instructions
    
  Note: CBZ/CBNZ and TBZ/TBNZ already combine compare+branch
  in a single instruction — no fusion needed.

MICRO-OP FUSION (back-end):
  Keeps complex instructions as a single entity through the
  pipeline, even though they use multiple execution resources.
  
  LDR X0, [X1, X2, LSL #3]
  This needs:
  • Shift unit: X2 << 3
  • AGU: X1 + (X2 << 3) = address
  • Load unit: load from address
  
  Without fusion: 3 separate µ-ops (uses 3 issue queue slots)
  With fusion: 1 fused µ-op (1 issue queue slot, routes to
  shift → AGU → load internally)
  
  This preserves ROB/issue queue capacity for other instructions.

Impact:
  Macro-op fusion: ~5-10% IPC improvement on branch-heavy code
  Micro-op fusion: reduces internal bandwidth pressure
  Both are transparent to software — compiler doesn't need to
  know (but can help by generating fusable sequences).
```

---

## Q20. [L3] How do ARM cores handle simultaneous multithreading (SMT)? Does ARMv8 support it?

**Answer:**

```
SMT (Simultaneous Multithreading) allows a SINGLE core to execute
instructions from MULTIPLE threads simultaneously, sharing
execution resources.

ARM's approach historically:
  Most ARM cores do NOT implement SMT.
  ARM's strategy has been: more cores, not SMT per core.
  
  Reason: SMT adds ~5-15% more transistors but gives only
  ~30% throughput gain. For mobile (power-sensitive), it's
  better to have separate small cores (big.LITTLE).

Exceptions — ARM does have SMT designs:
  
  Cortex-A65AE (ARMv8.2, 2019):
    • 2-way SMT (2 threads per core)
    • Designed for automotive (ADAS, self-driving)
    • Can run in LOCKSTEP mode (both threads execute SAME code
      → compare results → fault detection for safety-critical)
    • Or SPLIT mode (2 independent threads for throughput)
  
  Neoverse E1 (ARMv8.2):
    • 2-way SMT
    • Designed for edge/networking (packet processing)
    • High thread count per watt matters for networking
  
  ARM server roadmap:
    Neoverse V-series: NO SMT (single-thread performance focus)
    Neoverse N-series: NO SMT (efficiency per core focus)

How SMT works mechanically:
  ┌────────────────────────────────────────────────────────┐
  │ Shared:                                               │
  │   • Execution units (ALU, FPU, LSU)                  │
  │   • L1/L2 caches                                     │
  │   • Branch predictor structures                      │
  │   • Reorder buffer (partitioned or competitive)      │
  │                                                       │
  │ Per-thread (duplicated):                              │
  │   • Architectural register file (X0-X30 per thread)  │
  │   • Program Counter                                   │
  │   • PSTATE / exception state                         │
  │   • ASID / VMID                                      │
  │   • SP_EL0, TPIDR_EL0                               │
  │                                                       │
  │ Benefits:                                             │
  │   Thread A stalls on cache miss → Thread B uses       │
  │   execution units → better utilization                │
  │                                                       │
  │ Drawbacks:                                            │
  │   • Cache thrashing between threads                  │
  │   • Single-thread performance reduced (~10-15%)      │
  │   • Side-channel attacks between threads (Spectre!)  │
  │   • More complex OS scheduling                       │
  └────────────────────────────────────────────────────────┘

MPIDR_EL1.MT bit:
  MT=1: indicates the core has SMT
  Aff0 then identifies the THREAD within the core
  Linux topology code uses this to set up cpu_smt_control
```

---

Back to [Question & Answers Index](./README.md)
