# Q21 — Design a Kernel Profiling System (Like perf/eBPF-lite)

---

## 1. Problem Statement

A kernel profiling system must answer: "Where is the CPU spending its time, and why?" At NVIDIA scale, profiling must:
- Attribute CPU time to kernel functions, system calls, and user code.
- Collect hardware performance counter data (cache misses, branch mispredictions, IPC).
- Produce call graphs (flame graphs) without instrumenting every function.
- Run with minimal overhead in production (< 1% CPU).
- Support GPU-CPU correlation: align GPU kernel execution timelines with CPU traces.

Design a complete profiling system: event sources, sampling engine, call chain collection, aggregation, and export.

---

## 2. Requirements

### 2.1 Functional Requirements
- CPU time profiling: statistical sampling at configurable frequency (100–10000 Hz).
- Hardware PMU events: cycles, instructions, cache-misses, branch-misses, etc.
- Call chain collection: user-space + kernel-space stack unwind per sample.
- Software events: context switches, page faults, CPU migrations.
- eBPF programs: attach to perf events for in-kernel aggregation.
- Export: mmap ring buffer to user space (zero-copy sample export).

### 2.2 Non-Functional Requirements
- Sampling overhead: < 1% CPU at 1000 Hz sampling.
- PMU counter precision: ± 1 event accuracy (Intel PEBS / AMD IBS).
- Call chain depth: up to 64 frames.
- Simultaneous profilers: support multiple perf_event instances per CPU.

---

## 3. Constraints & Assumptions

- x86-64 with Intel Architectural PMU v4 (Skylake+).
- Linux `perf_event` subsystem (`kernel/events/`).
- Intel PEBS (Precise Event-Based Sampling) for precise IP.
- Frame pointer-based or ORC (Oops Rewind Capability) unwinding for kernel stack.
- DWARF-based unwinding for user-space stack.

---

## 4. Architecture Overview

```
  ┌──────────────────────────────────────────────────────────────┐
  │  Event Sources                                               │
  │  ┌──────────────┐ ┌──────────────┐ ┌───────────────────┐   │
  │  │ Hardware PMU │ │ Software     │ │ Tracepoints /      │   │
  │  │ Cycles, LLC  │ │ Events:      │ │ kprobes / uprobes  │   │
  │  │ miss, DTLB   │ │ ctx-switch,  │ │ (via trace_event)  │   │
  │  │ PEBS precise │ │ page-fault   │ │                    │   │
  │  └──────┬───────┘ └──────┬───────┘ └────────┬───────────┘  │
  └─────────┼────────────────┼─────────────────-┼──────────────┘
            │                │                  │
            ▼                ▼                  ▼
  ┌──────────────────────────────────────────────────────────────┐
  │  perf_event core (kernel/events/core.c)                      │
  │  perf_event_open() syscall                                   │
  │  ┌──────────────────────────────────────────────────────┐   │
  │  │  perf_event struct: counter, sample_type, overflow   │   │
  │  │  perf_sample_data: IP, PID, time, call chain         │   │
  │  └──────────────────────────────────────────────────────┘   │
  └─────────────────────────┬────────────────────────────────────┘
                            │
                ┌───────────┼───────────────┐
                ▼           ▼               ▼
  ┌─────────────────┐ ┌─────────────┐ ┌────────────────────┐
  │ mmap ring buffer│ │ BPF program │ │ perf_event_output  │
  │ (user reads w/  │ │ aggregate   │ │ to cgroup/system   │
  │  no syscall)    │ │ in-kernel   │ │ wide ring          │
  └─────────────────┘ └─────────────┘ └────────────────────┘
```

---

## 5. Core Data Structures

### 5.1 perf_event

```c
struct perf_event {
    struct pmu              *pmu;          /* PMU driver (Intel, AMD, software) */
    struct perf_event_attr   attr;         /* user-provided configuration */
    struct hw_perf_event     hw;           /* hardware state: MSRs, PEBS buffer */

    /* Sampling */
    u64                      sample_period;
    u64                      samples_left; /* countdown to next sample */
    atomic64_t               count;        /* event count so far */

    /* Output ring */
    struct ring_buffer       *rb;          /* per-event or shared mmap ring */

    /* BPF program attached to this event */
    struct bpf_prog          *prog;

    /* Context */
    struct perf_event_context *ctx;        /* per-task or per-CPU context */
    struct task_struct        *owner;
};

struct perf_event_attr {
    __u32  type;              /* PERF_TYPE_HARDWARE, SOFTWARE, TRACEPOINT */
    __u64  config;            /* PERF_COUNT_HW_CPU_CYCLES, etc. */
    __u64  sample_period;     /* fire every N events */
    __u64  sample_freq;       /* OR: fire N times/sec (auto-adjusts period) */
    __u64  sample_type;       /* what to record: IP, PID, TIME, CALLCHAIN... */
    __u8   exclude_kernel;    /* count only user-space events */
    __u8   exclude_user;
    __u8   precise_ip;        /* 0=no skid, 1=constant skid, 2=PEBS precise */
    __u8   mmap;              /* include mmap events in sample stream */
    __u8   comm;              /* include comm (process name) change events */
};
```

### 5.2 perf_sample_data

```c
struct perf_sample_data {
    u64                      type;       /* which sample_type fields are valid */
    u64                      ip;         /* instruction pointer at sample */
    struct {
        u32  pid, tid;
    }                        tid_entry;
    u64                      time;       /* ktime_get_ns() */
    u64                      addr;       /* address (for mem sampling) */
    u64                      id;         /* event ID */
    u64                      stream_id;
    struct {
        u32  cpu, reserved;
    }                        cpu_entry;
    struct perf_callchain_entry *callchain; /* stack frames */
    struct perf_raw_record   *raw;       /* raw PMU sample data (PEBS record) */
};
```

### 5.3 mmap Ring Buffer

```c
struct perf_mmap_data {
    struct perf_event_mmap_page *user_page; /* control page (mmap page 0) */
    void                        *data_pages[]; /* sample data pages */
    int                          nr_pages;
    /* Lock-free producer-consumer: */
    /* user_page->data_head = producer (kernel writes) */
    /* user_page->data_tail = consumer (user space writes after reading) */
};

struct perf_event_mmap_page {
    __u32  version;
    __u32  compat_version;
    __u32  lock;              /* seqlock for concurrent access */
    __u64  index;             /* PMU counter hardware register index */
    __s64  offset;            /* add to hardware counter read to get event count */
    __u64  time_enabled;
    __u64  time_running;
    __u64  data_head;         /* head of sample ring (bytes, mod data_size) */
    __u64  data_tail;         /* tail: user updates after consuming samples */
    __u64  data_offset;       /* offset from mmap start to ring data */
    __u64  data_size;         /* ring buffer size in bytes */
};
```

---

## 6. Key Algorithms & Design Decisions

### 6.1 Statistical Sampling — PMI Handler

```c
/* When PMU counter overflows (count reaches 0 from -period): */
/* CPU generates PMI (Performance Monitoring Interrupt = NMI on x86) */

void perf_event_nmi_handler(struct perf_event *event, struct pt_regs *regs)
{
    struct perf_sample_data data;

    /* 1. Capture instruction pointer */
    perf_sample_data_init(&data, 0, event->hw.last_period);
    data.ip = instruction_pointer(regs);
    data.time = ktime_get_ns();
    data.tid_entry.pid = current->tgid;
    data.tid_entry.tid = current->pid;

    /* 2. Collect call chain (kernel + user) */
    if (event->attr.sample_type & PERF_SAMPLE_CALLCHAIN) {
        data.callchain = perf_callchain(event, regs);
        /* kernel: ORC unwinder traverses _THIS_IP_, return addresses */
        /* user: get_user_pages() + stack walk via frame pointers or DWARF */
    }

    /* 3. Run BPF program (if attached) for in-kernel aggregation */
    if (event->prog)
        bpf_prog_run(event->prog, &data);

    /* 4. Write sample to mmap ring */
    perf_output_sample(&handle, &data, event);

    /* 5. Reload PMU counter */
    perf_event_update_userpage(event);
}
```

### 6.2 Adaptive Sampling Frequency

For `sample_freq` mode (not `sample_period`):
```
Kernel adjusts period dynamically to hit target frequency:
    if actual_rate > target_freq * 1.1:
        period = period * 1.1   (less frequent)
    if actual_rate < target_freq * 0.9:
        period = period * 0.9   (more frequent)

This prevents PMI storms under high-event-rate conditions.
```

### 6.3 PEBS — Precise Event-Based Sampling

Standard PMI has "skid": the instruction at interrupt might be 10–100 instructions after the faulting instruction. PEBS eliminates skid:

```
PEBS hardware records the precise IP, register state, and memory address
    AT THE EXACT INSTRUCTION that caused the counter overflow.

Setup:
    Write to IA32_PEBS_ENABLE MSR to enable PEBS for a counter
    Provide PEBS buffer (DS — Debug Store) in IA32_DS_AREA MSR
    When PEBS buffer fills → DS interrupt → kernel drains PEBS records
    Each PEBS record contains: precise IP, GP registers, PEBS metadata
```

PEBS critical for memory access profiling: identifies exact load/store instructions with cache misses.

### 6.4 Kernel Stack Unwinding — ORC

Frame-pointer-based unwinding requires `-fno-omit-frame-pointer` (performance cost). ORC is compile-time generated:

```
ORC table: array of (PC_range → unwind_hint) entries
Each entry describes: where SP is, where the return address is at this PC.

Unwind algorithm:
    current_ip = regs->ip
    while current_ip in kernel text:
        orc = lookup_orc_entry(current_ip)   /* binary search */
        sp = compute_sp(regs, orc)
        ret_addr = *(sp + orc->sp_offset)
        push ret_addr onto call_chain
        current_ip = ret_addr
```

ORC is faster than DWARF (no complex bytecode interpretation) and more reliable than frame pointers.

### 6.5 BPF Profiling — In-Kernel Aggregation

```c
/* Example: BPF program attached to CPU cycles event */
/* Builds a histogram of time spent in each kernel function */

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, u64);        /* instruction pointer */
    __type(value, u64);      /* count */
    __uint(max_entries, 4096);
} ip_hist SEC(".maps");

SEC("perf_event")
int profile_ip(struct bpf_perf_event_data *ctx)
{
    u64 ip = ctx->regs.ip;
    u64 *count = bpf_map_lookup_elem(&ip_hist, &ip);
    if (count)
        (*count)++;
    else {
        u64 init = 1;
        bpf_map_update_elem(&ip_hist, &ip, &init, BPF_ANY);
    }
    return 0;
}

/* Result: userspace reads ip_hist once/sec → resolves IPs via /proc/kallsyms */
/* vs: emitting 10M individual sample records to user space */
```

---

## 7. Trade-off Analysis

| Sampling Type | Overhead | Precision | Use Case |
|---|---|---|---|
| Statistical (1000 Hz) | < 1% | Statistical (±1%) | Production profiling |
| PEBS precise | 1–5% | Exact IP, exact address | Memory bottleneck analysis |
| BPF aggregation | 0.1–0.5% | Per-event | Custom metrics, in-kernel histograms |
| Function trace | 10–50% | 100% | Deep debugging only |
| Intel PT (trace) | 5–15% | Full instruction trace | Exact root cause analysis |

---

## 8. Real Linux Kernel References

| Component | Source | Symbol |
|---|---|---|
| perf_event core | `kernel/events/core.c` | `perf_event_open()`, `perf_output_sample()` |
| Intel PMU driver | `arch/x86/events/intel/core.c` | `intel_pmu_handle_irq()`, `intel_pmu_pebs_enable()` |
| ORC unwinder | `arch/x86/kernel/unwind_orc.c` | `unwind_next_frame()`, `lookup_unwind_table()` |
| Call chain | `kernel/events/callchain.c` | `perf_callchain_kernel()`, `perf_callchain_user()` |
| mmap ring buffer | `kernel/events/ring_buffer.c` | `perf_output_begin()`, `perf_output_end()` |
| BPF perf event | `kernel/bpf/tracing.c` | `bpf_perf_prog_read_value()` |
| perf_event_attr | `include/uapi/linux/perf_event.h` | `PERF_TYPE_*`, `PERF_COUNT_HW_*` |

---

## 9. Failure Modes & Debug Strategies

### 9.1 perf: too many open files
```bash
# Profiling all CPUs × all events exhausts file descriptors
ulimit -n 65536
# Or: use perf stat -a (system-wide, single fd per event)
```

### 9.2 Samples Missing in User Space
```bash
# Ring buffer overflowed: user space not reading fast enough
# Check: perf stat --no-big-num 2>&1 | grep "lost"
# Fix: increase mmap pages: perf record -m 256 (256 pages = 1MB)
```

### 9.3 Incorrect Call Chain
```bash
# Missing frames: binary not compiled with -fno-omit-frame-pointer
# Fix for user-space: use DWARF unwinding: perf record --call-graph dwarf
# Kernel: ORC always available (kernel compiled with CONFIG_UNWINDER_ORC)
```

---

## 10. Performance Considerations

- **NMI-based PMI:** PMI is NMI on x86 — it fires even in critical sections. This makes sampling truly non-intrusive but requires NMI-safe handlers.
- **Multiplexing:** When events > available PMU counters, the kernel time-multiplexes events (round-robin scheduling). `time_enabled / time_running` ratio gives scaling factor.
- **User-space counter read:** `perf_event_mmap_page.offset + rdpmc()` — read PMU counter directly from user space without syscall.
- **BPF map lock:** `BPF_MAP_TYPE_PERCPU_HASH` eliminates contention in per-CPU sampling scenarios.

---

## 11. Interview Answer Strategy (NVIDIA 10-yr Level)

**What they want to hear:**
1. perf_event subsystem: single API for hardware PMU, software events, tracepoints.
2. PMI → NMI path: sample capture is non-preemptible, NMI-safe code only.
3. ORC unwinder: compile-time table, O(log N) lookup per frame, no frame pointer required.
4. PEBS: precise IP for memory access profiling (cache miss attribution).
5. Adaptive sampling frequency: prevents PMI storms under heavy event load.
6. BPF aggregation: histogram in-kernel eliminates per-sample user-space copy.
7. mmap ring: lock-free producer (kernel) / consumer (user) — data_head / data_tail protocol.
8. rdpmc from user space: zero-syscall counter read for hot profiling loops.
