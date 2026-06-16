# `random_init_early()` — Early CRNG Seeding

## Purpose

Seeds the kernel's Cryptographically Secure Pseudo-Random Number Generator (CRNG) using entropy sources available very early in boot: the kernel command line hash, hardware RNG (if available), and any seed provided by the bootloader. This ensures that random numbers are not trivially predictable from the very start of boot.

## Source File

`drivers/char/random.c`

## Why Early Seeding Matters

The kernel needs random numbers for:
- ASLR/KASLR (already happened in arch code, but needs continuation)
- Stack canary values
- Network sequence number generation
- Heap allocation randomization
- Cryptographic key generation

If the CRNG is not seeded before these operations, an attacker who knows the system was just booted can predict the "random" values.

## Entropy Sources at This Stage

`random_init_early()` uses:

| Source | How | Quality |
|--------|-----|---------|
| Command line hash | `get_random_bytes()` with cmdline as input | Low (predictable, but unique per boot config) |
| Hardware RNG (`RDRAND` on x86) | `arch_get_random_long()` | High (if available) |
| Bootloader-provided seed | `random_get_entropy()` via EFI variable or DT | Medium-High |
| CPU timestamp counter | `get_cycles()` | Low-Medium (timing jitter) |
| Boot ID from UEFI | | Medium |

## The CRNG Architecture

The Linux CRNG is a ChaCha20-based generator:

```
Input Pool (4096 bits of raw entropy)
    ↓ (when sufficiently mixed)
CRNG Key (256-bit ChaCha20 state)
    ↓
Output (via ChaCha20 stream cipher)
    ↓
/dev/urandom, get_random_bytes(), etc.
```

At this early stage, the CRNG may not be "fully initialized" (that happens in `random_init()` at Phase 10), but it is seeded enough to produce non-trivially predictable output.

## Pre-conditions

- `command_line` pointer passed from `start_kernel()`
- `setup_arch()` must have run (provides arch-specific entropy sources)

## Post-conditions

- CRNG has been seeded with best-available early entropy
- `get_random_bytes()` returns useful (if not fully-entropy) data
- `early_boot_irqs_disabled` flag is set (informs CRNG of IRQ-off mode)

## IRQ State

IRQs **disabled** — must use non-IRQ entropy sources only.

## Full CRNG Initialization

The full CRNG initialization (`random_init()`) happens later in Phase 10, after `timekeeping_init()` provides high-resolution timer entropy. The `getrandom()` syscall will block until the CRNG is fully initialized.

## Kconfig Dependencies

- `CONFIG_RANDOM_TRUST_CPU`: If set, RDRAND output is trusted fully (speeds up full initialization)
- `CONFIG_RANDOM_TRUST_BOOTLOADER`: Trust bootloader-provided seed

## Cross-references

- [Phase overview](../README.md)
- `random_init()` — full CRNG init: [../../11_security_randomness/random_init/README.md](../../11_security_randomness/random_init/README.md)
