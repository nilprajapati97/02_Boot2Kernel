# `calibrate_delay()` — BogoMIPS Measurement

## Purpose

Measures the speed of the CPU's busy-wait loop — producing the famous "BogoMIPS" (Bogus MIPS) value visible in boot messages and `/proc/cpuinfo`. The result is stored in `loops_per_jiffy`, used by `udelay()` and `ndelay()` for precise short delays.

## Source File

`init/calibrate.c`

```c
void __init calibrate_delay(void)
{
    unsigned long lpj;
    static bool printed;
    
    // Try to get loops_per_jiffy from known CPU frequency:
    lpj = calibrate_delay_is_known();
    if (!lpj) {
        // Binary search: how many loops fit in one jiffy?
        lpj = calibrate_loops_per_jiffy();
    }
    
    loops_per_jiffy = lpj;
    
    if (!printed)
        pr_info("%lu.%02lu BogoMIPS (lpj=%lu)\n",
                loops_per_jiffy / (500000 / HZ),
                (loops_per_jiffy / (5000 / HZ)) % 100,
                loops_per_jiffy);
    printed = true;
}
```

## The Algorithm

### Binary Search Phase

```c
// Find loops_per_jiffy using binary search:
// Start with a small value, double until we overshoot one jiffy,
// then binary search between previous value and current.

lpj = 1;
while ((lpj <<= 1) != 0) {
    ticks = jiffies;
    while (ticks == jiffies)  // Wait for jiffy transition
        ;
    ticks = jiffies;
    __delay(lpj);
    if (ticks != jiffies)  // Took more than one jiffy
        break;
}
// Now binary search between lpj/2 and lpj
```

### `__delay()` Inner Loop

```c
// The actual busy-wait (arch-specific):
void __delay(unsigned long loops)
{
    // On x86:
    asm volatile("1: dec %0; jnz 1b" : "+r" (loops));
}
```

## BogoMIPS Formula

$$\text{BogoMIPS} = \frac{\text{loops\_per\_jiffy} \times HZ \times 2}{1{,}000{,}000}$$

The `×2` is because each loop iteration is approximately 2 MIPS-equivalent instructions on x86.

A typical modern Intel CPU at 3GHz:
```
loops_per_jiffy ≈ 7,500,000  (at HZ=250)
BogoMIPS ≈ 7,500,000 × 250 × 2 / 1,000,000 ≈ 3750
```

Boot message:
```
Calibrating delay loop (skipped), value calculated using timer frequency.. 
5385.83 BogoMIPS (lpj=10771661)
```

## Uses of `loops_per_jiffy`

```c
// udelay(n) — delay n microseconds:
void udelay(unsigned long usecs)
{
    unsigned long loops = usecs * loops_per_jiffy * HZ / 1000000;
    __delay(loops);
}

// ndelay(n) — delay n nanoseconds:
void ndelay(unsigned long nsecs)
{
    unsigned long loops = nsecs * loops_per_jiffy * HZ / 1000000000;
    __delay(loops);
}
```

These are used by hardware drivers for tiny delays (e.g., I/O port strobe timing).

## Modern Optimization

On TSC-capable CPUs, `calibrate_delay()` can use `tsc_khz` directly instead of the jiffy-measurement loop, providing faster and more accurate calibration:

```c
lpj = calibrate_delay_is_known();
// Returns loops_per_jiffy based on cpu_khz if available
```

## Cross-references

- [Phase overview](../README.md)
- `time_init()`: [../../10_timekeeping_timers/time_init/README.md](../../10_timekeeping_timers/time_init/README.md) — TSC/HPET calibration
