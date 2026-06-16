# Interview Q&A Master — Linux Kernel, ARM, Qualcomm, Drivers, C

> Consolidated Q&A extracted from the 8 raw interview-prep documents in `_raw_text/`.
> Every question gets a concise but technically complete answer (the *why*, not just the *what*),
> plus a **Deep dive →** link to the matching topical knowledge-base doc.

---

## How to use this document

- Use the **Topic Index** to jump to a subject area.
- Each question is numbered within its topic so you can refer to e.g. *§5 Q3* in a study session.
- Comparison questions use Markdown tables; code answers use fenced ```c blocks.
- The **Quick-Fire One-Liners** section at the end is for last-night rapid recall.
- When you want background or a deeper walkthrough, follow the **Deep dive →** link to the
  topical reference document (these live in the same folder).

---

## Topic Index

| §  | Topic                                              | Deep-dive doc                                                                                  |
|----|----------------------------------------------------|------------------------------------------------------------------------------------------------|
| 1  | C Language, Memory Layout & Bitwise                | [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)                   |
| 2  | ARM/ARM64 Architecture & Memory Management         | [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)                         |
| 3  | Linux Kernel Memory Management                     | [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)                         |
| 4  | Scheduling & Context Switch                        | [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)                   |
| 5  | Synchronization, Locks & RCU                       | [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)                   |
| 6  | Interrupts, IRQs, IPIs & Watchdog                  | [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)                         |
| 7  | Linux Drivers — Char, Block, Platform, Misc        | [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)       |
| 8  | Device Tree                                        | [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)       |
| 9  | procfs / sysfs / debugfs / configfs                | [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)       |
| 10 | System Calls                                       | [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)       |
| 11 | Boot Flow, U-Boot, APPSBL, ATF                     | [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)           |
| 12 | Hibernation & Suspend                              | [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)           |
| 13 | Crash Dump & Kernel Debugging                      | [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)                       |
| 14 | Qualcomm Platform & Subsystems                     | [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)                       |
| 15 | IOMMU / SMMU                                       | [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)                       |
| 16 | Behavioral / Scenario-Based Debugging Questions    | (cross-topic — links per question)                                                             |
| 17 | Quick-Fire One-Liners (50+ rapid-recall items)     | (cross-topic)                                                                                  |

---

## 1. C Language, Memory Layout & Bitwise (→ 08)

### Q1. How do you SET, CLEAR, TOGGLE and TEST a single bit in C?
**A.** Four one-liners cover every register-level need. `SET` uses OR with a 1-mask, `CLEAR`
uses AND with an inverted mask, `TOGGLE` uses XOR, and `TEST` masks and checks for non-zero.
```c
#define SET_BIT(x,n)    ((x) |=  (1U << (n)))
#define CLEAR_BIT(x,n)  ((x) &= ~(1U << (n)))
#define TOGGLE_BIT(x,n) ((x) ^=  (1U << (n)))
#define TEST_BIT(x,n)   (((x) >> (n)) & 1U)
```
For MMIO use `volatile uint32_t *` pointers so the compiler never elides the access.
*Deep dive:* [C Strings & Bitwise Fundamentals](08_C_Strings_Bitwise_Fundamentals.md)

### Q2. How does Brian Kernighan's algorithm count set bits, and why is it faster than naive shifting?
**A.** `n &= (n - 1)` clears the lowest set bit in one step, so the loop runs **O(k)** times
(k = number of 1-bits) instead of O(32). For `n = 13 (0b1101)` it iterates three times. In
production, prefer `__builtin_popcount(n)` which maps to a single ARMv8 `CNT` instruction.
```c
int popcount(unsigned n){ int c=0; while(n){ n &= n-1; c++; } return c; }
```
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q3. How do you test whether a number is a power of two?
**A.** A power of two has exactly one set bit, so `n & (n-1)` clears it leaving zero. Guard
against `n == 0` (which would falsely match). This is exactly Linux's `is_power_of_2()`.
```c
static inline int is_pow2(unsigned n){ return n && !(n & (n-1)); }
```
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q4. Swap two integers without a temporary variable — and what's the trap?
**A.** XOR-swap works because `x ^ x = 0` and `x ^ 0 = x`. **Critical pitfall:** if both
pointers reference the same memory the value is destroyed (zeroed). Always guard.
```c
void swap(int *a,int *b){ if(a==b) return; *a^=*b; *b^=*a; *a^=*b; }
```
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q5. How do you find the single element that appears an odd number of times?
**A.** XOR all elements: pairs cancel (`x^x=0`), leaving the unique value. O(n) time,
O(1) space — optimal. Same algorithm finds the lone element when all others appear twice.
```c
int unique(int *a,int n){ int r=0; for(int i=0;i<n;i++) r^=a[i]; return r; }
```
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q6. How do you find the TWO unique numbers when all others occur twice?
**A.** Three steps: (1) XOR all elements → result = `a^b`; (2) isolate any set bit
`set = xor_all & -xor_all` (this bit differs between a and b); (3) partition the array by
that bit and XOR each half independently. Each half then collapses to a single unique value.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q7. How do you isolate the rightmost set bit, and why does `n & -n` work?
**A.** In two's complement `-n = ~n + 1`. The carry from `+1` propagates exactly up to the
rightmost 1 of `n`, flipping every trailing zero to 1 and the rightmost 1 stays 1. ANDing
selects only that bit. Used in Fenwick trees and to find the lowest-numbered set IRQ.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q8. How do you compute branchless absolute value?
**A.** `mask = n >> 31` (arithmetic shift gives 0 or -1). Then `abs = (n ^ mask) - mask`.
For non-negative n it reduces to `n`; for negative n the XOR inverts and the `-(-1)` adds 1
— exactly two's-complement negation. No branch ⇒ no mispredict penalty in tight loops.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q9. How do you reverse the bits of a 32-bit integer?
**A.** Two approaches. **Loop** (O(32)): shift result left, OR in the LSB of n, shift n right.
**Divide-and-conquer** (O(1) — 5 ops): swap 16-bit halves, then bytes, then nibbles, then
pairs, then individual bits using masks `0xFF00FF00 / 0xF0F0F0F0 / 0xCCCCCCCC / 0xAAAAAAAA`.
The D&C variant is what's used in firmware where every cycle counts.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q10. Hamming distance between two integers?
**A.** XOR yields 1 where bits differ, then popcount: `__builtin_popcount(x ^ y)`. O(1) with
GCC builtins; O(k) using Brian Kernighan loop.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q11. How can you detect that two integers have opposite signs without comparison?
**A.** `(x ^ y) < 0`. The MSB of the XOR equals 1 iff the sign bits differ, making the
signed result negative. Avoids the branch that comparison-based tests need.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q12. Add two integers without using `+`.
**A.** XOR gives sum without carry, AND gives carry bits; shift the carry left and repeat
until carry is zero. Loops at most 32 times for `int`.
```c
int add(int a,int b){ while(b){ int c=a&b; a^=b; b=c<<1; } return a; }
```
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q13. Find the missing number in [1..N] with one element absent.
**A.** XOR-based formula avoids the overflow risk of the sum formula:
`missing = XOR(1..N) ^ XOR(arr)`. Every present number cancels, the absent one survives.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q14. Compute parity of a 32-bit integer in O(log n).
**A.** Fold the word: `n ^= n>>16; n ^= n>>8; n ^= n>>4; n ^= n>>2; n ^= n>>1; return n&1;`.
Or use `__builtin_parity(n)` which becomes a single PMULL/EOR sequence on ARM.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q15. Generate all subsets of an n-element set using bitmasks.
**A.** Iterate mask from `0` to `(1<<n)-1`. For each mask, element `i` belongs to the subset
iff `mask & (1<<i)`. Total `2^n` subsets — used in bitmask DP, TSP, subset-sum.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q16. How do you safely program a W1C (Write-1-to-Clear) status register?
**A.** For W1C, you do **not** do read-modify-write — that re-clears other already-set bits.
Instead write a mask with 1s **only** at the bits you wish to clear. For ordinary R/W
registers, use the RMW idiom `val = *reg; val &= ~MASK; *reg = val;` via a `volatile`
pointer so the compiler cannot fold or reorder accesses.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q17. How do you implement a power-of-two circular buffer without using `%`?
**A.** Force `SIZE` to be a power of two and use `index & (SIZE-1)` for wrap-around — a
single-cycle AND instead of a divide. This is exactly how Linux's `kfifo` is implemented.
Single-producer/single-consumer wants `smp_wmb()` after the data write and `smp_rmb()`
before the read to prevent reordering across cores.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q18. Endianness — how do you swap a 32-bit value, and what kernel helpers exist?
**A.** Manually: byte 0↔3, byte 1↔2 using shifts/masks (`0xFF000000` etc.). In the kernel,
use the typed wrappers: `cpu_to_be32() / be32_to_cpu()`, `cpu_to_le32() / le32_to_cpu()`,
`htonl()/ntohl()`. They expand to the ARM `REV` instruction on AArch64.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q19. What does `volatile` do — and why is it **not** enough for SMP synchronization?
**A.** `volatile` only forbids the **compiler** from caching/reordering loads and stores
through that pointer; it does nothing about CPU out-of-order execution or cache coherency
between cores. For SMP you also need explicit memory barriers (`DMB/DSB/ISB` in ARM ASM,
`smp_mb()/smp_rmb()/smp_wmb()` in Linux). Volatile alone is the standard interview trap.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q20. Difference between `volatile`, `const volatile`, and `restrict`?
**A.** `volatile` ⇒ value can change outside program control (hardware register, ISR).
`const volatile` ⇒ HW may change it but software must not write (read-only status reg).
`restrict` (C99) tells the compiler the pointer is the **only** alias for that memory,
enabling aggressive vectorisation — useful for DSP/SIMD copy loops.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q21. How would you encode multiple simultaneous error flags in one return code?
**A.** Define one bit per error: `ERR_TIMEOUT=1<<0`, `ERR_OVERFLOW=1<<1`, …. OR them
together as detected, then the caller tests each independently with `&`. This is exactly
how Linux `poll()` event masks and ioctl status returns work.
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

### Q22. Write generic `REG_SET_FIELD` / `REG_GET_FIELD` macros.
**A.** Use `do { … } while(0)` to keep statement semantics. Always `volatile` the MMIO
pointer.
```c
#define REG_SET_FIELD(reg,mask,shift,val) do {       \
    uint32_t t = readl(reg); t &= ~(mask);           \
    t |= ((val) << (shift)) & (mask); writel(t,reg); \
} while(0)
#define REG_GET_FIELD(reg,mask,shift) ((readl(reg)&(mask))>>(shift))
```
*Deep dive:* [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md)

---

## 2. ARM/ARM64 Architecture & Memory Management (→ 01)

### Q1. What are the four ARMv8-A Exception Levels and what runs at each?
**A.** EL0 = unprivileged user apps; EL1 = OS kernel (Linux); EL2 = hypervisor (KVM, pKVM,
QHEE); EL3 = secure monitor (ATF/QSEE TrustZone). Each EL has its own SP, SPSR/ELR, vector
base (VBAR_ELn). Transitions: `SVC` → EL1, `HVC` → EL2, `SMC` → EL3, `ERET` returns down.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q2. Compare ARMv7-A modes vs ARMv8-A exception levels.

| ARMv7-A mode      | ARMv8-A EL | Notes                                            |
|-------------------|------------|--------------------------------------------------|
| USR               | EL0        | Unprivileged                                     |
| SVC               | EL1        | Kernel runs here; entered by `SVC`               |
| SYS               | n/a        | Replaced by SP_EL0 access from EL1               |
| HYP               | EL2        | Hypervisor; entered by `HVC`                     |
| MON               | EL3        | Secure monitor; entered by `SMC`                 |
| FIQ/IRQ/ABT/UND   | EL1        | Unified async/sync exception vectors             |

*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q3. What is the difference between SVC mode and SYS mode on ARMv7-A?
**A.** Both are privileged. SVC (`0x13`) is entered by hardware on `SVC` instruction/reset,
has **banked** `SP_svc/LR_svc/SPSR_svc` — i.e. a clean kernel stack. SYS (`0x1F`) is entered
**only by software** (`MSR CPSR_c, #0x1F`), shares USR's R0–R14 (no banking), has no SPSR.
SYS exists so privileged code can touch the user-mode SP/LR directly (signal frame setup,
context switch). The classic SVC trap: nested SVC corrupts `LR_svc` unless you push it first.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q4. What happens if you execute `SVC` while already in SVC mode?
**A.** `LR_svc` is overwritten with the new return PC and `SPSR_svc` is overwritten with the
current CPSR — **the original return address back to user space is destroyed**, hanging the
kernel. Fix: every SVC entry path *must* `PUSH {r0-r12, lr}` to the SVC stack before any
nested call or before re-enabling interrupts.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q5. Can SYS mode ever be entered by an exception?
**A.** No. There is no SYS vector in the ARM exception table — that's exactly why SYS mode
has no SPSR. It can only be entered explicitly via `MSR CPSR_c, #0x1F`. If exceptions could
enter SYS, they would clobber the user-mode SP/LR that SYS is supposed to access transparently.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q6. How does the ARM64 vector table work — and how is it different from ARMv7?
**A.** ARMv8 has a per-EL `VBAR_ELn` register holding the base of a 2KB vector table with
**16 entries** (4 groups × 4 exception types). The groups distinguish: current EL using
SP_EL0, current EL using SP_ELn, lower EL in AArch64, lower EL in AArch32. The 4 types per
group are Synchronous, IRQ, FIQ, SError. Each entry is 128 bytes (32 instructions). ARMv7
had a fixed 8-entry table at `0x00000000` or `0xFFFF0000` (HIVECS).
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q7. What do ESR_EL1, FAR_EL1 and ELR_EL1 tell you?
**A.** **ESR_EL1** = Exception Syndrome — its EC field gives the exception class
(0x15 = SVC, 0x24/0x25 = data abort, 0x20/0x21 = instr abort), ISS field gives the
specific syndrome (translation level, permission, alignment, etc.). **FAR_EL1** holds the
virtual address that triggered a fault. **ELR_EL1** holds the PC of the interrupted
instruction so `ERET` can resume.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q8. How does the ARM64 TTBR0/TTBR1 user/kernel split work?
**A.** With 48-bit VAs, bit 63 selects the table: `0` → TTBR0 (user, 0x0000_…), `1` →
TTBR1 (kernel, 0xFFFF_…). TCR_EL1 sets `T0SZ`/`T1SZ` (size), `TG0`/`TG1` (granule —
4K/16K/64K), shareability and ASID width. The MMU does the selection in hardware on every
access. KPTI keeps two TTBR1 page-tables (minimal + full) and switches them on syscall.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q9. How many levels of ARM64 page tables and what page sizes are supported?
**A.** With 4K granule and 48-bit VA, 4 levels (L0/PGD → L1/PUD → L2/PMD → L3/PTE), each
indexed by 9 bits. 16K granule also uses 4 levels; 64K granule uses 3 levels. Block
mappings at PMD = 2 MB huge page; at PUD = 1 GB; "contiguous-bit" hint groups 16×4 KB
PTEs into a single 64 KB TLB entry.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q10. What is an ASID and why does it matter for context switch performance?
**A.** Address Space ID — 8- or 16-bit tag stored in TTBR0_EL1 alongside the page-table
base and replicated in every TLB entry. On context switch, only TTBR0 + ASID changes; the
hardware uses the ASID to disambiguate user-space TLB entries belonging to different
processes, so a full TLB invalidation is avoided. 16-bit ASIDs (TCR_EL1.AS=1) allow ~64K
live ASIDs before reuse.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q11. How does TLB shootdown work on ARM64 SMP?
**A.** ARM64 TLBI instructions with the IS (Inner Shareable) suffix are broadcast in
hardware across the coherency domain — no IPI needed (unlike x86). Sequence:
`TLBI VAE1IS, Xt` (or `VMALLE1IS` for all), `DSB ISH` (wait for completion), `ISB`
(refetch instructions). The kernel calls this from `flush_tlb_range()`.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q12. What is MAIR_EL1 and what memory types does ARM64 define?
**A.** MAIR_EL1 holds 8 indirection slots, each describing a memory type encoding pointed
to by `AttrIndx` bits in each PTE. Common types: **Normal-WB** (cached, write-back),
**Normal-NC** (non-cacheable), **Device-nGnRnE** (no Gather, no Reorder, no Early write
ack — strict MMIO ordering), **Device-GRE** (relaxed — framebuffers).
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q13. What does ARM cache coherency MESI/MOESI provide?
**A.** **MESI**: cache lines in Modified/Exclusive/Shared/Invalid states; only one cache
can hold a Modified line. **MOESI** (ARM ACE) adds **Owned** so dirty data can be shared
without writeback. ARM CCI/CCN/CMN interconnect implements ACE (full coherency) or
ACE-Lite (one-way, used for non-coherent GPU/DSP masters).
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q14. What is the difference between PoU and PoC for cache maintenance?
**A.** **PoU** (Point of Unification) is where I-cache, D-cache and table walks see the
same data — usually L2. Cache maintenance for code patching needs PoU
(`flush_icache_range`). **PoC** (Point of Coherency) is where all masters (CPUs, DMA) see
the same data — usually DRAM. DMA cleans/invalidates to PoC.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q15. How does `LDXR/STXR` exclusive access work, and what improved in ARMv8.1 LSE?
**A.** LDXR places a per-core exclusive monitor on a line; STXR completes only if the
monitor is still set, else fails (returns 1) and the loop retries. ARMv8.1 LSE atomics
(`LDADD`, `SWP`, `CAS`, `STADD`) are single-instruction RMWs that don't loop — much
faster under heavy contention on big SMP systems.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q16. Explain ARM big.LITTLE / DynamIQ topology.
**A.** Heterogeneous CPUs: a "big/Prime" Cortex-X with high IPC/power, a few "Performance"
A715/A720, and "Efficiency" A510/A520 cores at low power. **DynamIQ** (DSU — DynamIQ Shared
Unit) puts mixed core types in one cluster sharing L3, with per-core DVFS. The kernel
models this via `sched_domain` hierarchy (SMT → MC → DIE → NUMA) and
`arch_scale_cpu_capacity()`.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q17. What are AArch64 system register access instructions?
**A.** `MRS Xt, SYSREG` (read) and `MSR SYSREG, Xt` (write). Replaces ARMv7's `MCR/MRC` to
CP15. Example: `MRS x0, ESR_EL1` or `MSR TTBR0_EL1, x1`.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q18. What does ARMv8.5 MTE (Memory Tagging Extension) do?
**A.** Top 4 bits of a pointer carry a "logical tag"; every 16-byte granule of memory has
an "allocation tag". On access the hardware checks they match — mismatched ⇒ trap. Catches
heap-overflow, use-after-free at native speed. ARMv8.3 PAC signs pointer bits to prevent
ROP, ARMv8.5 BTI marks valid indirect-branch targets.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q19. What is KASLR on ARM64 and how does it interact with KPTI?
**A.** KASLR randomizes the kernel image base at boot (seed from DTB `kaslr-seed` or
UEFI RNG). `kimage_voffset` translates `__pa()/__va()`. KPTI (post-Meltdown) maintains a
minimal trampoline page-table set for EL0 + a full set for EL1, switching TTBR1 on each
syscall — orthogonal to KASLR; both run together.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q20. What is the TrustZone Secure / Non-Secure model and how does Linux call into it?
**A.** Hardware splits CPU state into Secure and Non-Secure worlds. ARMv8 EL3 owns the
switch via `SCR_EL3.NS`. Linux (Non-Secure EL1) calls into the secure world by `SMC`
which traps to EL3 (ATF). On Qualcomm, ATF dispatches the call to QSEE / a Trusted App.
Wrapper: `arm_smccc_smc()` or `qcom_scm_call()`.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

---

## 3. Linux Kernel Memory Management (→ 01)

### Q1. Compare `kmalloc`, `kzalloc`, `vmalloc`, `alloc_pages`, `dma_alloc_coherent`.

| API                     | Phys-contig | Can sleep        | Max size           | Typical use                       |
|-------------------------|-------------|------------------|--------------------|-----------------------------------|
| `kmalloc(size, flags)`  | Yes         | Depends on flags | ~128 KB pow-of-2   | Small structs, DMA-able buffers   |
| `kzalloc`               | Yes (zeroed)| Depends          | ~128 KB            | Same but zeroed                   |
| `vmalloc(size)`         | No          | Yes              | GBs (VA-limited)   | Large allocations, module data    |
| `alloc_pages(gfp,order)`| Yes         | Depends          | `2^MAX_ORDER` pages| Buddy-direct, page tables         |
| `dma_alloc_coherent`    | Yes (DMA)   | Yes              | Device-limited     | Cache-coherent DMA buffers        |

*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q2. What is the difference between `GFP_KERNEL` and `GFP_ATOMIC`?
**A.** `GFP_KERNEL` may sleep — only legal in process context (can do reclaim, swap, wait
for pages). `GFP_ATOMIC` cannot sleep, taps emergency reserves, must be used in IRQ
handlers, spinlock-held sections, anywhere preemption is disabled. `GFP_NOWAIT` is like
ATOMIC but skips emergency reserves (failure tolerant).
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q3. Walk through the ARM64 page-fault handler from exception to PTE installation.
**A.** Hardware MMU faults → exception to EL1 via `VBAR_EL1` (`el1_sync` / `el0_sync`) →
read ESR_EL1 / FAR_EL1 → `do_mem_abort()` → `do_page_fault()` →
`find_vma(mm, addr)` (no VMA ⇒ SIGSEGV / oops) → `handle_mm_fault()` walks PGD→PUD→PMD→PTE
→ dispatches `do_anonymous_page()` (zero-fill on demand), `do_fault()` (file-backed read),
or `do_wp_page()` (CoW: alloc + copy + set writable).
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q4. Minor vs major page fault?
**A.** **Minor** = page already in RAM, just not yet in this process's table (CoW, first
touch of an anon page, shared page). No disk I/O, microseconds. Bumps `minflt`.
**Major** = must read from disk/swap. Blocking I/O, milliseconds, bumps `majflt`. High
majflt rate ⇒ memory pressure.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q5. How does the buddy allocator work?
**A.** Manages physical memory in power-of-two blocks (orders 0..MAX_ORDER-1) per NUMA
zone. On `alloc_pages(gfp, order)`, find smallest free block ≥ requested; split bigger
ones recursively. On `free_pages`, merge with sibling ("buddy") if free, walk up orders.
Free lists per migrate type (UNMOVABLE/MOVABLE/RECLAIMABLE) to limit fragmentation.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q6. SLAB vs SLUB vs SLOB?
**A.** All sit on top of the buddy allocator and serve small, fixed-size objects. **SLUB**
(default) — per-CPU slabs, minimal metadata, lowest SMP overhead. **SLAB** — legacy with
per-CPU + shared pools, more tuneable but heavier. **SLOB** — tiny first-fit, only for
embedded/single-CPU. Use `kmem_cache_create()` for typed object caches; `kmalloc()` is
backed by `kmalloc-*` slabs.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q7. Explain Transparent Huge Pages on ARM64.
**A.** With 4K granule, the kernel can promote 2 MB-aligned, fully-populated anonymous
regions to a single PMD block entry (one 2 MB TLB) by `khugepaged`. `madvise(MADV_HUGEPAGE)`
opts in; `MADV_NOHUGEPAGE` opts out. ARM64 also offers the "contiguous-PTE" hint to map
16×4 KB pages with one 64 KB TLB entry — cheap intermediate win.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q8. What is `mmap_lock` and why is it a bottleneck?
**A.** Per-`mm_struct` rwsem that protects the VMA tree. **Read** for page faults and
`/proc/pid/maps`; **write** for `mmap/munmap/mprotect/brk/execve`. Holding write blocks
every fault for the process. Linux 6.1 replaces the rbtree of VMAs with a maple tree;
Linux 6.3+ adds per-VMA locking for parallel fault handling.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q9. What are the memory watermarks and how do they drive reclaim?
**A.** Per zone: `pages_min` (OOM threshold), `pages_low` (wake `kswapd`), `pages_high`
(kswapd sleeps). When free < low, `kswapd` runs in background. When an allocator can't
satisfy a request, it does **direct reclaim** synchronously. If both fail and watermark
< min, the **OOM killer** runs.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q10. How does the OOM killer score victims?
**A.** `oom_score = (rss + swap) * 1000 / total_memory`, adjusted by `oom_score_adj`
(-1000..+1000). `-1000` makes a task unkillable. Highest score is SIGKILLed.
`memory.oom.group` (cgroup v2) kills every member together, useful for containers.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q11. What is CMA and when do you use it?
**A.** Contiguous Memory Allocator reserves a region at boot (`cma=64M` cmdline or DT
`reserved-memory`). Movable user pages can use it freely; when a DMA driver (V4L2, ION,
GPU) needs a large contiguous buffer the kernel migrates those pages out and hands the
contiguous range to the driver.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q12. Explain DMA-coherent vs streaming DMA, and the role of cache maintenance.
**A.** **Coherent** (`dma_alloc_coherent`) returns memory that's always coherent — either
uncached or HW-coherent via CCI/CCN — no explicit cache maintenance. **Streaming**
(`dma_map_single/sg`) reuses existing buffers and the framework does the necessary clean
(TO_DEVICE), invalidate (FROM_DEVICE) or both (BIDIRECTIONAL) at map/unmap. On
`dma-coherent` DT property the maintenance is skipped.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q13. What is the difference between `ioremap()`, `ioremap_wc()`, `ioremap_cache()`?
**A.** `ioremap` maps device memory as `Device-nGnRnE` — strictly ordered MMIO. `ioremap_wc`
uses Write-Combining (`Normal-NC` or `Device-GRE`) — coalesces writes, great for
framebuffers/display. `ioremap_cache` maps as cached Normal memory — rarely correct for
MMIO; mainly for RAM-like resources.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q14. What is the difference between PSI memory pressure and free memory?
**A.** Free memory is instantaneous availability. **PSI memory** (`/proc/pressure/memory`)
reports the fraction of wall-clock time tasks were *stalled* waiting for memory reclaim —
a *latency* signal. LMKD and systemd use PSI thresholds to act before hard limits hit.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q15. How do you find a kernel memory leak?
**A.** Enable `CONFIG_DEBUG_KMEMLEAK`, run `echo scan > /sys/kernel/debug/kmemleak`, read
the report for unreferenced allocations with stack traces. Pair with KASAN
(use-after-free, OOB), `slabtop` (per-cache growth), `/proc/meminfo` Slab line. Long-term
fix: switch driver allocations to `devm_*` so resources auto-free on unbind.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

---

## 4. Scheduling & Context Switch (→ 02)

### Q1. How does CFS pick the next task?
**A.** Each task accumulates `vruntime = delta_exec * NICE_0_LOAD / task_weight` (lower
nice ⇒ higher weight ⇒ smaller vruntime increment, so it runs more). The per-CPU run
queue is a red-black tree keyed by `vruntime`; **leftmost node** is picked in O(log N).
`sched_latency_ns` is the target period; each runnable task gets `sched_latency / N`.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q2. What is EEVDF and why did it replace CFS?
**A.** EEVDF (Earliest Eligible Virtual Deadline First, Linux 6.6) keeps the vruntime model
but selects the **eligible** task (vruntime ≤ min_vruntime) with the earliest virtual
deadline (`vruntime + slice`), not the leftmost node. Result: better tail latency for
interactive tasks under mixed loads, with explicit `sched_base_slice_ns` tuning.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q3. Compare SCHED_NORMAL, SCHED_FIFO, SCHED_RR, SCHED_DEADLINE, SCHED_IDLE.

| Policy           | Priority         | Preemption                | Slice            | Use case                       |
|------------------|------------------|---------------------------|------------------|--------------------------------|
| SCHED_NORMAL     | nice -20..+19    | Yes (CFS/EEVDF)           | `latency/N`      | Default                        |
| SCHED_FIFO       | RT 1-99          | Only by higher RT         | None until block | Audio, modem, sensor ISRs      |
| SCHED_RR         | RT 1-99          | Higher RT or quantum end  | 100 ms default   | RT round-robin                 |
| SCHED_DEADLINE   | Above RT (EDF)   | Yes on missed deadline    | App-specified    | Sporadic, hard-deadline tasks  |
| SCHED_IDLE       | Lowest           | Always preemptible        | Idle quantum     | Background cleanup             |

*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q4. How does SCHED_DEADLINE work?
**A.** Each task declares (runtime, deadline, period). The kernel uses EDF with the
Constant Bandwidth Server (CBS) for isolation — a task that overruns is throttled, not
allowed to starve others. Admission control rejects sets that aren't schedulable.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q5. Explain CPU affinity and IRQ affinity.
**A.** `sched_setaffinity()` writes `task_struct->cpus_mask`. The scheduler respects it for
placement and migration. **cpusets** (cgroup) apply this hierarchically. **IRQ affinity**
is set via `/proc/irq/N/smp_affinity`; on GICv3 the `GICR_IROUTER` register routes the
SPI to a particular PE.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q6. What is Energy-Aware Scheduling on big.LITTLE/DynamIQ?
**A.** When asymmetric capacity is detected and `schedutil` + an Energy Model are
registered, the wakeup path calls `find_energy_efficient_cpu()`. For each candidate CPU
it computes the energy delta of adding the task using the EM cost table; picks the lowest
energy that still meets capacity. Falls back to raw load-balancing if "overutilized".
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q7. How does PELT load tracking work?
**A.** Per-entity exponentially-decayed signals (`util_avg`, `load_avg`) with ~32 ms
half-life. `util_avg` measures CPU utilization; feeds the `schedutil` cpufreq governor
for OPP selection and EAS for placement decisions. Runs at every scheduler tick.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q8. What is sched-ext and why does Qualcomm care about it?
**A.** Linux 6.12 scheduling class (`ext_sched_class`) that lets a BPF program implement
the scheduler at runtime — no kernel recompile. Hooks: `enqueue`, `dequeue`, `dispatch`,
`select_cpu`, `running`, `stopping`. Tasks are staged on Dispatch Queues (DSQs:
`SCX_DSQ_GLOBAL`, `SCX_DSQ_LOCAL`, custom). BPF verifier ensures safety; on crash the
kernel reverts to CFS. Enables platform-specific power/latency policies (gaming, AI
prioritization, thermal-aware placement) without forking the kernel.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q9. Name three production sched-ext schedulers and what they do.
**A.** **scx_rusty** (Rust + BPF) — NUMA-aware, userspace global decisions + BPF dispatch.
**scx_lavd** — Latency-Aware Virtual Deadline, optimized for interactive/gaming. **scx_bpfland**
— prioritises I/O-bound and frequently-sleeping interactive tasks. Reference:
`scx_simple` (minimal FIFO).
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q10. How does the CFS load balancer choose tasks to migrate?
**A.** `load_balance()` runs from the scheduler tick and on CPU idle. Walks the
`sched_domain` hierarchy (SMT → MC → DIE → NUMA); at each level
`find_busiest_group → find_busiest_queue → move_tasks` pulls tasks to the calling CPU,
weighted by load. Migration cost rises at higher domains. **Misfit migration** moves a
task off a small core when its util exceeds the core's capacity.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q11. What is RT throttling and why is it on by default?
**A.** `sched_rt_period_us` (1 000 000) and `sched_rt_runtime_us` (950 000) cap RT classes
to 95 % of CPU per period, preventing a misbehaving SCHED_FIFO loop from starving
SCHED_NORMAL/kworkers/migration threads. Tunable via `/proc/sys/kernel/`.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q12. What is uclamp and how does it bend DVFS?
**A.** Per-task (or cgroup) `uclamp_min`/`uclamp_max` (0..1024) clamp the `util_est`
signal fed to `schedutil`. `uclamp_min` sets a frequency floor (latency-critical
foreground app), `uclamp_max` sets a ceiling (cap background work). Applied via
`sched_setattr()` with `SCHED_FLAG_UTIL_CLAMP_MIN/MAX` or `cpu.uclamp.{min,max}` in cgroup
v2.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q13. How does a context switch actually happen in Linux?
**A.** `schedule()` picks `next` via the scheduling class. `context_switch()` calls
`switch_mm()` (writes TTBR0 + ASID for the new mm), then `switch_to()` (arch asm save/restore
of callee-saved regs, SP, FP/SIMD lazy). On ARM64 KPTI may swap the TTBR1 set. After
return the CPU is running `next`.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q14. What is NUMA balancing in Linux?
**A.** `CONFIG_NUMA_BALANCING` periodically marks pages of a running task as
"NUMA-protected" (no-access); the resulting fault is intercepted to record which node
touched the page. The scheduler then migrates either the task or the pages so locality is
optimal.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

---

## 5. Synchronization, Locks & RCU (→ 02)

### Q1. Compare spinlock vs mutex.

| Property        | Spinlock                          | Mutex                              |
|-----------------|-----------------------------------|------------------------------------|
| Waiting         | Busy-wait                          | Sleeps (blocks)                    |
| Context         | Any (IRQ-safe with `_irqsave`)     | Process context only               |
| Sleep in CS     | NEVER                              | Allowed                            |
| Hold time       | Must be very short                 | Can be long                        |
| Preemption      | Disabled while held                | Preemption allowed while waiting   |

Use spinlock in ISRs/softirqs/short critical sections; mutex when you may call
`kmalloc(GFP_KERNEL)`, `copy_to_user()` etc. Shared with IRQ ⇒ `spin_lock_irqsave()`.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q2. Why does an interrupt handler need `spin_lock_irqsave` instead of `spin_lock`?
**A.** If the lock is also taken by an IRQ handler, taking it in process context with IRQs
enabled risks deadlock: the IRQ fires on the same CPU while you hold it and spins forever.
`spin_lock_irqsave` disables local IRQs first and saves the flags so the caller's prior
IRQ state is restored on unlock.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q3. What is RCU and when do you prefer it over a rwlock?
**A.** Read-Copy-Update — readers run lock-free between `rcu_read_lock()/unlock()` (which
on a non-preemptible kernel are no-ops). Writers update via copy-then-swap with
`rcu_assign_pointer()`, then call `synchronize_rcu()` or `call_rcu()` to defer free until
all pre-existing readers complete their grace period. Use RCU for read-heavy, latency-
sensitive paths (scheduler lists, routing tables, dentry cache). `rwlock` still blocks
readers on writers — RCU never does.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q4. What is a grace period in RCU?
**A.** The interval after a writer updates a pointer during which any pre-existing reader
might still be looking at the old version. Once every CPU has passed through a quiescent
state (context switch, idle, user-space) the grace period ends and the old version is
safe to free.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q5. What ARM64 memory barriers exist and when do you need them?
**A.** `DMB` (Data Memory Barrier) — orders memory ops up to the named shareability;
`DSB` — waits for completion (TLB invalidate, cache clean); `ISB` — synchronizes the
instruction pipeline (re-fetch). Linux wrappers: `smp_mb()`, `smp_rmb()`, `smp_wmb()` for
SMP ordering; `mb()`, `rmb()`, `wmb()` for full system ordering including DMA.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q6. Why are atomics not enough on weakly-ordered architectures?
**A.** `atomic_t` guarantees indivisible RMW but does not by itself imply ordering of
surrounding loads/stores. ARM64 is a weak memory model — you still need
`smp_mb__before_atomic()` / `smp_mb__after_atomic()` (or use `atomic_*_release` /
`_acquire` variants) when one variable's ordering depends on another.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q7. What is a seqlock and where is it used?
**A.** Optimised for **write-rare, read-frequent** data. Writers take a spinlock and
increment a sequence count (odd ⇒ write in progress). Readers loop reading data + the
sequence count; if it changed or is odd, retry. No reader-side locking ⇒ very cheap. Used
for `gettimeofday()`, the VFS jiffies, kernel timekeeping.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q8. What are per-CPU variables and why use them?
**A.** `DEFINE_PER_CPU(type, var)` allocates one copy per CPU; access via
`this_cpu_read/write/inc()` which disable preemption first. No cache-line bouncing between
cores ⇒ scales perfectly for stats, scheduler queues, RCU bookkeeping.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q9. What is lockdep and what does it catch?
**A.** `CONFIG_PROVE_LOCKING` runtime validator. Each lock has a "class"; lockdep records
the order in which classes are taken and reports any cycle (A→B in one path, B→A in
another) before the deadlock ever happens. Also catches sleeping-while-atomic, double
unlocks, IRQ vs non-IRQ inversion.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

### Q10. What is priority inversion and how does the kernel mitigate it?
**A.** A high-priority RT task waits for a lock held by a low-priority task that is
preempted by a medium-priority task. Linux uses **PI mutexes** (`rt_mutex`) that
temporarily boost the lock holder to the waiter's priority until release.
*Deep dive:* [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md)

---

## 6. Interrupts, IRQs, IPIs & Watchdog (→ 03)

### Q1. Describe ARM GICv3 architecture.
**A.** Three blocks: **GICD** (Distributor) — global SPI (32–1019) config, priority,
routing; **GICR** (Redistributor, per-CPU) — manages PPIs (16–31) and SGIs (0–15);
**CPU Interface** — system registers `ICC_IAR1_EL1` (acknowledge), `ICC_EOIR1_EL1` (EOI),
`ICC_PMR_EL1` (priority mask). GICv3 also adds **LPI** (8192+) for PCIe MSI/MSI-X via the
**ITS**.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q2. SGI vs PPI vs SPI vs LPI?

| Type | ID range  | Distribution           | Examples                              |
|------|-----------|------------------------|---------------------------------------|
| SGI  | 0–15      | Software, per-CPU      | IPI: smp_call_function, TLB shootdown |
| PPI  | 16–31     | Private per-CPU        | Generic timer, PMU                    |
| SPI  | 32–1019   | Shared peripheral      | GPIO, UART, USB, PCIe, camera         |
| LPI  | 8192+     | Message-signaled (ITS) | PCIe MSI/MSI-X                        |

*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q3. Trace an IRQ from hardware pin to Linux handler.
**A.** Peripheral asserts SPI → GICD prioritises and routes to a redistributor → CPU IRQ
exception to EL1 (`VBAR_EL1 + 0x280` for current EL using SP_ELn). `el1_irq` saves
regs → `handle_arch_irq` → GIC driver reads `ICC_IAR1_EL1` to claim & get irqnr →
`generic_handle_irq(irq)` → `irq_desc[irq].handle_irq` (`handle_fasteoi_irq` for level) →
`action->handler` chain → write `ICC_EOIR1_EL1` for priority drop.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q4. Compare hardirq, softirq, tasklet, workqueue.

| Type        | Context                | Sleep? | CPU pin    | Best for                     |
|-------------|------------------------|--------|------------|------------------------------|
| Hardirq     | IRQ ctx (IRQs off)     | No     | Any        | ACK HW, schedule deferred    |
| Softirq     | IRQ ctx (IRQs on)      | No     | Same CPU   | NET_RX, BLOCK_SOFTIRQ        |
| Tasklet     | Softirq ctx            | No     | Same CPU   | One-shot driver deferred work|
| Workqueue   | Process ctx (kthread)  | Yes    | Configurable| Long/sleeping work          |

Threaded IRQ (`request_threaded_irq` with `IRQF_ONESHOT`) lets you sleep in the handler
itself, running at SCHED_FIFO 50.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q5. Why use a threaded IRQ?
**A.** When the work is non-trivial: you can sleep, take mutexes, do `copy_to_user`, call
SCM, etc. The kernel thread runs as `irq/N-name` at SCHED_FIFO 50. The hard handler stays
tiny (`return IRQ_WAKE_THREAD;`), keeping interrupt latency low.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q6. How are IPIs implemented on ARM64?
**A.** Via SGIs (0–15). The kernel writes `ICC_SGI1R_EL1` (target list, SGI ID, affinity)
to fire the interrupt at the target CPUs. Used by `smp_call_function`, `scheduler_ipi`
(wake remote CPU), TLB invalidation broadcast (in practice ARM64 doesn't need IPIs for
TLB — TLBI IS broadcasts in hardware), `kgdb_roundup_cpus`.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q7. What's the difference between IRQ context and process context?
**A.** IRQ context: triggered asynchronously, can preempt anything, `current` may not be
the "owner", `in_interrupt()` is true, cannot sleep, GFP_ATOMIC only. Process context:
running on behalf of a syscall/work-item with a valid `current->mm`, can sleep, take
mutexes, allocate with GFP_KERNEL.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q8. What is interrupt coalescing?
**A.** Aggregating multiple device events into a single IRQ to reduce CPU overhead — NIC
RX descriptors, NVMe completion queues, NPU completion all use it. Trade-off: higher
throughput, slightly worse latency. Tune via ethtool `-c`.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q9. Explain interrupt storm — symptoms and mitigation.
**A.** Mis-configured level-sensitive line keeps asserting because the device isn't
ack'ed, or a spurious EMI keeps re-triggering. Symptoms: 100 % `%si`/`%hi` in `top`,
soft-lockups, watchdog bark. Mitigation: `disable_irq_nosync()` from the handler,
threaded IRQ with rate-limit, fix HW ack ordering, switch to edge-triggered if HW allows.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q10. What is the Qualcomm watchdog and what is "IPI ping"?
**A.** Qualcomm SoCs have a per-cluster MPM watchdog and an APSS soft-lockup watchdog
that runs as a kernel thread per CPU. It pings the other CPUs via SGI IPI ("IPI ping")
and waits for each to ACK. A missing ACK marks that CPU stuck → kernel panic → ramdump.
Useful to catch CPUs stuck in disabled-IRQ critical sections.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q11. How do you set IRQ affinity?
**A.** `echo MASK > /proc/irq/N/smp_affinity` (hex bitmask) or `_list` form. Driver-level
`irq_set_affinity()`. Use to bind high-traffic NIC IRQs to specific cores (e.g. dedicate
cores 4-7 to RX, leave 0-3 for app).
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q12. What does GICv4 add?
**A.** Direct injection of virtual LPIs into guest VMs through the ITS — KVM doesn't need
to vmexit for each MSI from a passed-through PCIe device. Hugely reduces VM-exit overhead
for high-PPS NICs and NVMe in guests.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

---

## 7. Linux Drivers — Char, Block, Platform, Misc (→ 04)

### Q1. Outline the Linux device driver model.
**A.** Three abstractions: **bus** (PCI, USB, I2C, SPI, platform), **device** and
**driver**. The bus's `match()` connects them; on match `driver.probe()` runs. Built atop
`kobject/kset/ktype` — that's the sysfs backbone.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q2. What does a platform driver's probe do, in order?
**A.** `platform_get_resource()` for the MMIO `reg` → `devm_ioremap_resource()` → 
`platform_get_irq()` → `devm_request_irq()` (or threaded) → `devm_clk_get_optional()` +
`clk_prepare_enable()` → `devm_reset_control_get()` + `reset_control_deassert()` →
power domain / regulators → register with subsystem (`misc_register`, `cdev_add`, `register_netdev`,
…). `devm_*` ensures everything tears down on unbind/error.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q3. Char device vs block device — fundamental differences.

| Aspect              | Char device                        | Block device                        |
|---------------------|------------------------------------|-------------------------------------|
| Access unit         | Byte stream                         | Fixed blocks (typically 4 KB)       |
| Buffering           | None (driver decides)              | Page cache, I/O scheduler           |
| Random access       | Optional                            | Required                            |
| API                 | `file_operations` (read/write/ioctl)| `bio` + `request_queue`             |
| Examples            | tty, /dev/random, sensors           | sda, mmcblk0, nvme0n1               |

*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q4. When do you choose a misc device vs a full character device?
**A.** Misc device (`misc_register()`) when you want exactly **one** node, don't care
about the major/minor (uses `MISC_DYNAMIC_MINOR` under major 10), and a tiny
`file_operations`. Full char dev with `alloc_chrdev_region()` + `cdev_add()` when you
need multiple minors, custom device class, or per-instance metadata.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q5. Write the skeleton of an I2C client driver.
**A.**
```c
static const struct of_device_id mysens_of[] = {
    { .compatible = "vendor,mysens" }, {}
};
MODULE_DEVICE_TABLE(of, mysens_of);
static int mysens_probe(struct i2c_client *cl) {
    if (!i2c_check_functionality(cl->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
        return -ENODEV;
    int id = i2c_smbus_read_byte_data(cl, REG_CHIPID);
    if (id != EXPECTED) return -ENODEV;
    /* register subsystem (iio/input/etc.) */
    return 0;
}
static struct i2c_driver mysens_drv = {
    .driver = { .name = "mysens", .of_match_table = mysens_of },
    .probe  = mysens_probe,
};
module_i2c_driver(mysens_drv);
```
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q6. Difference between `i2c_smbus_read_byte_data` and `i2c_transfer`.
**A.** `i2c_smbus_*` are restricted to SMBus shapes (byte, word, block) — simple,
internally calls `i2c_transfer`. `i2c_transfer` takes a raw `i2c_msg[]` so you control
START/STOP/repeated-START — needed for "write reg-addr then read N bytes without STOP",
the common sensor pattern.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q7. Walk through SPI controller + slave registration.
**A.** Controller driver provides `spi_controller_ops.transfer_one`. Slaves are defined
in DT under the controller node (`spi-max-frequency`, `spi-cpol`, `spi-cpha`). On probe
the slave driver gets a `struct spi_device` and uses `spi_sync_transfer()` /
`spi_async()` with `spi_message`/`spi_transfer` arrays. Modern alternative: `regmap-spi`.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q8. What does `regmap` solve?
**A.** Bus-agnostic register access — same driver works over I2C/SPI/MMIO by configuring
`regmap_config`. Caching, range checks, locking and debugfs dumps come for free. Use
`regmap_update_bits()` for read-modify-write of bit fields.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q9. Outline PCIe enumeration on Linux.
**A.** Root complex driver registers; kernel walks bus 0 reading config space at offset
0x00 (Vendor/Device); for each found device it assigns BAR space from the parent bridge's
windows (`pci_assign_resource`), walks bridges with `pci_scan_bus`, then `pci_bus_add_devices`
triggers driver `probe()`. ECAM gives 256 MB MMIO for 256 buses of config space. On
Qualcomm, IOMMU isolation via `iommus = <&apps_smmu SID mask>`.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q10. What is a PCIe BAR?
**A.** Base Address Register — defines a device MMIO/IO region. Up to 6 per function. OS
writes all-1s, reads back to find the size (the lowest bit that remains 0 = size), then
assigns a physical address. Driver maps via `pci_iomap()` / `pcim_iomap()`.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q11. USB enumeration — USB 2.0 vs 3.x driver implications.
**A.** Hub detects attach, host resets, assigns address (`SET_ADDRESS`), reads
device/config/iface/endpoint descriptors, selects configuration; usbcore matches a driver
via `usb_device_id`. USB 2.0 = polled token-based via EHCI (legacy) / xHCI; USB 3.x =
packet/credit-based, SuperSpeed bulk streams. Linux unified host driver is xHCI;
peripheral side on Qualcomm is Synopsys DWC3 (`dwc3-qcom.c`) with role switching via
extcon.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q12. What are `devm_*` APIs and why use them?
**A.** Device-managed resource allocators (`devm_kzalloc`, `devm_ioremap_resource`,
`devm_request_irq`, `devm_clk_get`). The resources are auto-freed on driver detach **or**
on probe failure — eliminates the cascade-of-`goto`-cleanup pattern and prevents leaks.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q13. How do you implement Runtime PM in a driver?
**A.**
```c
pm_runtime_set_autosuspend_delay(dev, 50);
pm_runtime_use_autosuspend(dev);
pm_runtime_set_active(dev);
pm_runtime_enable(dev);
/* before HW access: */
pm_runtime_get_sync(dev);
/* after: */
pm_runtime_put_autosuspend(dev);
```
Implement `.runtime_suspend / .runtime_resume` in `dev_pm_ops` to gate clocks/regulators.
Use-count == 0 + autosuspend delay ⇒ device suspended.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q14. What is the Common Clock Framework and how do you use it?
**A.** `clk_get()/devm_clk_get()` returns a `struct clk *`. Always pair
`clk_prepare_enable()` with `clk_disable_unprepare()`. `clk_set_rate()` rounds; verify
with `clk_get_rate()`. `CLK_SET_RATE_PARENT` propagates to parent. Inspect with
`/sys/kernel/debug/clk/clk_summary`.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q15. How do you handle GPIOs in a modern driver?
**A.** Use the descriptor API: `devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW)`. Set with
`gpiod_set_value_cansleep(g, 1)` (cansleep variant is safe for I2C-expander GPIOs).
Active-low semantics are declared in DT (`gpios = <&tlmm 10 GPIO_ACTIVE_LOW>`) so the
driver just says "assert".
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q16. Outline DMA Engine usage for a slave device.
**A.** `dma_request_chan(dev, "tx")` → configure with `dmaengine_slave_config()` →
`dmaengine_prep_slave_sg()` returns a `dma_async_tx_descriptor` →
`dmaengine_submit()` + `dma_async_issue_pending()` → completion via callback or
`dma_wait_for_async_tx()`.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q17. What is `dma-buf` and what problem does it solve?
**A.** A cross-driver buffer-sharing primitive — a producer (camera ISP) exports a
buffer, consumers (encoder, GPU, display) import via fd. The framework handles per-driver
attach/map/detach and sync via `dma_fence`. Foundation of zero-copy camera→GPU→display
pipelines on Qualcomm.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q18. How do you write a block driver in modern Linux?
**A.** Allocate `struct request_queue` via `blk_mq_init_queue()` with a `blk_mq_ops` that
implements `queue_rq()`. Allocate `struct gendisk` with `alloc_disk()` / `blk_alloc_disk()`,
set capacity with `set_capacity()`, register with `add_disk()`. Each request is a `struct
request` containing one or more `bio`s.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q19. How do you debug a driver that won't probe?
**A.** `dmesg | tail`, `ls /sys/bus/platform/devices/<addr>.<name>/driver` (symlink missing
⇒ no match), `ls /sys/kernel/debug/devices_deferred` to see deferred-probe stalls,
`echo 8 > /proc/sys/kernel/printk` and dynamic-debug
(`echo 'module foo +p' > /sys/kernel/debug/dynamic_debug/control`) to enable `pr_debug`.
Check DT `status = "okay"` and `compatible` string match.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q20. What is deferred probing and async probing?
**A.** **Deferred**: `probe` returns `-EPROBE_DEFER` when a dependency (clock/regulator/
phy) isn't ready; the kernel re-queues the device for later. **Async**: drivers marked
`PROBE_PREFER_ASYNCHRONOUS` run probe on a kworker, parallelising boot. Both help with
boot-time parallelism and dependency cycles.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

---

## 8. Device Tree (→ 04)

### Q1. What is Device Tree and why does Linux ARM use it?
**A.** A hardware-description format (DTS source → DTB binary) handed to the kernel by
the bootloader. Replaces hard-coded board files. Each node describes a hardware block
(MMIO base, IRQ, clocks, pins, reset, regulator, …). Kernel parses it, populates platform
devices via `of_platform_populate()`, and matches drivers by `compatible` string.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q2. What is the `compatible` string and why is order significant?
**A.** Identifies the hardware (`"vendor,specific-part", "vendor,family"`). The kernel
matches the list in order — most-specific first lets a generic driver fall back if the
specific one isn't loaded. Each `compatible` value needs a DT binding doc in
`Documentation/devicetree/bindings/`.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q3. What is `#address-cells` / `#size-cells` and why does it matter?
**A.** They tell the parser how many u32 cells make up an address and a size in a child
`reg` property. ARM64 typically uses `<2 2>` (64-bit addr + 64-bit size). Wrong values
⇒ MMIO mapping at the wrong address.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q4. What does `phandle` mean in DT?
**A.** A 32-bit token (assigned automatically by `dtc` via `&label`) that lets one node
reference another (`clocks = <&gcc 12>`, `iommus = <&apps_smmu 0x740 0>`,
`interrupt-parent = <&intc>`). Resolved by `of_parse_phandle*` helpers.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q5. How is the DTB chosen on Qualcomm platforms?
**A.** ABL reads the SoC + board + SKU IDs from SMEM at boot, walks the list of appended
DTBs (`qcom,msm-id`, `qcom,board-id`, `qcom,pmic-id` properties) and picks the matching
one before passing its address to the kernel in `x0`.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q6. What are DT overlays?
**A.** Fragment DTBs (`.dtbo`) that can be applied to a base tree at runtime to add/
modify nodes — used to describe HATs, daughterboards, hot-plug accessories, or to flip
board variants without a kernel rebuild.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q7. How does pinctrl bind to a device in DT?
**A.** A device node declares pinctrl states:
```dts
pinctrl-names = "default", "sleep";
pinctrl-0 = <&uart0_active>;
pinctrl-1 = <&uart0_sleep>;
```
The kernel calls `pinctrl_select_state()` on probe/suspend, applying the pin-mux + bias +
drive-strength configuration to TLMM.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q8. Where do you find binding documentation, and what does YAML validation give you?
**A.** `Documentation/devicetree/bindings/` (YAML schemas since kernel 5.x). `make dtbs_check`
validates all DTBs in the tree against schemas — catches missing required props, wrong
cell counts, illegal enum values before runtime.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q9. How do you expose Device Tree to userspace?
**A.** Raw tree under `/sys/firmware/devicetree/base/` — each node is a directory, each
property a file. `cat /sys/firmware/devicetree/base/compatible` reads the root compatible
list. Platform devices created from DT also appear at `/sys/devices/platform/`.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q10. How do Qualcomm-specific compatible strings indicate the binding?
**A.** `qcom,geni-i2c` → GENI-based I2C in QUPv3; `qcom,qup-spi` → QUP SPI; `qcom,msm-uart` /
`qcom,geni-uart` → serial; `qcom,spmi-pmic` → SPMI PMIC; `qcom,arm-smmu` / `qcom,sm8550-smmu-500` →
SMMU. Each is documented under `bindings/` and matched by the corresponding driver.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

---

## 9. procfs / sysfs / debugfs / configfs (→ 04)

### Q1. What is `/proc` and how is it implemented?
**A.** A pseudo-filesystem mounted at `/proc`, backed entirely by RAM. Each entry is
created with `proc_create()` / `proc_mkdir()` and a `struct proc_ops` (was `file_operations`
pre-5.6). Reads are most cleanly implemented via the `seq_file` iterator (`start/next/stop/show`)
which handles pagination automatically.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q2. Compare `/proc` and `/sys`.

| Aspect              | `/proc`                                   | `/sys`                              |
|---------------------|-------------------------------------------|-------------------------------------|
| Purpose             | Process info, sysctl, misc kernel         | Device-model hierarchy              |
| Layout              | Multi-value, free-form                    | **One value per file** (strict)     |
| Backend             | `proc_ops` + seq_file                     | kobject + kernfs                    |
| Hotplug / uevents   | No                                        | Yes                                 |
| ABI stability       | Stable (sysctl)                           | Stable (strict)                     |
| Introduced          | Linux 1.0 (1994)                          | Linux 2.6 (2003)                    |

*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q3. Why is `sysfs_emit()` preferred over `sprintf()` in a sysfs show callback?
**A.** `sysfs_emit()` is bounded to PAGE_SIZE — it cannot overflow the buffer the sysfs
core gives you. `sprintf()` has no bound; a bug in the format string corrupts kernel
memory. `sysfs_emit` is the modern (≥5.10) standard.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q4. What is the correct return value from a sysfs `store()`?
**A.** **`count`** on success (the number of bytes consumed). Returning 0 causes user-space
to loop forever rewriting. Returning a negative errno signals failure.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q5. Why prefer attribute groups (`.dev_groups`) over `device_create_file()`?
**A.** Atomic create/destroy, automatic cleanup on driver unbind, `is_visible()` callback
to hide attributes that the hardware doesn't support, attributes exist *before* the
`uevent` is sent to udev (no race), and zero manual lifecycle management.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q6. What is the kobject / kset / ktype trinity?
**A.** **kobject** = reference-counted base object, represented as a directory in sysfs.
**kset** = a collection of kobjects; generates `uevents` for member adds/removes. **ktype** =
behaviour (default attributes, `sysfs_ops`, `release()`). Lifecycle:
`kobject_init → kobject_add → kobject_uevent(KOBJ_ADD) → … → kobject_put` (release on
refcount 0).
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q7. What is kernfs and why was it introduced?
**A.** Linux 3.14 generic VFS backend that both sysfs and cgroup-fs now use. It decouples
kobject lifetime from VFS inode lifetime, eliminating the old race conditions where a
process could be reading a sysfs file while the underlying kobject was being freed.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q8. How does udev cooperate with sysfs?
**A.** On `kobject_uevent(KOBJ_ADD)` the kernel sends a netlink message on
`NETLINK_KOBJECT_UEVENT`. `udevd` receives it, reads `DEVPATH/SUBSYSTEM/MAJOR/MINOR`
plus extra attributes from `/sys/…`, matches `/etc/udev/rules.d/`, creates `/dev/xxx`,
runs `RUN+=` commands. `udevadm monitor --kernel --udev` watches the flow live.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q9. When do you use debugfs, and what's the ABI rule?
**A.** Developer-only diagnostics under `/sys/kernel/debug` (root-only). **No ABI
guarantee** — entries can be renamed, removed, restructured between kernels at any time.
Never script against debugfs in production. APIs: `debugfs_create_dir/file/u32/bool` and
`debugfs_remove_recursive` at exit.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q10. What is `/proc/iomem` vs `/proc/ioports` on ARM?
**A.** `/proc/iomem` lists the physical address space — registered via
`request_mem_region()` and accessed via `ioremap`. `/proc/ioports` describes the legacy
x86 I/O-port space — effectively empty on ARM since ARM SoCs use MMIO only.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q11. Walk through what `/proc/<pid>/maps` shows.
**A.** Per-VMA line: `start-end perms offset dev:inode pathname`. perms = `rwxp` (private)
or `s` (shared). Pathname is `[heap]`, `[stack]`, `[vdso]`, or a file path. Backed by
iterating the process `mm_struct`'s VMA tree (rbtree pre-6.1, maple tree from 6.1).
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q12. What is `/proc/sys` and what's the modern way to register a sysctl?
**A.** Tunable kernel parameters; same as the `sysctl` tool. Register at runtime:
```c
static struct ctl_table t[] = {
    { .procname="my_param", .data=&v, .maxlen=sizeof(int),
      .mode=0644, .proc_handler=proc_dointvec },
    {}
};
register_sysctl("kernel", t);
```
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q13. What is the common ABI pitfall when adding a sysfs file?
**A.** Putting multiple values into one file violates the *one value per file* contract —
once shipped, that becomes ABI. Always one scalar (number, string, enum) per file, with
its own attribute.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

---

## 10. System Calls (→ 04)

### Q1. Walk the ARM64 syscall path from user `SVC #0` to handler return.
**A.** User code puts syscall # in `x8`, args in `x0..x5`, executes `SVC #0` → CPU takes
sync exception to EL1 at `VBAR_EL1 + 0x400` (lower EL AArch64) → `el0_sync` saves
`pt_regs` → reads ESR_EL1 (`EC=0x15` ⇒ SVC) → `el0_svc()` → `do_el0_svc()` indexes
`sys_call_table[x8]` → handler runs (`sys_read`/`sys_write`/…) → return value in `x0` →
`ret_to_user` handles pending signals/reschedule → `ERET` back to EL0.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q2. How is the syscall path different on ARMv7?
**A.** Syscall number is in `r7`, args in `r0..r5`, `SVC #0` traps to SVC mode → vector at
`0xFFFF0008` → `vector_swi` saves regs → looks up `sys_call_table` → handler → ERET via
`MOVS PC, LR` which restores CPSR from SPSR_svc and returns to USR.
*Deep dive:* [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md)

### Q3. How do you add a new syscall to the kernel?
**A.** Pick a free number in `arch/arm64/include/uapi/asm/unistd.h`, add it to the syscall
table (`asm-generic/unistd.h`), declare with `SYSCALL_DEFINEN(name, …)` in the
implementation file, write the userspace `libc` wrapper or use `syscall(__NR_x, …)`.
Bump `__NR_syscalls` in the table.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q4. What is the role of vDSO?
**A.** A small shared object the kernel maps into every process providing fast
implementations of `gettimeofday()`, `clock_gettime()`, `getcpu()` that read clock data
from a shared page without an actual syscall — zero kernel-entry overhead.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q5. What does seccomp do to syscalls?
**A.** Filters syscalls in the kernel using a BPF program attached to `current`. Modes:
`SECCOMP_MODE_STRICT` (only read/write/exit/sigreturn), `SECCOMP_MODE_FILTER` (BPF
decides ALLOW/ERRNO/KILL/TRAP/LOG per syscall+args). Docker's default profile blocks
~44 dangerous syscalls.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

---

## 11. Boot Flow, U-Boot, APPSBL, ATF (→ 05)

### Q1. Walk a Qualcomm SoC from power-on to userspace shell.
**A.** **PBL** in ROM authenticates and loads **XBL** (UEFI-based) → XBL does DDR
training, PMIC init, clock setup, loads **QSEE/TZ** at EL3 and **ABL** → ABL (LK-based)
reads board ID from SMEM, picks the matching DTB, loads `Image(.gz)` + DTB + initramfs
from the boot partition, applies bootargs → jumps to kernel `_head` at EL2/EL1 →
`start_kernel()` → driver `do_initcalls()` → spawns init/systemd (PID 1) → userspace.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q2. What does ARM Trusted Firmware-A (ATF / TF-A) do?
**A.** Reference EL3 firmware. **BL1** = ROM trampoline, **BL2** = trusted boot/auth
loader, **BL31** = the runtime EL3 monitor (handles `SMC`, PSCI CPU on/off/suspend,
context switches between secure and non-secure worlds). On Qualcomm the secure runtime
is QSEE rather than a stock OP-TEE, but the EL3 / PSCI interface is the same.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q3. What is PSCI and why do we use it?
**A.** Power State Coordination Interface — a standard set of SMC calls from EL1 to EL3
firmware: `CPU_ON`, `CPU_OFF`, `CPU_SUSPEND`, `SYSTEM_RESET`, `SYSTEM_OFF`. Lets one
kernel binary cleanly bring up secondary cores and enter deep idle on any compliant SoC
without SoC-specific code.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q4. What is U-Boot's role and what's the equivalent on Qualcomm?
**A.** U-Boot is a generic open-source bootloader doing storage init, image loading,
fastboot, env handling. Qualcomm replaces it with the closed ABL (Android Boot Loader,
LK-based) which performs the same job but also implements verified boot, A/B partitions,
and the SMEM board-ID DTB selector.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q5. What is Verified Boot / chain of trust?
**A.** Each stage cryptographically verifies the next image's signature against a public
key whose hash is fused at manufacture (Qualcomm: PKHash in QFPROM). PBL → XBL → ABL →
boot/vbmeta → kernel. Any mismatch ⇒ device refuses to boot or enters orange/red dm-verity
state. Roots out persistent malware.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q6. How is the kernel command line passed and where do bootargs come from?
**A.** ABL writes `bootargs` into the DTB `/chosen/bootargs` node before passing the DTB
address in `x0`. The kernel parses it in `setup_arch()`. Sources of args: bootloader env,
`/etc/default/grub` (UEFI), `boot.img` cmdline, A/B vbmeta.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q7. What is `earlycon` and why is it the first debug tool you turn on?
**A.** A console driver that runs before the platform serial driver probes — relies only
on a hard-coded MMIO base in the `earlycon=` cmdline param. Lets you see kernel messages
during early `start_kernel()` when the full serial driver hasn't initialized yet — vital
for "no console output" bring-up problems.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q8. Major boot-time optimisation levers from firmware to userspace?

| Stage      | Technique                                    | Typical saving |
|------------|----------------------------------------------|----------------|
| Firmware   | DDR-training cache / warm-boot skip          | 100–500 ms     |
| Bootloader | Parallel storage, preload ramdisk            | 50–200 ms      |
| Decompress | gzip → LZ4                                   | 50–150 ms      |
| Kernel     | Async probe + deferred probe                 | 200–800 ms     |
| Initcalls  | Push non-critical to `late_initcall` / `=m`  | 100–400 ms     |
| Userspace  | systemd socket activation / parallel start   | 0.5–2 s        |

*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q9. How do you measure boot time?
**A.** `initcall_debug` on cmdline + `printk.time=1` → `dmesg | grep "initcall"` shows
each driver's init duration. `systemd-analyze [blame|critical-chain|plot]` for userspace.
`bootchart2` for full timeline. Custom `printk("checkpoint %llu", ktime_get_boot_ns())`
for any code path.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q10. What is the role of initramfs?
**A.** A cpio archive unpacked into `rootfs` (tmpfs) by the kernel after decompression.
Contains the minimum to mount the real root: udev, modprobe, the storage/network/dm-verity
modules. Without an initramfs the kernel must have the root filesystem driver built in.
Built into the kernel via `CONFIG_INITRAMFS_SOURCE` to skip a load step.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q11. What's the difference between initrd and initramfs?
**A.** **initrd** = a real block-device image; the kernel sets up a ramdisk, mounts it
as the root, runs `/linuxrc`, then pivot_roots to the real root. **initramfs** = a cpio
unpacked into a `tmpfs` rooted at `/`; no block device, much simpler, no `pivot_root`
needed (use `switch_root`). All modern systems use initramfs.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

---

## 12. Hibernation & Suspend (→ 05)

### Q1. Compare suspend-to-idle, S3 suspend-to-RAM, hibernate.

| State   | Cmd                    | RAM         | CPUs    | Wake time | Power     |
|---------|------------------------|-------------|---------|-----------|-----------|
| s2idle  | `echo freeze > /sys/power/state` | live  | deep-WFI | <100 ms   | ~200 mW   |
| S3      | `echo mem > …`         | self-refresh| all off | tens ms   | 10–50 mW  |
| S4      | `echo disk > …`        | dumped to swap | off  | seconds   | 0 mW      |

*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q2. Walk through the suspend-to-RAM flow.
**A.** `echo mem > /sys/power/state` → `pm_suspend(PM_SUSPEND_MEM)` → freeze tasks
(`freeze_processes`) → `dpm_suspend_start` runs every device's `.prepare/.suspend/.suspend_late/
.suspend_noirq` → disable non-boot CPUs (`disable_nonboot_cpus`) → arch-level
`syscore_suspend` → call PSCI `CPU_SUSPEND` to enter platform LPM. Resume reverses
everything in opposite order.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q3. What is hibernation and what writes the image?
**A.** S4: kernel snapshots its memory image and writes it (compressed) to the swap
partition via `swsusp_save → swsusp_write`. On resume, an early kernel reads the image
back, restores it, and continues — slow but full power-off in between. Rarely used on
mobile (battery-backed deep idle is cheaper). Tooling: `uswsusp` (`s2disk`), `pm-utils`.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q4. What is Runtime PM and how is it different from system suspend?
**A.** Runtime PM suspends individual devices when idle, independent of system state. Use
count + autosuspend delay drives `.runtime_suspend`. System suspend (S3/S4) suspends
**everything** at once on a global event. The two callbacks live side by side in
`dev_pm_ops`.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q5. What is RPMH on Qualcomm and where does Linux talk to it?
**A.** Resource Power Manager Hardened — a dedicated ARC processor that owns shared
resources (rails, clocks, bus). Linux votes asynchronously via TCS (Triggered Command
Sets) sent through the RSC. Drivers: `rpmh-rsc.c`, `cmd-db.c`, `qcom-rpmh-regulator.c`,
`rpmhpd.c`. Three TCS sets: ACTIVE (immediate), SLEEP (on CPU idle), WAKE (on resume).
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q6. What is a wakeup source and how do you debug a wakeup leak?
**A.** Any subsystem that can prevent suspend or wake the system (modem RX, RTC alarm,
GPIO key, USB). Read `/sys/kernel/debug/wakeup_sources` for stats. `wakeup_count` racing
against `echo mem > state` lets userland safely arm suspend. Frequent unexplained wakes ⇒
PSI of `power/wakeup_*` tracepoints.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

---

## 13. Crash Dump & Kernel Debugging (→ 06)

### Q1. Kernel oops vs kernel panic — what's the difference?
**A.** **Oops** = recoverable: NULL deref, BUG(), bad memory access. System stays alive
(possibly unstable). **Panic** = fatal: `panic()`, double fault, oops in critical context.
System halts; `panic_timeout` may auto-reboot. `panic_on_oops=1` promotes any oops to a
panic so it's caught immediately.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q2. What does an ARM64 oops backtrace tell you?
**A.** **PC** = faulting instruction; **LR / x30** = caller; **SP** = current stack;
**ESR_EL1** = exception class + syndrome (translation/permission/alignment fault);
**FAR_EL1** = the bad VA; **Call Trace** = unwound stack from fault to entry. With
`vmlinux` you can resolve every PC to a source line.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q3. How do you decode a crash backtrace?
**A.** `scripts/decode_stacktrace.sh vmlinux < oops.txt`,
`addr2line -e vmlinux -i 0xffffffc0081234ab`, `objdump -d vmlinux | less +/<symbol>`.
For full post-mortem, use `crash vmlinux vmcore`.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q4. What is kdump and how does it capture a vmcore?
**A.** A second "capture" kernel pre-loaded with `kexec`. On panic the primary kernel
hands control to the capture kernel which boots in a reserved memory window
(`crashkernel=` cmdline), then dumps the old kernel's memory as `/proc/vmcore` to disk
via `makedumpfile`. On Qualcomm consumer devices the equivalent is the DLOAD/EDL ramdump
collected by QPST.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q5. What can you do with the `crash` utility?
**A.** Post-mortem inspection of vmcore: `bt` (current task backtrace), `bt -a` (all
tasks), `bt -p PID`, `ps`, `log` (dmesg at crash), `kmem -s` (slab stats), `vm` (current
mm), `dis <fn>`, `struct task_struct ffff...` to pretty-print structures. Also works on
the live kernel via `/proc/kcore`.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q6. What is KASAN and what does it catch?
**A.** Kernel Address SANitizer — shadow-memory based detector for use-after-free,
out-of-bounds, double-free in slab and stack. Catches the bug at the *write*, prints a
report with stacks for alloc / free / access. Run-time overhead ~1.5–3×; for debug
builds. Companion: UBSAN (undefined behaviour), KMSAN (uninit reads), KCSAN (data races).
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q7. Common debug `CONFIG_*` options you turn on when chasing a hang?
**A.** `CONFIG_DEBUG_INFO`, `CONFIG_FRAME_POINTER`, `CONFIG_PROVE_LOCKING` (lockdep),
`CONFIG_DEBUG_ATOMIC_SLEEP`, `CONFIG_DEBUG_MUTEXES`, `CONFIG_DEBUG_SPINLOCK`,
`CONFIG_KASAN`, `CONFIG_DETECT_HUNG_TASK`, `CONFIG_SOFTLOCKUP_DETECTOR`, `CONFIG_KGDB`,
`CONFIG_DYNAMIC_DEBUG`, `CONFIG_FTRACE`.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q8. How do you debug a soft-lockup?
**A.** `watchdog/N` thread didn't get to run for `watchdog_thresh` seconds ⇒ kernel
prints a backtrace of the stuck CPU. Almost always: someone holding `local_irq_disable`
or a spinlock too long. Check the trace, find the function, enable `irqsoff` tracer to
catch the offender. On Qualcomm the APSS watchdog uses the IPI-ping mechanism to confirm
which CPU is stuck.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q9. How do you debug a hung task?
**A.** `CONFIG_DETECT_HUNG_TASK` prints a backtrace of any task in `D` (uninterruptible)
state for `hung_task_timeout_secs`. Combine with `echo w > /proc/sysrq-trigger` to dump
all blocked tasks on demand. Inspect what mutex/lock they're blocked on.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q10. Walk through using JTAG to debug an early-boot hang.
**A.** Wire Lauterbach TRACE32 or DS-5 onto the 20-pin debug header; build kernel with
`CONFIG_DEBUG_INFO`. Load `vmlinux` with DWARF in TRACE32, halt the core, set HW
breakpoint at `start_kernel` (or earlier — `__primary_switched`). Step through, inspect
registers + memory. On Qualcomm use QDSS CoreSight to capture ETM trace if the bug is
timing-dependent.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q11. What is ftrace and how do you use it interactively?
**A.** In-kernel function tracer accessed via `/sys/kernel/debug/tracing/`. Set
`current_tracer` to `function`, `function_graph`, `irqsoff`, `wakeup`, etc.; filter with
`set_ftrace_filter`; enable static tracepoints in `events/*`; read `trace` or `trace_pipe`
(streaming).
```sh
echo function_graph > current_tracer
echo my_driver_irq > set_ftrace_filter
echo 1 > tracing_on
cat trace_pipe
```
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q12. perf vs ftrace vs eBPF — when do you use each?

| Tool   | Best for                                       | Overhead |
|--------|------------------------------------------------|----------|
| perf   | Sampling profiler, PMU counters, flamegraphs   | Very low |
| ftrace | Always-on function/event tracing, latencies    | Low      |
| eBPF   | Programmable aggregation, custom histograms, production observability | Low (verified) |

Real answer: combine them — `perf` for "what's hot", `ftrace` for "what's the sequence",
`bpftrace` for "what's the distribution / correlation".
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q13. Show three handy bpftrace one-liners for kernel debugging.
**A.**
```sh
bpftrace -e 'kretprobe:vfs_read { @lat = hist(retval); }'
bpftrace -e 'tracepoint:sched:sched_switch { @[args->next_comm] = count(); }'
bpftrace -e 'profile:hz:99 { @[kstack] = count(); }'
```
Latency histogram of `vfs_read`, top tasks getting scheduled, on-CPU sampling of kernel
stacks.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q14. How would you debug "a userspace app suddenly shows latency spikes"?
**A.** Layered approach: `perf stat` for IPC/cache; `perf record -g` for hotspots;
`ftrace irqsoff` / `preemptoff` for interrupt/preempt-off blackouts; `bpftrace offcputime`
for off-CPU stacks; `perf sched latency` for scheduler delays; check
`/proc/<pid>/sched`, `/proc/pressure/cpu`, RT throttling, thermal throttling, frequency
governor. Always isolate the layer (HW vs scheduler vs kernel vs app).
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q15. What is dynamic_debug?
**A.** Lets you turn `pr_debug()` / `dev_dbg()` on or off per file, function or line at
runtime via `/sys/kernel/debug/dynamic_debug/control` — no recompile. Example:
`echo 'file my_drv.c +p' > control`.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

---

## 14. Qualcomm Platform & Subsystems (→ 07)

### Q1. What is GENI / QUPv3 and why is it special?
**A.** Qualcomm's Generic Interface (GENI) inside the QUPv3 wrapper is a configurable
Serial Engine: each SE can be programmed as I2C, SPI, UART or I3C, with a built-in GSI
DMA engine. One driver framework (`drivers/{i2c,spi,tty/serial}/*geni*`) handles all
flavours. FIFO mode for short transfers, GSI DMA for long ones.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q2. What is TLMM and how do you describe a pin?
**A.** Top-Level Mode Multiplexer — Qualcomm pin controller for every SoC pad. Driver:
`pinctrl-msm.c` plus per-SoC table (`pinctrl-sm8550.c`). DT state defines pin function
mux, drive strength, bias and direction:
```dts
uart0_active: uart0-active-state {
    pins = "gpio4","gpio5"; function = "qup0_se0_l0";
    drive-strength = <2>; bias-disable;
};
```
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q3. What is SMEM on Qualcomm?
**A.** Shared Memory — a fixed DRAM region (~2 MB) used as the common bulletin board
between APSS (Linux), MPSS (modem), ADSP, CDSP, WCSS. Header + table of items
(BOARD_INFO, BOOT_INFO, MODEM_REASON, …). Linux driver: `drivers/soc/qcom/smem.c`;
hwlock for atomicity.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q4. What is QMI and how is it transported?
**A.** Qualcomm MSM Interface — TLV-encoded structured messages between HLOS and modem/
ADSP/WCSS. Transport is QRTR sockets (`AF_QIPCRTR`) over shared memory + SMP2P
signalling. Used for Wi-Fi firmware control, rmnet (modem data), sensor HAL, thermal,
SSR notifications.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q5. What is Subsystem Restart (SSR)?
**A.** When a co-processor (modem/ADSP/CDSP) crashes, Linux receives a notification via
SMP2P/QMI, runs registered subsystem-restart notifiers (drivers can quiesce/clean up),
collects a remoteproc ramdump (`/sys/class/remoteproc/`), then reloads the firmware
(`PIL` / `remoteproc-qcom-pas`) and reboots the subsystem — without rebooting the
phone.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q6. What is FastRPC / Hexagon DSP?
**A.** RPC framework that marshals function calls + DMA-buf buffers between Linux and the
Hexagon DSP (ADSP/CDSP). Driver `drivers/misc/fastrpc.c` exposes `/dev/fastrpc-*`.
Userspace (QNN, audio HAL, sensor HAL) opens it, invokes a "stub" with `FASTRPC_IOCTL_INVOKE`;
the DSP runs the corresponding skel. SMMU isolates DSP DMA.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q7. What is the Qualcomm SCM driver and what does it do?
**A.** `drivers/firmware/qcom_scm.c` — wrapper for ARM `SMC` calls to QSEE/TZ. Provides
APIs to read fuses, assign memory ownership between VMs/secure-heap (`qcom_scm_assign_mem`),
boot peripherals (`qcom_scm_pas_*`), call into a Trusted Application, configure secure
SMMU contexts. Every non-secure interaction with the secure world goes through it.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q8. Describe Qualcomm camera stack from sensor to userspace.
**A.** CSI-2 lanes → CSIPHY → CSID (deserializer) → VFE/ISP (image signal processor) →
DMA-buf into a memory pool. Kernel: `drivers/media/platform/qcom/camss/` (V4L2 subdev
graph). Userspace: CamX-CHI HAL builds the pipeline and feeds it via V4L2 ioctls /
videobuf2. SMMU per stream provides DMA isolation; secure CB for DRM/encrypted captures.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q9. What is QDSS / CoreSight on Qualcomm?
**A.** ARM CoreSight trace infrastructure. **ETM** = instruction-level trace per CPU,
**STM** = software-instrumented trace, **funnel** = combine streams, **TMC/ETB** = on-chip
trace buffer, **TPIU** = external trace out. Linux driver:
`drivers/hwtracing/coresight/`. Captures pre-crash PC history without breakpoints —
indispensable for production failure analysis.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q10. What is GDSC?
**A.** Global Distributed Switch Controller — Qualcomm's hardware power-domain gating
(per-subsystem). Modeled as generic PM domains (`genpd`) via
`power-domains = <&clock_gcc GCC_GPU_GDSC>` in DT so any device in that subsystem is
automatically powered up on `pm_runtime_get`.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q11. What is the Qualcomm thermal stack?
**A.** TSENS sensors → Linux thermal framework zones → governors: **step_wise** (default)
or **power_allocator** (IPA — PID controller distributing a thermal budget across CPU/GPU
cooling actors). DT defines trip points; cooling devices are cpufreq, devfreq, display
brightness. Critical trip ⇒ `orderly_poweroff`.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q12. Walk through the Yocto recipe / layer / bbappend concept.
**A.** **Recipe (.bb)** = build instructions for ONE component (URL, patches, compile,
install). **Layer (meta-*)** = collection of recipes/configs/classes (listed in
`bblayers.conf`). **.bbappend** = overlay that extends an existing recipe without
modifying it — adds patches or flips configs per platform. `bitbake <recipe>` runs the
dependency graph and tasks.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q13. What is `meta-qcom` and what does it provide?
**A.** Qualcomm's BSP layer for Yocto — recipes for the Qualcomm-specific kernel,
proprietary firmware blobs (modem/Wi-Fi/DSP/GPU), QSEE images, board configs. Access via
CodeLinaro.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q14. UART console doesn't come up on a new Qualcomm board — how do you debug?
**A.** 6 steps:
1. Schematic — confirm UART instance + pin numbers; scope TX line at boot.
2. DT — node `status = "okay"`, correct `reg`, `clocks`, `pinctrl-0/-1`.
3. Clocks/power: `/sys/kernel/debug/clk/gcc_qupv3_uart0_clk/clk_enable_count`,
   regulator counts.
4. Pinmux: `/sys/kernel/debug/pinctrl/*`, `/sys/kernel/debug/gpio`.
5. Add `earlycon=msm_serial_dm,<paddr>` to bootargs — proves whether issue is HW or
   driver.
6. JTAG: break in `msm_geni_serial_startup`, walk register state vs TRM.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q15. Outline TrustZone communication: Linux → SMC → QSEE → Trusted App.
**A.** Driver calls `qcom_scm_call_*` / `qseecom_send_cmd` → SCM helper builds the
function ID and args → executes `SMC #0` → trap to EL3 (ATF) → ATF validates and dispatches
to QSEE → QSEE routes to the target Trusted Application — TA processes the request,
writes the response to shared memory → return path back through EL3 → SCM driver returns
to caller. Memory shared by reference (PA) and reassigned via `qcom_scm_assign_mem`.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

---

## 15. IOMMU / SMMU (→ 07)

### Q1. What is the difference between IOMMU and SMMU?
**A.** IOMMU is the architecture-agnostic concept of an MMU for DMA-capable devices. SMMU
is ARM's implementation (v1/v2/v3). Intel uses VT-d, AMD uses AMD-Vi. On Qualcomm the
SMMU IP is the ARM MMU-500 (SMMUv2) on most Snapdragons or SMMUv3 on newer designs.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q2. Why does the IOMMU exist?
**A.** Five reasons: (1) **DMA protection** — confine each device to its own page table,
preventing a rogue or compromised PCIe device from DMAing arbitrary memory. (2) **Address
translation** — 32-bit-DMA devices can access RAM above 4 GB through IOVA remapping.
(3) **Scatter-gather** — present physically fragmented pages as a contiguous IOVA to the
device. (4) **Virtualization** — Stage-2 translation lets a hypervisor pass a device to
a VM safely. (5) **SVA** — share the CPU process's page tables with the device for
zero-overhead userspace DMA (AI/ML accelerators).
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q3. Outline SMMUv2 architecture.
**A.** A Stream ID identifies the DMA master. **SMR** (Stream Match Register) +
**S2CR** map SID to a **Context Bank** (CB). Each CB has its own TTBR0 (Stage-1) and
optionally VTTBR (Stage-2), TCR, MAIR, FSR/FAR — basically an independent address space.
The CB walks the LPAE page tables (same format as the CPU MMU). Up to ~128 CBs per
instance. Per-CB fault interrupts.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q4. Compare SMMUv2 and SMMUv3.

| Feature              | SMMUv2                        | SMMUv3                                          |
|----------------------|-------------------------------|-------------------------------------------------|
| Config interface     | MMIO registers (SMR, S2CR)    | DRAM-based CMDQ/EVTQ                            |
| Stream mapping       | ~128 SMR entries              | Stream Table in DRAM (linear or 2-level)        |
| Per-stream config    | Context Bank                  | Context Descriptor table (per SID/SSID)         |
| PASID / SVA          | No                            | Yes — multiple address spaces per device        |
| Stall model          | No (immediate fault)          | Yes — stall DMA, OS fixes up, RESUME            |
| PCIe ATS/PRI         | No                            | Yes                                             |
| TLB invalidation     | Register writes               | CMDQ commands + SYNC                            |
| Fault reporting      | Per-CB IRQ                    | EVTQ + one IRQ                                  |

*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q5. What is a Stream ID and how is it assigned?
**A.** A 16-bit (SMMUv2) or larger (SMMUv3) tag identifying the originating DMA master.
PCIe: derived from the Requester ID (BDF). Platform devices: declared in DT as
`iommus = <&apps_smmu SID mask>`. USB DWC3 on SM8550 is e.g. `0x740`; PCIe gets a base
+ mask so each function maps to a different SID.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q6. What is a Context Bank?
**A.** SMMUv2 per-device (or per-domain) address space. Holds its own TTBR0, TCR, MAIR,
ASID-equivalent (CONTEXTIDR), and FSR/FAR for fault reporting. Configured as
S1-only (HLOS), S2-only (hypervisor passthrough) or nested S1+S2 (VM device passthrough)
via the CBAR.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q7. Walk through `dma_alloc_coherent` with SMMU enabled.
**A.** `dma_alloc_coherent` → `iommu_dma_alloc` → `alloc_pages` (gives PA) → `iova_alloc`
(reserves a free IOVA range in the device's iommu_domain) → `iommu_map(domain, iova, pa,
size, prot)` → `arm_smmu_map` → `io_pgtable_ops->map` writes the PTE. The caller gets the
IOVA as `dma_handle` and programs that into the device's DMA address register; the device
sees IOVA, the SMMU translates to PA on each transaction.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q8. What happens if a device DMAs to an unmapped IOVA?
**A.** SMMUv2: translation fault → CB raises IRQ → Linux `arm_smmu_context_fault()` reads
FSR/FAR/FSYNR0, logs `Unhandled context fault: iova=… sid=… fsr=…`, terminates the
transaction. SMMUv3 with STALL: SMMU stalls the device, posts a STALL event to EVTQ,
Linux can fix the mapping and send RESUME(RETRY) — effectively device demand-paging.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q9. What are the major SMMU fault types?

| Bit | Name                   | Cause                                                              |
|-----|------------------------|--------------------------------------------------------------------|
| TF  | Translation fault       | IOVA has no PTE — usually a buggy driver that forgot to map         |
| AFF | Access flag fault       | PTE found but AF=0 — page table walker reports                      |
| PF  | Permission fault        | PTE mapped read-only but device tried to write                      |
| EF  | External fault          | Page table walk hit a bus error                                     |
| SS  | Streamside              | Fault on the device side (vs page-table-walk side)                  |

*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q10. How do you debug an SMMU fault?
**A.** `dmesg | grep -iE 'smmu|iommu|context fault'` for the iova/sid/fsr triple. Map
`sid` back to the device in the SoC TRM. `ls /sys/kernel/iommu_groups/`, look at
`/sys/bus/platform/devices/.../iommu_group`. Boot with `arm-smmu.disable_bypass=1` and
`iommu.strict=0` for stricter checking. For SVA paths verify the CD/SSID setup with the
arm-smmu-v3 debugfs.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q11. What is `dma-coherent` in DT and how does it relate to SMMU?
**A.** Signals that the device's DMA is HW-coherent with CPU caches (via CCI/CCN/DSU);
the DMA framework skips cache maintenance on `dma_map/unmap`. **Orthogonal to SMMU** —
you can have SMMU translation with or without HW coherency.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q12. What is an IOMMU group?
**A.** Smallest set of devices that must share an iommu_domain because they can peer-to-
peer DMA. For PCIe this is determined by ACS capability — functions that can talk to each
other below the IOMMU must be grouped. Platform devices are usually their own group. For
VFIO passthrough the entire group must go to the VM.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q13. What is SVA (Shared Virtual Addressing)?
**A.** SMMUv3 feature where a device's CD points directly at a userspace process's CPU
page tables (same TTBR0 + ASID). The accelerator can dereference pointers the CPU
allocates — no separate IOVA, no `iommu_map` per buffer. Foundation of zero-copy
AI/ML accelerator integration. API: `iommu_sva_bind_device()` + PASID allocation.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q14. How does SMMU + TrustZone cooperate?
**A.** Same SMMU instance has secure and non-secure CBs. TZ (QSEE) programs the secure
CBs via SMC; Linux only sees the non-secure ones. For DRM/widevine content the camera
ISP / video decoder is bound to a secure CB so the frame data is unreachable from the
HLOS even via DMA. `qcom_scm_assign_mem` reassigns ownership of a memory region between
HLOS and secure heap.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q15. How does VFIO use the SMMU?
**A.** Unbinds the device from its host driver, binds to `vfio-pci` → creates a new
iommu_domain (isolated address space) → userspace (or QEMU) issues
`VFIO_IOMMU_MAP_DMA` ioctls to populate the domain → guest does DMA; SMMU enforces it
can only touch mapped regions. With nested translation, guest manages Stage-1, host
manages Stage-2 (GPA → HPA).
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q16. Compare SMMU vs CPU MMU briefly.
**A.** Same page table format (LPAE/AArch64) and same kernel `io-pgtable-arm.c`. CPU MMU
translates CPU VA→PA on instruction/data accesses, configured via TTBR0/TTBR1_EL1.
SMMU translates device IOVA→PA per Stream ID, configured per Context Bank. CPU faults
are synchronous exceptions; SMMU faults are asynchronous IRQs to a CPU.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

---

## 16. Behavioral / Scenario-Based Debugging Questions

### Q1. A device's interrupt is firing constantly; the system is unusable. What do you do?
**A.** Confirm with `cat /proc/interrupts` and `top` (high `%hi`). Identify the IRQ
number and the driver. Check whether the handler is properly clearing the source register
(level-triggered HW that isn't ack'ed → re-fires). Temporary mitigation:
`echo 0 > /proc/irq/N/smp_affinity_list` or `disable_irq(N)` from a debug shell. Permanent
fix in driver: ensure status register is cleared before EOI; switch threaded IRQ if work
is heavy; consider edge-triggered config.
*Deep dive:* [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md)

### Q2. A workload is slower after a kernel upgrade. How do you bisect?
**A.** `git bisect` the kernel tree between known-good and known-bad commits. Each
iteration: build, boot, run the benchmark, mark `good`/`bad`. Use a small reproducible
benchmark (`perf stat -r 5`). Once the offending commit is identified, look at the patch
diff for likely scheduling/MM/driver changes.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q3. Suspend works on the EVT board but resume hangs on PVT — debug.
**A.** Compare schematics (rail differences). Add `no_console_suspend` and `console_resume`
to keep the UART alive across suspend. Use `pm_test` modes (`echo devices > /sys/power/pm_test`)
to suspend at each phase incrementally and find which device's resume hangs. Check
`/sys/kernel/debug/wakeup_sources`. Often a regulator/clock parent that's still off
during a device's resume.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q4. Kernel panics with "Unable to handle kernel NULL pointer dereference at 0x10".
**A.** Decode the oops (PC, LR, call trace) with `decode_stacktrace.sh`. The address `0x10`
is a near-NULL offset — a struct field deref through a NULL pointer at offset 0x10. Find
the function in the trace, look at the source, identify which pointer wasn't validated.
Add a NULL check; check why the caller passed NULL (likely a probe-failure path).
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q5. PCIe link up but device probe fails on a new Qualcomm board.
**A.** Check link state in `lspci -vvv`, confirm Gen/width matches expected. Verify
`pcie-qcom` PHY init in dmesg; check QMP PHY device tree (`qcom,phy-init-sequence`).
Confirm `iommus = <&apps_smmu …>` is correct or absent (bypass mode). Check power rails
(perst, refclk) toggled correctly. JTAG into the BAR allocation if needed.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q6. App is missing camera frames intermittently — where do you look?
**A.** Check CSI errors (`/sys/kernel/debug/dynamic_debug/control` for camss),
`dmesg | grep -i csi`. Verify ISP throughput vs sensor framerate. Look for SMMU faults
that aborted a DMA. Profile with `perf sched` to ensure the V4L2 thread is getting CPU on
time (uclamp_min?). Inspect dropped frames counter in V4L2 stats.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q7. Modem subsystem keeps crashing — how do you triage?
**A.** Capture the SSR ramdump from `/dev/ramdump_modem` (or via DLOAD). Check
`/sys/kernel/debug/remoteproc/remoteproc0/state` and `recovery`. `dmesg | grep -i 'pil\|ssr\|mpss'`
for the reason. Use QPST / QCAP to decode the modem crash. Coordinate with modem team
with PIL/QMI logs.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q8. CPU shows high `iowait` during a benchmark — debug.
**A.** `iostat -x 1` to see per-device utilization. `bpftrace biolatency` for distribution.
`perf record -e block:*` to trace I/O issue/complete. Likely culprits: small random I/O
without an elevator, fsync-heavy workload, slow eMMC vs UFS, encryption overhead.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

### Q9. New SMMU context fault appears after a driver refactor — debug.
**A.** Reproduce, capture `iova/sid/fsr` from dmesg. Match `iova` to the buffer the driver
just programmed — most likely missed a `dma_map_*` or used a stale `dma_addr` after
`dma_unmap`. Enable `arm-smmu` debug, dump the page table for the CB. If TF: missing
mapping. If PF: write to read-only DMA-mapped buffer.
*Deep dive:* [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md)

### Q10. Device shows up in `/sys/bus/platform/devices` but `driver/` symlink is missing.
**A.** No driver matched. Check `compatible` string spelling. Confirm module is loaded
(`lsmod`) or built-in (`grep CONFIG_FOO .config`). `modprobe foo` manually. If still
nothing, add a `pr_info` at probe entry to see whether it's even being called.
*Deep dive:* [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md)

### Q11. Reduce boot time by 500 ms — strategy?
**A.** Measure first (`initcall_debug`, `systemd-analyze blame`). Top wins are usually:
async/deferred probing, LZ4 compression, push slow drivers to `=m`, set unused DT nodes
`status="disabled"`, slim initramfs, parallelise systemd services, defer non-critical
services with socket activation, ensure DDR-training cache is hot on warm boot.
*Deep dive:* [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md)

### Q12. Find a slow path inside the scheduler hot code without slowing the system.
**A.** Use eBPF (`bpftrace` with `kprobe:pick_next_task`) to get latency histograms
without instrumentation overhead. Or `perf sched record` followed by `perf sched
latency`. For function-graph timing of small windows, ftrace's `function_graph` tracer
with `set_graph_function` is precise.
*Deep dive:* [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md)

---

## 17. Quick-Fire One-Liners (50+ rapid-recall items)

1. **Set bit n:** `x |= (1<<n)`
2. **Clear bit n:** `x &= ~(1<<n)`
3. **Toggle bit n:** `x ^= (1<<n)`
4. **Test bit n:** `x & (1<<n)`
5. **Lowest set bit isolate:** `x & -x`
6. **Lowest set bit clear:** `x & (x-1)` — also Brian Kernighan popcount step
7. **Power of 2 test:** `x && !(x & (x-1))`
8. **Modulo 2^n:** `x & (n-1)` (n must be power of 2)
9. **Branchless abs:** `(x ^ (x>>31)) - (x>>31)`
10. **XOR swap:** `a^=b; b^=a; a^=b;` (guard `a==b`)
11. **Opposite signs:** `(x ^ y) < 0`
12. **GFP_KERNEL** sleeps, **GFP_ATOMIC** doesn't.
13. **vmalloc** = virtually contiguous, **kmalloc** = physically contiguous.
14. **spinlock** = no sleep, **mutex** = can sleep.
15. **Use spin_lock_irqsave** when ISR shares the lock.
16. **RCU readers** never block; writers do copy-update-grace-period.
17. **EL0=user, EL1=kernel, EL2=hypervisor, EL3=secure monitor.**
18. **TTBR0=user, TTBR1=kernel** on ARM64; bit 63 selects.
19. **ASID** tags user TLB entries to avoid flush on context switch.
20. **TLBI VAE1IS + DSB ISH + ISB** = ARM64 TLB shootdown (HW broadcast, no IPI).
21. **MAIR_EL1** = 8 memory-attribute slots (Normal-WB, Device-nGnRnE, …).
22. **ESR_EL1** = exception syndrome; **FAR_EL1** = faulting VA; **ELR_EL1** = return PC.
23. **vDSO** = syscall-less `gettimeofday/clock_gettime`.
24. **SVC** → EL1, **HVC** → EL2, **SMC** → EL3.
25. **PSCI** = standard SMC API for CPU on/off/suspend.
26. **GIC SGI 0-15, PPI 16-31, SPI 32-1019, LPI 8192+.**
27. **Threaded IRQ** = `IRQ_WAKE_THREAD` + can sleep, runs at SCHED_FIFO 50.
28. **Tasklet** is softirq context; **workqueue** is process context.
29. **kernel oops** ≠ panic; `panic_on_oops=1` makes it one.
30. **KASAN** catches use-after-free / OOB; **lockdep** catches deadlock cycles.
31. **CFS** picks the leftmost rb-tree node by **vruntime**.
32. **EEVDF** (6.6) replaces CFS — picks **eligible** task with earliest deadline.
33. **sched-ext** (6.12) = BPF-implemented scheduler; falls back to CFS on crash.
34. **EAS** = pick CPU that minimises energy delta from EM cost table.
35. **schedutil** = `next_freq = max_freq * util / max_capacity`.
36. **uclamp_min/max** = per-task/cgroup floor/ceiling for util_est.
37. **PSI** = wall-clock fraction stalled on CPU/memory/IO.
38. **OOM score:** `(rss + swap) * 1000 / total`, adjusted by `oom_score_adj`.
39. **CMA** = boot-time reserved contiguous region, reusable for movable pages.
40. **THP** = 2 MB PMD-level huge pages on ARM64 (with 4 K granule).
41. **Stream ID** = SMMU's identity for a DMA master (`iommus = <&smmu SID mask>`).
42. **dma_alloc_coherent** returns IOVA when SMMU is enabled, PA when bypassed.
43. **TF**, **PF**, **AFF** = the SMMU fault types (Translation, Permission, Access flag).
44. **SVA** = device shares CPU process page tables via PASID/SSID (SMMUv3).
45. **VHE (ARMv8.1)** = host Linux runs at EL2 → no world-switch on VM-exit.
46. **Stage-2** translation = IPA → PA, controlled by EL2 / VTTBR_EL2.
47. **VBAR_ELn** = base of exception vector table per EL on ARM64.
48. **`sysfs_emit`** is the safe replacement for `sprintf` in sysfs `show()`.
49. **DEVICE_ATTR_RW / .dev_groups** is the modern attribute pattern.
50. **debugfs** has **no ABI guarantee** — never script against it in production.
51. **kernfs** is the VFS backend that powers both sysfs and cgroup-fs.
52. **proc_ops** replaced `file_operations` for `/proc` entries in Linux 5.6.
53. **systemd slices** = system.slice / user.slice / machine.slice (cgroup v2).
54. **Namespaces** types (8): PID, NET, MNT, UTS, IPC, USER, CGROUP, TIME.
55. **Containers = namespaces + cgroups + OverlayFS** (+ seccomp/capabilities for sec).
56. **Qualcomm boot**: PBL → XBL → ABL → kernel → init.
57. **TLMM** = Qualcomm pin controller; per-pin **function/drive-strength/bias** in DT.
58. **GENI/QUPv3** = unified serial engine (I2C/SPI/UART/I3C).
59. **RPMH** = ARC-based resource manager; TCS ACTIVE/SLEEP/WAKE sets.
60. **SMEM** = shared DRAM region; **QMI** = TLV IPC over QRTR sockets.

---

## Cross-References

| Concept asked here | Best deep-dive doc |
|---|---|
| ARM64 page tables, TTBR0/1, MAIR, ASID | [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md) |
| Buddy / SLUB / vmalloc / OOM / CMA | [01_ARM_ARM64_Memory_Management.md](01_ARM_ARM64_Memory_Management.md) |
| CFS / EEVDF / sched-ext / EAS | [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md) |
| RCU / mutexes / spinlocks / barriers | [02_Scheduling_and_Synchronization.md](02_Scheduling_and_Synchronization.md) |
| GIC / IPI / threaded IRQ / watchdog | [03_Interrupts_IPI_and_Watchdog.md](03_Interrupts_IPI_and_Watchdog.md) |
| Drivers / Device Tree / sysfs / syscalls | [04_Linux_Drivers_DT_proc_sysfs_Syscalls.md](04_Linux_Drivers_DT_proc_sysfs_Syscalls.md) |
| Boot / ATF / PSCI / Hibernation | [05_Boot_Flow_UBoot_APPSBL_Hibernation.md](05_Boot_Flow_UBoot_APPSBL_Hibernation.md) |
| Oops / panic / kdump / KASAN / ftrace | [06_Crash_Dump_and_Kernel_Errors.md](06_Crash_Dump_and_Kernel_Errors.md) |
| Qualcomm platform / IOMMU / SMMU / TrustZone | [07_Qualcomm_Platform_IOMMU_SMMU.md](07_Qualcomm_Platform_IOMMU_SMMU.md) |
| C bitwise, register patterns, endianness | [08_C_Strings_Bitwise_Fundamentals.md](08_C_Strings_Bitwise_Fundamentals.md) |

---

## Further Reading

The Q&A in this master document was extracted and consolidated from the following raw
source files in `_raw_text/`:

1. `bitwise_interview_guide.md`
2. `proc_sysfs_interview_guide.md`
3. `Qualcomm_Interview_Guide_Part1.md`
4. `Qualcomm_Interview_Guide_Part2.md`
5. `Qualcomm_Interview_Part3_IOMMU_SMMU.md`
6. `qualcomm_linux_kernel_prep_part1.md`
7. `qualcomm_linux_kernel_prep_part2.md`
8. `qualcomm_linux_kernel_prep_part3.md`

— End of `09_Interview_QA_Master.md` —
