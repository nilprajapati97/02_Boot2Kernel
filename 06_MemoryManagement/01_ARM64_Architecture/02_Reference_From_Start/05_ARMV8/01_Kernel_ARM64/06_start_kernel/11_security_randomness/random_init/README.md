# `random_init()` — Full CRNG Initialization

## Purpose

Completes the kernel CRNG (Cryptographically secure Random Number Generator) initialization. Seeds the generator with high-quality entropy from hardware timers, CPU RNG, and any other available sources to establish a fully initialized random state.

## Source File

`drivers/char/random.c`

## Relationship to `random_init_early()`

```
random_init_early() [Phase 3]:
  - Called before timekeeping
  - Seeds from: cmdline hash, RDRAND, bootloader seed, cycle counter
  - CRNG state: "partial init" — usable but not cryptographically strong

random_init() [Phase 11]:
  - Called after timekeeping_init()
  - Seeds from: timer jitter, RDRAND, RDSEED, all early sources
  - CRNG state: "fully initialized"
```

## Entropy Sources Used

```c
void __init random_init(const char *command_line)
{
    ktime_t now = ktime_get_real();
    
    // 1. Current real time (high entropy after timekeeping_init)
    _mix_pool_bytes(&now, sizeof(now));
    
    // 2. CPU-specific entropy (from RDRAND/RDSEED on x86)
    for_each_possible_cpu(cpu) {
        if (arch_get_random_seed_long(&rv) || arch_get_random_long(&rv))
            _mix_pool_bytes(&rv, sizeof(rv));
    }
    
    // 3. More cycle counter jitter
    add_interrupt_randomness_early();
    
    // Mark CRNG as fully initialized
    crng_init = CRNG_READY;
    crng_init_time = jiffies;
    
    // Wake up any processes waiting for /dev/random
    wake_up_interruptible(&crng_init_wait);
}
```

## The ChaCha20 CRNG

The CRNG state is a 256-bit ChaCha20 key:

```c
struct crng {
    u32 state[16];        // ChaCha20 state: constant + key + counter + nonce
    unsigned long init_time; // When this was last re-seeded
};
```

Output generation:
```c
// Generate 64 bytes of output from current CRNG state:
chacha20_block(&crng.state[0], output);
crng.state[12]++;  // Increment counter
```

## CRNG States

```c
enum {
    CRNG_EMPTY  = 0,  // Not seeded
    CRNG_EARLY  = 1,  // Partially seeded (random_init_early() done)
    CRNG_READY  = 2,  // Fully seeded (random_init() done)
};
```

## Blocking Reads

`/dev/random` (historically) blocked until `CRNG_READY`. Since kernel 5.18, `/dev/random` and `/dev/urandom` behave identically (both return output from CRNG). The `getrandom()` syscall with `GRND_RANDOM` still waits for `CRNG_READY`.

## Entropy Accumulation

The kernel continuously accumulates entropy from:
- Interrupt timing (via `add_interrupt_randomness()`)
- Disk I/O completion timing
- Hardware events (mouse, keyboard)
- RDRAND/RDSEED (CPU hardware RNG)

This keeps the CRNG re-seeded over time.

## Cross-references

- [Phase overview](../README.md)
- `random_init_early()`: [../../03_parameter_parsing/random_init_early/README.md](../../03_parameter_parsing/random_init_early/README.md)
- `boot_init_stack_canary()`: [../boot_init_stack_canary/README.md](../boot_init_stack_canary/README.md) — uses random bytes
