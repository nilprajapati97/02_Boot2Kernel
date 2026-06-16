# Phase 7: Data Structures Initialization

## Overview

Initializes two fundamental kernel data structures used pervasively: the radix tree (now replaced by XArray) and the maple tree (a B-tree variant used for VMA management). These provide the core indexed lookup structures for many kernel subsystems.

## Execution Order

| # | Function | Source File | Description |
|---|----------|-------------|-------------|
| 1 | [`radix_tree_init()`](radix_tree_init/README.md) | `lib/radix-tree.c` | Initialize radix tree slab cache |
| 2 | [`maple_tree_init()`](maple_tree_init/README.md) | `lib/maple_tree.c` | Initialize maple tree slab cache |

## IRQ State

- **Entry**: Disabled
- **Exit**: Disabled

## Memory State

- Full `kmalloc` and `vmalloc` available

## Why These Two?

### Radix Tree / XArray

Used for:
- Page cache (file data pages indexed by page offset)
- IDR (ID allocator — PID → task_struct lookup)
- BPF maps
- Many driver structures

### Maple Tree

Introduced in Linux 6.1 to replace `rb_tree` for VMA management:
- `mm_struct.mm_mt` (maple tree of VMAs)
- Designed for range queries: "find VMA covering address X"
- Better cache locality than rb_tree for this use case

## Function Index

- [radix_tree_init/](radix_tree_init/README.md)
- [maple_tree_init/](maple_tree_init/README.md)
