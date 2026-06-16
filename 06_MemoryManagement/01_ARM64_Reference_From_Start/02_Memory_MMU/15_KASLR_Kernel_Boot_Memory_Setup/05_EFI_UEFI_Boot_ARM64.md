# EFI and UEFI Boot Path on ARM64 Deep Dive

Category: KASLR and Kernel Boot Memory Setup  
Platform: ARM64 (AArch64), UEFI systems

---

## 1. Concept Foundation

UEFI provides a standardized boot environment for ARM64 platforms.

Why it matters for memory setup:
- Firmware owns memory map before ExitBootServices.
- Kernel must ingest and preserve runtime-service regions correctly.
- Early page-table construction depends on accurate EFI descriptors.

Core stages:
1. UEFI loads Linux Image.
2. EFI stub executes and negotiates handoff.
3. ExitBootServices transfers ownership model to OS.
4. Kernel finalizes memory map and runtime mappings.

---

## 2. ARM64 Hardware Detail

### 2.1 Image entry and execution state

ARM64 Image header and PE/COFF compatibility allow UEFI loader interoperability.
- Loader passes control in defined register/state conventions.
- CPU is in AArch64 state with expected exception-level setup per platform policy.

### 2.2 EFI memory descriptor semantics

EFI memory map contains typed regions, for example:
- Conventional memory
- Loader code and data
- Boot services code and data
- Runtime services code and data
- ACPI reclaim and NVS areas

Kernel interpretation determines:
- what becomes general RAM
- what must remain reserved
- what requires stable virtual mapping for runtime calls

### 2.3 Runtime mapping constraints

Runtime service regions require persistent mapping attributes:
- executable permissions where needed
- correct cacheability and access flags
- stable virtual address assignment after SetVirtualAddressMap

---

## 3. Linux Kernel Implementation

### 3.1 EFI stub responsibilities

EFI stub does early work before normal kernel init:
- parse and validate memory map
- gather entropy and boot parameters
- prepare initrd and command line handoff
- coordinate ExitBootServices call timing

### 3.2 ExitBootServices flow

Critical sequence:
1. Get memory map and map key.
2. Prepare to exit boot services.
3. Call ExitBootServices with matching key.
4. If map changed and key invalid, retry process.

After success:
- boot services are unavailable
- kernel owns memory management decisions

### 3.3 Runtime service setup

Kernel runtime initialization includes:
- preserving runtime descriptors
- constructing runtime page-table view
- setting virtual addresses for runtime regions
- enabling runtime calls through mapped EFI context

Conceptually tied pieces:
- runtime page table root
- efi runtime init hooks
- architecture-specific page attribute helpers

---

## 4. Hardware-Software Interaction

End-to-end UEFI boot memory path:
1. Firmware allocates pages and loads kernel image.
2. EFI stub reads descriptor map.
3. Kernel exits boot services safely.
4. memblock ingests usable ranges and reserves special areas.
5. paging_init builds final kernel mappings.
6. EFI runtime regions stay mapped for later variable and clock services.

Failure examples:
- stale memory-map key at ExitBootServices causes handoff retry loops.
- incorrect runtime mapping attributes break runtime service calls later in boot or suspend cycles.

---

## 5. Interview Q and A

Q1: Why is ExitBootServices timing sensitive?
Because any memory map mutation between key retrieval and call invalidates the key and forces retry.

Q2: What happens to Boot Services memory after ExitBootServices?
Ownership transitions to the OS, which may reclaim it according to descriptor semantics.

Q3: Why keep runtime regions mapped?
UEFI runtime services remain callable after OS boot, requiring stable virtual mappings.

Q4: Is EFI memory map directly equivalent to Linux zones?
No. Linux converts EFI descriptors through architecture-specific policy into memblock and zone layouts.

Q5: What is a common boot bug in this area?
Incorrect handling of runtime descriptor attributes leading to runtime service faults.

Q6: Does UEFI eliminate need for device tree or ACPI parsing?
No. Platform description data is still needed for topology and devices.

---

## 6. Pitfalls and Gotchas

- Calling ExitBootServices with stale map key due to late allocations.
- Misclassifying descriptor types and accidentally reclaiming reserved firmware memory.
- Forgetting runtime mappings on kernels that need EFI variable support post-boot.
- Assuming all firmware implementations conform perfectly to spec behavior.
- Ignoring secure boot and measured-boot interactions in deployment design.

---

## 7. Quick Reference Table

| Stage | Main responsibility |
|---|---|
| EFI loader | Load kernel and initrd into memory |
| EFI stub | Parse map and prepare handoff |
| ExitBootServices | Transfer memory control to kernel |
| Kernel early mm | Build memblock and page tables |
| EFI runtime init | Preserve virtual mappings for runtime services |

| Descriptor class | Typical OS treatment |
|---|---|
| Conventional memory | Candidate for general RAM |
| Runtime services | Reserved plus persistently mapped |
| ACPI reclaim/NVS | Reserved by policy |
