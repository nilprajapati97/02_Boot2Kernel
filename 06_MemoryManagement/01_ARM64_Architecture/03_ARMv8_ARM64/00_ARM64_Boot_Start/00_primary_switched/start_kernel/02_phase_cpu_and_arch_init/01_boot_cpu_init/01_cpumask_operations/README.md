# `cpumask_t` and CPU Mask Operations

## Overview

`cpumask_t` is a **bitmask** where each bit corresponds to a logical CPU number. Bit N = 1 means CPU N is in the set.

```c
// include/linux/cpumask.h
typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;

// NR_CPUS is a compile-time constant (e.g., 256 on most configs, 8192 for large server configs)
```

## Core Operations

```c
// Set/clear individual CPUs
set_cpu_online(cpu, val);           // atomic set/clear in cpu_online_mask
set_cpu_active(cpu, val);
set_cpu_present(cpu, val);
set_cpu_possible(cpu, val);

// Query
cpu_online(cpu)                     // true if cpu in cpu_online_mask
cpu_possible(cpu)                   // true if cpu in cpu_possible_mask
num_online_cpus()                   // count of online CPUs
num_possible_cpus()                 // max possible CPUs

// Iteration
for_each_online_cpu(cpu)            // iterate over cpu_online_mask
for_each_possible_cpu(cpu)          // iterate over cpu_possible_mask
for_each_present_cpu(cpu)           // iterate over cpu_present_mask

// Bitwise operations
cpumask_and(dst, src1, src2)        // dst = src1 & src2
cpumask_or(dst, src1, src2)         // dst = src1 | src2
cpumask_andnot(dst, src1, src2)     // dst = src1 & ~src2
cpumask_weight(mask)                // popcount (number of set bits)

// First/next CPU
cpumask_first(mask)                 // lowest set bit
cpumask_next(cpu, mask)             // next set bit after cpu
```

## NUMA-Aware CPU Masks

```c
// Per-NUMA-node CPU masks
cpumask_of_node(node)               // CPUs on a specific NUMA node
node_cpumask(node)                  // synonym
cpumask_of_pcibus(pci_bus)          // CPUs nearest to a PCI bus
```

## Performance Consideration

For systems with ≤64 CPUs (the common case), `cpumask_t` fits in a single `unsigned long` and operations are a single instruction. For 128+ CPUs, it uses multiple `unsigned long` words and operations loop over them. Google's Borg scheduler is specifically optimized for 1024-CPU servers where `cpumask_and()` loops over 16 `uint64_t` words.

## Interview Q&A

### Q1: Why is `for_each_possible_cpu` used for per-CPU data allocation instead of `for_each_online_cpu`?
**A:** Per-CPU data (via `DEFINE_PER_CPU` or `alloc_percpu()`) must be allocated for ALL possible CPUs, not just currently online ones. If a CPU hot-plugs in after boot, its per-CPU data area must already exist — you can't resize the allocation on the fly without complex memory management. Using `cpu_possible_mask` at allocation time ensures the memory is pre-reserved for any CPU that might come online later.

### Q2: What is `cpu_die_mask` and when is it used?
**A:** `cpu_die_mask` tracks CPUs that are in the process of dying (being hot-unplugged). During the brief window when a CPU has left `cpu_online_mask` but is still executing `cpu_die()`, other CPUs must not try to send it IPIs or schedule tasks to it. Checking `!cpu_dying(cpu)` prevents races during this transition.
