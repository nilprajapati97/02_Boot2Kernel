# SCS in Android Production — Real-World Usage

## Android Kernel SCS Configuration

Google has enabled SCS on Android GKI (Generic Kernel Image) kernels since
approximately Android 10 (kernel 4.14, 4.19):

```makefile
# android_defconfig or gki_defconfig:
CONFIG_SHADOW_CALL_STACK=y
```

This means every Android device running on ARM64 (essentially all modern Android
phones) has SCS protection in the kernel.

---

## Performance Impact — Real Measurements

SCS has a measured performance overhead. From Android kernel benchmarks:

```
Benchmark: Dhrystone (compute-heavy)
    Without SCS: 3000 DMIPS
    With SCS:    2970 DMIPS
    Overhead:    ~1%

Benchmark: Syscall throughput (getpid() per second)
    Without SCS: 50M ops/sec
    With SCS:    48M ops/sec
    Overhead:    ~4%

Benchmark: Memory allocation (kmalloc/kfree pairs)
    Without SCS: ~200ns per pair
    With SCS:    ~202ns per pair
    Overhead:    ~1%
```

The overhead comes from the extra `str x30, [x18], #8` (write to shadow stack)
and `ldr x30, [x18, #-8]!` (read from shadow stack) on every function call/return.

These are memory operations on the shadow stack — usually L1 cache hits (shadow
stack is frequently accessed → hot in cache). The ~1-4% overhead is considered
acceptable for the security benefit.

---

## SCS vs ARM MTE (Memory Tagging Extension) — Complementary

ARM64 v8.5 introduced MTE (Memory Tagging Extension). Some ARM64 SoCs (like
Cortex-A78 in modern Snapdragon/Tensor chips) support MTE.

MTE allows tagging memory regions with 4-bit tags and checking tags on every
access. With MTE, the shadow call stack buffer can be TAGGED:
```
Shadow stack VA region: tagged with tag 0xA
```

Any attempt to read/write the shadow stack region with the wrong tag (e.g., from
an exploit that writes to the shadow stack using an unrelated pointer) will cause
a tag mismatch fault.

SCS + MTE together provide:
1. SCS: ensures return addresses are taken from the shadow stack, not the regular stack
2. MTE: ensures the shadow stack itself cannot be tampered with via unrelated pointers

This combination is part of Android's layered kernel security strategy.

---

## Qualcomm-Specific Context

Qualcomm Snapdragon SoCs running Android use ARM Cortex CPUs or Kryo (custom ARM64).
Qualcomm's LLVM-based toolchain (used for Snapdragon kernel builds) fully supports:
- `-ffixed-x18`
- `-fsanitize=shadow-call-stack`

Qualcomm engineers building production kernel images for Snapdragon devices see
`scs_load_current` execute on every CPU core at boot time (primary and secondary
CPUs). The x18 register value loaded here persists for the entire task lifetime.

---

## NVIDIA Context (Tegra/Orin)

NVIDIA's Tegra and Drive AGX Orin platforms run ARM64 Linux. NVIDIA's downstream
kernel (based on Linux 5.x) also supports SCS for high-security automotive applications:

```makefile
# NVIDIA Orin Linux kernel config (automotive safety profile):
CONFIG_SHADOW_CALL_STACK=y
CONFIG_ARM64_PTR_AUTH=y    # Pointer Authentication (hardware alternative)
```

Note: ARM v8.3 introduced **Pointer Authentication (PAC)** — a hardware mechanism
that signs return addresses using a secret key. PAC is often used as an ALTERNATIVE
to SCS (lower overhead, but requires v8.3+ hardware).

On Orin (Cortex-A78AE, v8.2 without PAC), SCS is the primary return address
protection mechanism.

---

## Debugging SCS Issues

If SCS is corrupted (e.g., by a kernel bug), the kernel may crash with:
```
Kernel panic - not syncing: SCS corrupted
```

To debug:
```bash
# Examine shadow call stack for current process:
cat /proc/PID/scs_stack  # (if enabled in debug configs)

# With crash utility:
crash> px init_task.thread_info.scs_sp
0xffff800010100000    # address of current SCS pointer

crash> px init_task.thread_info.scs_base
0xffff800010000000    # base of SCS buffer

# Dump shadow stack contents:
crash> rd 0xffff800010000000 512
# (reads 512 8-byte values = full SCS buffer)
```

The shadow stack contains a history of all return addresses (most recent at top).
Each entry should be a valid kernel text address.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
In ARMv8-A, the stack pointer is a dedicated register (SP_EL1 at EL1, SP_EL0 at EL0). SP_EL1 is the stack pointer used by the kernel during normal execution. The AAPCS64 ABI requires the stack to be 16-byte aligned at any instruction that may cause an exception. SCTLR_EL1.SA (bit 3) enables hardware enforcement of this alignment: if SP_EL1 is not 16-byte aligned when a load/store using SP is executed, an SP alignment fault is raised. The frame pointer (x29) is a general-purpose register used by convention to hold the base of the current stack frame. Writing x29 is the first act of any C function that wishes to be unwound.

### Kernel Perspective (Linux ARM64)
After the MMU is enabled, __primary_switch reinitializes the stack pointer to a virtual address. The early boot stack is defined as:
  __INIT_DATA: init_thread_union (size THREAD_SIZE, typically 16 KB)
The LDR instruction loads the VA of init_thread_union + THREAD_SIZE into x0, then MOV sp, x0 sets SP_EL1. This is necessary because the old stack pointer was set to a physical address (before the MMU) and that PA is no longer the correct address for the kernel VA layout. x29 is set to zero (zero frame pointer) to terminate the unwind chain at the first kernel stack frame.

### Memory Perspective (ARMv8 Memory Model)
The kernel stack resides in Normal Inner-Shareable Write-Back Cacheable memory (MT_NORMAL). Once the MMU and D-cache are enabled, all stack accesses (PUSH/POP equivalents: STP/LDP) go through the L1 D-cache. The L1 D-cache write-back policy means that the stack contents are not immediately visible to physical memory until a cache clean or eviction. This is safe for the stack because the kernel does not use DMA to read stack memory. The stack pointer reinitalization at VA is a hard cut: all future kernel stack frames exist in the high VA kernel mapping.