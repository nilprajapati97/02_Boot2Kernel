# ARM64 Boot Sequence and Memory Setup Complete Reference

Category: KASLR and Kernel Boot Memory Setup  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Boot memory setup is a phased transition from firmware-managed physical memory to fully featured kernel virtual memory management.

Phases:
1. firmware handoff
2. early identity and temporary mappings
3. memblock discovery and reservations
4. full kernel page-table construction
5. allocator handoff from memblock to buddy

Goal:
- reach stable, protected, and performant kernel address space safely.

---

## 2. ARM64 Hardware Detail

### 2.1 Core register timeline

Major register programming order:
- MAIR_EL1 and TCR_EL1 configured
- TTBR roots installed for early and then final mappings
- SCTLR_EL1 enables MMU and cache behavior
- barriers ensure architectural visibility and fetch correctness

### 2.2 Address-space construction checkpoints

Key layout regions established during boot:
- kernel image mapping with correct permissions
- linear map for RAM
- fixmap slots for low-level deterministic mappings
- vmemmap for struct page metadata
- vmalloc area for later dynamic kernel virtual allocations

---

## 3. Linux Kernel Implementation

### 3.1 High-level call-chain reference

Representative progression:
1. early architecture entry and setup
2. architecture memory initialization
3. paging initialization
4. sparse and memmap setup
5. mm initialization and allocator bring-up

Core concepts in this category:
- KASLR offset and placement
- idmap transition safety
- memblock region accounting
- final swapper root installation
- handoff to buddy allocator

### 3.2 Allocator availability timeline

Early phase:
- memblock only

Mid phase:
- page metadata and zones being built

Late phase:
- buddy allocator active
- higher-level allocators and vmalloc fully usable

---

## 4. Hardware-Software Interaction

Integrated timeline:
1. Firmware loads kernel and passes memory descriptors.
2. Kernel runs early transition code using temporary mappings.
3. MMU setup completes and final kernel translation context is installed.
4. memblock reserves kernel, initrd, firmware-critical, and special ranges.
5. page metadata and zones are initialized.
6. free pages are released to buddy allocator.
7. normal kernel runtime memory model starts.

Risk-controlled progression:
- each stage limits dependency on subsystems not initialized yet
- deterministic early mappings prevent control-flow loss during transitions

---

## 5. Interview Q and A

Q1: Why does Linux keep memblock after buddy is active?
It remains useful for bookkeeping and some late early-boot style operations, but buddy becomes primary page allocator.

Q2: What is the most fragile moment in boot memory setup?
The MMU and translation-root transition window where mapping assumptions are changing.

Q3: Why are fixed mappings needed if linear map exists later?
Because early stages and special low-level paths need deterministic addresses before full VM infrastructure is ready.

Q4: What ties KASLR into memory initialization?
KASLR changes effective kernel base while all mapping and section-permission logic must still be applied correctly.

Q5: When can the kernel safely free init-only memory?
After one-time initialization paths complete and no live references remain.

Q6: Why separate early and final page tables?
Early tables are minimal and transitional; final tables implement complete long-term policy and layout.

---

## 6. Pitfalls and Gotchas

- Treating early boot mappings as equivalent to final runtime mappings.
- Losing track of reserved regions during memblock to buddy handoff.
- Underestimating firmware memory-map variability across platforms.
- Benchmarking boot-time overhead without separating entropy, mapping, and device-init costs.
- Failing to align security hardening with boot-time debug instrumentation.

---

## 7. Quick Reference Table

| Boot stage | Memory manager focus |
|---|---|
| Early entry | idmap and temporary tables |
| Arch init | memblock discovery and reservations |
| Paging init | final kernel mapping construction |
| mm init | zones plus page metadata |
| Post-handoff | buddy allocator primary |

| Decision area | Primary concern |
|---|---|
| KASLR | exploit uncertainty versus debuggability |
| fixmap | deterministic low-level mapping slots |
| idmap | safe MMU transition control flow |
| swapper root | stable long-term kernel translation context |
