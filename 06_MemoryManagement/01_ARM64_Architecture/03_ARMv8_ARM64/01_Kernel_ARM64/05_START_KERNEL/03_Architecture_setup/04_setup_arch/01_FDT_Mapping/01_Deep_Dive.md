The two lines in setup.c are doing one very specific early-boot job:

if the bootloader passed a boot-data pointer in r2, Linux saves that raw physical address in __atags_pointer and then converts it into a temporary virtual address so early C code can safely inspect it.

The exact code is:

```c
if (__atags_pointer)
        atags_vaddr = FDT_VIRT_BASE(__atags_pointer);
```

What each piece means:

- __atags_pointer is the raw boot-data pointer from the ARM boot ABI. In early assembly, the kernel stores register r2 into this variable in head-common.S.
- According to the ARM boot protocol, r2 contains the physical address of either:
  1. an ATAGS list, or
  2. a DTB, device tree blob.
  That is documented in booting.rst.
- FDT_VIRT_BASE is not reading the data yet. It is translating that physical address into the fixed early virtual mapping used by ARM during boot. The macro is defined in memory.h.

The important interview point is that this is an address-space conversion, not a format conversion.

Why is that needed? Because once setup_arch() runs, the MMU is active, and C code cannot just dereference an arbitrary physical address directly. So ARM creates a temporary fixed virtual mapping for the boot-data area. That mapping is set up in two places:

- In early page-table setup, head.S maps the region pointed to by r2 into the fixed FDT area, as shown in head.S.
- Later, the MMU code creates a read-only mapping for that same physical region at the fixed virtual base, in mmu.c.

That is why the macro is:

- FDT_FIXED_BASE = fixed virtual window
- FDT_VIRT_BASE(physbase) = fixed virtual base plus the offset within the section

So if the bootloader placed the DTB or ATAGS at some physical address like 0x82001000, the kernel does not access 0x82001000 directly in C. It accesses the corresponding fixed virtual address, conceptually something like 0xff801000. The offset is preserved, only the base changes.

The naming can be confusing: __atags_pointer sounds like it must always point to ATAGS, but in modern ARM boot it may actually point to a DTB. The code handles that by trying DT first:

- setup_arch() computes atags_vaddr from the bootloader pointer in setup.c.
- It first calls setup_machine_fdt(atags_vaddr) in setup.c.
- setup_machine_fdt() verifies whether the memory at that address is a valid DTB using early_init_dt_verify(), in devtree.c.
- If that succeeds, the kernel treats it as a DTB, scans the device tree, and even updates the machine type from the matched machine_desc in devtree.c.
- If DT parsing fails, the kernel falls back to setup_machine_tags(atags_vaddr, __machine_arch_type) in setup.c, which interprets the same memory as a legacy ATAGS list in atags_parse.c.

So the real control flow is:

1. Bootloader passes machine type in r1 and boot-data physical address in r2.
2. Kernel stores r1 into __machine_arch_type and r2 into __atags_pointer in early assembly.
3. setup_arch() converts the physical boot-data pointer into an early virtual address.
4. Kernel first tests whether that memory contains a valid DTB.
5. If yes, use device tree boot.
6. If not, try legacy ATAGS parsing.
7. If neither works, boot fails with the invalid dtb / unsupported machine error.

One more subtle point that interviewers like: after successful DT parsing, the kernel reserves the DTB memory using the original physical address, not the temporary virtual address:

- memblock_reserve(__atags_pointer, fdt_totalsize(atags_vaddr)) in setup.c

That is correct because memblock tracks physical memory reservations.

A clean interview answer would be:

This code checks whether the bootloader passed boot parameters in r2. If it did, the kernel converts that physical pointer into an early fixed virtual address so C code can safely read it after the MMU is enabled. The same pointer may reference either a legacy ATAGS list or a modern DTB. Linux first tries to validate it as a DTB, and if that fails, falls back to ATAGS parsing. So the line is part of the ARM early-boot bridge from bootloader-provided physical boot data to kernel-readable virtual memory.

A slightly stronger version if they ask why FDT_VIRT_BASE is needed:

Because r2 is a physical address from firmware, but setup_arch() is running in virtual-addressed kernel code. ARM solves that by mapping the boot-data region into a fixed early virtual window and then preserving the offset with FDT_VIRT_BASE().

If you want, I can also turn this into:
1. a 30-second interview answer,
2. a 2-minute deep answer, or
3. a diagram of the ARM boot flow from r1/r2 to setup_machine_fdt/setup_machine_tags.

**Core Idea**

In ARMv8, memory addressing is not “one address used directly by the CPU” in all cases. You should separate it into three layers:

1. Physical address
2. Virtual address
3. CPU translation and caching

The most important interview answer is this:

Physical addresses are assigned by the hardware platform and firmware description, while virtual addresses are assigned by the operating system. The CPU uses the MMU, page tables, TLBs, and caches to translate and access memory efficiently.

**What “address assignment” means in ARMv8**

There are two different kinds of address assignment.

1. Physical address assignment

This is the SoC or board memory map. The chip designer decides where DRAM, UART, GIC, SRAM, PCIe windows, and other devices live in the physical address space.

Example from this tree:
morello.dtsi
shows devices with fixed physical addresses such as:
- mailbox at 0x45000000
- sram at 0x06000000

That means those addresses are hardware-decided MMIO regions, not chosen by a user process and not invented by the CPU at runtime.

2. Virtual address assignment

This is done by the OS. Linux gives each process its own virtual address space and also maintains a kernel virtual address space. The actual arm64 Linux layout is described in memory.rst.

So the same CPU core may execute:
- user code using user virtual addresses
- kernel code using kernel virtual addresses
- both eventually map to physical memory or device regions

**How the CPU sees memory**

From the CPU point of view, the flow is:

Virtual Address
-> MMU lookup through translation tables
-> Physical Address
-> Cache/TLB system
-> DRAM or MMIO device

So the CPU does not usually “use physical address directly” once the MMU is enabled.

What the CPU uses:

- TTBR0_EL1
  Holds the base of user-space page tables
- TTBR1_EL1
  Holds the base of kernel page tables
- TCR_EL1
  Controls translation parameters such as address size and granule size
- MAIR_EL1
  Describes memory attributes such as normal memory vs device memory
- TLB
  Caches recent virtual-to-physical translations
- Caches
  Cache actual memory contents after translation

A useful kernel note is in memory.rst, which explains TTBR selection. On arm64 Linux, user mappings and kernel mappings are separated, and the CPU selects the appropriate translation base depending on the virtual address range.

**With respect to CPU and exception levels**

ARMv8 defines exception levels:

- EL0: user applications
- EL1: kernel
- EL2: hypervisor
- EL3: secure monitor / trusted firmware

This matters because memory access permissions and visible translation controls depend on the current exception level.

A clean interview statement is:

The CPU does not treat all code equally. At EL0 it can only access user-permitted mappings. At EL1 the kernel can access privileged mappings. At EL2 a hypervisor can control or virtualize memory for guest kernels. At EL3 secure firmware manages secure-world control.

The arm64 boot protocol in booting.rst also makes this explicit: the kernel is entered in EL1 or EL2, with MMU off initially.

**How memory is translated**

ARMv8 translation is page-table based.

Typical Stage 1 translation:

- A process uses a virtual address
- CPU checks TLB first
- If translation is not in TLB, hardware page-table walk starts
- Page-table entries describe:
  - physical frame number
  - read/write/execute permissions
  - privileged/user accessibility
  - cacheability / memory type
  - shareability

The Linux arm64 memory document notes that AArch64 can use multiple levels of translation tables, depending on page size and VA size:
memory.rst

So when people ask “how is memory assigned,” the correct answer is:

- hardware assigns the physical map
- kernel assigns virtual mappings
- CPU performs translation using translation tables

**Address spaces in practice**

Think of three common cases:

1. Normal DRAM for a process

A process sees something like a stack or heap virtual address.
That virtual address is translated to some physical DRAM page.
Another process may use the same virtual address but map to a different physical page.

2. Kernel linear map

Linux usually keeps a direct or linear mapping of RAM in the kernel virtual space, so the kernel can access RAM efficiently through a stable kernel VA region.
That is part of the arm64 virtual layout described in memory.rst.

3. Device MMIO

A device has a fixed physical address, for example a UART register block.
The kernel maps that physical MMIO region into kernel virtual address space.
Then the CPU accesses the virtual mapping, not the raw physical address directly after MMU is enabled.

**Where ASID fits with CPU**

ASID means Address Space Identifier.

This is extremely important with respect to CPU efficiency.

Without ASID:
when switching from process A to process B, the CPU would need frequent TLB invalidation because the same virtual address could mean different physical pages.

With ASID:
the CPU tags TLB entries with an address-space identity, so translations from different processes can coexist safely.

Linux arm64 manages this in context.c.

Interview line:

ASID allows the CPU to distinguish translations belonging to different processes, reducing TLB flushes and improving context-switch performance.

**How caches relate to addresses**

Another interview trap is confusing cache lookup with translation.

The safe explanation is:

- the CPU starts with a virtual address
- translation gives a physical address
- cache and coherency rules determine how data is fetched or shared
- for normal memory, caching is enabled according to attributes
- for device memory, accesses are strongly controlled and not treated like normal cacheable RAM

Memory attributes are part of the page-table mapping. That is why the same CPU can treat:
- DRAM as cacheable normal memory
- MMIO as device memory
even though both are “memory accesses”

**Stage 2 translation in virtualization**

If virtualization is enabled:

- guest OS thinks it owns “physical” memory
- that guest physical address is not final
- EL2 hypervisor performs a second translation:
  - guest virtual -> guest physical at Stage 1
  - guest physical -> host physical at Stage 2

So under virtualization, the CPU may do two levels of translation.

A crisp interview line:

In ARMv8 virtualization, guest memory is usually translated twice: once by the guest OS and again by the hypervisor.

**Boot-time perspective**

At boot on arm64:

- firmware loads the kernel and DTB into physical RAM
- x0 contains physical address of the DTB
- kernel enters at EL1 or EL2
- MMU starts off disabled
- then Linux builds early page tables and turns MMU on

That is documented in booting.rst.

This is the best answer if asked:
“How does memory addressing start before the MMU is on?”

Answer:
Initially the CPU uses physical addressing. After the kernel builds page tables and enables the MMU, execution proceeds using virtual addressing.

**Simple mental model**

Use this model in interviews:

- Hardware designer decides physical map
- Firmware describes the platform, usually via device tree
- Kernel creates virtual mappings
- CPU translates virtual to physical through MMU
- TLB accelerates translation
- ASID separates process address spaces
- Caches accelerate data access
- EL determines privilege and control over memory translation

**One strong interview answer**

ARMv8 memory addressing works in two domains. Physical addresses belong to the hardware memory map, while virtual addresses are created by the operating system for processes and the kernel. The CPU normally works with virtual addresses, and the MMU translates them through page tables into physical addresses. TTBR registers point to the translation tables, the TLB caches recent translations, ASIDs keep process address spaces separate, and cache and memory attributes decide how the access behaves. At EL0 user code uses user mappings, at EL1 the kernel uses privileged mappings, and at EL2 a hypervisor can add second-stage translation for guests.

**If they ask “who assigns addresses?”**

Give this exact distinction:

- Physical addresses are assigned by SoC design and board integration
- Virtual addresses are assigned by the OS memory manager
- Translation permissions and behavior are enforced by the CPU MMU

**If they ask “with respect to CPU?”**

Say:

With respect to the CPU, memory is seen through translation state, privilege level, TLB, and cacheability attributes. The CPU does not just read an address blindly; it interprets it according to current exception level, translation tables, and memory attributes.

