# early_mm_init() — System Design Approach and Q&A

## 1. Why early_mm_init() Exists

`early_mm_init()` solves two problems that must be resolved **before** `paging_init()` builds the final page tables:

### Problem 1: Hardware Heterogeneity in Cache Policy Encoding

ARM32 processors across generations encode cache attributes differently. A kernel compiled for ARMv7 uses different page-table bits than one for ARMv5 — but the Linux kernel supports a **single binary** running across generations. The solution is to defer the translation of abstract memory types (MT_DEVICE, MT_MEMORY_RW) into hardware bits until runtime, after the CPU is identified.

Without `build_mem_type_table()`:
- `paging_init()` would hardcode TEX+C+B bits for one specific CPU
- The same kernel binary would be incorrect on different ARM generations
- A kernel compiled for ARMv7 would set wrong cache attributes on ARMv5, causing cache coherency failures or hard faults

### Problem 2: LPAE Physical Address Space Mismatch

ARM32 LPAE (Large Physical Address Extension) enables addressing of up to 40-bit physical addresses. But the kernel is compiled with a fixed `PHYS_OFFSET` constant embedded in hundreds of `__pa()` and `__va()` macro expansions. When the actual physical RAM is at a different address than assumed at compile time (Keystone2: physical RAM at 32GB), all these embedded constants are wrong.

`early_paging_init()` patches the constants at boot before any of the wrong PA/VA conversions are used.

---

## 2. Design Principles

### Principle 1: Separate Policy from Mechanism

`build_mem_type_table()` separates **what** we want (a certain cache policy for device memory) from **how** the hardware encodes it (TEX+C+B bits). The `MT_*` type enum is the policy interface. The `mem_types[]` table is the mechanism.

This is the classic Strategy Pattern — different CPUs have different encoding strategies, but all callers use the same `MT_*` interface.

### Principle 2: Runtime Detection Over Compile-Time Configuration

Rather than `#ifdef CONFIG_ARM_V7` / `#ifdef CONFIG_ARM_V5` paths in `create_mapping()`, the table is built once at boot using runtime CPU detection. This means:
- One binary can boot on ARMv5, ARMv6, ARMv7
- The table overhead (one-time cost) is negligible
- `create_mapping()` is a simple table lookup — fast and maintainable

### Principle 3: Fail Fast with LPAE PV Fixup

If the physical address fixup is needed and LPAE is not enabled, `early_paging_init()` calls `add_taint(TAINT_CPU_OUT_OF_SPEC)` and prints a critical message:

```
CPU: Physical address space modification is only to support Keystone2.
Please enable ARM_LPAE and ARM_PATCH_PHYS_VIRT support.
Your kernel may crash now, have a good day.
```

This is a deliberate, explicit failure rather than a silent wrong behavior. Better to tell the user what's wrong immediately.

---

## 3. Dependency Graph

```
                    CPU identification (setup_processor)
                            │
                            ▼
               build_mem_type_table()
                    │               │
         mem_types[MT_MEMORY_RW]   mem_types[MT_DEVICE]
                    │               │
                    └───────┬───────┘
                            ▼
                    paging_init() → create_mapping()
                            │
                    ┌───────┴────────┐
                    │                │
              map_lowmem()    devicemaps_init()
         (uses MT_MEMORY_RW)  (uses MT_DEVICE)
```

```
            mdesc->pv_fixup (board-specific Keystone2)
                        │
                        ▼
            early_paging_init()
                        │
              ┌─────────┴──────────┐
              │                    │
       __pv_offset              kernel_sec_start/end
       __pv_phys_pfn_offset     (corrected)
              │                    │
              ▼                    ▼
         All __pa()/__va()     paging_init()
         now correct           maps correct sections
```

---

## 4. Why early_mm_init() Is Before setup_dma_zone() and xen_early_init()

```
early_mm_init(mdesc)     ← HERE
setup_dma_zone(mdesc)    ← sets DMA boundary
xen_early_init()         ← Xen hypervisor detection
arm_efi_init()           ← EFI memory map
adjust_lowmem_bounds()   ← computes lowmem limit
arm_memblock_init()      ← finalizes memory regions
paging_init()            ← creates page tables (uses mem_types[])
```

`early_mm_init()` must complete before `paging_init()` which is the critical dependency. It is placed as early as possible after `parse_early_param()` because:

1. The PV fixup (`early_paging_init`) must happen before any `__pa()` or `__va()` conversion is trusted, and `setup_dma_zone()` uses `PHYS_OFFSET` in its calculation.
2. `build_mem_type_table()` only reads CPU state — it has no dependencies on memblock or DMA zones.

Placing it immediately after `parse_early_param()` is the safest ordering.

---

## 5. The ARM32 ARM_PV_FIXUP Feature

### What CONFIG_ARM_PATCH_PHYS_VIRT Does

This config option enables the infrastructure for `early_paging_init()`:

1. **In the assembler** (`arch/arm/include/asm/assembler.h`): All `__pa()` / `__va()` usages emit an `ALU` instruction with a special encoding and add the instruction address to `__pv_table_begin...__pv_table_end` section.

2. **`fixup_pv_table()`** iterates this table and patches the immediate value in each ALU instruction to reflect the correct `PHYS_OFFSET`.

3. **Result**: The kernel binary effectively has all physical-to-virtual constants replaced at boot, without recompilation.

This is analogous to ELF relocations but done entirely in kernel boot code.

---

## 6. Alternative Designs Considered

### Alternative A: Separate kernel binaries per ARM generation

Each ARMv5/ARMv6/ARMv7 gets its own kernel. Rejected:
- Embedded product ecosystem requires one binary that works on multiple SoCs
- Distros (Debian ARM, Ubuntu ARM) cannot ship one kernel per SoC generation

### Alternative B: Virtual machine abstraction layer

Abstract cache policy through a full hypervisor layer. Rejected:
- Massive performance overhead for cache attribute writes
- ARM hasn't mandated a hypervisor for bare-metal Linux

### Alternative C: Compile-time specialization via Kconfig

`CONFIG_CPU_ARMv5`, `CONFIG_CPU_ARMv7` etc., pick the right encoding at compile time. Partially done (LPAE is a Kconfig option). But CPU-specific tuning within a generation (Cortex-A9 vs Cortex-A8 cache quirks) still requires runtime detection.

### Alternative D: Full runtime discovery for PHYS_OFFSET (as ARM64 does)

Store `PHYS_OFFSET` as a runtime variable from the start. ARM64 does this. ARM32 didn't adopt it early enough — too much code was written assuming `PHYS_OFFSET` is a compile-time constant that can be used in instruction immediates. The PV fixup is the migration path.

---

## 7. Security Considerations

### Self-Modifying Code Risk (PV Fixup)

`fixup_pv_table()` modifies the kernel's own code (instruction immediates). This is a form of **self-modifying code** which has security implications:
- The code pages must be writable during fixup
- After fixup, they should be marked read-only
- The kernel's W^X (Write XOR Execute) invariant is temporarily violated

Mitigation: `early_paging_init()` runs before `paging_init()` establishes W^X protection. The final `paging_init()` sets the kernel code section to read-only + executable, and data sections to read-write + non-executable, restoring the invariant.

### Domain Permissions (build_mem_type_table)

`build_mem_type_table()` sets domain assignments. If domains are misconfigured (e.g., device memory in DOMAIN_KERNEL with Manager access), DMA engines or MMIO registers could be read/written without access checks. The table values are hardcoded constants — not attacker-controllable — but a kernel bug here could be a privilege escalation path.

---

## 8. System Design Q&A

**Q: What would break if you moved early_mm_init() to just before paging_init()?**
> The PV fixup part (`early_paging_init`) would break. Between the current position and `paging_init()`, code like `setup_dma_zone()` calls `PHYS_OFFSET` arithmetic. If `PHYS_OFFSET` hasn't been corrected yet (Keystone2 case), `arm_dma_limit` would be computed against the wrong physical base. More subtly, `adjust_lowmem_bounds()` uses `phys_addr_t` comparisons that rely on correct physical addresses.

**Q: How does the kernel ensure mem_types[] is not used before build_mem_type_table() initializes it?**
> The only users of `mem_types[]` are `create_mapping()` and related mapping functions. All these are called from `paging_init()` which is called after `early_mm_init()`. There is no `WARN_ON` or locking — the ordering is enforced by the call sequence in `setup_arch()`. This is a design-by-construction constraint, not runtime enforcement.

**Q: What is the relationship between ARM domains and Linux's own memory protection?**
> ARM hardware domains (0-15, controlled by DACR) are a coarse-grained hardware protection layer. Linux uses them for quick switching between user-space access (DOMAIN_USER = Client) and no-access (DOMAIN_USER = No Access) during system calls. This avoids a full TLB flush. Linux's fine-grained page protection (read-only, execute-never, etc.) uses page table permission bits (AP, XN) independently of domains. ARM64 dropped domains entirely in favor of relying solely on page table permissions.

**Q: What is the NOCACHE/WRITETHRU/WRITEBACK policy selection logic?**
> `build_mem_type_table()` reads `cachepolicy` from `arch/arm/mm/cache.c` — it tests if the CPU supports write-allocate and sets the default cache policy. `MT_MEMORY_RW` defaults to `CPOLICY_WRITEBACK` (write-back with write-allocate). The user can override with `mem_types=writethru` or similar parameters for debugging cache coherency issues.

**Q: On a production ARM Cortex-A53 system (which is ARMv8 in 32-bit mode), does early_mm_init() still run?**
> Yes, if booting a 32-bit kernel on Cortex-A53. The ARMv8 architecture supports a 32-bit AArch32 execution state. A 32-bit Linux kernel (`CONFIG_ARM=y`) with `CONFIG_MMU` will call `early_mm_init()`. `build_mem_type_table()` will detect ARMv7-compatible cache behavior (Cortex-A53 presents as ARMv7 to 32-bit OS) and set LPAE-style descriptors if `CONFIG_ARM_LPAE=y`. The PV fixup is needed only if the physical RAM is actually above 4GB.

**Q: Could the Linux kernel be redesigned to not need build_mem_type_table()?**
> Yes — ARM64 proved this. The solution is to mandate a hardware memory attribute register (MAIR_EL1 in ARMv8) that the OS configures once, and use a fixed 3-bit index in page table descriptors. This makes the page table format independent of cache policy encoding. ARM32 cannot adopt this without breaking backward compatibility with existing ARMv5/v6 silicon.
