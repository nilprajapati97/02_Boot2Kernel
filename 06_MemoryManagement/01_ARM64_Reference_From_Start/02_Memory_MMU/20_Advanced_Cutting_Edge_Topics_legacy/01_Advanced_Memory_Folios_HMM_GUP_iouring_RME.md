# Advanced Memory Topics: Folios, HMM, GUP, io_uring, ARMv9 RME

**Category**: Advanced and Cutting-Edge Topics  
**Platform**: ARM64 (AArch64)

---

## 1. Struct Folio (Linux 5.16+)

```
struct folio: replacement for compound struct page in Linux

Background - struct page problem:
  Old: struct page represents 1 page (4KB)
  Compound page: 2^order contiguous pages linked together
    head page: full struct page, additional compound fields
    tail pages: minimal struct page, point back to head
  Problem: every kernel function must check if page is compound,
           call compound_head(page), handle tail pages specially
           → complex, error-prone code everywhere

struct folio:
  New abstraction: guaranteed-head page (never a tail page)
  folio = first page of a compound page (or single 4KB page for order-0)
  folio_order(folio): the order of the compound page
  folio_size(folio): total bytes = PAGE_SIZE << folio_order(folio)
  folio_nr_pages(folio): number of pages = 1 << folio_order(folio)
  
  Conversion:
    page_folio(page): → folio (may be any page in compound; returns head folio)
    folio_page(folio, n): → struct page n within folio
    &folio->page: first page of folio
  
  Why better:
    folio_get(folio): safe reference count (always on head)
    folio_put(folio): safe release
    No compound_head() checks needed in hot paths
    Type safety: passing folio_t to function that expects folio

Folio in page cache:
  Old: page cache uses struct page, special THP compound handling
  New: page cache uses struct folio directly
  One folio can represent: single 4KB page OR 2MB THP in page cache
  
  struct address_space:
    xarray i_pages: stores folio pointers (not raw page pointers)
    xa_tagged for dirty, writeback state
  
  Key functions:
    filemap_get_folio(): get folio from page cache
    folio_add_to_page_cache(): add folio to page cache
    folio_mark_dirty(): mark folio dirty
    folio_wait_locked(): wait for folio I/O to complete
    filemap_grab_folio(): get or create folio in page cache

Benefits of folio conversion:
  ~6% improvement in some workloads (fewer indirect pointer chases)
  Cleaner semantics: file reads return folio (no implicit tail page)
  Foundation for large folio support (mmap'd files with 2MB+ pages)
```

---

## 2. HMM (Heterogeneous Memory Management)

```
HMM: allow GPU/accelerator to access CPU process virtual memory

Problem without HMM:
  GPU has: its own physical memory (GDDR/HBM)
  CPU process: has its own VA space (DDR)
  GPU kernel driver: must manually pin CPU pages (get_user_pages)
                     copy to GPU memory for compute
  → Expensive: copy latency, double memory usage, coherency complexity

With HMM:
  GPU driver registers: hmm_range_fault() to fault in CPU pages
  GPU can: directly read/write CPU virtual memory (via IOMMU + p2p)
  Or: GPU pages appear in CPU VA space (mirror the GPU's VA into CPU)
  
  HMM mirror:
    struct hmm_mirror: GPU's view of CPU address space
    hmm_range_fault():
      Take notifier lock
      Walk CPU page tables
      For each VA: return PA (for GPU IOMMU programming)
      Handle: swap-in if needed, allocate if COW
    
    On CPU page table change (fork, unmap, swap out):
      HMM notifier fires → GPU driver invalidates its IOMMU mapping
      Next GPU access: triggers hmm_range_fault() again → re-faults

HMM and SVM (Shared Virtual Memory) for GPU:
  GPU programs use CPU virtual addresses directly
  GPU command: "load from 0x7f0000001000" (a CPU VA)
  GPU IOMMU/SMMU: translates CPU VA → PA (using CPU page tables)
  SMMU Stage 1: CD[PASID] = CPU process page table
  SMMU uses PASID: process-specific addressing for GPU DMA
  
  Linux paths: drivers/gpu/drm/amdgpu/ (AMD ROCm + HMM)
               drivers/gpu/drm/nouveau/ (NVIDIA + HMM)

MIGRATE_VMA (HMM device memory):
  GPU has its own local memory (HBM, GDDR)
  HMM: can migrate CPU pages to GPU local memory and back
  migrate_vma_setup(): capture CPU PTE state
  migrate_vma_pages(): copy data from CPU DRAM → GPU DRAM
  migrate_vma_finalize(): install device-private PTEs in CPU page table
  
  Device-private PTE: CPU PTE marks page as "not present, on device"
  CPU access to device-private page: special page fault → migrate back
  GPU access to the page: direct (no fault needed)
  
  net result: page physically lives in GPU memory
              CPU can still "access" it (causes migration back to CPU DRAM first)
```

---

## 3. GUP (Get User Pages) and FOLL_PIN

```
GUP (get_user_pages): pin user pages in memory for DMA

get_user_pages(uaddr, npages, gup_flags, pages, vmas):
  Walk process page tables at uaddr
  For each page: get reference (pin it)
  Return: array of struct page pointers
  Effect: pages cannot be freed, swapped, or migrated until released
  Release: put_page(page) × npages

Use cases:
  DMA: device needs physical address of user buffer
    VFIO driver: pin user pages for device pass-through
    NVMe driver: pin user DMA buffers for zero-copy I/O
    Network: zero-copy send buffers
  
  GPU compute: pin input/output buffers before submitting GPU work

GUP flags:
  FOLL_WRITE: require writable PTEs (for write DMA into buffer)
  FOLL_PIN: new flag (since 5.6) — longer-lifetime pins
  FOLL_LONGTERM: page will be pinned for an extended time

FOLL_PIN vs refcount pin:
  Old: get_user_pages() increments page refcount
       Problem: page refcount approach breaks with memory migration, CMA
  New: FOLL_PIN: uses separate pin counter (page_pincount in folio)
       Incompatible: with movable pages (CMA, balloon) while pinned
       Safety: pinned pages cannot be migrated/freed

FOLL_PIN and CMA:
  CMA (Contiguous Memory Allocator): reserves large physically contiguous zones
  CMA pages: used for huge DMA allocations (camera sensor frame buffers)
  Problem: CMA pages may be "loaned" to buddy and given to userspace
  FOLL_PIN on CMA page: blocks CMA reclamation of that page
  Linux: warns when FOLL_PIN is used on CMA pages (may cause CMA failures)

long-term pin (FOLL_LONGTERM):
  For: RDMA, VFIO (persistent pins lasting hours or days)
  Not allowed on: CMA zones, ZONE_MOVABLE (would block reclaim/migration)
  GUP: checks zone, rejects FOLL_LONGTERM on movable zones
  Alternative for RDMA: use RDMA-specific registration (ibv_reg_mr) which
                        uses proper pin_user_pages() with DMA-coherent memory

pin_user_pages():
  New preferred API (Linux 5.6+): cleaner semantics than get_user_pages
  Explicitly indicates: caller is doing DMA (long-term pin)
  Uses FOLL_PIN internally
  Must be paired with: unpin_user_pages()
```

---

## 4. io_uring and Zero-Copy Memory

```
io_uring: high-performance async I/O interface (Linux 5.1+)

Architecture:
  Shared ring buffers: between userspace and kernel (no syscall for each I/O)
  Submission Queue (SQ): userspace writes SQEs (requests)
  Completion Queue (CQ): kernel writes CQEs (completions)
  io_uring_enter(): single syscall to submit + wait
  
  Ring memory:
    Mapped with: mmap(IORING_MMAP_SQ/CQ)
    Shared: same physical pages accessible from user and kernel
    No copy: user writes request → kernel reads directly (shared memory)
    No copy: kernel writes completion → user reads directly (shared memory)

io_uring fixed buffers (zero-copy):
  io_uring_register(IORING_REGISTER_BUFFERS):
    Register user buffer (VA + size) with io_uring
    Kernel: pins the pages (pin_user_pages())
    Stores: struct iov_itr with all page pointers
  
  Subsequent I/O to registered buffer:
    No GUP needed per I/O (already pinned)
    DMA: directly to/from user pages (zero copy)
    Network: splice/sendmsg with pre-pinned pages

io_uring and memory safety:
  Fixed buffers: pinned for duration of io_uring lifetime
  IOPOLL mode: kernel polls for completions (no IRQ overhead)
  Avoid:
    re-registering same buffer (causes double-pin)
    Accessing buffer while DMA is in flight (causes data corruption)
  Mitigation:
    io_uring handles buffer locking internally
    Userspace: must not touch buffer until CQE received

ARM64 io_uring performance:
  Cache coherency: ARM64 requires explicit cache maintenance for DMA
  io_uring + SMMU: if SMMU coherent mode → no explicit cache flush needed
  io_uring + non-coherent DMA: kernel inserts dma_sync_for_device() before DMA
  Hardware coherent (CMN-700 with coherent PCIe): true zero-copy possible

ARMv9 RME (Realm Management Extension):
  New ARM security model beyond TrustZone
  
  Four security states (not two):
    Normal world (NS): Linux, Android
    Secure world (S): TrustZone TEE (OP-TEE)
    Realm world (R): new! Hardware-isolated VMs (Realms)
    Root world (Root): firmware (TF-A RMM layer)
  
  Realm VMs:
    Isolated from: hypervisor (KVM), host Linux, AND Secure world
    Hardware guarantee: even a compromised hypervisor cannot read Realm memory
    GPC (Granule Protection Check): hardware checks every memory access:
      Each 4KB granule assigned: NS/Realm/Secure/Root
      GPC table enforced at memory bus level (cannot be bypassed by software)
  
  RMM (Realm Management Monitor):
    At Root level (separate from EL3 TF-A bits)
    Manages: Realm creation/destruction, Stage 2 for Realms
    CCA (Confidential Compute Architecture): brand name for RME-based system
  
  Use case: Confidential computing — cloud tenant runs VM, cloud provider
            cannot inspect its memory (hardware guarantee)
  vs TrustZone: TZ protects from normal world but not from EL3 TrustFirmware
                RME: Realms protected from EVERYONE including firmware
```

---

## 5. Memory Hotplug and CXL Memory

```
Memory Hotplug:
  Add/remove physical memory at runtime (hot-add/hot-remove)
  
  Memory hot-add:
    Platform signals: ACPI DSDT _OST + Notify(_ACPI_MEMORY_HOTPLUG_RESOURCE)
    Linux: online_pages(pfn, nr_pages, zone):
      1. Allocate struct page backing (sparse_add_section_mem_map)
      2. Initialize pages: init_page_count, LRU, etc.
      3. Assign to zone: zone_spanned_pages += nr_pages
      4. buddy_add_pages(): donate to buddy allocator
    After: new pages allocatable by normal kernel paths
  
  ZONE_MOVABLE:
    Purpose: movable pages enable memory hot-remove
    Hot-remove: must migrate all pages off the physical range
    Migration: only possible for movable pages
    ZONE_MOVABLE: keep "hot-removable" pages here
    Sized: kernelcore= boot parameter controls non-movable vs movable split
    sysctl: memory_hotplug.online_policy
  
  Memory hot-remove:
    offline_pages(): migrate all pages from the target range
      1. Migrate movable pages away (migrate_pages)
      2. Fail if non-movable pages found
    After migration: unmap from linear map, free struct pages
    Platform: ACPI eject or firmware command

CXL (Compute Express Link) Memory:
  PCIe-based memory expansion protocol
  CXL.mem: expose device memory as host-attached memory (Type 3 CXL device)
  
  Use cases:
    Memory expansion: add 256GB+ CXL DIMMs to system
    Near-memory processing: smart memory with compute
    Persistent memory: CXL attached PMem (like Optane replacement)
  
  CXL memory vs DDR:
    DDR: directly attached to CPU memory controller (2-3ns latency)
    CXL: via PCIe link → ~50-100ns extra latency
    Bandwidth: PCIe 5.0 × 16 = 64GB/s (vs DDR5 per-channel ~50GB/s)
    But: DDR5: 8 channels = 400GB/s >> CXL 64GB/s
  
  Linux CXL support:
    drivers/cxl/: CXL driver subsystem
    CXL memory: appears as NUMA node (with higher latency)
    Kernel: can use for: cold-data tiering (DAMON-driven)
                        page cache (lower cost than DRAM)
                        huge-page pools (less fragmentation concern)
    
    Tiered memory (DAMON + CXL):
      DAMON: monitor "cold" (rarely accessed) pages
      Migrate cold pages: DRAM → CXL memory (lower cost, higher capacity)
      Keep hot pages: in local DDR DRAM
      Result: more total "fast" memory without replacing all DDR with expensive DRAM
```

---

## 6. Interview Questions & Answers

**Q1: What problem does struct folio solve?**
`struct page` has an ambiguity problem: a raw `struct page *` might point to a head page (of a compound page) or a tail page. Every caller must call `compound_head(page)` to get the head before operating on it. This is error-prone (many historical bugs from forgetting compound_head) and adds overhead. `struct folio` is a type-level guarantee that you have the head. Functions accepting `struct folio *` never receive a tail page. Compiler enforces correctness. Additionally, `struct folio` allows one object to represent any power-of-2 multiple of pages (for large file mappings, THP in page cache), unifying multiple code paths.

**Q2: Explain HMM and when it's needed for GPU programming.**
HMM enables GPU drivers to directly reference CPU process virtual addresses without pinning all of physical memory. Without HMM: GPU driver must manually call `get_user_pages()`, pin CPU pages, program GPU IOMMU with physical addresses. When CPU page table changes: GPU mapping is stale. With HMM: `hmm_range_fault()` walks the CPU page tables and returns current PAs. MMU notifiers fire when CPU mappings change → GPU driver invalidates its IOMMU entries → next access re-faults. This enables true "shared virtual memory" where GPU and CPU use the same VA space, with page faults handling data placement transparently.

**Q3: What is FOLL_PIN and why is it better than old get_user_pages?**
Old `get_user_pages()` increments `page->_refcount`. Problem: many subsystems use `_refcount` for different purposes — a pinned page's refcount bump is indistinguishable from a page cache reference. This caused bugs with: balloon drivers (think page is free, but it's pinned for DMA), memory migration (cannot migrate pinned page, but old code didn't always check properly), CMA (CMA page loaned to buddy then pinned for DMA). `FOLL_PIN` / `pin_user_pages()` uses a separate pin counter (`folio->_pincount`), making long-term DMA pins explicit, trackable, and incompatible with movable zones. `unpin_user_pages()` clears the pin cleanly.

**Q4: How does io_uring achieve zero-copy I/O?**
1. Application registers buffers: `io_uring_register(IORING_REGISTER_BUFFERS, iovecs, count)`. 2. Kernel: `pin_user_pages()` — pins the physical pages backing those buffers permanently for the io_uring's lifetime. 3. When I/O request references a registered buffer: kernel uses already-pinned pages directly for DMA (no `get_user_pages()` overhead per operation). 4. For network zero-copy: registered buffer pages passed directly to the NIC's scatter-gather DMA list. 5. For storage: registered buffer pages passed directly to the block layer's BIO. No data copies from user buffer → kernel buffer → device.

**Q5: What is ARM RME and how does it differ from TrustZone?**
TrustZone provides two worlds (Normal and Secure). The Secure world (OP-TEE) is trusted, but the EL3 firmware (TF-A) has full system visibility. A compromised EL3 could read Secure world memory. ARM RME (Realm Management Extension) adds a 4th security state: Realm world. Realm VMs are protected by the Granule Protection Check (GPC) hardware: a table at the memory bus level that assigns each 4KB granule to a security state. Even EL3 firmware cannot read Realm memory. The RMM (Realm Management Monitor) manages Realm stage 2 tables at "Root" level — above TrustZone's EL3. This provides hardware-enforced confidential computing for cloud workloads.

---

## 7. Quick Reference

| Advanced Feature | Linux Version | Purpose |
|---|---|---|
| struct folio | 5.16+ | Clean compound page abstraction |
| HMM | 4.14+ | GPU shared virtual memory |
| FOLL_PIN | 5.6+ | Long-term DMA page pinning |
| io_uring | 5.1+ | High-performance async I/O |
| DAMON | 5.15+ | Data access monitoring |
| CXL memory | 5.12+ | PCIe-attached memory expansion |
| ARM RME/CCA | ARMv9.2 | Hardware confidential compute |
| Memory hotplug | 2.6+ | Runtime DIMM add/remove |
