# ARM32 MMU — Interview Master Guide
## Document 10: NVIDIA, Qualcomm, AMD, and Google Interview Preparation

**Author:** Senior Kernel Engineer  
**Target:** Senior/Staff/Principal Kernel Engineer Interviews (5–15 YOE)  
**Scope:** 50+ Expert Q&A, Debugging Walkthroughs, Code Review, Company Focus  
**Revision:** 1.0  
**Date:** 2026  
**Prerequisite:** Documents 01–09 (full series)

---

## Table of Contents
1. [ARM32 MMU Fundamentals — Core Questions](#1-arm32-mmu-fundamentals--core-questions)
2. [TLB and ASID — Deep Dive Questions](#2-tlb-and-asid--deep-dive-questions)
3. [Cache Architecture Questions](#3-cache-architecture-questions)
4. [Memory Barriers and Atomics Questions](#4-memory-barriers-and-atomics-questions)
5. [SMMU / IOMMU Questions](#5-smmu--iommu-questions)
6. [TrustZone / Secure World Questions](#6-trustzone--secure-world-questions)
7. [Virtualization / KVM Questions](#7-virtualization--kvm-questions)
8. [NVIDIA-Specific Questions](#8-nvidia-specific-questions)
9. [Qualcomm-Specific Questions](#9-qualcomm-specific-questions)
10. [AMD-Specific Questions](#10-amd-specific-questions)
11. [Google-Specific Questions](#11-google-specific-questions)
12. [Debugging Walkthroughs](#12-debugging-walkthroughs)
13. [Spot the Bug — Code Review](#13-spot-the-bug--code-review)
14. [Architecture Trade-off Discussions](#14-architecture-trade-off-discussions)
15. [Performance Analysis Scenarios](#15-performance-analysis-scenarios)

---

## 1. ARM32 MMU Fundamentals — Core Questions

---

**Q1. Walk me through a complete VA→PA translation on ARM32, from hardware registers to final physical address.**

**A:** ARM32 uses the TTBCR.N field to split the 32-bit VA space between TTBR0 and TTBR1. If N=0, TTBR0 covers the entire 4GB space. With N>0, addresses below 2^(32-N) use TTBR0 (user space), above use TTBR1 (kernel).

For a user-space access with a 1MB Section mapping:
1. Hardware reads `TTBR0` (bits [31:14-N]) to get the L1 table base PA.
2. VA[31:20] (12 bits) index into L1 table — selects 1 of 4096 entries.
3. L1 entry type bits [1:0]: `10` = Section descriptor.
4. PA = L1_entry[31:20] concatenated with VA[19:0].
5. AP/APX bits and Domain field checked against DACR — permission fault if violated.
6. TEX/C/B → memory type attribute applied.

For a Small Page (4KB) mapping:
1. L1[31:20] points to L2 table base.
2. VA[19:12] (8 bits) index into L2 table (256 entries × 4B = 1KB table).
3. L2 entry[31:12] gives page frame, concatenated with VA[11:0].

Key interview addition: Linux ARM32 uses a *software/hardware PTE split* — the Linux PGD entry points to a 2KB page where the first 2KB holds Linux SW PTEs and the second 2KB holds the hardware-format PTEs. `set_pte_at()` writes both copies.

---

**Q2. What is the difference between a Section, Supersection, Large Page, and Small Page in ARM32?**

**A:**

| Type | Size | L1 Entry | L2 Entry | Address Bits |
|------|------|----------|----------|--------------|
| Section | 1MB | bits[1:0]=10 | — | PA[31:20]+VA[19:0] |
| Supersection | 16MB | bits[1:0]=10 + bit[18]=1 | — | PA[31:24]+VA[23:0] |
| Large Page | 64KB | bits[1:0]=01 (L1→L2) | bits[1:0]=01 | PA[31:16]+VA[15:0] |
| Small Page | 4KB | bits[1:0]=01 (L1→L2) | bits[1:0]=11 | PA[31:12]+VA[11:0] |

Supersections require 16 consecutive L1 entries pointing to the same Supersection (hardware quirk — avoids 16× TLB entries).

Linux primarily uses Small Pages (4KB). Sections are used for the kernel direct-map (linear map). Huge pages (via THP) can produce Sections.

---

**Q3. What is the ARM32 Domain mechanism and why was it deprecated in ARM64?**

**A:** ARM32 has 16 domains (DACR register, 2 bits each). Each page table entry has a 4-bit domain field. DACR controls access per domain:
- `00` = No access
- `01` = Client (check AP/APX bits)
- `11` = Manager (bypass permission checks entirely!)
- `10` = Reserved

Linux historically used Domain 0 for kernel, Domain 1 for user. The Manager mode was a security bypass mechanism used in early context switch code (faster than changing PTEs). This was considered a security liability.

ARM64 removed domains entirely. Access control is purely through AP bits (read/write/execute per-EL). The kernel no longer can bypass page protections — this is the correct security model.

---

**Q4. Explain the difference between TTBR0 and TTBR1 in ARM32, and how Linux uses them.**

**A:** Both TTBR0 and TTBR1 point to L1 page tables. TTBCR.N determines the split:
- VA[31:(32-N)] == 0 → use TTBR0
- VA[31:(32-N)] == all-1s → use TTBR1

Linux uses `N=2` (CONFIG_VMSPLIT_3G):
- TTBR0 → user PGD (VA 0x00000000–0xBFFFFFFF, 3GB)
- TTBR1 → kernel PGD (VA 0xC0000000–0xFFFFFFFF, 1GB) — FIXED, never changes

On context switch, only TTBR0 changes (user PGD). TTBR1 never changes — kernel maps stay in all processes' address spaces. This is the 3G/1G split. No TLB flush needed for kernel entries because they're global (nG=0).

---

## 2. TLB and ASID — Deep Dive Questions

---

**Q5. How does ASID work in ARM32, and what happens when the 8-bit ASID space (256 values) is exhausted?**

**A:** ASID (Application Space ID) is stored in CONTEXTIDR[7:0] (8-bit = 256 values, 0 reserved for kernel). TLB entries are tagged with {ASID, VA} so switching contexts doesn't require flushing the TLB.

When all 256 ASIDs are allocated:
1. Linux increments `asid_generation` (a 64-bit counter, upper 56 bits = generation, lower 8 bits = ASID counter).
2. All per-CPU `reserved_asids[cpu]` are cleared.
3. A full TLB flush (`local_flush_tlb_all()`) is broadcast to all CPUs via IPI.
4. New ASIDs are assigned from 1 again (generation incremented, so old TLB entries with old generation are considered stale).

On context switch, `check_and_switch_context()` checks if the task's ASID generation matches current. If not, a new ASID is allocated. The ASID is written to CONTEXTIDR *before* TTBR0 — this is critical to avoid a window where the TLB might match wrong entries.

---

**Q6. Why does ARM32 require software TLB shootdown (IPI-based) while ARM64 has hardware TLB broadcast with TLBI IS?**

**A:** ARM32 TLB maintenance instructions (`MCR p15, 0, r0, c8, ...`) only affect the local CPU. There is no "Inner Shareable" broadcast from the TLB maintenance instructions (unlike ARM64 where `TLBI VMALLE1IS` broadcasts to all CPUs in the Inner Shareable domain).

In Linux ARM32 SMP:
1. CPU initiating the TLB invalidation calls `on_each_cpu(tlb_flush_fn, range, 1)`.
2. This sends an IPI (Inter-Processor Interrupt) to all other CPUs.
3. Each CPU executes the TLB invalidation instruction locally.
4. The initiating CPU waits for all CPUs to complete.

This is expensive for `munmap()` — Linux uses `mmu_gather` (batching) to collect multiple page table entries to unmap, then issues one TLB shootdown at the end rather than one per page.

ARM64 improvement: `TLBI VMALLE1IS` — hardware broadcasts to all CPUs sharing the Inner Shareable domain. Much cheaper than IPI.

---

**Q7. What is the correct sequence for updating TTBR0 on an ARM32 context switch? What goes wrong if you get the order wrong?**

**A:** The correct sequence (from `arch/arm/mm/context.c`, `cpu_v7_switch_mm`):

```assembly
MCR p15, 0, new_asid, c13, c0, 1   @ write CONTEXTIDR (new ASID)
ISB                                  @ flush pipeline — ASID active
MCR p15, 0, new_pgd, c2, c0, 0     @ write TTBR0 (new page table)
ISB                                  @ flush pipeline — new TTBR0 active
```

**If you write TTBR0 before CONTEXTIDR:** The CPU starts translating with the new page table but the old ASID. TLB entries tagged with the old {ASID, VA} from the previous process could match and give wrong translations. This is a classic security vulnerability — wrong process could momentarily access another process's pages.

**If you omit the ISB between CONTEXTIDR and TTBR0:** The CPU pipeline may have already started translating some instructions using the old CONTEXTIDR. The ISB ensures the pipeline is flushed and restarts with the new ASID before the TTBR0 write is processed.

---

## 3. Cache Architecture Questions

---

**Q8. What is VIPT cache aliasing and how does Linux prevent it?**

**A:** A VIPT (Virtually Indexed, Physically Tagged) cache uses VA bits for the cache index. If the page size (4KB) is smaller than the cache set×ways size (e.g., 32KB 4-way = 8KB index bits = 13 bits), then bits 12–(13-1)=12–12 (i.e., bit 12) are used for indexing but are NOT part of the page offset.

Two virtual pages mapping the same physical page could have different VA bit 12 values → they'd map to different cache sets → two cache copies of the same data → **aliasing**.

Linux prevention:
1. `arch_get_unmapped_area()` enforces **page coloring** for shared mappings — the `(vaddr >> PAGE_SHIFT) & color_mask` must match `(paddr >> PAGE_SHIFT) & color_mask`.
2. The kernel direct-map uses identity offsets, so VA and PA always have the same lower bits — no aliasing.
3. Cortex-A9 uses PIPT (Physically Indexed, Physically Tagged) — no aliasing problem. Aliasing only affects Cortex-A8 (VIPT L1, 32KB).

---

**Q9. Describe the Point of Coherency (PoC) vs Point of Unification (PoU) and when each matters.**

**A:**
- **PoC**: The point in the memory hierarchy where all observers (cores, DMA, GPU) share the same data. Typically main DRAM. Cache maintenance to PoC (`DC CIVAC` — clean+invalidate to PoC) ensures the data is in DRAM and all caches are invalidated.

- **PoU**: The point where instruction and data caches are unified for a given core. On Cortex-A9, the L2 cache is the PoU (both L1-I and L1-D see L2 as ground truth).

**When PoU matters:** JIT compilation, self-modifying code, module loading.
1. JIT writes code to buffer via D-cache → `DC CVAU` (clean D-cache to PoU).
2. Then `IC IVAU` (invalidate I-cache to PoU).
3. Then `DSB ISH + ISB`.
This ensures the processor's I-cache sees the new code.

**When PoC matters:** DMA operations. Before DMA-FROM-DEVICE: `DC IVAC` to PoC (invalidate D-cache so CPU reads from DRAM after DMA writes). Before DMA-TO-DEVICE: `DC CVAC` to PoC (flush D-cache so DMA reads fresh data from DRAM).

---

## 4. Memory Barriers and Atomics Questions

---

**Q10. What is the difference between DMB, DSB, and ISB? Give a concrete use case for each.**

**A:**
- **DMB (Data Memory Barrier):** Ensures all memory accesses *before* the DMB are globally visible before any memory accesses *after* the DMB. Does not wait for CP15 register updates or cache maintenance.
  - Use case: Producer writes data then sets a flag: `str data; DMB ISH; str flag`.

- **DSB (Data Synchronization Barrier):** Stronger than DMB. Waits for ALL memory accesses, cache maintenance, and CP15 register writes to complete. No subsequent instruction executes until DSB completes.
  - Use case: After TLB invalidation: `MCR TLBIALL; DSB ISH` — ensures TLB is actually clean before next translation.

- **ISB (Instruction Synchronization Barrier):** Flushes the CPU pipeline. Does not affect memory ordering — it affects *instruction fetch*.
  - Use case: After enabling MMU: `MCR SCTLR (enable MMU); ISB` — ensures subsequent instruction fetches use the new MMU configuration.

**Common mistake:** Using DMB instead of DSB after a TLB invalidation — DMB does not wait for TLB maintenance to complete, only memory accesses. This can cause stale TLB entries to remain active.

---

**Q11. Why do we need DMB both before and after a spinlock release?**

**A:** 
**Before unlock (release barrier):**
```assembly
DMB ISH
STR  r0, [lock]    @ unlock: store 0 to lock
```
The DMB before the store ensures all writes in the critical section are globally visible to other CPUs *before* the lock release is visible. Without it, another CPU acquiring the lock immediately after might see stale data from the critical section.

**After unlock (on next lock acquire):**
```assembly
LDR  r0, [lock]    @ acquire: load lock
DMB ISH
```
The DMB after acquiring the lock ensures the locker sees all writes from the previous lock holder that were made before their release DMB. Without this, loads in the new critical section might be speculatively reordered to before the lock acquire, seeing pre-lock-holder data.

ARM32 spinlock: `spin_lock = LDREX/STREX loop + DMB ISH`, `spin_unlock = DMB ISH + STR + DSB+SEV`.

---

**Q12. What is LDREX/STREX and why can't ARM32 do atomic CAS without them?**

**A:** ARM32 has no `LOCK CMPXCHG` like x86. The ARM bus lock mechanism (which older architectures used: `SWP` instruction) was deprecated in ARMv6 because it requires a bus lock for the entire atomic sequence — incompatible with multi-core caches and interconnects.

LDREX/STREX implements load-linked/store-conditional:
- `LDREX r0, [r1]`: Loads the value AND sets an exclusive reservation on that address in the hardware exclusive monitor.
- `STREX r2, r3, [r1]`: Attempts to store r3 to [r1]. Succeeds (r2=0) only if the exclusive reservation is still valid (no other CPU did STREX to the same cache line). Fails (r2=1) otherwise.
- Retry loop: if STREX fails, loop back to LDREX.

This is safe because:
- The exclusive monitor is per-cache-line granularity.
- Any successful STREX by *any CPU* clears all exclusives for that cache line.
- An exception clears the exclusive monitor (`CLREX` is called in exception entry).
- The loop always converges — maximum starvation bounded by number of CPUs.

---

## 5. SMMU / IOMMU Questions

---

**Q13. A device driver calls dma_map_single(). Walk through exactly what happens when an SMMU is present.**

**A:** (See also Doc 07, Section 7.1 for code)

1. `dma_map_single()` calls `iommu_dma_map_page()` (the IOMMU DMA ops variant).
2. Get the physical address of the buffer: `virt_to_phys(ptr)` → `pa`.
3. Allocate an IOVA from the device's IOVA domain: `alloc_iova()` → `iova_start`. The IOVA is chosen within the device's DMA mask (e.g., 32-bit mask → IOVA below 4GB).
4. Install SMMU page table mapping: `iommu_map(domain, iova_start, pa, size, IOMMU_READ|IOMMU_WRITE)` → writes LPAE/short descriptors into the SMMU context bank page tables.
5. Flush the SMMU TLB for the new IOVA range: `arm_smmu_iotlb_range_add()` — ensures stale SMMU TLB entries are invalidated.
6. Perform CPU cache maintenance: `arch_sync_dma_for_device(pa, size, dir)` — flush D-cache to DRAM for `DMA_TO_DEVICE`, invalidate D-cache for `DMA_FROM_DEVICE`.
7. Return `iova_start` to the driver.
8. Driver programs device: `writel(iova_start, DMA_ADDR_REG)`.
9. Device does DMA using `iova_start` → SMMU translates → actual `pa`.

---

**Q14. What is a Stream ID and why does it matter for SMMU security?**

**A:** A Stream ID (SID) is a hardware-assigned identifier that tags every DMA transaction with the identity of the originating DMA master. It's assigned by the SoC fabric (bus interconnect) and cannot be spoofed by software.

SMMU uses the SID to look up which Context Bank (and therefore which page table / IOVA domain) applies to this transaction. Different devices get different Context Banks → isolated IOVA spaces.

**Security implication:** Without SIDs, all DMA transactions would look the same to the SMMU. A compromised device driver could reconfigure the SMMU for another device's context bank and allow malicious DMA. With SIDs, even if a driver is compromised, the hardware ensures DMA from `GPU` uses `GPU's context bank`, not the modem's secure context bank.

**Qualcomm example:** Modem DMA transactions have SID in range 0x50–0x5F, mapped to CB6 (Secure). Android host kernel (Non-Secure PL1) cannot read or write CB6 registers — only QSEE (Secure world) can configure the modem's DMA permissions.

---

**Q15. What is the IOVA allocator bottleneck in high-frequency DMA workloads and how is it mitigated?**

**A:** The IOVA allocator uses a red-black tree to track free/allocated IOVA ranges. Every `dma_map_single()` + `dma_unmap_single()` pair requires an insert + delete from the rb-tree, protected by a spinlock. Under high DMA frequency (e.g., 100K+ USB/audio DMA ops per second), this spinlock becomes a serialization point.

**Mitigation — Per-CPU IOVA rcache:**
Linux IOVA allocator maintains per-CPU caches of recently freed IOVA ranges, organized by size buckets (1 page, 2 pages, 4 pages, ... powers of 2). 
- `dma_unmap_single()`: push IOVA to per-CPU cache (no lock).
- `dma_map_single()`: pop from per-CPU cache (no lock, ~10ns).
- Cache miss: fall back to rb-tree (lock needed, ~100ns).

Result: Most DMA map/unmap pairs for small buffers are nearly lock-free. Measured improvement: 3M → 10M DMA ops/sec on Cortex-A9.

**Additional mitigation:** `dma_map_sg()` for scatter-gather — batch multiple pages into one SMMU operation, one IOVA allocation for the entire sg list.

---

## 6. TrustZone / Secure World Questions

---

**Q16. Describe the sequence of events when user-space calls a TrustZone API (e.g., through OP-TEE's libteec).**

**A:** 
1. User-space calls `TEEC_OpenSession()` in `libteec.so`.
2. libteec calls `ioctl(fd, TEE_IOC_OPEN_SESSION, ...)` on `/dev/tee0`.
3. OP-TEE kernel driver (`optee.ko`) in Linux kernel handles the ioctl.
4. Driver calls `optee_smccc_smc(OPTEE_SMC_OPEN_SESSION, ...)` → executes `SMC` instruction.
5. `SMC` triggers exception → CPU enters **Monitor mode** (PL3, Secure world entry point).
6. Monitor (TF-A/BL31) saves Non-Secure world register state.
7. Clears `SCR.NS = 0` → CPU is now in Secure world.
8. Restores Secure world register state.
9. Jumps to OP-TEE OS handler.
10. OP-TEE OS creates a session, loads Trusted Application (TA) if needed.
11. TA runs, processes command.
12. OP-TEE OS calls `SMC` again to return to Non-Secure world.
13. Monitor saves Secure state, sets `SCR.NS = 1`, restores NS state.
14. CPU returns to Linux kernel in `optee.ko` handler.
15. ioctl returns result to user-space.

Key detail: Shared memory between NS and Secure world uses physical pages with NS=1 in Secure world page tables — Secure world can access them but they're not protected from NS world. Sensitive TA data uses NS=0 pages — invisible to NS world.

---

**Q17. What is the NS bit on the AXI bus and how does it interact with TZASC?**

**A:** Every AXI bus transaction in an ARM TrustZone system carries an `NS` (Non-Secure) bit. This single bit propagates the security state of the originating CPU access to all downstream IP (memory controllers, peripherals).

- CPU in Secure world reading DRAM: AXI NS=0 → TZASC checks region.
- CPU in Non-Secure world reading DRAM: AXI NS=1 → TZASC checks region.
- DMA from NS peripheral: AXI NS=1 always (DMA hardware has fixed NS bit).

**TZASC (TrustZone Address Space Controller / TZC-400):**
Sits between AXI bus and DRAM. Configured by secure firmware (TF-A BL2) at boot. Defines up to 9 regions with NS-access permissions:
- Region 0: Default — covers all DRAM not covered by other regions.
- Regions 1–8: Override specific PA ranges.

Each region configures: `nsaid_read_en`, `nsaid_write_en` (bitmap of which NSAID = Non-Secure requester IDs are allowed). If a Non-Secure AXI transaction hits a Secure region → DECERR (decode error) → bus fault in NS world.

---

## 7. Virtualization / KVM Questions

---

**Q18. What happens on a Stage-2 page fault in KVM/ARM? Walk through the entire software path.**

**A:** (See Doc 09, Section 9.1 for code)

1. Guest accesses IPA that has no Stage-2 mapping.
2. Hardware: Stage-2 PTW finds no valid entry → Hyp data abort exception.
3. CPU vectors to `HVBAR + 0x10` (Data Abort taken to Hyp).
4. KVM vector (`__kvm_vcpu_run_nvhe`) saves guest register state.
5. Calls `kvm_handle_guest_abort(vcpu, run)`.
6. Reads `HSR.EC` → Stage-2 fault (EC=0x24 or 0x26).
7. Reads `HPFAR` → faulting IPA (shifted right by 4 in register).
8. `gfn_to_memslot()`: Is this IPA in a VM memory region (RAM)?
   - YES → `user_mem_abort()`: find host PFN, call `kvm_pgtable_stage2_map()` to install mapping.
   - NO → `io_mem_abort()`: this is MMIO — decode instruction, fill `run->mmio`, return `KVM_EXIT_MMIO` to QEMU.
9. If RAM: Stage-2 mapping installed → `kvm_tlb_flush_vmid_ipa()` → TLB invalidate.
10. `kvm_vcpu_run()` re-enters the guest (`ERET`).
11. Guest re-executes the faulting instruction — now Stage-2 TLB has entry → no fault.

---

**Q19. What is pKVM and why is it important for Android security?**

**A:** pKVM (Protected KVM) is Google's hypervisor for Android, merged into mainline Linux 5.20+. Standard KVM's threat model does NOT protect VMs from a compromised host kernel — the Linux kernel at EL1 has read/write access to all physical RAM, including VM memory.

pKVM changes this:
1. A small hypervisor (~10K lines, minimal TCB) runs at EL2 (Hyp mode) permanently.
2. The host Linux kernel is "deprivileged" — it runs at EL1 with its own Stage-2 page tables (like a VM).
3. VM memory is removed from the host's Stage-2 mapping → host kernel cannot read VM memory even if compromised.
4. Memory donation: host must explicitly "donate" pages to VMs via hypervisor calls; hypervisor removes them from host's mapping.

**Android use case:** Protected VMs for sensitive workloads (Credential Manager, Keymint, DRM). Even if Android kernel is exploited, attacker cannot extract secrets from pKVM-protected VMs. This is equivalent to AMD SEV / Intel TDX for mobile.

---

## 8. NVIDIA-Specific Questions

---

**Q20. How does CUDA Unified Virtual Memory (UVM) interact with the SMMU/IOMMU on NVIDIA Tegra?**

**A:** CUDA UVM allows CPU and GPU to share a unified address space where pages are migrated on demand. On NVIDIA Tegra (ARM CPU + integrated GPU):

1. `cudaMallocManaged()` allocates a virtual address range but does NOT allocate physical pages yet.
2. When GPU kernel accesses an unmapped VA → **GPU page fault** (Pascal+ has hardware GPU MMU with fault support).
3. CUDA UVM driver handles the GPU page fault:
   - Allocates a physical page.
   - Maps it in the GPU MMU (IOMMU domain for GPU).
   - Maps it in Tegra SMMU (for GPU DMA access to system RAM).
4. When CPU accesses the same VA → CPU page fault → UVM driver:
   - May migrate page from GPU-mapped memory to system RAM.
   - Updates CPU page tables (Stage-1) + SMMU mappings.
5. Coherency: CCI-400/500 provides hardware coherency between ARM CPU cluster and GPU on some Tegra variants — no explicit cache maintenance needed for UVM pages.

**Performance implication:** First access to any UVM page causes a fault (lazy allocation). Hot pages eventually stay wherever they're accessed most. `cudaMemPrefetchAsync()` proactively migrates to avoid fault latency.

---

**Q21. Describe how the Tegra SMMU differs from the standard ARM SMMU v2.**

**A:** Tegra SMMU (used in Tegra K1/X1/Xavier pre-SMMU v3):

1. **Register layout**: Different from ARM SMMU spec. Uses `tegra-smmu.c` instead of `arm-smmu.c`. SWGROUP registers instead of SMR/S2CR. Each engine (GPU, display, ISP, etc.) has a fixed SWGROUP ID.

2. **Page table format**: Tegra SMMU uses 32-bit page tables only (two-level, similar to ARM32 short descriptor). No LPAE/64-bit descriptor support in older Tegra.

3. **TLB flush**: Write to `SMMU_TLB_FLUSH` register to invalidate. No per-entry invalidation in older Tegra SMMU.

4. **No Stage-2**: Tegra SMMU (older) supports Stage-1 only. Stage-2 for virtualization added in Xavier (Tegra194) with SMMU v2 compliant hardware.

5. **Linux driver**: `drivers/iommu/tegra-smmu.c` implements custom iommu_ops. Xavier onwards uses `arm-smmu.c` with Tegra quirks via `arm_smmu_impl_ops`.

---

## 9. Qualcomm-Specific Questions

---

**Q22. Explain how Qualcomm uses SMMU to isolate the modem from the Android kernel.**

**A:** Qualcomm's cellular modem (hexagon DSP + modem CPU) runs proprietary firmware and has DMA access to shared memory for IPC with Android. Without isolation, a modem firmware bug could DMA to arbitrary Android kernel memory.

Isolation mechanism:
1. Modem DMA transactions use SIDs in range 0x50–0x5F (firmware-assigned, hardwired in SoC fabric).
2. These SIDs are mapped to CB6 (Context Bank 6) in Qualcomm's SMMU.
3. CB6 is a **Secure context bank** — CBAR.IRPTNDX indicates Secure IRQ, CBAR.TYPE = Secure Stage-1.
4. CB6 registers can only be written from Secure world (QSEE).
5. Android kernel (Non-Secure PL1) cannot access CB6 registers → cannot change modem's IOVA mappings.
6. At boot, QSEE configures CB6 to allow modem access only to its allocated memory (shared IPC buffers, modem carve-out).
7. Modem DMA to any address outside CB6's mappings → SMMU fault → modem is reset, Android keeps running.

---

**Q23. What is the Qualcomm SCM (Secure Channel Manager) driver and when would you use it?**

**A:** The SCM driver (`drivers/firmware/qcom_scm.c`) provides an interface from Linux kernel to Qualcomm's Secure Monitor (QSEE) via `SMC` instructions.

Qualcomm SMC calling convention (ARM32 version):
- `r0` = service/command ID
- `r1`–`r4` = arguments
- `SMC #0` instruction
- Return in `r0`–`r3`

Linux wrapper: `qcom_scm_call(svc_id, cmd_id, req, req_size, resp, resp_size)`.

**Use cases:**
1. **PIL (Peripheral Image Loader):** Loading signed firmware for modem/ADSP. Linux calls `qcom_scm_pas_auth_and_reset()` → QSEE verifies firmware signature, maps it to protected memory, releases peripheral from reset.
2. **Memory protection:** `qcom_scm_assign_mem()` → reassign physical memory between Non-Secure and Secure domains (TZASC/SMMU configuration).
3. **QFPROM (fuse) access:** Read device-unique keys, boot certificate chain, debug policy fuses.
4. **Crypto services:** Key derivation, random number generation from hardware TRNG.

---

## 10. AMD-Specific Questions

---

**Q24. Explain AMD IOMMUv2 Device Table structure and how it differs from ARM SMMU context banks.**

**A:** AMD IOMMU uses a flat array called the **Device Table** indexed by PCIe requester ID (Bus[7:0]:Dev[4:0]:Func[2:0] = 16-bit BDF):
- Up to 65536 DTEs (Device Table Entries), 32 bytes each.
- Each DTE contains: domain ID, page table pointer, enable flags, IR/IW permission bits.
- Multiple devices can share the same domain ID → shared page tables.

Comparison with ARM SMMU:
- ARM SMMU: Stream ID → SMR lookup → Context Bank → per-CB page tables (CB is the unit of isolation)
- AMD IOMMU: BDF → Device Table[BDF] → domain ID → per-domain page tables

AMD IOMMU page tables use same format as x86-64 CPU page tables (4-level, 4KB pages). This means an OS could theoretically share page tables between CPU and IOMMU. AMD IOMMU v2 adds **PASID** (Process Address Space ID) support — multiple page tables per device, one per process context.

AMD **PPR (Peripheral Page Request) log**: Devices with PCIe ATS (Address Translation Services) can send page requests to the IOMMU when they fault. The OS processes these and resolves the fault by installing page table entries, then completes the request. This is the device equivalent of a page fault handler.

---

**Q25. What is AMD IOMMU strict mode and why would you disable it?**

**A:** AMD IOMMU strict mode (Linux kernel option `iommu=strict`): When a DMA mapping is unmapped (`dma_unmap_*`), the IOMMU TLB is immediately flushed synchronously for the unmapped IOVA range.

Without strict mode (`iommu=nomerge` or default): IOMMU TLB flushes are deferred/batched — unmapped entries remain in IOMMU TLB until the next explicit flush. A buggy or compromised device could still DMA to the recently-unmapped IOVA for a brief window.

**Why disable (performance):** IOMMU TLB flush (IOTLB_INV command) is expensive — ~1-5 µs per flush. For network drivers doing 100K+ DMA unmaps per second, synchronous TLB flushes add 100ms+ of overhead per second of processing. Batching TLB flushes reduces overhead by 10–100×.

**Security consideration:** For multi-tenant cloud (VMs with PCIe passthrough), strict mode is critical — a VM's DMA must be fully invalidated before its device is reassigned to another VM. For embedded single-tenant systems, relaxed mode is generally safe.

---

## 11. Google-Specific Questions

---

**Q26. How does Android's dm-verity work and what is its relationship to the MMU?**

**A:** `dm-verity` is a Linux device mapper target that provides integrity verification for block devices. Every 4KB block read from the verified partition is hashed, and the hash is verified against a hash tree (Merkle tree).

**MMU relationship:**
1. The read-only system partition (`/system`) is mounted via dm-verity.
2. Page faults for memory-mapped files from `/system` (e.g., shared libraries) go through the page fault handler → `do_read_fault()` → reads from block device via dm-verity.
3. dm-verity verifies the block's hash before allowing it into the page cache.
4. If hash mismatch: dm-verity triggers a kernel panic (or remounts read-only in non-fatal mode).
5. Page is then mapped into the process address space read-only (XN=0, AP=01).

The **verified boot** chain: Bootloader verifies kernel signature (using AVB — Android Verified Boot) → kernel verifies dm-verity root hash embedded in kernel command line → dm-verity verifies all system partition reads → any modification of `/system` is detected.

---

**Q27. Explain Google's ION allocator (now DMA-BUF) and how it uses SMMU for security isolation.**

**A:** ION (now superseded by DMA-BUF heaps in Linux 5.6+) was Android's cross-process buffer allocator. It solved the problem of zero-copy sharing between Camera, GPU, Display, and Video hardware without exposing physical addresses.

**Old ION model:**
- Heaps: `ION_HEAP_TYPE_SYSTEM` (page allocator), `ION_HEAP_TYPE_CARVEOUT` (dedicated RAM region).
- Process allocates buffer → gets a `fd` (file descriptor).
- Share `fd` with GPU driver → GPU driver `mmap()`s the buffer into GPU IOVA via SMMU.
- Share `fd` with Display driver → Display maps it into display SMMU context.

**Security with SMMU:**
- Camera buffer: mapped only in Camera SMMU context bank.
- GPU can only access it after explicitly mapping via GPU's context bank.
- Display can only access it after mapping via display's context bank.
- An exploit in GPU driver cannot DMA to Camera's private buffer (different SMMU domain).

**DMA-BUF improvements:**
- Standardized in upstream Linux (not Android-specific).
- `dma_buf_attach()` + `dma_buf_map_attachment()` → properly maps the buffer into the attaching device's IOMMU domain.
- `dma_buf_unmap_attachment()` + `dma_buf_detach()` → unmaps and invalidates IOMMU mappings.

---

## 12. Debugging Walkthroughs

---

### 12.1 Decode a Page Fault Oops

```
Scenario: Kernel oops in production:

Unable to handle kernel paging request at virtual address deadbeef
pgd = c0004000
[deadbeef] *pgd=00000000
Internal error: Oops: 5 [#1] PREEMPT SMP ARM
...
PC is at 0xc012abc4 in function driver_read+0x44/0x120
```

**Decode steps:**
1. Virtual address `0xdeadbeef` — classic uninitialized pointer.
2. `pgd = c0004000` — this is `swapper_pg_dir` (kernel page table, starts at 0xc0004000).
3. `*pgd=00000000` — the L1 PGD entry for `0xdeadbeef` is 0 (unmapped).
4. `Oops: 5` = `0b0101`: 
   - bit 0: translation fault (not present)
   - bit 2: Access from PL1 (kernel mode = 1)
5. PC at `driver_read+0x44` — use `addr2line` or objdump:
   `arm-linux-gnueabihf-objdump -d driver.ko | grep -A5 "driver_read"`
6. Find the instruction at +0x44: likely a `LDR r0, [r0, #offset]` where r0=0xdeadbeef.
7. Root cause: uninitialized pointer or use-after-free.

**Additional registers to check:**
- `R0–R15`: which register held the bad address?
- `DFSR` (Data Fault Status Register): encoded fault type.
- `DFAR` (Data Fault Address Register): faulting VA (=0xdeadbeef here).
- Stack trace: backtrace from `sp` using frame pointers or `unwinder`.

---

### 12.2 Diagnose TLB Thrashing

```
Symptom: System performance degrades sharply when running
         specific workload. perf shows high cache miss rates.
         
Diagnosis:
1. perf stat -e dTLB-load-misses,dTLB-store-misses,iTLB-load-misses ./workload
   
   Result: 5M dTLB-load-misses/sec (normally <100K)
   → TLB miss rate is 50× normal

2. perf record -e dTLB-load-misses:u ./workload ; perf report
   → Shows hotspot in hash table lookup function
   → Hash table: 128MB, random access pattern
   
3. Analysis:
   4KB pages: 128MB / 4KB = 32768 pages
   Cortex-A9 unified TLB: 128 entries
   32768 entries >> 128 TLB entries → thrashing guaranteed
   
Fix options:
  a. Huge pages (2MB): 128MB / 2MB = 64 entries → fits in TLB
     madvise(ptr, size, MADV_HUGEPAGE)  ← request THP
     
  b. Explicit 1MB Section pages (kernel driver only):
     Use ioremap() with 1MB alignment, kernel mm will use Section mapping
     
  c. Change algorithm: improve spatial locality to reduce unique page accesses
```

---

### 12.3 Debug DMA Coherency Bug

```
Symptom: Intermittent data corruption when receiving network packets.
         Problem appears on SMP but not on single-core.

Root cause identification:

1. Add debug: print first bytes of every received packet.
   → Sometimes correct, sometimes all-zeros or old data.
   
2. Hypothesis: DMA buffer still in CPU D-cache (stale) when CPU reads it.

3. Check driver:
   
   /* Current code (BUGGY): */
   dma_addr = dma_map_single(dev, buf, size, DMA_FROM_DEVICE);
   /* ... program DMA ... wait for completion ... */
   dma_unmap_single(dev, dma_addr, size, DMA_FROM_DEVICE);
   process_data(buf);   /* BUG: may read from D-cache, not DRAM */

4. The problem: dma_unmap_single with DMA_FROM_DEVICE should call
   arch_sync_dma_for_cpu() which invalidates D-cache.
   
   But on non-cache-coherent ARM32 platforms, if the DMA buffer
   was accessed by CPU before DMA started (e.g., for initialization),
   those cache lines may be dirty → invalidation removes both dirty
   and clean lines → CPU sees zeros (initial cache state).
   
   Critical: Buffer must be CACHE-LINE ALIGNED.
   If buf starts at 0x80001003 (not 64-byte aligned):
   → First cache line (0x80001000–0x8000103F) is partially CPU-owned
   → Invalidating this line discards CPU-written data in first 3 bytes
   → Corruption!
   
Fix:
   buf = kmalloc(size + 63, GFP_KERNEL);
   buf_aligned = PTR_ALIGN(buf, 64);  /* cache line = 64 bytes */
   dma_addr = dma_map_single(dev, buf_aligned, size, DMA_FROM_DEVICE);
```

---

## 13. Spot the Bug — Code Review

---

**Bug 1: Missing ISB after TTBR0 update**

```assembly
/* Context switch — find the bug */
MCR  p15, 0, new_asid, c13, c0, 1   @ write CONTEXTIDR
MCR  p15, 0, new_pgd,  c2,  c0, 0   @ write TTBR0
/* BUG: No ISB between CONTEXTIDR and TTBR0, no ISB after TTBR0 */
/* Instructions already in pipeline may use old TTBR0 after new ASID active */
/* Result: possible translation with wrong {ASID, page_table} combination */

/* FIX: */
MCR  p15, 0, new_asid, c13, c0, 1
ISB
MCR  p15, 0, new_pgd,  c2,  c0, 0
ISB
```

**Bug 2: DMA buffer not cache-line aligned**

```c
/* Network driver RX ring */
struct rx_buf {
    u8   header[14];   /* Ethernet header */
    u8   payload[1500];
};
/* BUG: sizeof(rx_buf) = 1514, not cache-line aligned */
/* Two adjacent rx_buf entries share a cache line at the boundary */
/* DMA to buf[0].payload end + DMA to buf[1].header start = same cache line */
/* If CPU touches buf[0].payload after DMA: partial invalidation bug */

/* FIX: */
struct rx_buf {
    u8 header[14];
    u8 payload[1500];
    u8 pad[66];    /* total = 1580, rounds up to next 64-byte boundary */
} __attribute__((aligned(64)));
```

**Bug 3: Spinlock unlock without barrier**

```c
/* BUGGY: lock-free update without proper ordering */
static atomic_t ref_count = ATOMIC_INIT(0);
static struct data *shared_ptr = NULL;

void publish(struct data *d) {
    shared_ptr = d;          /* store pointer */
    atomic_inc(&ref_count);  /* increment counter */
    /* BUG: atomic_inc doesn't include a barrier */
    /* CPU may reorder: atomic_inc visible BEFORE shared_ptr store */
    /* Consumer sees ref_count > 0 but shared_ptr = NULL */
}

/* FIX: */
void publish_fixed(struct data *d) {
    smp_store_release(&shared_ptr, d);  /* release: stores before this are visible */
    atomic_inc(&ref_count);
}
```

**Bug 4: SMMU TLB not flushed after mapping update**

```c
/* BUG: Update SMMU mapping without TLB invalidation */
void remap_dma_buffer(struct iommu_domain *domain, dma_addr_t iova,
                       phys_addr_t new_pa, size_t size)
{
    iommu_unmap(domain, iova, size);   /* remove old mapping */
    iommu_map(domain, iova, new_pa, size, IOMMU_READ | IOMMU_WRITE);
    /* BUG: Old SMMU TLB entry for iova still cached! */
    /* Device may DMA to old_pa for some time until TLB entry ages out */
}

/* FIX: */
void remap_dma_buffer_fixed(struct iommu_domain *domain, dma_addr_t iova,
                              phys_addr_t new_pa, size_t size)
{
    iommu_unmap(domain, iova, size);
    /* iommu_unmap should flush TLB — verify driver implements iotlb_sync */
    iommu_map(domain, iova, new_pa, size, IOMMU_READ | IOMMU_WRITE);
    /* Explicit flush if driver's unmap doesn't guarantee sync: */
    iommu_flush_iotlb_all(domain);
}
```

---

## 14. Architecture Trade-off Discussions

---

**Q28. Describe the trade-offs between VIPT and PIPT cache design.**

**A:**

| Property | VIPT | PIPT |
|----------|------|------|
| Lookup speed | Faster (use VA bits directly) | Slower (wait for PA from TLB) |
| Aliasing | Yes (if index bits > page offset bits) | No (PA uniquely identifies cache line) |
| Tag bits | Physical (PA-tagged) | Physical (PA-tagged) |
| Coherency | Complex (flush on remap) | Simple (PA unique) |
| Example | Cortex-A8 L1 32KB | Cortex-A9 L1 32KB |

VIPT is chosen for speed: the cache index bits come from VA which are available immediately (no TLB wait). But if the cache index uses bits above bit 11 (page offset), aliasing occurs for shared pages.

**PIPT advantage (Cortex-A9 design choice):** Eliminates aliasing entirely. The TLB and cache lookups can overlap (parallel TLB walk + cache lookup with pessimistic index). For Cortex-A9 with hardware PTW, the TLB hit is fast enough that PIPT latency penalty is minimal.

**ARM64 choice:** All Cortex-A5x/A7x+ use PIPT for L1 D-cache. Aliasing handling in software is too complex and error-prone.

---

**Q29. When would you choose SMMU strict mode over passthrough mode for a production system?**

**A:**

**Use strict mode (translation, security enforced):**
- Multi-tenant systems (cloud, container hosts) — one tenant's device must not DMA to another's memory
- Devices with complex, untrusted firmware (cellular modem, WiFi chip)
- Systems with buggy device drivers (SMMU catches DMA errors immediately)
- Security-critical deployments (automotive, medical, financial)

**Use passthrough mode (IOVA == PA, no translation overhead):**
- Single-tenant embedded systems with trusted device firmware
- Performance-critical real-time applications where SMMU latency is unacceptable
- Devices that are hardware-coherent and trusted (internal bus masters, CPU DMA engines)
- Development/debugging where SMMU complexity adds noise

**Hybrid (most production ARM SoCs):**
- Trusted peripherals (internal SRAM DMA): passthrough
- Untrusted external interfaces (PCIe, USB): strict mode
- Secure peripherals (modem): Secure SMMU context (NS world cannot access)

---

## 15. Performance Analysis Scenarios

---

**Q30. A kernel engineer reports the IOMMU is adding 30% overhead to GPU DMA operations. How do you diagnose and optimize?**

**A:** Systematic approach:

**Step 1: Quantify the components**
```bash
perf stat -e iommu:map,iommu:unmap,iommu:tlbi ./gpu_workload
# How many IOMMU operations? What's the average size?
```

**Step 2: Profile IOVA allocator**
```bash
perf record -g -e lock:lock_acquire ./gpu_workload
perf report --sort=symbol,dso
# Look for iova_alloc_rcaches lock contention
```

**Step 3: Measure TLB flush overhead**
```bash
# Check if iommu=strict is enabled (synchronous TLB flush on unmap)
cat /proc/cmdline | grep iommu
# Try: iommu=nomerge (defer TLB flushes)
```

**Optimization strategies based on findings:**

1. **Large buffer allocations (>64KB):** Switch to `dma_alloc_coherent()` — allocates once, keeps mapping alive, no map/unmap overhead.

2. **High-frequency small buffers:** Ensure IOVA rcache is working:
   ```c
   /* Check buffer sizes: are they power-of-2 pages? */
   /* IOVA rcache only caches power-of-2 page counts */
   size = ALIGN(actual_size, 128);  /* align to rcache bucket size */
   ```

3. **GPU-specific: IOMMU DMA pool**
   Keep a pool of pre-mapped IOVA regions, use them in round-robin fashion. Zero map/unmap overhead.

4. **SMMU page size:** Use 2MB blocks for GPU command buffers (static, large allocations). Fewer SMMU TLB entries needed, better TLB hit rate.

5. **Strict mode:** If not security-required, switch to `iommu=nomerge` to allow batched TLB flushes.

---

## Quick Reference: Company Interview Focus Areas

### NVIDIA Focus Areas
- GPU MMU + SMMU (IOMMU for GPU DMA)
- CUDA UVM page migration mechanics
- Tegra SMMU (`tegra-smmu.c`) register details
- PCIe DMA and IOMMU interaction
- Cache coherency between ARM CPU and GPU

### Qualcomm Focus Areas
- SMMU stream ID assignment and security (modem isolation)
- QSEE/SCM driver (`qcom_scm_call`) usage
- PSCI implementation (`arch/arm/firmware/psci.c`)
- ARM32 SMP spinlock/IPI implementation
- TrustZone integration (QSEE, secure boot, QFPROM)

### AMD Focus Areas
- AMD IOMMUv2 Device Table vs ARM SMMU Context Banks
- PASID (per-process IOMMU translations)
- PPR (Page Request Interface) and its interaction with Linux MM
- PCIe ATS (Address Translation Services)
- RDMA memory registration via IOMMU

### Google Focus Areas
- pKVM architecture and memory donation protocol
- Android Verified Boot / dm-verity / fscrypt interaction
- Trusty TEE (LK microkernel, tipc IPC, TAs)
- ION → DMA-BUF migration and SMMU isolation
- KVM page fault handling and MMIO emulation

---

**Cross-References:**
- Doc 01–09: All previous documents
- ARM Architecture Reference Manual ARMv7-A (ARM DDI 0406C)
- ARM SMMU Architecture Spec (ARM IHI0062)
- Linux kernel: `arch/arm/`, `drivers/iommu/`, `virt/kvm/arm/`

---
**End of Document 10 — ARM32 MMU Design Document Series Complete**
