# Debug Architecture

## 1. Debug Modes

ARMv8 supports two debugging approaches:

```
┌──────────────────────────────────────────────────────────────────┐
│                                                                    │
│  Self-Hosted Debug (software debugger)                            │
│  • Debugger runs on the same CPU (e.g., GDB + Linux ptrace)     │
│  • Uses debug exceptions (breakpoint/watchpoint → exception)      │
│  • Debug registers accessed via system registers (MRS/MSR)        │
│  • Requires OS support                                            │
│                                                                    │
│  External Debug (hardware debugger — JTAG/SWD)                   │
│  • External tool (Lauterbach TRACE32, ARM DS-5, J-Link)         │
│  • Debug Access Port (DAP) over JTAG/SWD physical connection    │
│  • Can halt the core (debug state) — invasive                    │
│  • Debug registers accessed via memory-mapped interface          │
│  • Works even if OS is crashed/hung                               │
│                                                                    │
│  ┌───────────────────────────────────────────────────────────┐   │
│  │              Debug Register Access                         │   │
│  │                                                             │   │
│  │  Self-Hosted:   MSR DBGBVR0_EL1, X0  (system register)   │   │
│  │  External:      Write to 0x8000_0400  (memory-mapped)     │   │
│  │                 Both access SAME hardware registers         │   │
│  └───────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────┘
```

---

## 2. Breakpoints

Hardware breakpoints stop execution when the PC matches a specified address.

```
ARMv8 provides 2-16 hardware breakpoints (typically 6):

  DBGBVR<n>_EL1 — Breakpoint Value Register (address to match)
  DBGBCR<n>_EL1 — Breakpoint Control Register

  DBGBCR format:
  ┌─────┬───────┬────────────────────────────────────────────────┐
  │ Bit │ Field │ Description                                    │
  ├─────┼───────┼────────────────────────────────────────────────┤
  │  0  │ E     │ Enable (1=active)                              │
  │ 2:1 │ PMC   │ Privilege: 01=EL1, 10=EL0, 11=EL0+EL1        │
  │ 4:3 │ -     │ Reserved                                       │
  │ 8:5 │ BAS   │ Byte Address Select (which bytes in word)     │
  │ 13:9│ -     │ Reserved                                       │
  │15:14│ HMC   │ Higher mode: include/exclude EL2              │
  │19:16│ SSC   │ Security state control                        │
  │23:20│ LBN   │ Linked breakpoint number                      │
  │27:24│ BT    │ Breakpoint type:                              │
  │     │       │  0000 = Address match (unlinked)              │
  │     │       │  0001 = Address match (linked)                │
  │     │       │  0010 = Context ID match                      │
  │     │       │  0011 = Address + Context linked              │
  │     │       │  0100 = VMID match                            │
  └─────┴───────┴────────────────────────────────────────────────┘

Set a breakpoint at address 0x1000:
  // Self-hosted (from EL1)
  MOV X0, #0x1000
  MSR DBGBVR0_EL1, X0           // Address to break on
  MOV X0, #0x000001E7            // E=1, PMC=11, BAS=1111
  MSR DBGBCR0_EL1, X0           // Enable breakpoint 0
  ISB                             // Ensure debug setup takes effect

When PC = 0x1000:
  → Debug exception to current EL (self-hosted)
  → Or: Core halts in debug state (external debugger)
```

---

## 3. Watchpoints

Hardware watchpoints stop execution on data address access (load/store).

```
ARMv8 provides 2-16 hardware watchpoints (typically 4):

  DBGWVR<n>_EL1 — Watchpoint Value Register (data address)
  DBGWCR<n>_EL1 — Watchpoint Control Register

  DBGWCR format:
  ┌─────┬───────┬────────────────────────────────────────────────┐
  │ Bit │ Field │ Description                                    │
  ├─────┼───────┼────────────────────────────────────────────────┤
  │  0  │ E     │ Enable                                         │
  │ 2:1 │ PAC   │ Privilege access control                      │
  │ 4:3 │ LSC   │ Load/Store: 01=Load, 10=Store, 11=Both       │
  │ 12:5│ BAS   │ Byte Address Select (1 bit per byte, 8 bytes)│
  │15:14│ HMC   │ Higher mode control                           │
  │19:16│ SSC   │ Security state control                        │
  │23:20│ LBN   │ Linked breakpoint number                      │
  │28:24│ MASK  │ Address mask (0=exact, N=2^N byte range)      │
  └─────┴───────┴────────────────────────────────────────────────┘

Example: Watch writes to address 0x2000 (any byte in 8-byte range):
  MOV X0, #0x2000
  MSR DBGWVR0_EL1, X0           // Watch address
  MOV X0, #0x0000_01F5          // E=1, LSC=10 (store), BAS=FF
  MSR DBGWCR0_EL1, X0
  ISB

When any store hits 0x2000-0x2007:
  → Watchpoint debug exception
  → FAR_EL1 = faulting data address
  → Handler can inspect old/new values

GDB usage:
  (gdb) watch *0x2000          → hardware watchpoint on write
  (gdb) rwatch *0x2000         → on read
  (gdb) awatch *0x2000         → on read or write
```

---

## 4. Debug State vs Debug Exceptions

```
Two ways the core responds to debug events:

┌─────────────────┬───────────────────────┬──────────────────────┐
│                 │ Debug Exception       │ Debug State (Halt)   │
├─────────────────┼───────────────────────┼──────────────────────┤
│ Trigger         │ MDSCR_EL1.KDE=1,     │ External debugger    │
│                 │ PSTATE.D=0            │ via EDSCR.HDE=1     │
├─────────────────┼───────────────────────┼──────────────────────┤
│ Response        │ Synchronous exception │ Core HALTS (stops   │
│                 │ to current or higher  │ executing all code)  │
│                 │ EL                    │                      │
├─────────────────┼───────────────────────┼──────────────────────┤
│ Who handles     │ OS debug handler      │ External debugger   │
│                 │ (ptrace, GDB stub)    │ (JTAG/SWD tool)     │
├─────────────────┼───────────────────────┼──────────────────────┤
│ Core state      │ Still running         │ Stopped (halted)     │
│                 │ (in exception handler)│                      │
├─────────────────┼───────────────────────┼──────────────────────┤
│ Use case        │ GDB on Linux          │ Bare-metal debug,   │
│                 │ (userspace debugging)  │ kernel bringup      │
└─────────────────┴───────────────────────┴──────────────────────┘

MDSCR_EL1 — Monitor Debug System Control Register:
  KDE [13]: Kernel Debug Enable (1=allow debug exceptions at EL1)
  MDE [15]: Monitor Debug Events (1=enable breakpoints/watchpoints)
  SS  [0]:  Software Step (1=single-step mode)
```

---

## 5. Software Single-Stepping

```
Single-step executes ONE instruction then takes a debug exception:

Enable:
  1. Set MDSCR_EL1.SS = 1
  2. Set PSTATE.SS = 1 (in SPSR when returning to target)
  3. ERET to target instruction

Flow:
  ERET → target EL
    ↓ Execute ONE instruction
    ↓ PSTATE.SS → 0
    ↓ Software Step exception
  Back to debugger handler
    → Inspect state
    → Set PSTATE.SS = 1 again (in SPSR)
    → ERET for next instruction

Linux ptrace:
  ptrace(PTRACE_SINGLESTEP, pid, ...)
    → Kernel sets SS/PSTATE.SS
    → ERET to tracee
    → One instruction
    → Debug exception back to kernel
    → Signal SIGTRAP to debugger (GDB)
```

---

## 6. Performance Monitor Unit (PMU)

The PMU counts hardware events: cycles, cache misses, branch mispredicts, etc.

```
PMU Architecture:
  ┌────────────────────────────────────────────────────────────┐
  │  PMU per core:                                              │
  │  • 1 Cycle Counter (PMCCNTR_EL0) — counts CPU cycles      │
  │  • 4-8 Event Counters (PMEVCNTR<n>_EL0)                   │
  │  • Each counter: 32 or 64 bits (ARMv8.5: 64-bit)          │
  │                                                              │
  │  Event selection:                                           │
  │  ┌────────────┬────────────────────────────────────────┐   │
  │  │ Event ID   │ Description                            │   │
  │  ├────────────┼────────────────────────────────────────┤   │
  │  │ 0x00       │ SW_INCR — Software increment           │   │
  │  │ 0x01       │ L1I_CACHE_REFILL                       │   │
  │  │ 0x02       │ L1I_TLB_REFILL                         │   │
  │  │ 0x03       │ L1D_CACHE_REFILL                       │   │
  │  │ 0x04       │ L1D_CACHE                              │   │
  │  │ 0x05       │ L1D_TLB_REFILL                         │   │
  │  │ 0x06       │ LD_RETIRED (loads retired)             │   │
  │  │ 0x07       │ ST_RETIRED (stores retired)            │   │
  │  │ 0x08       │ INST_RETIRED (instructions)            │   │
  │  │ 0x09       │ EXC_TAKEN (exceptions)                 │   │
  │  │ 0x0A       │ EXC_RETURN                             │   │
  │  │ 0x10       │ BR_MIS_PRED (branch mispred)           │   │
  │  │ 0x11       │ CPU_CYCLES                             │   │
  │  │ 0x12       │ BR_PRED (branch predicted)             │   │
  │  │ 0x13       │ MEM_ACCESS (data memory access)        │   │
  │  │ 0x14       │ L1I_CACHE (L1 icache access)           │   │
  │  │ 0x15       │ L1D_CACHE_WB (L1 writeback)           │   │
  │  │ 0x16       │ L2D_CACHE                              │   │
  │  │ 0x17       │ L2D_CACHE_REFILL                       │   │
  │  │ 0x18       │ L2D_CACHE_WB                           │   │
  │  │ 0x19       │ BUS_ACCESS                             │   │
  │  │ 0x1D       │ BUS_CYCLES                             │   │
  │  │ 0x3A       │ STALL_FRONTEND (front-end stall)       │   │
  │  │ 0x3B       │ STALL_BACKEND (back-end stall)         │   │
  │  │ 0x40+      │ Implementation-defined events          │   │
  │  └────────────┴────────────────────────────────────────┘   │
  └────────────────────────────────────────────────────────────┘

PMU setup example (count L1D cache misses):
  // Select event for counter 0
  MOV X0, #0          // Counter 0
  MSR PMSELR_EL0, X0
  MOV X0, #0x03       // L1D_CACHE_REFILL event
  MSR PMXEVTYPER_EL0, X0
  
  // Enable counter 0 and cycle counter
  MOV X0, #(1 << 0) | (1 << 31)  // Counter 0 + Cycle counter
  MSR PMCNTENSET_EL0, X0
  
  // Enable PMU
  MOV X0, #1
  MSR PMCR_EL0, X0    // E=1 (enable), reset counters
  
  // ... run workload ...
  
  // Read results
  MRS X0, PMEVCNTR0_EL0   // L1D cache misses
  MRS X1, PMCCNTR_EL0     // CPU cycles elapsed
  
  // CPI = cycles / instructions
  // Miss rate = L1D_CACHE_REFILL / L1D_CACHE

Linux perf tool uses PMU:
  $ perf stat -e cycles,instructions,cache-misses ./my_program
  $ perf record -e cycles -g ./my_program   # record with callgraph
  $ perf report                              # analyze
```

---

## 7. Statistical Profiling Extension (SPE) — ARMv8.2

```
SPE samples instructions statistically (for profiling without overhead):

  PMU counters: count ALL events (aggregate)
  SPE:          sample INDIVIDUAL instructions (detailed)

  SPE captures per sample:
  • PC (instruction address)
  • Data Virtual Address (for loads/stores)
  • Latency (how many cycles the instruction took)
  • Events (cache miss, TLB miss, branch mispredict, etc.)
  • Source (which level satisfied the data: L1/L2/L3/DRAM)

  SPE flow:
  1. Program PMSIDR_EL1 (sample interval, e.g., every 1000 ops)
  2. Hardware randomly samples matching operations
  3. Sample records written to memory buffer
  4. Buffer full → interrupt → OS collects samples
  5. Profiler correlates PC → source code line

  Linux:
  $ perf record -e arm_spe_0/load_filter=1/ ./my_program
  $ perf report --sort=symbol,srcline,mem

  SPE is much more useful than PMU for finding:
  • Which specific loads cause cache misses
  • Data access patterns and latencies
  • True source of pipeline stalls
```

---

## 8. Debug Authentication (DBGAUTHSTATUS)

```
Debug access can be locked down per security state:

  DBGAUTHSTATUS_EL1:
  ┌──────────────────────────────────────────────────────────────┐
  │ Bits  │ Signal    │ Meaning                                  │
  ├───────┼───────────┼──────────────────────────────────────────┤
  │ [1:0] │ DBGEN     │ Non-secure invasive debug (breakpoints)  │
  │ [3:2] │ NIDEN     │ Non-secure non-invasive debug (trace)    │
  │ [5:4] │ SPIDEN    │ Secure invasive debug                    │
  │ [7:6] │ SPNIDEN   │ Secure non-invasive debug (secure trace) │
  └───────┴───────────┴──────────────────────────────────────────┘

  Values: 00=disabled, 10=enabled (implementation signal)
  
  Production devices typically:
  • DBGEN=1, NIDEN=1 (allow debugging Normal World)
  • SPIDEN=0, SPNIDEN=0 (lock out Secure World debug)
  
  Controlled by: SoC eFuse or debug authentication signals
  This prevents attackers from using JTAG to read secure keys
```

---

Next: [Trace & CoreSight →](./02_Trace_CoreSight.md) | Back to [Debug & Trace Overview](./README.md)
