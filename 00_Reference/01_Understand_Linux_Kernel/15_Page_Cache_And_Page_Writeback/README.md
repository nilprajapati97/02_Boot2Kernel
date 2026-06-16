# Chapter 15 — Page Cache and Page Writeback

## Overview

The **page cache** caches file data in RAM to speed up file I/O.

```mermaid
graph TD
    App["Application read()"] -->|Cache Hit| PC["Page Cache\n(in-memory pages)"]
    App -->|Cache Miss| PC
    PC -->|Miss: read from disk| Disk["Block Device"]
    App["Application write()"] --> PC
    PC -->|dirty pages| WB["Writeback\n(pdflush / kworker)"]
    WB --> Disk
```

## Topics

1. [01_Page_Cache_Overview.md](./01_Page_Cache_Overview.md)
2. [02_address_space.md](./02_address_space.md)
3. [03_Writeback_Mechanism.md](./03_Writeback_Mechanism.md)
4. [04_Dirty_Page_Tracking.md](./04_Dirty_Page_Tracking.md)
5. [05_pdflush_kworker.md](./05_pdflush_kworker.md)
