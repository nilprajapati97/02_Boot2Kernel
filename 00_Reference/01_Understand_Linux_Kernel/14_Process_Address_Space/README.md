# Chapter 14 — Process Address Space

## Overview

Each process has its own **virtual address space** managed by the kernel.

```mermaid
graph TD
    Task["task_struct"] --> MM["mm_struct\n(address space)"]
    MM --> VMA1["vm_area_struct\ntext segment"]
    MM --> VMA2["vm_area_struct\ndata segment"]
    MM --> VMA3["vm_area_struct\nstack"]
    MM --> VMA4["vm_area_struct\nmmap region"]
    MM --> PGD["pgd (page global directory)\nHW page tables"]
```

## Topics

1. [01_mm_struct.md](./01_mm_struct.md)
2. [02_Virtual_Memory_Areas.md](./02_Virtual_Memory_Areas.md)
3. [03_Page_Tables.md](./03_Page_Tables.md)
4. [04_Page_Faults.md](./04_Page_Faults.md)
5. [05_mmap.md](./05_mmap.md)
