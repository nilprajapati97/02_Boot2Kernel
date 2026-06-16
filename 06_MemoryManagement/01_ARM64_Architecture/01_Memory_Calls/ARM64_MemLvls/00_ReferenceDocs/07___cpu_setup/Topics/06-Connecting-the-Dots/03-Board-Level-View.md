# Board-Level View

A board boots through firmware, reset configuration, and CPU release mechanisms before Linux reaches `__cpu_setup`.

## What the board or firmware typically provides

- a powered and released CPU core
- an entry point into the kernel image or bootloader handoff path
- memory map knowledge through firmware conventions or device tree
- a chosen starting exception level depending on platform policy

## What Linux cannot assume blindly

Even if the board boots reliably, Linux still has to establish its own architectural state. It cannot assume that the existing register state is already the one the kernel wants.

## Why `__cpu_setup` matters from a board perspective

Different boards may use different firmware and may boot different ARM64 implementations, but the kernel still needs a predictable EL1 memory-management policy. `__cpu_setup` is part of enforcing that predictability.

## Beginner conclusion

Board bring-up gets the CPU to the door. `__cpu_setup` helps the kernel arrange the room before normal kernel execution begins.
