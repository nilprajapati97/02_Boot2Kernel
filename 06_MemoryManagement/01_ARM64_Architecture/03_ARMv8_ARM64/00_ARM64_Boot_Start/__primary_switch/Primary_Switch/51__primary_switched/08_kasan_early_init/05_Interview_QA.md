# KASAN Early Init — Interview Q&A

---

## Q1: What is KASAN and what is its purpose in the Linux kernel?

**A:** KASAN (Kernel Address SANitizer) is a dynamic memory error detection tool
for the Linux kernel. It uses compiler instrumentation to insert checks before
every memory access and a "shadow memory" region (1 byte per 8 real bytes) to
track whether each memory byte is valid.

KASAN detects:
- Use-after-free (accessing freed memory)
- Heap/stack/global buffer overflows
- Use-after-scope (accessing local variable beyond its scope)

It is a debugging tool — not enabled in production due to ~2× memory overhead
and 15–50% performance overhead (generic mode). ARM MTE-based KASAN (`CONFIG_KASAN_HW_TAGS`)
approaches production-usable with <5% overhead on ARMv8.5+ hardware.

---

## Q2: Why is `kasan_early_init` called in `__primary_switched` rather than in `start_kernel`?

**A:** Because `start_kernel` itself (and all C code it calls) is compiled with
KASAN instrumentation. When KASAN-instrumented code accesses any memory, the
compiler emits instructions that read the corresponding shadow memory byte.
If shadow memory isn't mapped, this causes a page fault.

`__primary_switched` calls `bl start_kernel` as its last real operation. By
the time this `bl` executes, KASAN shadow must already be mapped for the kernel
image region. Therefore, `kasan_early_init` must run in `__primary_switched`,
BEFORE `start_kernel` is jumped to.

---

## Q3: What is `kasan_zero_page` and why is it used during early boot?

**A:** `kasan_zero_page` is a single read-only physical page containing all zeros.
During `kasan_early_init`, ALL shadow memory is mapped to point to this one page
(via PMD/PUD entries pointing to a shared zero page rather than unique pages).

A shadow byte of 0x00 means "all 8 bytes are accessible" — so a zero shadow byte
is interpreted as "no error." Mapping all shadow to zero bytes means KASAN makes
no false positives during early boot, while still being able to access shadow
memory without page faults.

After `kasan_init()` (much later, during `mm_init`), real per-page shadow
memory is allocated and initialized properly.

---

## Q4: Explain the `__no_sanitize_address` attribute and where it's critical.

**A:** `__no_sanitize_address` is a GCC/Clang attribute that disables KASAN
instrumentation for a specific function. The compiler will NOT emit shadow
memory check code for functions marked this way.

Critical uses:
1. `kasan_early_init` itself — must run before shadow is mapped
2. `kasan_mem_to_shadow` — the shadow address calculator (infinite recursion risk)
3. `kasan_report` — the error reporter (would recurse into itself)
4. `__asan_load*`/`__asan_store*` — KASAN runtime functions
5. Any early boot code that runs before `kasan_early_init`

Without `__no_sanitize_address` on these functions, KASAN would instrument
itself, causing infinite recursion or page faults during early boot.

---

## Q5: What is the shadow memory ratio and how is it calculated?

**A:** The default shadow ratio is 1:8 — 1 shadow byte covers 8 real bytes.
This is chosen because:
1. Memory allocations are typically 8-byte aligned on 64-bit systems
2. A single shadow byte can encode all states for an 8-byte aligned block:
   - 0: all 8 bytes valid
   - 1-7: first N bytes valid (partial allocation)
   - negative (0x80-0xFF): entire block invalid (with type encoding)

For a 16 GB system: shadow = 16 GB / 8 = 2 GB.

Software tag-based KASAN uses 1:16 ratio (tags embedded in pointer top byte,
16-byte aligned allocations) → 1 GB shadow for 16 GB RAM.

Hardware MTE uses 4-bit tags per 16-byte granule stored in dedicated tag storage
in DRAM — no shadow memory in virtual address space at all.

---

## Q6: How does KASAN detect use-after-free?

**A:** When `kfree(ptr)` is called:
1. KASAN "poisons" the freed region's shadow bytes with `KASAN_FREE_PAGE` (0xFF)
2. The freed memory is NOT immediately returned to the allocator (quarantine)
3. Later accesses to the freed memory trigger shadow byte check: 0xFF ≠ 0 → report

For hardware tag KASAN:
1. `kfree(ptr)` stores a "poisoned" tag (e.g., 0xFE) in the 4-bit tag storage
2. The old pointer (with original tag e.g., 0x05) still exists in code
3. Next access through the old pointer: hardware checks 0x05 vs 0xFE → mismatch → fault

The report includes:
- The invalid address
- The size of access
- Stack trace of the bad access
- Stack trace of the original allocation
- Stack trace of the free operation

This information is invaluable for debugging kernel memory corruption bugs.

---

## Q7: Can KASAN be used on production ARM64 systems? Under what conditions?

**A:** Generic KASAN: No. 2× memory overhead and 15–50% slowdown are unacceptable
for production workloads.

Software tag KASAN: Rarely. ~1× memory overhead and 10–20% slowdown make it
feasible for some security-sensitive applications that prioritize correctness
over performance.

Hardware tag KASAN (MTE): Yes, potentially. On ARMv8.5-A+ hardware:
- < 5% performance overhead (hardware tag checks are nearly free)
- Zero shadow memory overhead (tags in dedicated DRAM tag storage)
- Android 12+ enables MTE on Pixel phones with Tensor chips for heap bug detection
- NVIDIA is evaluating MTE KASAN for automotive safety-critical Orin applications

The key requirements: ARMv8.5-A CPU, kernel with `CONFIG_KASAN_HW_TAGS`, and
an allocator (SLUB) that uses MTE-tagged allocations.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
KASAN (Kernel Address SANitizer) can use two modes on ARM64:
- Software KASAN (CONFIG_KASAN_GENERIC, CONFIG_KASAN_SW_TAGS): every memory access is instrumented by the compiler to check a shadow byte before the access. The shadow memory occupies 1/8 of the kernel address space (one shadow byte per 8 bytes of real memory).
- Hardware KASAN (CONFIG_KASAN_HW_TAGS, ARMv8.5 MTE): uses ARM Memory Tagging Extension. Every 16-byte-aligned allocation gets a 4-bit tag in the address (VA bits 59:56, using the TBI/Top Byte Ignore feature) and a matching tag stored in a special memory tag granule. The CPU checks tags on every load/store automatically.

### Kernel Perspective (Linux ARM64)
KASAN shadow memory is mapped during early boot in kasan_early_init() (arch/arm64/mm/kasan_init.c). For software KASAN, the shadow region at KASAN_SHADOW_START is mapped before start_kernel. For hardware KASAN (MTE), the kernel sets SCR_EL3.ATA and SCTLR_EL1.ATA to enable tag checking. KASAN errors generate a kernel panic (or WARN, depending on config) when an out-of-bounds or use-after-free access is detected. This is critical for kernel security.

### Memory Perspective (ARMv8 Memory Model)
MTE (Memory Tagging Extension, ARMv8.5) adds a 4-bit tag to each 16-byte granule of memory. The tag is stored in a separate physical memory region (tag memory) alongside the data memory. On each load/store, the CPU compares the tag in the VA (bits 59:56) with the tag in tag memory: a mismatch raises a tag check fault. This is transparent to the page tables (tags are handled by the memory controller, not the MMU). The MAIR_EL1 must set the TAGGED attribute for memory regions where MTE is active.