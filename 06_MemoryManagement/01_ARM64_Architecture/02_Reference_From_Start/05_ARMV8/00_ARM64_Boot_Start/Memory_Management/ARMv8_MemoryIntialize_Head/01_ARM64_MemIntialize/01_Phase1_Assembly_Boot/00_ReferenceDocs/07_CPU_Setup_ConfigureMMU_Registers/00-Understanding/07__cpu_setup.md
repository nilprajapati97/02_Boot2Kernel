***====================================================================***

# ***__cpu_setup on ARMv8: Deep Walkthrough Index***

***======================================================================***

The remaining search confirms those MMFR3 fields are treated as optional architectural extensions, and sleep.S reuses `__cpu_setup` during resume, which is a strong clue this routine is “re-establish architectural execution state before turning EL1 MMU back on,” not just cold-boot setup. I’m composing the explanation around that contract now.For an interview-quality answer, the clean way to frame __cpu_setup is this:

It is the early EL1 architectural bring-up routine that prepares the CPU’s translation and execution control state so Linux can safely turn the MMU on. It does not enable the MMU itself. Instead, it programs MAIR_EL1, TCR_EL1, optionally TCR2_EL1 and permission-indirection registers, resets a few EL0-visible controls, and returns the final SCTLR_EL1 value in x0. The caller in head.S later loads TTBR0/TTBR1 and writes SCTLR_EL1 to actually enable translation and caches. You can see that split directly in proc.S, the call site in head.S, and the actual enable path in head.S. The same routine is reused on resume in sleep.S, which is a strong hint that its job is “restore required EL1 architectural state before turning translation back on.”

If you want a strong opening answer in an interview, say this:

__cpu_setup is the last major EL1 setup step before enabling the MMU on arm64. It sanitizes some privileged state, computes the correct translation control register values from kernel configuration plus CPU feature registers, sets memory attributes, handles runtime feature differences like 52-bit VA, hardware AF, HAFT, and permission indirection, and returns the SCTLR value that __enable_mmu will finally install. It runs from .idmap.text because the kernel must be able to execute it before the normal virtual mapping is active.

**Big Picture**
The important design points are:

1. It runs in the identity-mapped section, not normal kernel text.
2. It prepares control registers, but does not write TTBRs or enable M in SCTLR_EL1 itself.
3. It is deliberately feature-adaptive: the binary is built with many capabilities, but this routine narrows the configuration to what the current CPU really supports.
4. It uses both compile-time configuration and runtime feature probing.

The identity-mapped placement matters because at this point the kernel cannot assume the final virtual address map is active. That is why the function is placed in .idmap.text in proc.S.

**Where It Sits In Boot Flow**
The early boot sequence is roughly:

1. head.S enters at EL1 or drops from EL2 to EL1.
2. head.S calls __cpu_setup.
3. __cpu_setup prepares MAIR/TCR/TCR2 and returns x0 = desired SCTLR_EL1.
4. __enable_mmu loads TTBR0/TTBR1 and writes SCTLR_EL1.
5. The CPU starts running with the Linux virtual memory layout.

That split is visible in head.S and head.S.

Why not just write SCTLR_EL1 inside __cpu_setup? Because TTBR0 and TTBR1 are loaded later in __enable_mmu, and Linux wants a clean separation between “compute architectural control state” and “commit to live translation.”

**Line-By-Line Walkthrough**

1. TLB invalidation

At the top:

tlbi vmalle1
dsb nsh

This invalidates the local EL1 TLB state before Linux installs its own translation regime. The key idea is that stale translations are dangerous when you are about to change TCR/TTBR/SCTLR state. The dsb nsh makes the invalidation complete with respect to the local PE before proceeding.

Interview point:
This is local CPU preparation. At this stage the routine is per-CPU, and early boot does not yet rely on full SMP-wide TLB coordination here.

2. Reset access-control and debug-related state

The next block clears cpacr_el1, sets mdscr_el1, and disables PMU/AMU access from EL0.

The helper macros are defined in assembler.h.

What this means conceptually:

1. cpacr_el1 is reset to a known state.
2. mdscr_el1 is initialized with MDSCR_EL1_TDCC to block EL0 Debug Communications Channel access.
3. PMUSERENR_EL0 is cleared if PMUv3 exists.
4. AMUSERENR_EL0 is cleared if AMU exists.

Why Linux does this:
Early boot wants a deterministic, locked-down baseline. You do not want stale firmware state leaving EL0 with debug/performance monitor visibility before the kernel decides policy.

A good interview line:
This block is not about address translation directly. It is about establishing sane EL1 control defaults and preventing inherited firmware state from leaking EL0 privileges.

3. Build the default MAIR_EL1 and TCR_EL1 values

This is the core of the function.

Linux first aliases registers:

1. mair = x17
2. tcr = x16
3. tcr2 = x15

Then it builds MAIR_EL1_SET and a large initial TCR value. The memory-attribute definitions are at the top of proc.S.

MAIR_EL1:
MAIR describes what each AttrIndx means when page table entries point to a memory type. Linux sets up indices for:

1. Device nGnRnE
2. Device nGnRE
3. Normal non-cacheable
4. Normal WB cacheable
5. Normal Tagged, initially as normal memory and later upgraded if MTE is active

This is critical because page descriptors only carry an AttrIndx field. Without MAIR, the page tables cannot describe device vs normal memory behavior correctly.

TCR_EL1:
TCR is the translation regime definition. This one line encodes most of Linux’s stage-1 address-space policy.

The initial TCR value includes:

1. T0SZ(IDMAP_VA_BITS)
This sizes the TTBR0 address space used for the identity map during bring-up.

2. T1SZ(VA_BITS_MIN)
This sizes the TTBR1 kernel address space conservatively to the minimum virtual size Linux can safely boot with. The relevant VA constants are in memory.h, and IDMAP_VA_BITS is defined in kernel-pgtable.h.

3. TCR_CACHE_FLAGS
This makes page table walks cacheable as inner/outer WBWA.

4. TCR_SHARED
This sets inner shareable attributes for translation table walks.

5. TCR_TG_FLAGS
This selects granule size based on 4K, 16K, or 64K kernel configuration.

6. TCR_KASLR_FLAGS
If kernel address randomization is enabled, Linux adds the relevant TCR flag.

7. TCR_EL1_AS
This selects the larger ASID format when supported.

8. TCR_EL1_A1
This makes TTBR1_EL1 carry the ASID selection semantics Linux wants.

9. TCR_EL1_TBI0
This enables top-byte-ignore behavior for the TTBR0 side, which is relevant for Linux’s userspace tagged address model.

10. TCR_KASAN_SW_FLAGS and TCR_MTE_FLAGS
These add TBI and tag-control behavior for kernel tagging features.

A good summary sentence:
MAIR defines what the memory types mean; TCR defines how virtual addresses are interpreted and how the page-table walk itself behaves.

4. Clear CPU-specific erratum-triggering TCR bits

The helper is in assembler.h.

This is a very Linux detail and a very good interview point. Linux does not treat TCR as purely architectural. It also sanitizes it against known CPU errata, currently including a Fujitsu-specific workaround in this tree.

Why this matters:
If a TCR bit is architecturally valid but trips a CPU implementation bug, early boot is the right place to scrub it before the MMU comes on.

That tells the interviewer you understand Linux arm64 boot is not just “program architected registers,” but “program architected registers in a way that is safe on real shipped silicon.”

5. Handle 52-bit VA and LPA2 at runtime

This block is one of the most important to explain well.

If the kernel is built with CONFIG_ARM64_VA_BITS_52, Linux still cannot blindly boot using a 52-bit kernel VA space on every CPU. Some systems may not support VA52 at runtime. So Linux starts from VA_BITS_MIN and only patches in the larger T1SZ if the CPU actually has ARM64_HAS_VA52.

That logic depends on:

1. Compile-time configuration says the kernel knows how to support large VA.
2. Runtime alternative patching says this specific CPU supports it.

This is why Linux initially uses TCR_T1SZ(VA_BITS_MIN), then conditionally rewrites T1SZ with tcr_set_t1sz. The helper is in assembler.h.

Why VA_BITS_MIN instead of VA_BITS directly?
Because the kernel image may be built to support the larger address size, but the same image must still boot safely on CPUs without that feature. So __cpu_setup computes a conservative default, then expands only when runtime probing says it is legal.

Under CONFIG_ARM64_LPA2, Linux also sets TCR_EL1_DS when appropriate. The high-level meaning is that the translation format is extended for the LPA2 regime.

Strong interview wording:
Linux intentionally decouples “kernel built with support for large VAs” from “CPU is allowed to use large VAs right now.” __cpu_setup is where that negotiation happens.

6. Compute the physical address size for IPS

The helper is in assembler.h.

This macro reads ID_AA64MMFR0_EL1.PARange, clamps it to what Linux wants to support, and inserts it into TCR_EL1.IPS.

Why this matters:
Virtual address size and physical address size are separate questions. Even if the virtual layout is fixed by kernel configuration, the CPU’s supported physical address range can vary. TCR.IPS must match the implemented PARange or the translation regime can be invalid or suboptimal.

A good interview sentence:
This is the point where Linux translates architectural discovery data from ID_AA64MMFR0_EL1 into a live translation policy in TCR_EL1.

7. Enable hardware Access Flag support, and possibly HAFT

Under CONFIG_ARM64_HW_AFDBM, Linux checks ID_AA64MMFR1_EL1.HAFDBS.

If HAFDBS is present:
1. It sets TCR_EL1.HA so hardware can update the Access Flag.
2. If the CPU reaches the HAFT level and the kernel is configured for it, it sets TCR2_EL1.HAFT.

What this means:
Linux is asking the MMU hardware to perform part of the page-access bookkeeping automatically. That reduces the need for pure software fault-driven AF management.

Important nuance:
The comment explicitly says hardware dirty-bit management is enabled later via capabilities. So in this routine Linux only enables hardware Access Flag handling, not the full dirty state policy.

This is a strong point for senior-level discussion:
You can say Linux uses __cpu_setup for the early architectural subset that is safe and required immediately, while some higher-level MM capabilities are finalized later in cpufeature-driven code after the system is further along.

8. Write MAIR_EL1 and TCR_EL1

After all of the above adjustments, Linux writes:

1. mair_el1 = mair
2. tcr_el1 = tcr

This is the transition from “compute desired register value” to “program the live EL1 translation control state.”

Why here and not earlier?
Because Linux wanted to finish all runtime feature corrections first:
1. errata cleanup
2. VA52/LPA2 adaptation
3. PARange computation
4. hardware AF support

Only then does it commit the final register values.

9. Probe FEAT_S1PIE and program permission-indirection registers

This is the newest-looking part of the function and a place interviewers may test whether you can stay calm around unfamiliar ARM extensions.

Linux reads ID_AA64MMFR3_EL1 and extracts the S1PIE field. If present, it writes PIRE0_EL1 and PIR_EL1, then sets TCR2_EL1.PIE.

What to say safely and correctly:
This is stage-1 permission indirection support. Instead of hardwiring all permission interpretation solely from the leaf PTE bits, the architecture allows a level of indirection via dedicated registers. Linux initializes those permission-indirection tables early if the CPU advertises the feature.

That is enough for most interviews. You do not need to overclaim deep semantics of every PIR bit unless specifically asked.

What matters is your architectural read:
1. Feature discovered through MMFR3
2. Related control registers are programmed
3. TCR2 is used to enable the associated mode

10. Probe FEAT_TCRX and write TCR2_EL1 only if legal

Linux then re-reads MMFR3, checks TCRX support, and only then writes REG_TCR2_EL1.

Why the extra guard?
Because Linux must not touch TCR2_EL1 on CPUs where the register is not implemented.

This separate probe is a classic arm64 early-boot pattern:
The kernel may compute extended control-state values speculatively in a general-purpose register, but it only writes the architected system register if the CPU actually advertises the extension.

If asked why there are two separate MMFR3 checks:
Because S1PIE-related setup and TCR2 register accessibility are distinct architectural capabilities in the probing logic, and Linux keeps the write path explicitly safe.

11. Return the final SCTLR_EL1 value in x0

At the end:

mov_q x0, INIT_SCTLR_EL1_MMU_ON
ret

The definition is in sysreg.h.

This constant includes the bits Linux wants when EL1 MMU is turned on, including the major ones:

1. M: MMU enable
2. C: data/unified cache enable
3. I: instruction cache enable
4. SA and SA0: stack alignment checks
5. UCI, UCT, DZE: selected EL0 instruction/control behaviors
6. IESB, EOS, EIS, ITFSB: execution-synchronization/speculation-related controls
7. SPAN and EPAN: PAN-related hardening behavior
8. endianness selection
9. LSMAOE and nTLSMD: architectural execution controls Linux wants

The key point is:
__cpu_setup does not write SCTLR_EL1. It returns the correct boot-time value so __enable_mmu can install it at the right moment, after TTBR0 and TTBR1 have been loaded.

That separation is visible in head.S.

**What Each Register Means In One Sentence**
Use this if the interviewer asks for a fast register summary:

1. MAIR_EL1
Defines the actual memory-type meanings for AttrIndx values used by page descriptors.

2. TCR_EL1
Defines the stage-1 translation regime: address sizes, granule size, walk cacheability/shareability, IPS, tagging behavior, and other translation controls.

3. TCR2_EL1
Holds newer or extended translation controls beyond classic TCR_EL1.

4. SCTLR_EL1
Turns the MMU and caches on and controls major EL1 execution behavior.

5. PIRE0_EL1 / PIR_EL1
Permission-indirection configuration registers used when the CPU supports stage-1 permission indirection.

6. CPACR_EL1 / MDSCR_EL1 / PMUSERENR_EL0 / AMUSERENR_EL0
Execution/debug/perf-monitor control and lockdown state, mostly to avoid inheriting unsafe firmware defaults.

**Why It Runs From .idmap.text**
This is a very common interview follow-up.

Before the MMU is enabled, the kernel cannot rely on its normal higher-half virtual mapping. So the code that sets up the translation regime must live in an identity-mapped region that is valid both before and during the handoff to the final address space.

That is why __cpu_setup and other early helpers live in .idmap.text in proc.S.

**Why T0SZ Uses IDMAP_VA_BITS But T1SZ Uses VA_BITS_MIN**
This is one of the best questions they can ask.

TTBR0 side:
Linux uses TTBR0 during early bring-up for the identity map, so T0SZ is sized for the idmap.

TTBR1 side:
Linux uses TTBR1 for the kernel linear map and kernel image, but it starts from VA_BITS_MIN so the image can boot on CPUs that do not support the largest configured virtual address size.

So the asymmetry is intentional:
1. TTBR0 is about the temporary early idmap.
2. TTBR1 is about the eventual kernel address space, but booted conservatively.

**Why __cpu_setup Does Not Load TTBR0/TTBR1**
Because that is the job of __enable_mmu. Linux splits responsibilities:

1. __cpu_setup
Builds the control regime.

2. __enable_mmu
Loads the base addresses for the page tables and commits the SCTLR write.

This split makes the sequencing explicit and safe.

**What Makes This Linux-Specific Rather Than Generic ARMv8**
A senior answer should point out that this is not just textbook architecture code. It contains Linux policy:

1. Linux MAIR layout
2. Linux KASLR interaction
3. Linux tagged-address and KASAN/MTE choices
4. Linux runtime alternative patching for VA52
5. Linux errata handling
6. Linux decision to return SCTLR rather than write it here
7. Linux use of TTBR1 for the kernel half and TTBR0 for idmap/userspace semantics

That distinction helps you sound like a kernel engineer, not somebody reciting ARM ARM terminology.

**How To Present It In A 45–60 Minute Interview**
A good talk track is:

1. Start with the contract
Say __cpu_setup prepares EL1 system registers for the arm64 MMU enable path, but does not enable the MMU itself.

2. Explain where it is called
Point to head.S and sleep.S.

3. Explain why it is in .idmap.text
Because execution must remain valid before the normal VA map exists.

4. Walk the blocks in order
TLB invalidate, sanitize control state, compute MAIR/TCR, runtime feature adaptation, write registers, return SCTLR.

5. Finish with the handoff
Explain that __enable_mmu later loads TTBR0/TTBR1 and sets SCTLR.

If you speak that structure clearly, you can fill an hour easily by expanding each block and taking questions.

**Likely Interview Questions And Strong Answers**

1. Why invalidate the TLB before programming TCR?
Because stale translations from firmware or an earlier regime are unsafe once Linux changes the translation controls. Early boot wants a clean local translation context before loading its own tables.

2. Why return SCTLR instead of writing it in __cpu_setup?
Because enabling the MMU must happen only after TTBR0 and TTBR1 are loaded. Linux separates register preparation from the final commit point.

3. Why is T1SZ initially based on VA_BITS_MIN?
To allow a single kernel image built with large VA support to boot safely on CPUs that do not support the larger VA range.

4. Why are MAIR and TCR written before SCTLR?
Because they define the translation regime that will become active once SCTLR.M is set.

5. Why is there runtime probing if the kernel is already configured?
Because compile-time configuration says what the image can support, but runtime ID registers say what the current CPU actually implements.

6. What is the significance of CPACR_EL1 being reset here?
It clears inherited firmware state and prevents unexpected EL1 access policy for FP/SIMD-related functionality during early boot.

7. Why are PMUSERENR_EL0 and AMUSERENR_EL0 reset?
To ensure EL0 cannot access PMU or AMU state unless Linux explicitly allows it later.

8. What is the role of TCR2_EL1 here?
It carries newer translation controls such as HAFT and permission-indirection enablement that do not fit in classic TCR_EL1.

9. Is __cpu_setup only for the boot CPU?
No. It is also called for secondary CPUs and on resume, which shows it establishes a required per-CPU architectural baseline.

10. What is the difference between MAIR and TCR?
MAIR defines memory types; TCR defines address translation geometry and walk behavior.

**A Concise Senior-Level Summary You Can Say Verbatim**
__cpu_setup is the arm64 kernel’s early EL1 translation-setup routine. It runs from identity-mapped text, invalidates stale local TLB state, sanitizes inherited EL1 and EL0-visible control registers, builds MAIR_EL1 and TCR_EL1 from Linux policy plus CPU feature registers, applies runtime adjustments for VA52, LPA2, hardware AF, HAFT, and permission-indirection support, programs those registers, and returns the final SCTLR_EL1 value. The caller then loads TTBR0 and TTBR1 and writes SCTLR_EL1 in __enable_mmu. So the function’s job is not “turn the MMU on,” but “make the CPU architecturally ready for Linux’s stage-1 translation regime.”

If you want, I can turn this into either:

1. a 10-minute interview answer script in spoken English
2. a block-by-block explanation with every TCR bit decoded
3. a whiteboard-style boot-flow diagram from reset to __primary_switch