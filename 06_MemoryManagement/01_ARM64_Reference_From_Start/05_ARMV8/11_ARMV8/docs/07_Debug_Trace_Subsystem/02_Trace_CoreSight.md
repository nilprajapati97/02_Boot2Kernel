# Trace & CoreSight

## 1. What Is Instruction Trace?

Instruction trace records every instruction the CPU executes in real-time, allowing post-mortem analysis without stopping the core.

```
Why trace?
  • Breakpoints/watchpoints: show state at ONE point
  • Trace: shows ENTIRE execution history (every instruction)
  • Non-intrusive: doesn't slow down the CPU
  • Use cases: hard-to-reproduce bugs, timing analysis, code coverage

Trace output volume:
  A core running at 2 GHz executing ~1B instructions/sec
  generates ~100-500 MB/s of compressed trace data.
  → Requires dedicated hardware to capture and compress
```

---

## 2. ETM — Embedded Trace Macrocell

ETM is the per-core trace source. Each core has its own ETM.

```
ETM (v4.x for ARMv8):
  ┌──────────────────────────────────────────────────────────┐
  │  CPU Core Pipeline                                        │
  │    ↓ (instruction commit info)                            │
  │  ┌─────────────────────────────┐                         │
  │  │           ETM               │                         │
  │  │                             │                         │
  │  │  • Instruction tracing      │                         │
  │  │    - PC of every branch     │                         │
  │  │    - Branch taken/not taken │                         │
  │  │    - Exception entry/return │                         │
  │  │                             │                         │
  │  │  • Data tracing (optional)  │                         │
  │  │    - Load/store addresses   │                         │
  │  │    - Data values            │                         │
  │  │                             │                         │
  │  │  • Timestamps               │                         │
  │  │  • Context IDs              │                         │
  │  │  • Virtualization IDs       │                         │
  │  │                             │                         │
  │  │  Compression:               │                         │
  │  │  Only trace branch targets  │                         │
  │  │  + exceptions (not every    │                         │
  │  │  sequential instruction)    │                         │
  │  │  Decoder reconstructs full  │                         │
  │  │  trace from ELF binary      │                         │
  │  └────────────┬────────────────┘                         │
  │               │ Trace data stream                         │
  │               ▼                                           │
  │          ATB (AMBA Trace Bus)                              │
  └──────────────────────────────────────────────────────────┘

ETM trace protocol output format:
  • Trace atoms: E (executed, branch taken), N (not taken)
  • Address packets: target addresses for taken branches
  • Exception packets: exception type, return address
  • Timestamp packets: cycle counts
  • Context packets: CONTEXTIDR (process ID), VMID
```

---

## 3. CoreSight Trace Infrastructure

```
CoreSight connects trace sources to trace sinks:

  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐
  │ETM 0 │ │ETM 1 │ │ETM 2 │ │ETM 3 │   ← Trace Sources
  └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘
     │        │        │        │
  ┌──▼────────▼────────▼────────▼──┐
  │         Trace Funnel            │        ← Merge streams
  │  (Merges multiple ATB inputs   │
  │   into one ATB output stream)  │
  └──────────────┬─────────────────┘
                 │
  ┌──────────────▼─────────────────┐
  │        Replicator               │        ← Duplicate stream
  │  (Copies stream to multiple    │
  │   sinks simultaneously)        │
  └────────┬───────────┬───────────┘
           │           │
  ┌────────▼───┐ ┌─────▼──────┐
  │    TPIU    │ │    ETR     │              ← Trace Sinks
  │ (Trace Port│ │(Trace to   │
  │  Interface)│ │ System RAM)│
  └────────────┘ └────────────┘

Components:
┌──────────┬──────────────────────────────────────────────────────┐
│Component │ Function                                             │
├──────────┼──────────────────────────────────────────────────────┤
│ ETM      │ Per-core trace source (generates trace data)        │
│ STM      │ System Trace Macrocell (software instrumentation)   │
│ Funnel   │ Merges multiple trace streams into one              │
│ Replicator│ Copies one stream to multiple sinks                │
│ TPIU     │ Trace Port Interface Unit (external trace port)    │
│ ETB      │ Embedded Trace Buffer (small on-chip SRAM ~32KB)   │
│ ETR      │ Embedded Trace Router (DMA trace to system RAM)    │
│ CTI      │ Cross-Trigger Interface (sync events across cores) │
│ DAP      │ Debug Access Port (JTAG/SWD entry point)           │
│ ROM Table│ Discovery table (lists all CoreSight components)    │
└──────────┴──────────────────────────────────────────────────────┘
```

---

## 4. Trace Sinks

```
Three main ways to capture trace data:

1. TPIU → External Trace Port
   • High bandwidth (up to 32-bit parallel port or SWO serial)
   • Requires external trace probe (Lauterbach, ARM DSTREAM)
   • Can capture ALL trace data in real-time
   • Most expensive but most capable

2. ETB → On-Chip Buffer
   • Small SRAM buffer (typically 32-64 KB)
   • Circular buffer (overwrites old data)
   • Can only capture last N microseconds of trace
   • Useful for: "what happened just before the crash?"
   • No external hardware needed

3. ETR → System RAM
   • DMA trace data to DDR RAM circular buffer
   • Much larger buffer (megabytes)
   • Uses AXI bus bandwidth (may affect system performance)
   • Linux: perf + CoreSight driver framework
   • Most practical for real-world trace collection

Buffer sizes:
  ETB:  32-64 KB    → ~10 µs of trace at full speed
  ETR:  1-128 MB    → ~100 ms to seconds of trace
  TPIU: unlimited   → real-time streaming to external tool
```

---

## 5. Cross-Trigger Interface (CTI)

```
CTI connects debug events across cores and subsystems:

  ┌────────┐    ┌────────┐    ┌────────┐
  │ Core 0 │    │ Core 1 │    │ Core 2 │
  │ ┌────┐ │    │ ┌────┐ │    │ ┌────┐ │
  │ │CTI │ │    │ │CTI │ │    │ │CTI │ │
  │ └──┬─┘ │    │ └──┬─┘ │    │ └──┬─┘ │
  └────┼───┘    └────┼───┘    └────┼───┘
       │             │             │
  ┌────▼─────────────▼─────────────▼────┐
  │          CTM (Cross-Trigger Matrix) │
  └─────────────────────────────────────┘

Use cases:
  • Breakpoint on Core 0 → halt ALL cores simultaneously
    (needed for multi-core debugging)
  • Exception on Core 1 → trigger trace capture on Core 2
  • PMU overflow on Core 3 → interrupt Core 0

CTI channels (0-3):
  Input triggers: breakpoint hit, watchpoint hit, PMU overflow
  Output triggers: halt, restart, trace enable, interrupt
  
  Configuration: map inputs → channels → outputs
  
  Example: Core 0 breakpoint halts all cores
    CTI_Core0: map BRK → channel 0
    CTI_Core1: map channel 0 → HALT
    CTI_Core2: map channel 0 → HALT
    CTI_Core3: map channel 0 → HALT
```

---

## 6. Linux CoreSight Framework

```
Linux has a full CoreSight driver framework:

  /sys/bus/coresight/devices/
  ├── etm0/          → ETM for core 0
  ├── etm1/          → ETM for core 1
  ├── funnel0/       → Trace funnel
  ├── replicator0/   → Replicator
  ├── tpiu0/         → TPIU
  ├── etb0/          → ETB (on-chip buffer)
  └── etr0/          → ETR (to system RAM)

Using perf with CoreSight:
  # Record instruction trace using ETR sink
  $ perf record -e cs_etm/@etr0/ --per-thread ./my_program
  
  # Decode trace
  $ perf report --stdio
  
  # Full instruction-level trace
  $ perf script --itrace=i1t
  
  Output shows every instruction executed:
    my_program  1234  [cpu]  timestamp:  main+0x10  mov x0, x1
    my_program  1234  [cpu]  timestamp:  main+0x14  add x0, x0, #1
    my_program  1234  [cpu]  timestamp:  main+0x18  bl  helper
    my_program  1234  [cpu]  timestamp:  helper+0x0 stp x29, x30, [sp]
    ...

  # Coverage analysis
  $ perf script --itrace=i1t | sort -u > covered_addrs.txt

OpenCSD: open-source CoreSight trace decoder library
  Used by perf to decode ETM trace packets
```

---

## 7. Trace Filtering

```
ETM can be configured to trace only specific regions:

Address comparators:
  • Trace only within a function (start addr..end addr)
  • Trace only kernel code (0xFFFF...)
  • Exclude library code

Context ID filtering:
  • Trace only a specific process (CONTEXTIDR = PID)

Exception level filtering:
  • Trace EL0 only (user space)
  • Trace EL1 only (kernel)

Event-based control:
  • Start tracing when function_A is entered
  • Stop tracing when function_B returns
  • Trace N instructions after an event

ETM resource allocation:
  ┌─────────────────────────────────────────────────────────┐
  │  Resource           │ Typical count  │ Purpose          │
  ├─────────────────────┼────────────────┼──────────────────┤
  │ Address comparators │ 8-16           │ Address range    │
  │ Context ID comp.    │ 1-3            │ Process filter   │
  │ VMID comparators    │ 1-3            │ VM filter        │
  │ Event resources     │ 4              │ Start/stop       │
  │ Counters            │ 2-4            │ Count-based      │
  │ Sequencer states    │ 4              │ State machine    │
  └─────────────────────┴────────────────┴──────────────────┘
```

---

## 8. Embedded Logic Analyzer (ELA)

```
Some ARM SoCs include ELA for signal-level debugging:

  • Captures internal bus signals (not instructions)
  • Configurable trigger conditions
  • Small buffer (256-4096 entries)
  • Used for: bus protocol debugging, interconnect issues,
    power sequencing, clock domain crossing problems

  ELA is complementary to ETM:
    ETM  → what the CPU executed (instruction level)
    ELA  → what happened on the bus (signal level)
```

---

Next: Back to [Debug & Trace Overview](./README.md) | Continue to [Interconnect Subsystem →](../08_Interconnect_Subsystem/)
