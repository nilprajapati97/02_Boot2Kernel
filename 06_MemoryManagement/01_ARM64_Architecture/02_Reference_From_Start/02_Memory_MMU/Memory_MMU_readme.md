# Memory Management & MMU — ARM64 Interview Topics
### Targeted: NVIDIA, ARM, AMD, Qualcomm

---

## Category 1 — ARM64 Architecture Fundamentals

> *Asked heavily at ARM, Qualcomm — baseline for everything else*

- Exception Levels: EL0, EL1, EL2, EL3 — roles and transitions
- AArch64 vs AArch32 execution states
- System registers: `MSR`/`MRS` instructions vs ARM32's CP15 `MCR`/`MRC`
- `SCTLR_EL1` — MMU enable, cache enable, alignment check
- Processor modes at each EL (SPSel, stack pointer selection)
- `SCR_EL3` — Secure Configuration Register (NS bit, IRQ/FIQ routing)
- `HCR_EL2` — Hypervisor Configuration Register
- Banked registers per exception level
- `CPACR_EL1` — FP/SIMD/SVE access traps
- AArch64 register file: X0–X30, SP, PC, PSTATE

---

## Category 2 — Virtual Address Space

> *Core MMU theory — asked everywhere*

- 64-bit VA but only 48-bit or 52-bit used — why and how
- `TCR_EL1.T0SZ` and `T1SZ` — controlling VA range size
- TTBR0_EL1 (user) vs TTBR1_EL1 (kernel) — split rationale
- Canonical address rule — the "hole" between user/kernel VA
- VA tagging: `TCR_EL1.TBI0/TBI1` (Top Byte Ignore)
- Linux ARM64 kernel VA layout: linear map, vmalloc, vmemmap, modules, fixed maps
- `PAGE_OFFSET`, `VMALLOC_START`, `VMEMMAP_START` — values and purpose
- User space VA limit — how `mmap`, stack, heap are laid out
- Effect of `TCR_EL1.AS` — 8-bit vs 16-bit ASID
- 52-bit VA/PA with ARMv8.2-LPA — when and how enabled

---

## Category 3 — Page Tables & Translation

> *Deep-dive topic at ARM, Qualcomm — expect whiteboard questions*

- Translation granule sizes: 4 KB, 16 KB, 64 KB — tradeoffs
- Four-level page table: PGD → PUD → PMD → PTE (with 4 KB granule, 48-bit VA)
- VA bit field breakdown per level — know the exact bit ranges
- Block descriptors: 1 GB (L1), 2 MB (L2) — when hardware uses them vs page
- Descriptor types: Invalid (`00`), Block (`01`), Table/Page (`11`)
- All descriptor fields: `AttrIdx`, `AP[2:1]`, `SH`, `AF`, `nG`, `DBM`, `Cont`, `PXN`, `UXN`
- `AF` (Access Flag) — hardware vs software management
- `DBM` (Dirty Bit Management, ARMv8.1) — hardware dirty tracking
- `Cont` hint bit — TLB contiguous hint optimization
- `nG` bit — non-global entries and ASID tagging
- Table walk: hardware page table walker (PTW) — how it interacts with caches
- `TTBR` base address alignment requirements per granule
- Folded page tables in Linux — when PUD/PMD are collapsed
- Page table entry size: 8 bytes (64-bit) vs ARM32's 4 bytes

---

## Category 4 — Translation Control Register (TCR_EL1) Deep Dive

> *Qualcomm, ARM — expect to configure this from scratch*

- `T0SZ`/`T1SZ` — exact formula: VA size = $2^{64-\text{TxSZ}}$
- `TG0`/`TG1` — granule selection encoding (00=4KB, 01=64KB, 10=16KB)
- `IRGN0/1`, `ORGN0/1` — inner/outer cacheability for page table walks
- `SH0/SH1` — shareability domain for page table walks
- `IPS` — Intermediate Physical Address Size (output PA size)
- `EPD0`/`EPD1` — TTBR0/TTBR1 disable (TLB fault on access)
- `A1` bit — which TTBR's ASID field is used
- `HA`/`HD` bits (ARMv8.1) — hardware Access flag / Dirty management enable
- `HPD0`/`HPD1` — Hierarchical Permission Disable
- `HWU` bits — Hardware Use fields in descriptors

---

## Category 5 — Memory Attributes (MAIR_EL1)

> *Critical for driver and BSP engineers — NVIDIA, AMD heavily test this*

- Why MAIR exists: indirection to keep descriptor compact
- 8 attribute slots, `AttrIdx[2:0]` in PTE selects which slot
- Attribute byte encoding: upper nibble (outer) + lower nibble (inner)
- **Device memory types**: nGnRnE, nGnRE, nGRE, GRE — what each letter means
  - G = Gathering, R = Reordering, E = Early Write Acknowledgement
- **Normal memory types**: Write-Back, Write-Through, Non-Cacheable
- Write-Allocate vs no-write-allocate policies
- Why device registers MUST be mapped as Device/Strongly-Ordered
- Consequences of wrong memory type (data corruption, bus hangs)
- Linux `MT_DEVICE_nGnRnE`, `MT_NORMAL`, `MT_NORMAL_NC` — values Linux programs
- Shareability: Non-Shareable, Inner Shareable, Outer Shareable — difference on SMP
- `S` (Shareable) interaction with cache coherency protocol

---

## Category 6 — Access Permissions & Execute Never

> *Security-focused roles — ARM, Qualcomm TrustZone teams*

- `AP[2:1]` encoding table: RW/RO × EL1/EL0 combinations
- `PXN` (Privileged Execute Never) — kernel cannot execute this page
- `UXN`/`XN` (Unprivileged Execute Never) — user cannot execute this page
- W^X (Write XOR Execute) policy — why it matters
- How Linux sets XN for data sections, stack, heap
- Hierarchical permission overrides — table descriptor's `PXNTable`, `XNTable`, `APTable`
- `PSTATE.PAN` (Privileged Access Never, ARMv8.1) — kernel cannot access user pages directly without explicit `uaccess` wrappers
- `PSTATE.UAO` (User Access Override, ARMv8.2)

---

## Category 7 — TLB Architecture & Management

> *Performance-critical — asked at all four companies*

- TLB structure: fully associative, set-associative — microarchitecture tradeoffs
- Separate L1 iTLB and dTLB + unified L2 TLB
- TLB entry contents: VA, PA, ASID, attributes, permissions, size
- ASID-tagged (nG=1) vs Global (nG=0) entries
- ASID allocation and rollover in Linux (`context.c`)
- Generation-based ASID scheme — handling 16-bit ASID exhaustion
- `TLBI` instruction syntax and qualifiers:
  - `VMALLE1IS` — all entries, EL1, Inner Shareable broadcast
  - `VAE1IS` — by VA+ASID
  - `ASIDE1IS` — by ASID only
  - `VAAE1IS` — by VA, all ASIDs
  - `VALE1IS` — last-level only (optimization)
- Mandatory DSB+ISB sequence around TLB invalidations
- `CnP` bit (ARMv8.2) — Common not Private: SMP TLB sharing
- `TLBI RVAAE1IS` (ARMv8.4) — range-based TLB invalidation
- TLB shootdown on SMP — IPI cost, why it matters for performance
- Context switch TLB overhead with vs without ASIDs

---

## Category 8 — Cache Architecture

> *NVIDIA, AMD, Qualcomm — deeply tested for driver/platform roles*

- L1 I-cache, L1 D-cache, L2, L3 — sizes, associativity, inclusivity
- VIPT (Virtually Indexed, Physically Tagged) — ARM's typical L1 D-cache design
- PIPT (Physically Indexed, Physically Tagged) — L2 and above
- Cache aliasing problem with VIPT — conditions and Linux solution
- Cache coherency protocol (MESI/MOESI) in multi-core ARM
- Inner vs Outer Shareable domains — how they map to L1/L2/L3
- `DC CIVAC` — Clean and Invalidate to PoC (Point of Coherency)
- `DC CVAC` — Clean to PoC
- `DC IVAC` — Invalidate (dangerous — only use carefully)
- `IC IALLU` — Invalidate I-cache to PoU (Point of Unification)
- Point of Unification (PoU) vs Point of Coherency (PoC)
- DMA cache coherency: cache flush/invalidate before/after DMA
- Non-cacheable DMA buffers vs cache-coherent DMA (HW coherent interconnects)
- `__dma_flush_area()`, `__dma_inv_area()` in Linux

---

## Category 9 — Memory Ordering & Barriers

> *Hardest category — heavily tested at ARM, NVIDIA for correctness roles*

- ARM64 weakly-ordered memory model (TSO is x86 only)
- Load-Load, Load-Store, Store-Load, Store-Store orderings
- `DMB` (Data Memory Barrier) — variants: `ISH`, `OSH`, `SY`, `LD`, `ST`
- `DSB` (Data Synchronization Barrier) — stronger than DMB
- `ISB` (Instruction Synchronization Barrier) — pipeline flush
- `DMB` vs `DSB` — exact difference and when each is needed
- Acquire/Release semantics: `LDAPR`, `STLR` — ARMv8 Load-Acquire/Store-Release
- `LDAR`/`STLR` vs `LDR`/`STR` + `DMB` — performance difference
- `CAS`, `CASP` (Compare-And-Swap) — ARMv8.1 LSE atomics
- `LDXR`/`STXR` (Load/Store Exclusive) — LL/SC loop implementation
- `WFE`/`WFI`/`SEV`/`SEVL` — wait for event/interrupt primitives
- Kernel `smp_mb()`, `smp_rmb()`, `smp_wmb()` → ARM64 implementation
- `READ_ONCE()`, `WRITE_ONCE()` → compiler barrier + `volatile` interaction
- `rcu_dereference()` / `rcu_assign_pointer()` → memory ordering for RCU

---

## Category 10 — Linux Kernel Memory Management

> *Fundamental for all four companies — kernel/driver roles*

- `mm_struct` — per-process memory descriptor fields
- `vm_area_struct` (VMA) — virtual memory areas, flags, `vm_ops`
- `pgd_t`, `pud_t`, `pmd_t`, `pte_t` — Linux type hierarchy
- `page` struct (`struct page`) — fields: `_refcount`, `_mapcount`, `flags`, `lru`
- Page flags: `PG_locked`, `PG_uptodate`, `PG_dirty`, `PG_writeback`, `PG_referenced`, `PG_active`, `PG_slab`
- `pfn_to_page()`, `page_to_pfn()`, `virt_to_page()` — how implemented with vmemmap
- `phys_to_virt()` / `virt_to_phys()` — linear map arithmetic
- `__pa()`, `__va()` — macros and their limits (only valid for linear map!)
- Page fault handler flow: `do_page_fault()` → `handle_mm_fault()` → `__handle_pte_fault()`
- Fault types: anonymous page fault, file-backed, COW, demand paging
- COW (Copy-on-Write): how `fork()` leverages page table sharing
- Reverse mapping (`rmap`) — `anon_vma`, `address_space` for page reclaim
- `mmap()` / `munmap()` / `mprotect()` — VMA manipulation
- `brk()` — heap expansion, anonymous VMA
- `mlock()` / `munlock()` — pinning pages in RAM
- `madvise()` — `MADV_DONTNEED`, `MADV_HUGEPAGE`, `MADV_SEQUENTIAL`

---

## Category 11 — Memory Allocators

> *OS internals depth — Qualcomm, ARM platform teams*

- **Buddy Allocator**: free lists per order (0–10), splitting and coalescing, fragmentation
- `alloc_pages()`, `__get_free_pages()`, `free_pages()` — GFP flags
- GFP flags: `GFP_KERNEL`, `GFP_ATOMIC`, `GFP_DMA`, `GFP_NOWAIT`, `__GFP_ZERO`, `__GFP_COMP`
- When to use `GFP_ATOMIC` vs `GFP_KERNEL` — interrupt context constraint
- **SLUB Allocator**: slab, per-CPU freelist, partial list, `kmem_cache`
- `kmalloc()` / `kfree()` — size classes, when it's backed by SLUB vs buddy
- `kmem_cache_create()` / `kmem_cache_alloc()` / `kmem_cache_free()`
- SLUB vs SLAB vs SLOB — differences, why SLUB is default
- `vmalloc()` / `vfree()` — when to use, virtual contiguous but physically discontiguous
- `ioremap()` / `iounmap()` — mapping device registers, `noncached_pgprot`
- `kmap()` / `kunmap()` — mapping high memory (legacy, rare on ARM64)
- `dma_alloc_coherent()` — contiguous, cache-coherent DMA buffer
- **CMA (Contiguous Memory Allocator)** — reservation, migration, `dma_alloc_contiguous()`
- Memory compaction — `compact_zone()`, migration of movable pages
- **OOM Killer** — `oom_score_adj`, victim selection algorithm

---

## Category 12 — DMA & IOMMU / SMMU

> *Critical at NVIDIA, AMD, Qualcomm — GPU/SoC roles always ask this*

- Why DMA needs special handling (cache coherency, physical addressing)
- DMA API: `dma_map_single()`, `dma_map_sg()`, `dma_unmap_single()`
- `dma_alloc_coherent()` — always cache-coherent, uses CMA or bounce buffers
- DMA directions: `DMA_TO_DEVICE`, `DMA_FROM_DEVICE`, `DMA_BIDIRECTIONAL`
- Streaming DMA vs coherent DMA — tradeoffs
- Bounce buffers — why needed for devices with limited DMA address range
- **SMMU (System MMU)** — ARM's IOMMU
  - SMMU v1/v2/v3 differences (v3 is the major rewrite)
  - STE (Stream Table Entry) — per-device context
  - CD (Context Descriptor) — per-process PASID context
  - Two stages: Stage 1 (device VA → IOVA), Stage 2 (IOVA → PA)
  - SMMU TLB and `TLBI` commands in SMMU v3
  - PASID (Process Address Space ID) for SVM (Shared Virtual Memory)
- **IOVA (I/O Virtual Address)** — IOMMU domain, `iova_domain`
- `iommu_map()` / `iommu_unmap()` — kernel IOMMU API
- SMMU bypass mode vs translation mode vs abort mode
- **ATS (Address Translation Services)** — PCIe device requests translations
- **PRI (Page Request Interface)** — device page fault handling
- **SVM (Shared Virtual Memory)** — device and CPU share same VA space via SMMU PASID
- Importance for GPU: each GPU process gets own IOMMU context for isolation

---

## Category 13 — Virtualization Memory (Stage 2)

> *ARM, Qualcomm hypervisor/KVM teams*

- IPA (Intermediate Physical Address) — the "guest physical" concept
- Stage 1 (Guest OS controlled) vs Stage 2 (Hypervisor controlled) translation
- `VTTBR_EL2` — Stage 2 page table base, VMID field
- `VTCR_EL2` — Stage 2 translation control (granule, PA size, levels)
- Stage 2 descriptor format differences from Stage 1 (`S2AP`, `MemAttr`)
- `HCR_EL2.VM` bit — enabling Stage 2
- Combined VA→IPA→PA walk — latency impact, TLB caching of combined translations
- VMID (Virtual Machine ID) — analogous to ASID for Stage 2
- EL2 TLB operations: `TLBI VMALLS12E1IS` — both stages
- GPA (Guest Physical Address) fault → hypervisor fault handler → `kvm_handle_guest_abort()`
- KVM/ARM64 architecture: VHE (Virtualization Host Extensions, ARMv8.1)
- VHE — running Linux host kernel at EL2 directly
- Protected KVM (pKVM) — EL2 firmware isolates VM memory from host kernel

---

## Category 14 — Security Extensions

> *ARM TrustZone teams, Qualcomm Secure World engineers*

- TrustZone: Secure vs Non-Secure world, `SCR_EL3.NS`
- ATF (ARM Trusted Firmware) / TF-A — EL3 software, BL1/BL2/BL31/BL32/BL33
- OP-TEE — Open Portable TEE as Secure EL1 OS
- `SMC` instruction — Secure Monitor Call convention (SMCCC)
- TZASC (TrustZone Address Space Controller) — hardware memory firewall on bus
- NS bit in page table descriptors — Secure PA space access from Normal world
- **MTE (Memory Tagging Extension, ARMv8.5)**:
  - Tag granule: 16 bytes, 4-bit tag
  - `TBI` enabling for tagged pointers
  - Sync vs Async fault modes
  - Linux `PROT_MTE` mmap flag, use for heap safety
- **PAC (Pointer Authentication, ARMv8.3)**:
  - Keys: IA, IB, DA, DB, GA
  - `PACIA`/`AUTIA` — sign/verify
  - Linux `CONFIG_ARM64_PTR_AUTH` — kernel return address signing
- **BTI (Branch Target Identification, ARMv8.5)**:
  - `GP` (Guarded Page) bit in PTE
  - `BTI C`, `BTI J`, `BTI JC` — landing pad types
  - `BTYPE` field in PSTATE
- **KPTI (Kernel Page Table Isolation)**:
  - Problem: Meltdown (CVE-2017-5754)
  - `tramp_pg_dir` vs `swapper_pg_dir`
  - Entry/exit trampoline in `entry.S`
  - Performance cost: TTBR1 switch + TLB flush per syscall
  - `nopti` kernel cmdline to disable

---

## Category 15 — KASLR & Kernel Boot Memory Setup

> *Qualcomm BSP, ARM platform firmware engineers*

- **KASLR** — randomized kernel load address at boot
- `kaslr_offset()`, `kimage_vaddr`, relocation processing
- EFI stub randomization vs bootloader (U-Boot) randomization
- `memstart_addr` — physical memory start (not always 0 on ARM64 SoCs)
- `arm64_memblock_init()` — early physical memory map
- `memblock` allocator — pre-buddy-allocator for early boot allocations
- `early_fixmap_init()` — fixed virtual mappings before MMU is fully up
- `paging_init()` — setting up the permanent page tables
- Identity mapping — required during MMU enable sequence
- `__primary_switch` / `__enable_mmu` in `head.S` — exact steps
- TTBR0 = identity map during boot → switched to process tables after init
- Device Tree / ACPI memory nodes → `memblock` reservation
- `reserve_crashkernel()`, `reserve_initrd_mem()` — early memory reservations
- `setup_arch()` call chain on ARM64

---

## Category 16 — Huge Pages & Page Size Optimization

> *Performance engineering at NVIDIA, AMD*

- **THP (Transparent Huge Pages)** — 2 MB PMD-level promotion
  - `khugepaged` daemon — scans and collapses 512×4KB → 1×2MB
  - `MADV_HUGEPAGE`, `MADV_NOHUGEPAGE` hints
  - THP split on `mprotect()` or partial unmap
- **HugeTLB Pages** — statically pre-allocated huge pages
  - `/proc/sys/vm/nr_hugepages`
  - `hugetlbfs` mount, `mmap(MAP_HUGETLB)`
  - 1 GB HugeTLB on ARM64 (L1 block descriptor)
- **Contiguous PTE hint** — ARM64 `Cont` bit optimization
  - 16 PTEs marked contiguous → single TLB entry covers 16×4 KB = 64 KB
  - Linux `pte_cont` / `pmd_cont` support
- TLB reach — why huge pages drastically reduce TLB pressure
- `perf stat -e dTLB-load-misses` — measuring TLB miss impact

---

## Category 17 — NUMA & Multi-Core Memory

> *Server-grade ARM64 — Ampere, AWS Graviton platforms at AMD, NVIDIA*

- NUMA nodes — local vs remote memory latency
- `pgdat` (`pg_data_t`) — per-node structure
- Zone list fallback — `ZONE_MOVABLE` → `ZONE_NORMAL` → remote node
- `alloc_pages_node()`, `numa_node_id()`, `task_node()`
- `numactl`, `mbind()`, `set_mempolicy()` — NUMA memory policy API
- `MPOL_BIND`, `MPOL_PREFERRED`, `MPOL_INTERLEAVE`
- `AutoNUMA` — automatic NUMA balancing via page fault tracking
- `NUMA_BALANCING` — `numa_fault`, page migration to local node
- `sched_domain` and NUMA topology — how scheduler interacts with memory locality
- ARM64 NUMA: `ACPI SRAT table` or DT `numa-node-id` property

---

## Category 18 — Page Reclaim & Swap

> *Memory pressure handling — embedded Linux (Qualcomm) and server (NVIDIA)*

- LRU lists: Active/Inactive Anonymous + Active/Inactive File
- `kswapd` — background reclaim daemon, per-node
- `direct_reclaim` — synchronous reclaim under allocation pressure
- `watermarks`: `min_free_kbytes`, `low`, `high` watermarks per zone
- Page eviction: clean file pages → dirty file pages (writeback) → anonymous (swap)
- `shrink_page_list()` — core reclaim function
- Swap: `swapfile`/`zram`, `get_swap_page()`, `swap_writepage()`
- `zswap` — compressed swap cache in RAM
- `memory.limit_in_bytes` in cgroups v1 / `memory.max` in cgroups v2
- OOM scoring: `oom_score_adj` range (-1000 to +1000), `select_bad_process()`
- `mlock()` / `mlockall()` — preventing page reclaim (real-time use)

---

## Category 19 — Debugging & Profiling Tools

> *Practical knowledge — asked in every company's system debug rounds*

- `/proc/meminfo` — fields: `MemTotal`, `MemFree`, `Buffers`, `Cached`, `AnonPages`, `PageTables`, `HugePages`
- `/proc/PID/maps` and `/proc/PID/smaps` — VMA dump per process
- `/proc/PID/pagemap` — physical page mapping per VA
- `/proc/buddyinfo` — buddy allocator free lists
- `/proc/slabinfo` — SLUB cache statistics
- `/proc/vmstat` — page fault counters, reclaim stats
- `cat /sys/kernel/debug/memblock/memory` — early memory layout
- `perf stat -e cache-misses,dTLB-load-misses,iTLB-load-misses`
- `perf mem` — memory access latency profiling
- `kasan` (Kernel Address Sanitizer) — use-after-free, OOB on ARM64
- `kmemleak` — kernel memory leak detector
- `valgrind`, `AddressSanitizer (ASan)` — user space memory debug
- `crash` utility + `vmcore` — post-mortem kernel crash analysis
- `addr2line`, `gdb` with KASLR offset for kernel symbol resolution
- `ARM CoreSight` — hardware trace for memory access debugging
- `AT S1E1R <addr>` — address translation instruction for debugging mappings

---

## Category 20 — Advanced & Cutting-Edge Topics

> *Differentiating topics for senior roles*

- **ARMv9 additions**: Realm Management Extension (RME), 4th world (Realm)
- **CCA (Confidential Compute Architecture)** — hardware-isolated VMs
- **RME**: Granule Protection Table (GPT), Granule Protection Check (GPC)
- **SVE/SVE2** memory access semantics, predicated loads/stores
- **MPAM (Memory Partitioning and Monitoring, ARMv8.4)** — hardware cache/bandwidth partitioning (analogous to Intel RDT)
- **PBHA (Page Based Hardware Attributes)** — bits [62:59] in PTE, SoC-specific use (Qualcomm uses for metadata)
- **Lockless page table modification** (xchg-based) — Linux `ptep_set_access_flags()`
- **Kernel Samepage Merging (KSM)** — deduplicating anonymous pages via content hash
- **Balloon driver** in VMs — dynamic memory reclaim by hypervisor
- **Virtio-mem** — hotplug memory in VMs
- **PMDK / NVDIMM (DAX)** — persistent memory, `MAP_SYNC`, `ZONE_DEVICE`
- **io_uring** and memory pinning — `IORING_REGISTER_BUFFERS`
- **GUP (Get User Pages)** — pinning user pages for DMA from kernel drivers
  - `pin_user_pages()` vs `get_user_pages()` — pin vs reference counting
- **Folio** (Linux 5.16+) — replacing `struct page` for compound pages

---

## Category 21 — Production Memory Incident Response

> *Operational excellence for senior on-call and performance engineering rounds*

- Incident triage framework: detect, classify, contain, recover, harden
- PSI-driven diagnosis for user-visible stall time
- Reclaim efficiency interpretation (`pgscan` vs `pgsteal`, refault patterns)
- Tail-latency-aware memory tiering and migration guardrails
- TLB shootdown and IPI storm diagnosis/mitigation
- Live memory leak triage and containment strategy
- NUMA imbalance remediation with locality-first policy
- Confidential VM trust-failure recovery workflow

---

## Category 22 — Performance Engineering & Benchmarking

> *Senior performance and platform engineering rounds — NVIDIA, AMD, ARM, Qualcomm*

- Memory performance methodology (bandwidth vs latency-bound classification)
- STREAM benchmarking with correct NUMA and threading methodology
- Cache and TLB analysis with ARM PMU events (`L1D_CACHE_REFILL`, `DTLB_WALK`)
- Latency microbenchmarks via pointer chasing (lat_mem_rd, custom chains)
- False sharing detection with `perf c2c` and `____cacheline_aligned` idioms
- Lock contention impact on memory subsystem (LSE atomics, qspinlock, RCU)
- MPAM hardware partitioning and monitoring via `resctrl`
- Reproducible benchmarking practices and reporting standards

---

## Quick Reference — Topic Priority Matrix

| Category | ARM | Qualcomm | NVIDIA | AMD |
|---|---|---|---|---|
| ARM64 Architecture | ★★★ | ★★★ | ★★ | ★★ |
| Page Tables & TCR | ★★★ | ★★★ | ★★ | ★★ |
| Memory Attributes (MAIR) | ★★★ | ★★★ | ★★★ | ★★★ |
| TLB Management | ★★★ | ★★★ | ★★★ | ★★★ |
| Memory Ordering/Barriers | ★★★ | ★★★ | ★★★ | ★★★ |
| DMA & SMMU/IOMMU | ★★ | ★★★ | ★★★ | ★★★ |
| Linux Kernel MM | ★★★ | ★★★ | ★★★ | ★★★ |
| Allocators (Buddy/SLUB) | ★★ | ★★★ | ★★ | ★★ |
| Cache Architecture | ★★★ | ★★★ | ★★★ | ★★★ |
| Virtualization (Stage 2) | ★★★ | ★★ | ★★ | ★★ |
| Security (MTE/PAC/BTI/KPTI) | ★★★ | ★★★ | ★ | ★ |
| NUMA & Multi-core | ★★ | ★★ | ★★★ | ★★★ |
| Debugging Tools | ★★ | ★★★ | ★★★ | ★★★ |
| Incident Response Playbooks | ★★ | ★★ | ★★★ | ★★★ |
| Performance Engineering & Benchmarking | ★★★ | ★★ | ★★★ | ★★★ |
| Boot & MMU Init | ★★★ | ★★★ | ★ | ★ |

★★★ = Must Know Deeply | ★★ = Strong Understanding | ★ = Awareness Level