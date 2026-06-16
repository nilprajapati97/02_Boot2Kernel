# System Registers 101

System registers are how privileged ARM64 software controls CPU behavior.

## Registers you must know for `__cpu_setup`

### `MAIR_EL1`
Defines the memory-attribute encodings used by page-table entries.

Question it answers:
What does attribute index `n` actually mean when a page-table entry uses it?

### `TCR_EL1`
Controls translation behavior.

Questions it answers:
- How large are the address spaces behind `TTBR0_EL1` and `TTBR1_EL1`?
- What page granule size is used?
- What cacheability and shareability policy applies to table walks?
- Are top-byte-ignore and related tagging controls active?

### `TCR2_EL1`
Extends translation control for newer architectural features.

### `SCTLR_EL1`
The high-level control register for EL1.

Important bits include:
- MMU enable
- data cache enable
- instruction cache enable
- alignment and execution behavior controls

### `CPACR_EL1`
Controls access to certain architectural features from lower exception levels.

### `MDSCR_EL1`
Controls parts of debug behavior.

## Identification registers

`__cpu_setup` reads identification registers like `ID_AA64MMFR1_EL1` and `ID_AA64MMFR3_EL1`.

These registers tell the kernel what the CPU actually supports at runtime.

That means Linux can build one kernel that adapts to different ARM64 cores.
