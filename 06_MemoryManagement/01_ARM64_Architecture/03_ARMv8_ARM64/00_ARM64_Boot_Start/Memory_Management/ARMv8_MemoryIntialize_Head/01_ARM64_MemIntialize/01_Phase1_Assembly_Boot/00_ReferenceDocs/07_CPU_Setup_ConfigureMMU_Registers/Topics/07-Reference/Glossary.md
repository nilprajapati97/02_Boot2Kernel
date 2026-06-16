# Glossary

## AArch64
The 64-bit execution state of the ARM architecture.

## AMU
Activity Monitor Unit. Hardware counters related to CPU activity.

## ASID
Address Space Identifier. A tag used to separate TLB entries belonging to different address spaces.

## CPACR_EL1
Architectural register that controls access to certain coprocessor and floating-point functionality from lower exception levels.

## DSB
Data Synchronization Barrier. Ensures certain memory effects are complete before continuing.

## EL0, EL1, EL2
ARM exception levels. EL0 is user mode, EL1 is kernel mode, EL2 is hypervisor mode.

## HAFDBM
Hardware Access Flag and Dirty Bit Management. Lets hardware update page-table access state.

## ID map
Identity map. A temporary mapping where virtual and physical addresses correspond closely enough for early execution.

## IPS
Intermediate Physical Address Size field inside `TCR_EL1`. It selects the physical address size used by translation.

## ISB
Instruction Synchronization Barrier. Ensures later instructions execute using the latest architectural state.

## MAIR_EL1
Memory Attribute Indirection Register at EL1. It defines the meaning of attribute indexes used in page-table entries.

## MDSCR_EL1
Monitor Debug System Control Register at EL1. Controls debug-related behavior.

## MMU
Memory Management Unit. Hardware that translates virtual addresses to physical addresses and enforces attributes and protections.

## PIE
Permission Indirection Extension. A newer ARM feature for mapping permission indices to effective permissions.

## PMU
Performance Monitor Unit. Hardware counters used for profiling and performance observation.

## SCTLR_EL1
System Control Register at EL1. Contains key control bits such as MMU enable, cache enable, and alignment behavior.

## TCR_EL1
Translation Control Register at EL1. Controls address size, granule size, cacheability of table walks, shareability, tagging behavior, and more.

## TCR2_EL1
An extended translation control register used by newer architectural features.

## TLB
Translation Lookaside Buffer. A cache of recent address translations.

## TTBR0_EL1 and TTBR1_EL1
Translation Table Base Registers for EL1. TTBR0 usually covers lower VA space and TTBR1 usually covers the kernel higher VA space.

## VA
Virtual address.

## PA
Physical address.

## VA52
A 52-bit virtual-addressing capability available on supported ARM64 systems.
