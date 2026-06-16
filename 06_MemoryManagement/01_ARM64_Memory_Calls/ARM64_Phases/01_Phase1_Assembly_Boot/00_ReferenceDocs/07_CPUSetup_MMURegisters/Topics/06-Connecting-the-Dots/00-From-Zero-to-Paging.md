# From Zero to Paging

This document connects the board-level view, CPU view, and kernel view.

## Board-level view

A core starts executing from a firmware-defined entry point with limited assumptions. The kernel must establish its own runtime rules.

## CPU view

The CPU initially has no guarantee that the kernel's final translation regime is active. Early assembly must directly program architectural state.

## Kernel view

Linux wants to arrive at `start_kernel` with:

- a known privilege level
- active page tables
- meaningful memory attributes
- correct translation policy
- instruction and data behavior matching kernel expectations

## Where `__cpu_setup` fits in this story

It is the exact point where Linux transforms a generic ARM64 core from "early boot CPU" into "CPU ready for kernel MMU policy activation".
