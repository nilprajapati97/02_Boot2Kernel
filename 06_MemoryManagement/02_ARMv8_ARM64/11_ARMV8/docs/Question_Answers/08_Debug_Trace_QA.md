# Debug & Trace Subsystem — Questions & Answers

---

## Q1. [L1] What is the ARM Debug Architecture? What debug modes exist?

**Answer:**

```
ARM provides two debug modes for software debugging:

1. External Debug (Halt-mode Debug):
   ┌──────────────────────────────────────────────────────────┐
   │  External debugger (JTAG/SWD) controls the CPU          │
   │                                                          │
   │  JTAG Probe ─── DAP ─── Debug Logic ─── CPU             │
   │  (DS-5, Lauterbach)                                      │
   │                                                          │
   │  Capabilities:                                           │
   │    • Halt CPU execution                                 │
   │    • Single-step instructions                           │
   │    • Read/write registers and memory                    │
   │    • Set hardware breakpoints/watchpoints               │
   │    • Access even when OS is crashed                     │
   │                                                          │
   │  Entry: EDSCR.STATUS = 0x13 (halted by debug request)  │
   │  CPU enters Debug state (stops executing instructions)  │
   └──────────────────────────────────────────────────────────┘

2. Self-hosted Debug (Monitor-mode Debug):
   ┌──────────────────────────────────────────────────────────┐
   │  Software on the CPU handles debug exceptions            │
   │                                                          │
   │  Breakpoint hit → Debug exception → Vector to handler   │
   │                                                          │
   │  Used by: GDB, LLDB, Linux ptrace(), perf              │
   │                                                          │
   │  Capabilities:                                           │
   │    • Software breakpoints (BRK #imm instruction)        │
   │    • Hardware breakpoints (DBGBCR/DBGBVR)              │
   │    • Hardware watchpoints (DBGWCR/DBGWVR)              │
   │    • Single-step (MDSCR_EL1.SS=1)                      │
   │                                                          │
   │  Exception: Synchronous, EC=0x3C (BRK), EC=0x30/0x31   │
   │  (breakpoint), EC=0x34/0x35 (watchpoint)                │
   └──────────────────────────────────────────────────────────┘

Debug registers (system registers for self-hosted):
  MDSCR_EL1:    Monitor Debug System Control Register
    SS:  Single Step enable
    MDE: Monitor Debug Events enable
    KDE: Kernel Debug Events enable
  
  DBGBVR<n>_EL1: Breakpoint Value Register (address)
  DBGBCR<n>_EL1: Breakpoint Control Register (config)
  DBGWVR<n>_EL1: Watchpoint Value Register (address)
  DBGWCR<n>_EL1: Watchpoint Control Register (config)
  
  ID_AA64DFR0_EL1: how many breakpoints/watchpoints:
    BRPs: number of breakpoint register pairs (typically 6)
    WRPs: number of watchpoint register pairs (typically 4)
    CTX_CMPs: number of context-aware breakpoints
```

---

## Q2. [L2] How do hardware breakpoints and watchpoints work on ARM64?

**Answer:**

```
Hardware breakpoints: halt execution when PC matches an address.
Hardware watchpoints: halt execution when a memory address is accessed.

Hardware Breakpoints:
  DBGBVR<n>_EL1: breakpoint address (VA)
  DBGBCR<n>_EL1: breakpoint control
    ┌──────────────────────────────────────────────────────┐
    │ Bit [0]:    E (Enable)                               │
    │ Bit [2:1]:  PMC (Privilege Mode Control)             │
    │             01 = EL1 only                            │
    │             10 = EL0 only                            │
    │             11 = EL0 + EL1                           │
    │ Bit [4:3]:  HMC+SSC (Hyp Mode Control)              │
    │ Bit [8:5]:  BAS (Byte Address Select)                │
    │             0xF = match on any byte in word          │
    │ Bit [23:20]: BT (Breakpoint Type)                    │
    │             0000 = Address match (basic)             │
    │             0001 = Linked (must combine with context)│
    │             0010 = Context ID match (ASID)           │
    │             0011 = Linked Context ID                 │
    │             0100 = VMID match                        │
    └──────────────────────────────────────────────────────┘

Hardware Watchpoints:
  DBGWVR<n>_EL1: watched memory address (VA)
  DBGWCR<n>_EL1: watchpoint control
    ┌──────────────────────────────────────────────────────┐
    │ Bit [0]:    E (Enable)                               │
    │ Bit [2:1]:  PAC (Privilege Access Control)           │
    │ Bit [4:3]:  LSC (Load/Store Control)                 │
    │             01 = Load only (read watchpoint)         │
    │             10 = Store only (write watchpoint)       │
    │             11 = Load + Store (any access)           │
    │ Bit [12:5]: BAS (Byte Address Select)                │
    │             Bitmap: which bytes within 8-byte range  │
    │             0xFF = watch all 8 bytes                 │
    │             0x0F = watch lower 4 bytes               │
    │ Bit [28:24]: MASK (Address mask, for range)          │
    │             0 = exact address match                  │
    │             3 = watch 8-byte range                   │
    │             31 = watch 2GB range (!)                 │
    └──────────────────────────────────────────────────────┘

GDB usage on ARM64:
  (gdb) break *0x400568          // HW breakpoint via DBGBVR
  (gdb) watch *(int*)0x7fff1234  // HW watchpoint via DBGWVR
  (gdb) rwatch variable           // Read watchpoint (LSC=01)
  (gdb) awatch variable           // Access watchpoint (LSC=11)
  
  Linux kernel: ptrace(PTRACE_POKEUSER) sets debug registers
  via struct user_hwdebug_state
  
  Maximum: typically 6 breakpoints + 4 watchpoints per core

Single-stepping:
  MDSCR_EL1.SS = 1  →  CPU generates Software Step exception
  after executing ONE instruction
  PSTATE.SS = 1  →  exception taken after next instruction
  
  GDB: stepi → ptrace(PTRACE_SINGLESTEP) → kernel sets SS bit
```

---

## Q3. [L2] What is PMU (Performance Monitoring Unit)? How does perf use it?

**Answer:**

```
PMU counts hardware events (cycles, cache misses, branch
mispredictions, etc.) for performance profiling.

PMU registers:
  PMCR_EL0:     Performance Monitors Control Register
    E:  Enable (all counters)
    C:  Cycle counter reset
    P:  Event counter reset
    N:  Number of event counters (usually 6)
  
  PMCCNTR_EL0:  Cycle Count Register (64-bit)
  PMCNTENSET_EL0: Counter Enable Set (bit per counter)
  PMOVSSET_EL0:   Overflow Flag Set (for interrupt on overflow)
  
  PMEVTYPER<n>_EL0: Event Type Selection (which event to count)
  PMEVCNTR<n>_EL0:  Event Count Register

Common ARMv8 PMU events:
  ┌────────┬──────────────────────────────────────────────────┐
  │ Event  │ Description                                      │
  ├────────┼──────────────────────────────────────────────────┤
  │ 0x00   │ SW_INCR (software increment)                    │
  │ 0x01   │ L1I_CACHE_REFILL (L1 instruction cache miss)   │
  │ 0x03   │ L1D_CACHE_REFILL (L1 data cache miss)          │
  │ 0x04   │ L1D_CACHE (L1 data cache access)               │
  │ 0x05   │ L1D_TLB_REFILL (L1 data TLB miss)              │
  │ 0x08   │ INST_RETIRED (instructions retired)             │
  │ 0x09   │ EXC_TAKEN (exceptions taken)                    │
  │ 0x10   │ BR_MIS_PRED (branch misprediction)             │
  │ 0x11   │ CPU_CYCLES (clock cycles)                       │
  │ 0x12   │ BR_PRED (predicted branches)                    │
  │ 0x13   │ MEM_ACCESS (data memory access)                 │
  │ 0x14   │ L1I_CACHE (L1 instruction cache access)        │
  │ 0x16   │ L2D_CACHE (L2 data cache access)               │
  │ 0x17   │ L2D_CACHE_REFILL (L2 data cache miss)          │
  │ 0x19   │ BUS_ACCESS (external bus access)                │
  │ 0x1B   │ INST_SPEC (speculatively executed instructions) │
  │ 0x21   │ BUS_CYCLES (bus cycles)                         │
  │ 0x23   │ STALL_FRONTEND (frontend stall cycles)         │
  │ 0x24   │ STALL_BACKEND (backend stall cycles)           │
  └────────┴──────────────────────────────────────────────────┘

perf usage on ARM64:
  # Count specific events:
  perf stat -e cycles,instructions,cache-misses,cache-references ./app
  
  # CPI (Cycles Per Instruction) analysis:
  perf stat -e cpu-cycles,instructions ./app
  → CPI = cycles / instructions
  → CPI > 2.0 = memory-bound, CPI < 1.0 = compute-efficient
  
  # Profile with sampling (uses PMU overflow interrupt):
  perf record -g ./app
  perf report
  
  # Per-event sampling:
  perf record -e L1-dcache-load-misses -g ./app
  
  # Raw event code (vendor-specific):
  perf stat -e r0017 ./app    // Event 0x17 = L2D_CACHE_REFILL

Sampling mechanism:
  1. Configure PMU counter with event (e.g., cache misses)
  2. Set counter to overflow after N events (e.g., 10000)
  3. On overflow → PMU fires PPI interrupt (INTID 23)
  4. Interrupt handler: record PC + callchain
  5. Reset counter, continue
  → Statistical profile of where events occur
```

---

## Q4. [L2] What is CoreSight? Describe the trace infrastructure.

**Answer:**

```
CoreSight is ARM's on-chip debug and trace infrastructure,
providing non-intrusive visibility into CPU execution.

CoreSight components:
  ┌──────────────────────────────────────────────────────────┐
  │                 CoreSight Architecture                    │
  │                                                          │
  │  ┌──────────────┐  ┌──────────────┐                     │
  │  │  CPU Core 0  │  │  CPU Core 1  │                     │
  │  │  ┌─────────┐ │  │  ┌─────────┐ │                     │
  │  │  │  ETM    │ │  │  │  ETM    │ │  (Trace source)    │
  │  │  └────┬────┘ │  │  └────┬────┘ │                     │
  │  └───────┼──────┘  └───────┼──────┘                     │
  │          │                  │                             │
  │  ┌───────┴──────────────────┴──────┐                     │
  │  │         ATB (Trace Bus)          │                     │
  │  └──────────────┬──────────────────┘                     │
  │                 │                                         │
  │  ┌──────────────┴──────────────────┐                     │
  │  │        Trace Funnel             │  (merges streams)   │
  │  └──────────────┬──────────────────┘                     │
  │                 │                                         │
  │  ┌──────────────┴──────────────────┐                     │
  │  │        Replicator (optional)    │  (split to 2 sinks)│
  │  └──────────┬────────────┬─────────┘                     │
  │             │            │                                │
  │  ┌──────────┴───┐  ┌────┴──────────┐                     │
  │  │ ETR (RAM)    │  │ TPIU (port)   │  (Trace sinks)     │
  │  │ Store to sys │  │ External trace│                     │
  │  │ memory (DDR) │  │ port (SWO)    │                     │
  │  └──────────────┘  └───────────────┘                     │
  └──────────────────────────────────────────────────────────┘

Key Components:
  ETM (Embedded Trace Macrocell):
    • Generates instruction + data trace
    • Per-core, compresses trace data
    • Captures: branch targets, timestamps, context IDs
    • ETMv4: current version for ARMv8
  
  ETR (Embedded Trace Router):
    • Stores trace data to system memory (DDR)
    • Circular buffer mode (overwrite oldest)
    • Can trigger on events (stop on crash)
  
  TPIU (Trace Port Interface Unit):
    • Outputs trace to external analyzer
    • High-speed port (DS-5, Lauterbach)
  
  CTI (Cross Trigger Interface):
    • Synchronize debug events across cores
    • Core 0 breakpoint → halt all cores
    • Connect trace triggers to debug events
  
  STM (System Trace Macrocell):
    • Software instrumentation trace
    • printf-like trace output via stimulus ports
    • Non-intrusive (no CPU overhead for output)

Linux perf + CoreSight:
  # Record ETM trace (full instruction trace!):
  perf record -e cs_etm/@tmc_etr0/ --per-thread ./app
  
  # Decode trace (every instruction executed):
  perf report --itrace=i100
  
  # Use with OpenCSD (Open CoreSight Decoder):
  perf script --itrace=i100 > trace.txt
```

---

## Q5. [L3] What is SPE (Statistical Profiling Extension)?

**Answer:**

```
SPE (ARMv8.2) provides hardware-based statistical profiling
with much richer data than traditional PMU sampling.

PMU sampling limitations:
  • Sample PC at interrupt → imprecise (skid)
  • Don't know: was it a cache miss? What address? Latency?
  • Only get PC + callchain

SPE advantage:
  Hardware records detailed operation attributes for sampled
  instructions, stored directly to memory.
  
  ┌──────────────────────────────────────────────────────────┐
  │ SPE Sample Record (per sampled operation):              │
  │                                                          │
  │ ┌────────────────────────────────────────────────────┐  │
  │ │ Operation PC:     0x400568                          │  │
  │ │ Op Type:          Load (LDR X0, [X1])              │  │
  │ │ Virtual Address:  0x7FFF1234_5678                   │  │
  │ │ Physical Address: 0x8000_ABCD_0000                  │  │
  │ │ Data Source:      L2 cache                          │  │
  │ │ Latency:          12 cycles                        │  │
  │ │ Timestamp:        0x12345678                        │  │
  │ │ Events:           TLB miss, Cache miss L1           │  │
  │ │ Context:          EL1, NS                           │  │
  │ └────────────────────────────────────────────────────┘  │
  │                                                          │
  │ All of this per sample — incredibly rich data!          │
  └──────────────────────────────────────────────────────────┘

SPE registers:
  PMSIDR_EL1:    SPE ID Register (capabilities)
  PMSICR_EL1:    SPE Interval Counter (sampling interval)
  PMSIRR_EL1:    SPE Interval Reload Register
  PMSFCR_EL1:    SPE Filter Control (what to sample)
  PMSEVFR_EL1:   SPE Event Filter (filter by event type)
  PMSLATFR_EL1:  SPE Latency Filter (min latency threshold)
  PMBLIMITR_EL1: SPE Buffer Limit (storage buffer config)
  PMBPTR_EL1:    SPE Buffer Write Pointer

Filtering:
  Sample only loads:  PMSFCR_EL1.LD = 1
  Sample only stores: PMSFCR_EL1.ST = 1
  Sample only branches: PMSFCR_EL1.B = 1
  
  Latency filter: only record if latency > threshold
    PMSLATFR_EL1.MINLAT = 50  // Only record ops >50 cycles
    → Focuses on expensive operations (cache misses)

perf + SPE:
  # Record with SPE:
  perf record -e arm_spe_0/ts_enable=1,load_filter=1,min_latency=50/ ./app
  
  # Analyze data source (where data came from):
  perf report --mem-mode      // Show memory hierarchy hits
  
  # Find hot memory addresses:
  perf script -F ip,addr,data_src | sort | uniq -c | sort -rn
  
  Output tells you:
    "80% of cache misses come from 3 memory addresses"
    "Function X has average load latency of 200 cycles (DRAM)"
    → Precise optimization guidance!
```

---

## Q6. [L2] How does the ARM trace work for post-mortem crash analysis?

**Answer:**

```
ETR (Embedded Trace Router) provides "flight recorder" mode
for capturing execution history leading to a crash.

Flight Recorder Setup:
  ┌──────────────────────────────────────────────────────────┐
  │ 1. Configure ETM: trace all instructions                │
  │ 2. Configure ETR: circular buffer in DDR                │
  │    (e.g., 64MB buffer at reserved memory region)        │
  │ 3. Enable continuous tracing at boot                    │
  │                                                          │
  │ Normal operation:                                        │
  │   ETM → generates compressed trace                     │
  │   ETR → writes to circular buffer (overwrites old)     │
  │   No performance impact (trace is non-intrusive)        │
  │                                                          │
  │ Crash occurs:                                           │
  │   CPU panics / hangs / resets                           │
  │   ETR buffer still in DDR (survives warm reset)         │
  │   Debugger / crash dump tool reads ETR buffer           │
  │   → Last N million instructions before crash!          │
  │                                                          │
  │ Without trace: "PC was at 0x400568 when it crashed"     │
  │ With trace: "here's the last 10M instructions,          │
  │              including the exact sequence of events      │
  │              that led to the crash"                      │
  └──────────────────────────────────────────────────────────┘

Linux crash dump with ETR:
  1. Reserve memory: device tree → reserved-memory node
     etf_buffer: memory@0x90000000 {
         reg = <0x0 0x90000000 0x0 0x4000000>;  // 64MB
         no-map;
     };
  
  2. Enable trace in kernel:
     echo 1 > /sys/bus/coresight/devices/etm0/enable_source
     echo 1 > /sys/bus/coresight/devices/tmc_etr0/enable_sink
  
  3. After crash: read ETR buffer from debugger
     → Decode with perf + OpenCSD or Lauterbach TRACE32

CTI (Cross Trigger Interface) for crash:
  When one core panics:
    CTI trigger → halt ALL other cores
    → Consistent state snapshot across all cores
    → All ETM buffers stop simultaneously
    → No race conditions in crash analysis
  
  Linux: crash_kexec() → trigger CTI → halt → dump
```

---

## Q7. [L2] What is Self-Hosted Debug vs External Debug? When to use each?

**Answer:**

```
Self-Hosted Debug:
  Software on the CPU handles debug events
  
  ┌──────────────────────────────────────────────────────────┐  
  │ Use cases:                                               │
  │   • GDB debugging user applications via ptrace()        │
  │   • Kernel debugging (kgdb)                             │
  │   • perf profiling (PMU interrupts)                     │
  │   • Address sanitizer runtime                           │
  │                                                          │
  │ Mechanism:                                               │
  │   BRK #imm         → Debug exception (EC=0x3C)         │
  │   HW breakpoint hit → Debug exception (EC=0x30/0x31)   │
  │   HW watchpoint hit → Debug exception (EC=0x34/0x35)   │
  │   Single step       → Software Step exc (EC=0x32/0x33) │
  │                                                          │
  │ Pros:                                                    │
  │   + No special hardware needed                          │
  │   + Can debug remotely (gdbserver over network)         │
  │   + Integrated with OS (ptrace, perf)                   │
  │                                                          │
  │ Cons:                                                    │
  │   - Can't debug if OS crashes                           │
  │   - Limited by OS cooperation                           │
  │   - Software breakpoint (BRK) modifies code            │
  │   - Can't debug early boot (before OS is up)            │
  └──────────────────────────────────────────────────────────┘

External Debug:
  External debugger (JTAG/SWD) controls CPU via DAP
  
  ┌──────────────────────────────────────────────────────────┐
  │ Use cases:                                               │
  │   • Kernel/firmware debugging                           │
  │   • Bootloader bring-up (no OS yet)                     │
  │   • Hard crash debugging (CPU in undefined state)       │
  │   • Silicon bring-up and validation                     │
  │   • TrustZone secure world debugging                    │
  │                                                          │
  │ Mechanism:                                               │
  │   JTAG probe → DAP → Debug registers → Halt CPU        │
  │   CPU enters Debug state (EDSCR.STATUS)                 │
  │   Debugger: read/write memory, registers via DAP        │
  │                                                          │
  │ Access path:                                             │
  │   JTAG/SWD → DAP → APB-AP → Debug ROM → CoreSight     │
  │                                                          │
  │ Pros:                                                    │
  │   + Works even when OS is dead                          │
  │   + Can debug secure world (with authentication)        │
  │   + Non-intrusive trace (ETM/ETR)                       │
  │   + Full system visibility                              │
  │                                                          │
  │ Cons:                                                    │
  │   - Requires physical JTAG connection                   │
  │   - Expensive debug probe ($$$)                         │
  │   - Not available in production (security lock)         │
  └──────────────────────────────────────────────────────────┘

Debug authentication signals:
  DBGEN:      Invasive debug enable (breakpoints, halt)
  NIDEN:      Non-invasive debug enable (trace, PMU)
  SPIDEN:     Secure invasive debug enable
  SPNIDEN:    Secure non-invasive debug enable
  
  Production devices: all signals LOW (debug disabled)
  Development boards: signals HIGH (debug enabled)
  
  Fuse-based: OEM burns fuses to permanently disable debug
  on production chips (no way to re-enable = secure)
```

---

## Q8. [L2] How does Linux ftrace work on ARM64? What is the function tracer?

**Answer:**

```
ftrace provides dynamic function tracing in the Linux kernel
on ARM64, using compiler instrumentation.

Architecture:
  ┌──────────────────────────────────────────────────────────┐
  │ At compile time (-pg flag):                              │
  │   Every function gets a call to _mcount at entry:       │
  │                                                          │
  │   my_function:                                          │
  │     MOV X9, X30              // save LR                 │
  │     BL _mcount               // call tracer             │
  │     ... function body ...                                │
  │                                                          │
  │ At boot (ftrace disabled):                               │
  │   _mcount patched to NOP (no overhead):                 │
  │   my_function:                                          │
  │     MOV X9, X30                                         │
  │     NOP                      // no overhead!            │
  │     ... function body ...                                │
  │                                                          │
  │ When ftrace enabled for a function:                     │
  │   NOP patched back to BL ftrace_caller:                 │
  │   my_function:                                          │
  │     MOV X9, X30                                         │
  │     BL ftrace_caller         // trace active            │
  │     ... function body ...                                │
  └──────────────────────────────────────────────────────────┘

Dynamic patching on ARM64:
  Uses aarch64_insn_patch_text_nosync():
    Write new instruction to address
    IC IVAU (invalidate I-cache for that address)
    DSB ISH (ensure visibility)
    ISB (synchronize pipeline)
  
  Text patching is atomic (single instruction replacement)
  No stop-machine needed on ARM64 (unlike x86)

Usage:
  # List available tracers:
  cat /sys/kernel/debug/tracing/available_tracers
  # → function function_graph nop
  
  # Enable function tracer:
  echo function > /sys/kernel/debug/tracing/current_tracer
  
  # Trace specific function:
  echo schedule > /sys/kernel/debug/tracing/set_ftrace_filter
  echo 1 > /sys/kernel/debug/tracing/tracing_on
  
  # Function graph tracer (shows call graphs with timing):
  echo function_graph > /sys/kernel/debug/tracing/current_tracer
  cat /sys/kernel/debug/tracing/trace
  
  Output:
   0)               |  schedule() {
   0)               |    __schedule() {
   0)   0.500 us    |      rcu_note_context_switch();
   0)   0.250 us    |      pick_next_task_fair();
   0)               |      context_switch() {
   0)   1.200 us    |        switch_mm_irqs_off();
   0)   0.800 us    |        switch_to();
   0)   2.500 us    |      }
   0)   5.000 us    |    }
   0)   5.500 us    |  }

  # Trace events (tracepoints):
  echo 1 > /sys/kernel/debug/tracing/events/sched/sched_switch/enable
  cat /sys/kernel/debug/tracing/trace
  # Shows context switches with PID, priority, etc.
```

---

Back to [Question & Answers Index](./README.md)
