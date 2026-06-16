# Phase 13: Console and Locking Infrastructure

## Overview

This phase initializes the kernel console (enabling `printk` output to appear on screen/serial), lock validation (lockdep), and runs locking self-tests.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`console_init()`](console_init/README.md) | `kernel/printk/printk.c` | Register console drivers |
| 2 | [`lockdep_init()`](lockdep_init/README.md) | `kernel/locking/lockdep.c` | Lock dependency tracking |
| 3 | [`locking_selftest()`](locking_selftest/README.md) | `lib/locking-selftest.c` | Verify locking primitives |

## IRQ State

- **Entry**: Enabled
- **Exit**: Enabled

## Importance of `console_init()`

Before `console_init()`, printk messages accumulate in the ring buffer but are not displayed anywhere. After `console_init()`, registered console drivers (VGA, serial, netconsole) start flushing the buffer.

This is the moment boot messages first appear on the screen or serial console in a production kernel (unless `earlycon` was configured).

## Function Index

- [console_init/](console_init/README.md)
- [lockdep_init/](lockdep_init/README.md)
- [locking_selftest/](locking_selftest/README.md)
