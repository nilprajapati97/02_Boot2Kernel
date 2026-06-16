I’ll trace the exact macro path and page-table mapping so I can show precisely how FDT_VIRT_BASE transforms the bootloader physical pointer into the early fixed virtual window, including the arithmetic and why two sections are mapped.

Read [](file:///home/nilprajapti/Workspace/nilprajapati97/KernelRepo/linux/arch/arm/include/asm/memory.h#80-80), lines 80 to 110

Read [](file:///home/nilprajapti/Workspace/nilprajapati97/KernelRepo/linux/arch/arm/kernel/head.S#292-292), lines 292 to 320

Read [](file:///home/nilprajapti/Workspace/nilprajapati97/KernelRepo/linux/arch/arm/mm/mmu.c#1378-1378), lines 1378 to 1398

I’ve confirmed macro and both mapping sites; next I’ll grab SECTION_SIZE and shift definitions so the conversion can be explained numerically, not just conceptually.

Searched for regex `#define SECTION_SHIFT|#define SECTION_SIZE|#define SECTION_MASK` (`**/arch/arm/include/asm/**`), 7 results

Read [](file:///home/nilprajapti/Workspace/nilprajapati97/KernelRepo/linux/arch/arm/kernel/setup.c#1097-1097), lines 1097 to 1114

Read [](file:///home/nilprajapti/Workspace/nilprajapati97/KernelRepo/linux/arch/arm/include/asm/memory.h#120-120), lines 120 to 134

Read [](file:///home/nilprajapti/Workspace/nilprajapati97/KernelRepo/linux/arch/arm/kernel/head-common.S#134-134), lines 134 to 148

Great question. This line is a pure address remap step, and it is easiest to understand as a memory-level pipeline.

FDT_VIRT_BASE(__atags_pointer)

What it does:
It converts a bootloader physical pointer into a fixed early virtual pointer so C code can read DTB or ATAGS safely after MMU setup starts.

Where it is used:
setup.c

Where the macro is defined:
memory.h

Formula used by the macro:
For MMU builds,
$VA = FDT\_FIXED\_BASE \;|\; (PA \bmod SECTION\_SIZE)$

In code terms from the header:
- FDT_FIXED_BASE = 0xff800000
- FDT_FIXED_SIZE = 2 * SECTION_SIZE
- FDT_VIRT_BASE(physbase) = fixed base + offset inside one section

Source:
memory.h

Memory-level view (end-to-end)

1. Bootloader level
Bootloader passes boot-data physical address in register r2. Kernel stores it in __atags_pointer.
Source:
head-common.S

2. Early mapping level in assembly
Before full C init, kernel maps the r2 physical region into a fixed virtual window at FDT_FIXED_BASE. It maps two sections to handle boundary crossing.
Source:
head.S

3. C-level pointer conversion
setup_arch calls FDT_VIRT_BASE(__atags_pointer), which keeps only the intra-section offset and places it under the fixed VA window.
Source:
setup.c

4. MMU page-table level
Later, MMU code creates a read-only mapping:
- physical base = __atags_pointer & SECTION_MASK
- virtual base = FDT_FIXED_BASE
- length = FDT_FIXED_SIZE
Source:
mmu.c

5. Parser level
Using that converted VA, kernel first tries DT parse, then ATAGS fallback.
Source:
setup.c

Why the conversion looks like this

- It does not convert DT format.
- It converts address space only: physical to early virtual alias.
- Offset is preserved, base is replaced.

Example with numbers

Assume:
- __atags_pointer = 0x82001000
- SECTION_SIZE = 1MB (2-level case)

Then:
- offset = 0x82001000 mod 0x100000 = 0x1000
- VA = 0xff800000 | 0x1000 = 0xff801000

So C code reads at 0xff801000, which points to the same bytes as physical 0x82001000 through that temporary mapping.

Why map two sections

DTB or ATAGS may start near end of a section and continue into next one.
That is why assembly comment says map 2 sections.
Source:
head.S

Section size depends on paging mode

- 2-level ARM:
SECTION_SHIFT = 20, SECTION_SIZE = 1MB
Source:
pgtable-2level.h

- 3-level ARM:
SECTION_SHIFT = 21, SECTION_SIZE = 2MB
Source:
pgtable-3level.h

So FDT_FIXED_SIZE becomes:
- 2MB on 2-level
- 4MB on 3-level

Important interview point

This line is a temporary boot-time physical-to-virtual bridge for boot parameters. It preserves offset, changes region base, and relies on early section mappings so setup_arch can dereference boot data safely.

Extra nuance

In no-MMU builds, macro is identity:
FDT_VIRT_BASE(physbase) = physbase
Source:
memory.h

If you want, I can give you a small ASCII diagram with PA section, VA fixed window, and offset mapping for both 1MB and 2MB section cases.


I’ll pull the exact lines from the header and then rewrite the FDT_VIRT_BASE arithmetic using those constants so you can place raw code in your notes instead of a file reference.

Read [](file:///home/nilprajapti/Workspace/nilprajapati97/KernelRepo/linux/arch/arm/include/asm/pgtable-3level.h#44-44), lines 44 to 56

Use this directly in your notes instead of referencing pgtable-3level.h:

    /*
     * section address mask and size definitions.
     */
    #define SECTION_SHIFT        21
    #define SECTION_SIZE         (1UL << SECTION_SHIFT)
    #define SECTION_MASK         (~((1 << SECTION_SHIFT) - 1))

How this affects FDT_VIRT_BASE conversion:

    #define FDT_FIXED_BASE       UL(0xff800000)
    #define FDT_VIRT_BASE(physbase) \
            ((void *)(FDT_FIXED_BASE | ((physbase) % SECTION_SIZE)))

With SECTION_SHIFT = 21:
1. SECTION_SIZE = 2MB
2. SECTION_MASK = 0xFFE00000 style alignment mask
3. FDT_VIRT_BASE keeps only the offset inside that 2MB section and places it under fixed virtual base 0xff800000

Equivalent math view:

$$
VA = 0xff800000 + (PA \bmod 2MB)
$$

Example:
1. PA = 0x82001000
2. Offset inside 2MB = 0x001000
3. VA = 0xff801000

So this is physical to early-virtual remapping by preserving intra-section offset, not data-format conversion.
