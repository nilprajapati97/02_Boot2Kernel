# CPU versus MMU Interaction

A common beginner question is whether the CPU and MMU are separate or whether one controls the other.

## Simple answer

The MMU is part of the CPU's memory-access machinery. The CPU executes instructions, but address translation and memory attribute enforcement are carried out using the MMU logic configured by system registers.

## What `__cpu_setup` is really doing

The function is not changing C variables. It is programming the CPU's translation behavior.

That means when it writes `MAIR_EL1` and `TCR_EL1`, it is teaching the CPU how to interpret page tables once translation becomes active.

## Why barriers matter

The CPU pipeline, memory system, and translation logic are tightly related. Changing control registers is not enough by itself. Barrier instructions make the order and visibility of these state changes architecturally safe.
