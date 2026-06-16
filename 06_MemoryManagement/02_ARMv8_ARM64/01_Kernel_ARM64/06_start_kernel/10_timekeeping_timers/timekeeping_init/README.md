# `timekeeping_init()` — Wall Clock and Monotonic Time

## Purpose

Initializes the kernel's timekeeping subsystem: reads the current time from the hardware Real-Time Clock (RTC), sets up the `timekeeper` structure with the selected clock source, and initializes CLOCK_REALTIME, CLOCK_MONOTONIC, and other POSIX clocks.

## Source File

`kernel/time/timekeeping.c`

```c
void __init timekeeping_init(void)
{
    struct timespec64 wall_time, boot_offset;
    struct clocksource *clock;
    unsigned long flags;
    
    // Read current time from RTC
    read_persistent_clock64(&wall_time);
    
    raw_spin_lock_irqsave(&timekeeper_lock, flags);
    write_seqcount_begin(&tk_core.seq);
    
    ntp_init();  // NTP state machine initialization
    
    // Select the best available clocksource
    clock = clocksource_default_clock();
    
    // Initialize the timekeeper
    tk_setup_internals(tk, clock);
    
    // Set wall time from RTC
    tk_set_wall_to_mono(tk, wall_time);
    
    write_seqcount_end(&tk_core.seq);
    raw_spin_unlock_irqrestore(&timekeeper_lock, flags);
    
    // Initialize boot time offset
    timekeeping_update(tk, TK_MIRROR | TK_CLOCK_WAS_SET);
}
```

## The `timekeeper` Structure

```c
struct timekeeper {
    struct tk_read_base    tkr_mono;    // CLOCK_MONOTONIC reader state
    struct tk_read_base    tkr_raw;     // CLOCK_MONOTONIC_RAW
    
    u64        xtime_sec;              // Wall time (seconds)
    unsigned long xtime_nsec;          // Wall time (sub-second, shifted)
    
    struct timespec64 wall_to_monotonic; // offset: REALTIME → MONOTONIC
    
    ktime_t            offs_real;      // Offset to CLOCK_REALTIME
    ktime_t            offs_boot;      // Offset to CLOCK_BOOTTIME
    ktime_t            offs_tai;       // Offset to CLOCK_TAI
    
    s32            tai_offset;         // TAI - UTC offset (seconds)
    unsigned int   clock_was_set_seq;  // Sequence for set-clock detection
    
    /* NTP state */
    s64            ntp_error;
    u32            ntp_error_shift;
    u32            ntp_tick_length;
};
```

## Clock Monotonicity

The kernel maintains the invariant that CLOCK_MONOTONIC **never goes backward**:

```
CLOCK_REALTIME = wall time (can jump on ntpdate/settimeofday)
CLOCK_MONOTONIC = wall time - wall_to_monotonic offset (never jumps)
CLOCK_BOOTTIME = monotonic + time spent in suspend
CLOCK_TAI = wall time + tai_offset (atomic time, no leap seconds)
```

## The `xtime_nsec` Trick

The nanosecond part is stored shifted left by `tkr_mono.shift` bits for precision:

```
actual_nsec = xtime_nsec >> shift

// This allows sub-nanosecond accumulation without floating point:
xtime_nsec += (cycles_elapsed * mult) >> shift
// where mult is calibrated so that cycles_per_second * mult == 1<<shift nanoseconds
```

## Reading Time — `ktime_get()`

```c
ktime_t ktime_get(void)
{
    struct timekeeper *tk = &tk_core.timekeeper;
    
    do {
        seq = read_seqcount_begin(&tk_core.seq);
        
        base = ktime_to_ns(tk->tkr_mono.base);
        
        // Read hardware counter and convert to ns
        nsecs = timekeeping_get_ns(&tk->tkr_mono);
        
    } while (read_seqcount_retry(&tk_core.seq, seq));
    
    return ns_to_ktime(base + nsecs);
}
```

The `seqcount` ensures consistency if a timer interrupt updates the timekeeper mid-read.

## NTP Integration

After `timekeeping_init()`, the NTP daemon can adjust time via `adjtimex()`:
- Fine adjustments: `time_adj` variable (frequency adjustment)
- Coarse adjustments: `clock_adjtime()` (phase adjustment)

## Pre-conditions

- RTC accessible (via ACPI or direct port I/O)
- A clocksource registered (initially the PIT or HPET)

## Post-conditions

- `ktime_get()`, `ktime_get_real()`, `ktime_get_boottime()` functional
- `gettimeofday()`, `clock_gettime()` ready (after syscall infrastructure)
- NTP state machine initialized

## Cross-references

- [Phase overview](../README.md)
- `time_init()`: [../time_init/README.md](../time_init/README.md) — arch clocksource setup
