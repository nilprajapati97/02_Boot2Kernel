Yes — the exact early boot-time function is `__enable_mmu()` in `arch/arm64/kernel/head.S`. That is where Linux checks whether the CPU supports the kernel’s configured translation granule before turning on the MMU. ([GitHub][1])

Here is the relevant code:

```asm
SYM_FUNC_START(__enable_mmu)
    mrs x3, ID_AA64MMFR0_EL1
    ubfx x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
    cmp x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
    b.lt __no_granule_support
    cmp x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
    b.gt __no_granule_support

    phys_to_ttbr x2, x2
    msr ttbr0_el1, x2
    load_ttbr1 x1, x1, x3
    set_sctlr_el1 x0
    ret
SYM_FUNC_END(__enable_mmu)
```

That function is documented right above the code as: “Checks if the selected granule size is supported by the CPU. If it isn’t, park the CPU.” ([GitHub][1])

Line by line:

`mrs x3, ID_AA64MMFR0_EL1`
Reads the CPU feature register `ID_AA64MMFR0_EL1` into `x3`. This register contains the `TGRAN4`, `TGRAN16`, and `TGRAN64` capability fields. ([GitHub][1])

`ubfx x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4`
Extracts a 4-bit field from that register.
The starting bit position comes from `ID_AA64MMFR0_EL1_TGRAN_SHIFT`, which depends on the kernel config:

* 4K kernel: use `TGRAN4` bits `[31:28]`
* 16K kernel: use `TGRAN16` bits `[23:20]`
* 64K kernel: use `TGRAN64` bits `[27:24]`

So this one instruction says: “take the relevant TGRAN field for the page size this kernel was built with.” ([GitHub][1])

`cmp x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN`
Compares the extracted value against the minimum accepted encoding for that granule. ([GitHub][1])

`b.lt __no_granule_support`
If the value is smaller than the allowed minimum, jump to failure handling. ([GitHub][1])

`cmp x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX`
Compares the same extracted value against the maximum accepted encoding. ([GitHub][1])

`b.gt __no_granule_support`
If the value is greater than the allowed maximum, the kernel concludes that this granule is not supported and branches to `__no_granule_support`. ([GitHub][1])

So the actual logic is simply:

```c
tgran = extract_4_bits(ID_AA64MMFR0_EL1, selected_shift);

if (tgran < supported_min || tgran > supported_max)
    no_granule_support();
```

If the check passes, Linux continues MMU setup:

`phys_to_ttbr x2, x2`
Converts the physical address of the identity map page table into the form needed for `TTBR0_EL1`. ([GitHub][1])

`msr ttbr0_el1, x2`
Loads `TTBR0_EL1` with the identity-mapped page tables. ([GitHub][1])

`load_ttbr1 x1, x1, x3`
Loads `TTBR1_EL1` for the kernel mappings. ([GitHub][1])

`set_sctlr_el1 x0`
Programs `SCTLR_EL1`, which enables the MMU using the prepared translation tables. ([GitHub][1])

`ret`
Returns if MMU enable preparation succeeded. ([GitHub][1])

If the CPU does **not** support the configured granule, execution goes here:

```asm
SYM_FUNC_START_LOCAL(__no_granule_support)
    update_early_cpu_boot_status \
        CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_NO_GRAN, x1, x2
1:
    wfe
    wfi
    b 1b
SYM_FUNC_END(__no_granule_support)
```

That means Linux records an early boot failure status and then parks the CPU in an infinite low-power loop. It does not try to continue booting with an unsupported page granule. ([GitHub][1])

A concrete example:

If the kernel is built with `CONFIG_ARM64_4K_PAGES`, then:

* `ID_AA64MMFR0_EL1_TGRAN_SHIFT` points to `TGRAN4`
* `ubfx` extracts bits `[31:28]`
* Linux checks whether that 4-bit value lies in the accepted range for 4K support

If yes, boot continues.
If no, it jumps to `__no_granule_support` and stops that CPU. ([GitHub][1])

The important part is this: Linux is **not** probing all granule sizes and picking one dynamically. The kernel is already built for one specific granule size, and this boot-time code only verifies that the CPU can run that choice. ([GitHub][1])

I can also draw this as a tiny flow diagram from `primary_entry -> __primary_switch -> __enable_mmu -> __no_granule_support / MMU on` if you want.

[1]: https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/head.S?utm_source=chatgpt.com "linux/arch/arm64/kernel/head.S at master"

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
The MMU in ARMv8-A is enabled by writing bit 0 (M) of SCTLR_EL1 to 1 via an MSR instruction followed by an ISB. The ISB is the critical barrier: it flushes the instruction pipeline so that all instructions fetched AFTER the ISB use the new memory system configuration. Before the MMU is enabled, the CPU operates in a flat physical address space. After the bit is set, the TLB, page-table walker, TTBR0/TTBR1, TCR_EL1, and MAIR_EL1 all become active simultaneously. There is no intermediate state.

### Kernel Perspective (Linux ARM64)
Linux enables the MMU in __enable_mmu (arch/arm64/kernel/head.S), called from __primary_switch. The sequence is:
  1. Write TTBR0_EL1 (identity map root).
  2. Write TTBR1_EL1 (kernel map root).
  3. ISB to synchronize TTBR writes.
  4. Write SCTLR_EL1 with M=1 (via set_sctlr_el1 macro).
  5. ISB to flush the pipeline.
  6. RET -- the very next instruction is fetched through the new MMU.
The identity map ensures that the physical address of the code after the RET is also mapped at the same VA (PA==VA), so no instruction-fetch fault occurs.

### Memory Perspective (ARMv8 Memory Model)
The moment SCTLR_EL1.M is written to 1 and the ISB completes, the ARMv8 memory model transitions from "flat PA" to "two-stage VA->PA via page tables". The identity map (stored in __idmap_text_start to __idmap_text_end, mapped in the .idmap.text section) covers the physical pages of the MMU-enable code so the VA==PA invariant holds during the critical window. Without the identity map, the instruction fetch for the RET after set_sctlr_el1 would target a VA that has no valid TLB entry, causing a translation fault with no exception handler installed yet.