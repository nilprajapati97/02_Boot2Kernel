# Phase 11: Security and Randomness

## Overview

This phase initializes kernel security subsystems: the full CRNG, KFENCE memory safety, stack canaries, key management, and the Linux Security Module (LSM) framework.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`random_init()`](random_init/README.md) | `drivers/char/random.c` | Full CRNG seeding from all entropy sources |
| 2 | [`kfence_init()`](kfence_init/README.md) | `mm/kfence/core.c` | Kernel Electric Fence memory safety |
| 3 | [`boot_init_stack_canary()`](boot_init_stack_canary/README.md) | `arch/x86/include/asm/stackprotector.h` | Stack smashing protection |
| 4 | [`key_init()`](key_init/README.md) | `security/keys/key.c` | Key management service |
| 5 | [`security_init()`](security_init/README.md) | `security/security.c` | LSM framework init |

## IRQ State

- **Entry**: Enabled
- **Exit**: Enabled

## What Gets Secured Here

### CRNG (`random_init`)

The kernel random number generator is fully seeded now that `timekeeping_init()` has provided high-quality entropy from the hardware timer. This enables:
- Secure `get_random_bytes()` from userspace (`/dev/urandom`)
- Stack canaries (next step)
- Key generation

### KFENCE (`kfence_init`)

A sampling-based memory safety detector. Detects use-after-free, heap out-of-bounds, and memory corruption by placing special "guard pages" around selected allocations.

### Stack Canary (`boot_init_stack_canary`)

Places a random 64-bit value (the "canary") at the bottom of each kernel stack. If a stack overflow overwrites the canary, `__stack_chk_fail()` is called → kernel panic before the corruption can be exploited.

### Key Management (`key_init`)

The Linux key retention service — stores cryptographic keys, passwords, and other small secrets in kernel memory, accessible to processes via `add_key()`, `request_key()`, `keyctl()` syscalls.

### LSM (`security_init`)

The Linux Security Module framework — a set of hooks inserted throughout the kernel that allow security modules (SELinux, AppArmor, Smack, TOMOYO) to enforce mandatory access control policies.

## Function Index

- [random_init/](random_init/README.md)
- [kfence_init/](kfence_init/README.md)
- [boot_init_stack_canary/](boot_init_stack_canary/README.md)
- [key_init/](key_init/README.md)
- [security_init/](security_init/README.md)
