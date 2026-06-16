# ARM Linux Kernel Knowledgebase — Index

A consolidated, deep-dive technical knowledgebase synthesized from 36 source
`.docx` files and supporting code (see [Source Manifest](#source-manifest)).
All originals are preserved unchanged in the parent directory; this folder
contains the distilled, de-duplicated, topical reference set.

---

## Reading Order (beginner → advanced)

| Order | Document | Topic | Lines |
|-------|----------|-------|-------|
| 1 | [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md) | C strings, memory layout, bitwise programming | ~895 |
| 2 | [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md) | ARM/ARM64 MMU, page tables, kernel memory, mmap | ~1620 |
| 3 | [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md) | Char/block drivers, device tree, procfs/sysfs, syscalls | ~1045 |
| 4 | [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md) | GIC, IRQ subsystem, IPIs, watchdog, panic | ~1385 |
| 5 | [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md) | CFS/RT/DL, preemption, context switch, locks, RCU | ~1805 |
| 6 | [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md) | Power-on → kernel, U-Boot, APPSBL, ATF, hibernation | ~1415 |
| 7 | [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md) | Qualcomm SoC, SMMU/IOMMU, RPMh/SMP2P/SMEM | ~1505 |
| 8 | [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md) | Oops/BUG/panic, kdump, crash util, Qualcomm RAM dumps | ~1145 |
| 9 | [09_Interview_QA_Master.md](09_Interview_QA_Master.md) | Master Q&A bank with deep-dive links | ~1550 |

> The full set is ~11,400 lines (~450 KB) of consolidated technical content.

---

## Table of Contents

### [01 — ARM / ARM64 Memory Management](01_ARM_ARM64_Memory_Management.md)
ARM vs ARM64 memory architecture · MMU fundamentals (TTBR/TCR/MAIR/SCTLR) ·
4-level page table walks · translation granules (4K/16K/64K) · memory types &
attributes · TLB management (ASID/VMID) · cache architecture (PIPT/VIPT,
DSB/DMB/ISB) · Linux kernel memory layout on ARM64 (vmalloc, vmemmap, fixmap,
KASLR) · physical memory (memblock, buddy, zones) · SLUB · mm_struct &
vm_area_struct · ARM64 page-fault handling (ESR_EL1, FAR_EL1, fault.c) ·
mmap & driver mmap (remap_pfn_range, vm_insert_page) · ioremap & DMA · huge
pages & THP · CMA · KSM/compaction/reclaim/OOM · debugging.

### [02 — Scheduling & Synchronization](02_Scheduling_and_Synchronization.md)
Scheduling classes (stop/DL/RT/CFS/idle) · CFS internals (vruntime, rb-tree,
PELT) · SCHED_FIFO/RR/DEADLINE · SMP load balancing & sched domains ·
preemption models (NONE/VOLUNTARY/PREEMPT/RT) · ARM64 context switch
(`__switch_to`, FPSIMD lazy save) · timers, jiffies, hrtimers, NO_HZ ·
LDXR/STXR & LSE atomics, dmb/dsb/isb · spinlocks (ticket→qspinlock) · mutex
(OSQ optimistic spin) · semaphores & rwsem · RCU (classic/tree/SRCU) ·
per-CPU vars · completions, wait queues, seqlocks · lockdep · priority
inversion & PI futex.

### [03 — Interrupts, IPIs & Watchdog](03_Interrupts_IPI_and_Watchdog.md)
ARM64 exception model (ELs, VBAR_EL1, vector table, ESR/ELR/SPSR) · GIC v2/v3/v4
(distributor, redistributor, ICC registers, SGI/PPI/SPI/LPI) · Linux IRQ
subsystem (irq_desc/chip/domain, request_irq, flow handlers) · top/bottom
half (softirq, tasklet, workqueue, threaded IRQ) · IPIs (smp_call_function) ·
interrupt preemption & PMR masking · interrupt-storm detection & mitigation ·
hardware vs software watchdog, hard/softlockup detectors · Qualcomm APSS WDT,
SDI, SCM, bark vs bite · panic from watchdog, all-CPU backtrace, RAM dump.

### [04 — Linux Drivers, Device Tree, procfs/sysfs & System Call Flow](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)
Linux device model (kobject, kset, bus_type, device, driver) · character
drivers (cdev, file_operations, full skeleton) · block drivers (gendisk,
blk-mq, bio) · char vs block comparison · platform & misc drivers · device
tree (DTS/DTSI/DTB, dtc, overlays, of_* API, worked DTS+C example) · procfs
(seq_file) · sysfs (kobject + DEVICE_ATTR) · sysfs vs procfs vs debugfs vs
configfs · udev/uevent · ARM64 syscall flow (svc#0 → vector → sys_call_table
→ SYSCALL_DEFINE) · ioctl (_IO/_IOR/_IOW/_IOWR, compat_ioctl).

### [05 — Boot Flow, U-Boot, APPSBL & Hibernation](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)
ARM64 boot architecture (EL3→EL2→EL1) · Qualcomm power-on chain
(PBL→SBL/XBL→TZ/Hyp→APPSBL→kernel) · PBL ROM responsibilities · SBL/XBL,
DDR training, fuse blowing · TrustZone/QSEE & Hypervisor bring-up · APPSBL/
LK/ABL (DTB selection, fastboot, A/B slots) · APPSBL debugging scenarios ·
U-Boot architecture (SPL, env, fdt, bootcmd) · U-Boot verified boot (FIT,
signatures, rollback) · ARM Trusted Firmware (BL1/2/31/32/33, PSCI, SMC) ·
Linux kernel entry (head.S, `__primary_switched`, start_kernel) · cmdline &
DTB handoff · initramfs/initrd · Android boot.img/vbmeta/AVB 2.0 ·
hibernation (S4, swsusp) vs suspend-to-RAM · boot optimization (deferred
probe, async probe, image compression, PSCI parallel CPU_ON, initcall_debug)
· boot-time measurement (bootchart, systemd-analyze).

### [06 — Crash Dumps & Kernel Errors](06_Crash_Dump_and_Kernel_Errors.md)
Error taxonomy (Oops/BUG/panic/WARN/hardlockup/softlockup/RCU-stall) · ARM64
oops decoding (PC/LR/SP/ESR/FAR/x0..x30) · stack trace interpretation
(addr2line, faddr2line, kallsyms, frame pointer vs unwind tables) · BUG_ON /
WARN_ON / panic() internals · soft/hardlockup detectors · RCU stall · OOM
killer · KASAN, KFENCE, slub_debug, page-owner, kmemleak · UAF/double-free/
OOB signatures · lockdep deadlock reports · kdump & vmcore · `crash(8)`
commands · Qualcomm RAM dumps (SDI, T32/Trace32, QPST/QXDM, log_buf
extraction) · pstore/ramoops · watchdog-triggered dumps · 5 worked crash
walkthroughs · debugging-tool cheatsheet.

### [07 — Qualcomm Platform Internals — IOMMU/SMMU & Subsystems](07_Qualcomm_Platform_IOMMU_SMMU.md)
Qualcomm SoC architecture (APSS, modem, ADSP/CDSP/SLPI, GPU, video) ·
RPMh/RPM/APR · SMP2P/SMEM/QMI · clock framework (GCC, MMCC) · regulator &
PMIC (SPMI) · pinctrl/TLMM · ARM SMMU vs MMU · SMMUv2 vs SMMUv3 (stream IDs,
context banks, S1/S2/Nested) · key SMMU registers (CR0, S2CR, CB_TTBR/TCR/
MAIR) · stream matching · Linux IOMMU subsystem (iommu_ops, iommu_domain,
iommu_group, of_iommu) · DMA-IOMMU layer · arm-smmu driver walk-through ·
Qualcomm SMMU (qcom_iommu, secure CBs, TZ-owned SMMU) · fault handling
(CB_FSR, CB_FAR) · DMA-BUF for graphics/camera/video · debugging.

### [08 — C Strings, Memory Layout & Bitwise Fundamentals](08_C_Strings_Bitwise_Fundamentals.md)
Process memory layout (text/rodata/data/bss/heap/stack) · string literals
vs char arrays (`char *s` vs `char s[]`) · pointer arithmetic · strcpy /
strncpy / strlcpy / strdup / strtok / snprintf · safe string handling · UTF-8
basics · bitwise operators (& | ^ ~ << >>) & signed-shift UB · integer
promotion · bit tricks (set/clear/toggle/test, popcount, parity,
power-of-two, swap-without-temp, branchless abs) · endianness (htonl/ntohl,
detection) · bit-fields · bitmasks for register programming · kernel-style
bitmaps · IEEE 754 layout & FP bit tricks.

### [09 — Interview Q&A Master](09_Interview_QA_Master.md)
~200 distinct questions across 17 topics, each with a complete answer and a
deep-dive link into the matching topical doc above. Includes a "Quick-Fire
One-Liners" section of 50+ rapid-recall items and a Behavioral/Scenario
section for debugging-driven interviews.

---

## Source Manifest

The following 36 `.docx` files were extracted, normalized, and consolidated
into the documents above. Originals remain untouched in the parent folder.

| # | Source `.docx` | Consolidated into |
|---|----------------|-------------------|
| 1 | ARM_ARM64_Memory_Management_Part1.docx | 01 |
| 2 | ARM_ARM64_Memory_Management_Part2.docx | 01 |
| 3 | ARM_ARM64_Memory_Management_Part3.docx | 01 |
| 4 | ARM_ARM64_Memory_Management_Part4.docx | 01 |
| 5 | ARM_ARM64_Memory_Management_Part5.docx | 01 |
| 6 | Linux_Kernel_Memory_Management_mmap_Driver_Part1.docx | 01 |
| 7 | Linux_Scheduling_Internals.docx | 02 |
| 8 | ARM_Kernel_Scheduling_Part2.docx | 02 |
| 9 | linux_sync_part1.docx | 02 |
| 10 | linux_sync_part2.docx | 02 |
| 11 | ARM_Interrupt_Driver_Reference_Sandeep_Kumar.docx | 03 |
| 12 | ARM_Interrupt_Preemption_and_Interrupt_Storm___Complete_Guide_2026_05_14T20_23_08.docx | 03 |
| 13 | watchdog_ipi_ping_reference.docx | 03 |
| 14 | Part1_KernelPanic_QualcommWatchdog.docx | 03 |
| 15 | Linux_Block_vs_Character_Drivers_Part2.docx | 04 |
| 16 | Device_Tree_Concepts_Sandeep_Kumar.docx | 04 |
| 17 | proc_sysfs_interview_guide.docx | 04, 09 |
| 18 | Linux_Kernel_Internals_Part1_SystemCallFlow.docx | 04 |
| 19 | Qualcomm_Boot_Flow_Doc1_Power_On_to_Kernel.docx | 05 |
| 20 | Part2_UBoot_Verified_Boot.docx | 05 |
| 21 | Part3_Qualcomm_Device_Boot_Flow.docx | 05 |
| 22 | APPSBL_Debugging_Scenarios_Doc2.docx | 05 |
| 23 | Hibernation_and_Boot_Optimization_Doc3.docx | 05 |
| 24 | Crash_Dump_Analysis_Guide_Part1.docx | 06 |
| 25 | Crash_Dump_Analysis_Guide_Part2.docx | 06 |
| 26 | Crash_Dump_Analysis_Guide_Part3.docx | 06 |
| 27 | Linux_Kernel_Errors_Part1_of_3.docx | 06 |
| 28 | Linux_Kernel_Errors_Part2_of_3.docx | 06 |
| 29 | Linux_Kernel_Errors_Part3_of_3.docx | 06 |
| 30 | Qualcomm_Interview_Guide_Part1.docx | 07, 09 |
| 31 | Qualcomm_Interview_Guide_Part2.docx | 07, 09 |
| 32 | Qualcomm_Interview_Part3_IOMMU_SMMU.docx | 07, 09 |
| 33 | qualcomm_linux_kernel_prep_part1.docx | 07, 09 |
| 34 | qualcomm_linux_kernel_prep_part2.docx | 07, 09 |
| 35 | qualcomm_linux_kernel_prep_part3.docx | 07, 09 |
| 36 | bitwise_interview_guide.docx | 08, 09 |

Supplementary inputs folded in:

| Source | Type | Consolidated into |
|--------|------|-------------------|
| C_String_Fundamentals_and_Memory_Layout_2026_05_15T06_00_48.c | C source | 08 |
| Comprehensive_Bitwise_Interview_Questions_with_Detailed_Explanations_2026_05_15T05_36_37.c | C source | 08 |
| table_2026_05_15T05_36_54.md | Markdown table | 08 |
| arm_linux_memory_management.pdf | PDF (referenced; not text-extracted — see Note) | — |

> **Note on the PDF.** `arm_linux_memory_management.pdf` was identified as a
> source but no PDF text-extraction tool (`pdftotext`, `pdfminer`, `pandoc`,
> Python) was available on the build host. Its subject matter is already
> exhaustively covered by `ARM_ARM64_Memory_Management_Part1..5.docx` (which
> were extracted and consolidated into doc 01). Re-run extraction with
> `pdftotext` installed if PDF-only content is later required.

---

## Topic Cross-Reference Matrix

Where each major concept is treated in depth.

| Concept | Primary | Also discussed in |
|---------|---------|-------------------|
| Page tables / TLB | 01 | 07 (SMMU stage-1/2) |
| Page-fault handler | 01 | 06 (Oops decoding) |
| mmap / driver mmap | 01 | 04 (char driver) |
| DMA (coherent vs streaming) | 01 | 04, 07 (DMA-IOMMU) |
| Context switch on ARM64 | 02 | 03 (preemption from IRQ) |
| Spinlock / mutex / RCU | 02 | 06 (lockdep reports) |
| Preempt models | 02 | 03 (in_interrupt) |
| GIC / IRQ subsystem | 03 | 07 (Qualcomm-specific routing) |
| Watchdog / panic | 03 | 06 (panic path & dumps) |
| Device tree | 04 | 05 (DTB at boot), 07 (iommus property) |
| Character/block drivers | 04 | 01 (mmap fops) |
| procfs / sysfs | 04 | 06 (`/proc/vmcore`, `/sys/class/watchdog`) |
| Syscall flow | 04 | 02 (preempt on syscall exit) |
| Boot flow | 05 | 03 (early IRQs), 07 (subsystem bring-up) |
| ATF / PSCI | 05 | 02 (CPU hotplug via PSCI) |
| Hibernation / suspend | 05 | 03 (IRQ disable/restore) |
| Oops / panic / kdump | 06 | 03 (watchdog-triggered), 02 (lockdep) |
| KASAN / kmemleak | 06 | 01 (kernel mem layout) |
| Qualcomm subsystems | 07 | 05 (boot bring-up) |
| SMMU / IOMMU | 07 | 01 (page tables), 04 (DMA APIs) |
| C memory layout & bit tricks | 08 | 09 (Q&A) |
| Interview Q&A | 09 | 01–08 (deep-dive links) |

---

## Dedupe Log

The following source series had substantial verbatim or near-verbatim
overlap; the most complete version of each repeated section was kept and the
rest dropped during synthesis:

- `ARM_ARM64_Memory_Management_Part1..Part5.docx` — overlapping coverage of
  TTBR/TCR/MAIR, granule sizes, and Linux memory layout. Consolidated into
  unified sections §3, §5, §9 of doc 01.
- `Crash_Dump_Analysis_Guide_Part1..Part3.docx` — overlapping coverage of
  oops format and ARM64 register dump decoding. Consolidated into §3, §4
  of doc 06.
- `Linux_Kernel_Errors_Part1..Part3_of_3.docx` — overlapping coverage of
  BUG/WARN/panic semantics and softlockup. Consolidated into §2, §5, §6
  of doc 06.
- `linux_sync_part1.docx` / `part2.docx` — overlapping coverage of spinlock
  and mutex. Consolidated into §10, §11 of doc 02.
- `Qualcomm_Interview_Guide_Part1..2.docx` and
  `qualcomm_linux_kernel_prep_part1..3.docx` — overlapping coverage of
  RPMh/SMP2P/SMEM and clock framework. Consolidated into §3, §4, §5 of doc 07.
- Interview-style Q&A material across all `*_interview_guide.docx`,
  `Qualcomm_Interview_*.docx`, and `qualcomm_linux_kernel_prep_*.docx` was
  routed to doc 09 (deduped by normalized question text).

---

## Build Notes

- All `.docx` files were extracted with a PowerShell + .NET ZIP/XML pipeline
  (`_extract_docx.ps1` in the parent folder). Heading levels, bullet lists,
  and table rows were preserved.
- `_raw_text/` subfolder contains the per-source intermediate Markdown. Safe
  to delete once you have validated the consolidated documents.
- Re-run end-to-end: `powershell -ExecutionPolicy Bypass -File
  ..\_extract_docx.ps1` then re-author the topical docs.

---

## How to Use This Knowledgebase

1. **First-time read:** follow the [Reading Order](#reading-order-beginner--advanced)
   table above (08 → 01 → 04 → 03 → 02 → 05 → 07 → 06 → 09).
2. **Interview prep:** start with [09_Interview_QA_Master.md](09_Interview_QA_Master.md),
   click "Deep dive →" links for any answer you want to expand.
3. **On-call debugging:** jump straight to
   [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md) and
   [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md).
4. **New-platform bring-up:** read 05 → 07 → 04 in order.
