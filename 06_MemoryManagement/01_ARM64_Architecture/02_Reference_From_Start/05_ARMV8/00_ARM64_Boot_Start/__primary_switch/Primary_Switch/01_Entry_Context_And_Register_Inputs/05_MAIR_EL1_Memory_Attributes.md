# `MAIR_EL1` — Memory Attribute Indirection Register: Complete Reference

**Written by:** `__cpu_setup` in `arch/arm64/mm/proc.S`  
**Used by:** Every page table entry created during and after `__pi_early_map_kernel`  
**Kernel source:** `arch/arm64/include/asm/sysreg.h`  

---

## 0. The Problem MAIR Solves

In AArch64, **every page table entry** (at any level that creates a mapping)
contains a 3-bit `AttrIdx` field. This index into `MAIR_EL1` selects one of
8 possible memory attributes for that mapping. The CPU uses these attributes
to decide:

- Whether accesses to this region are cached or uncached
- Whether out-of-order memory access reordering is allowed
- Whether speculative prefetching is allowed
- How the region participates in cache coherency protocols
- Whether device-memory semantics apply (strict ordering, no speculation)

`MAIR_EL1` is the **lookup table** that maps those 3-bit indices to actual
hardware behavior. Without it programmed correctly before MMU-on, every
memory access through the MMU would use garbage attributes — instant undefined
behavior.

---

## 1. MAIR_EL1 Structure

`MAIR_EL1` is a 64-bit register encoding 8 × 8-bit attribute definitions:

```
MAIR_EL1[63:0]:

Bits [63:56]  = Attr7
Bits [55:48]  = Attr6
Bits [47:40]  = Attr5
Bits [39:32]  = Attr4
Bits [31:24]  = Attr3
Bits [23:16]  = Attr2
Bits [15:8]   = Attr1
Bits [7:0]    = Attr0
```

Each 8-bit attribute encodes:
- **Bits [7:4]**: Outer cache attributes (L2 / memory system level)
- **Bits [3:0]**: Inner cache attributes (L1 / within the CPU cluster)

For Device memory, bits [7:4] = `0b0000` and bits [3:2] encode the device type.

---

## 2. Linux ARM64 MAIR_EL1 Attribute Index Assignments

```c
// arch/arm64/include/asm/memory.h — MT_* index definitions
#define MT_DEVICE_nGnRnE    0  // Index 0 → Attr0
#define MT_DEVICE_nGnRE     1  // Index 1 → Attr1
#define MT_DEVICE_GRE       2  // Index 2 → Attr2
#define MT_NORMAL_NC        3  // Index 3 → Attr3
#define MT_NORMAL           4  // Index 4 → Attr4
#define MT_NORMAL_TAGGED    5  // Index 5 → Attr5

// arch/arm64/include/asm/sysreg.h — Encoding values
#define MAIR_ATTR_DEVICE_nGnRnE    UL(0x00)   // 0b_0000_0000
#define MAIR_ATTR_DEVICE_nGnRE     UL(0x04)   // 0b_0000_0100
#define MAIR_ATTR_NORMAL_NC        UL(0x44)   // 0b_0100_0100
#define MAIR_ATTR_NORMAL_TAGGED    UL(0xf0)   // 0b_1111_0000
#define MAIR_ATTR_NORMAL           UL(0xff)   // 0b_1111_1111
```

The full `MAIR_EL1` value assembled:

```c
MAIR_EL1_SET =
    MAIR_ATTRIDX(MAIR_ATTR_DEVICE_nGnRnE, MT_DEVICE_nGnRnE)  // Index 0: 0x00
  | MAIR_ATTRIDX(MAIR_ATTR_DEVICE_nGnRE,  MT_DEVICE_nGnRE)   // Index 1: 0x04
  | MAIR_ATTRIDX(0x0c,                     MT_DEVICE_GRE)     // Index 2: 0x0c
  | MAIR_ATTRIDX(MAIR_ATTR_NORMAL_NC,      MT_NORMAL_NC)      // Index 3: 0x44
  | MAIR_ATTRIDX(MAIR_ATTR_NORMAL,         MT_NORMAL)         // Index 4: 0xff
  | MAIR_ATTRIDX(MAIR_ATTR_NORMAL_TAGGED,  MT_NORMAL_TAGGED)  // Index 5: 0xf0

// Binary layout in MAIR_EL1:
// [63:48] = 0x0000 (Attr7, Attr6 unused)
// [47:40] = 0xf0   (Attr5 = NORMAL_TAGGED)
// [39:32] = 0xff   (Attr4 = NORMAL)
// [31:24] = 0x44   (Attr3 = NORMAL_NC)
// [23:16] = 0x0c   (Attr2 = DEVICE_GRE)
// [15:8]  = 0x04   (Attr1 = DEVICE_nGnRE)
// [7:0]   = 0x00   (Attr0 = DEVICE_nGnRnE)
```

---

## 3. Attribute Deep-Dive

### 3.1 `MT_DEVICE_nGnRnE` (Index 0) — Encoding `0x00`

**Device memory: non-Gathering, non-Reordering, no Early Write Acknowledgement**

This is the most restrictive memory type. Used for hardware registers that
have **side effects** on every access.

| Property | Meaning |
|---|---|
| **non-Gathering (nG)** | Each load/store is a separate transaction; adjacent accesses cannot be merged |
| **non-Reordering (nR)** | Accesses to this region cannot be reordered with respect to each other |
| **no Early Write Ack (nE)** | Write completes only when acknowledged by the endpoint, not when it reaches the bus |

**When used:**  
MMIO registers where:
- Order of writes matters absolutely (e.g., DMA descriptor chains)
- You need to know a write reached the device before continuing
- Reading a register has a side effect (e.g., clearing an interrupt)

**Example:** GIC (interrupt controller) registers, PCIe config space.

**CPU behavior:**
- No speculative loads
- No prefetching
- No out-of-order execution with other accesses
- Effectively a full memory barrier between each access

**Performance implication:** Very slow. Every access goes through the full
memory system pipeline with ordering guarantees.

---

### 3.2 `MT_DEVICE_nGnRE` (Index 1) — Encoding `0x04`

**Device memory: non-Gathering, non-Reordering, Early Write Acknowledgement**

Same as `nGnRnE` but writes are acknowledged early by the interconnect or
write buffer, not necessarily by the device. Allows the CPU pipeline to
proceed without waiting for the peripheral to ACK.

**When used:** Normal device MMIO where strict write completion ordering with
the device is not required, but access ordering must still be preserved.
Most device drivers default to `nGnRE` via `ioremap()`.

---

### 3.3 `MT_DEVICE_GRE` (Index 2) — Encoding `0x0c`

**Device memory: Gathering, Reordering, Early Write Acknowledgement**

The most relaxed device type. Multiple transactions to the same device may be
merged (gathered) into one, and accesses may be reordered.

**When used:** Bulk data transfers to devices where ordering is explicitly
managed by software (e.g., PCIe BAR for streaming writes with explicit flushes).
Rarely used directly; `ioremap_wc()` (write-combining) maps to this.

---

### 3.4 `MT_NORMAL_NC` (Index 3) — Encoding `0x44`

**Normal memory: Non-Cacheable, Inner and Outer**

The encoding `0x44`:
```
0x44 = 0b_0100_0100
         ^^^^  ^^^^
         Outer NC  Inner NC

Outer field [7:4] = 0b0100 = Outer Non-Cacheable
Inner field [3:0] = 0b0100 = Inner Non-Cacheable
```

Memory accesses go directly to the memory system, bypassing caches. But unlike
device memory:
- Accesses CAN be reordered
- Speculative prefetching IS allowed
- Gathering IS allowed

**When used:** Coherent DMA buffers that must be visible to I/O masters
(DMA engines) without explicit cache maintenance. When a device needs to read
data written by the CPU, using NC memory means the CPU doesn't need to clean
D-cache before the DMA operation — data is always in memory.

---

### 3.5 `MT_NORMAL` (Index 4) — Encoding `0xff` ★ The Critical One

**Normal memory: Write-Back Cacheable, Inner and Outer**

The encoding `0xff`:
```
0xff = 0b_1111_1111
         ^^^^  ^^^^
         Outer WB-RW-Alloc  Inner WB-RW-Alloc

Outer field [7:4] = 0b1111 = Outer Write-Back, Read-Allocate, Write-Allocate
Inner field [3:0] = 0b1111 = Inner Write-Back, Read-Allocate, Write-Allocate
```

**Read-Allocate:** On a cache miss, allocate a cache line and fill it from memory.  
**Write-Allocate:** On a write miss, allocate a cache line, fill it, then write.  
**Write-Back:** Writes to cached lines do not immediately propagate to memory;
dirty cache lines are written back to memory only when evicted.

This is the attribute for **all kernel code and data** — `.text`, `.data`,
`.bss`, kernel stacks, page tables (when accessed by software), etc.

**Why this is critical to `__primary_switch`:**  
When `__pi_early_map_kernel` builds the kernel page tables, all normal kernel
sections use `MT_NORMAL` (`AttrIdx = 4`). When the MMU is first enabled, these
mappings are already marked write-back cacheable. From the very first instruction
in `__primary_switched`, the CPU is running with the full benefit of L1/L2 caches.

---

### 3.6 `MT_NORMAL_TAGGED` (Index 5) — Encoding `0xf0`

**Normal memory: Write-Back Cacheable + MTE Tagging Enabled**

The encoding `0xf0`:
```
0xf0 = 0b_1111_0000
         ^^^^  ^^^^
         Outer WB-RW-Alloc  Inner Tag-Cache (FEAT_MTE)

Inner field [3:0] = 0b0000 (when outer = 1111) = Write-Back, Tagged
```

Used for memory regions where ARM Memory Tagging Extension (MTE / FEAT_MTE)
is active. MTE associates a 4-bit tag with every 16 bytes of memory and
a 4-bit tag with every pointer. On each access, hardware checks if the pointer
tag matches the memory tag; mismatch → fault.

`KASAN_HW` mode in Linux uses `MT_NORMAL_TAGGED` for kernel heap allocations.

---

## 4. How Page Table Entries Reference MAIR_EL1

In a page table descriptor (any level), the `AttrIdx` field selects the
MAIR attribute:

```
PTE descriptor bits [4:2] = AttrIdx[2:0]

AttrIdx = 0 → MAIR_ATTR_DEVICE_nGnRnE (0x00)  → nGnRnE device
AttrIdx = 1 → MAIR_ATTR_DEVICE_nGnRE  (0x04)  → nGnRE device
AttrIdx = 2 → 0x0c                             → GRE device
AttrIdx = 3 → MAIR_ATTR_NORMAL_NC     (0x44)  → Normal NC
AttrIdx = 4 → MAIR_ATTR_NORMAL        (0xff)  → Normal WB ← kernel code/data
AttrIdx = 5 → MAIR_ATTR_NORMAL_TAGGED (0xf0)  → MTE memory
```

When the page table walker fetches a PTE and reads `AttrIdx = 4`, it looks
up `MAIR_EL1[39:32]` (Attr4 = `0xff`) to determine the access attributes.

---

## 5. Shareability and its Interaction with MAIR

MAIR defines whether memory is cacheable. `TCR_EL1.SH*` fields define the
**shareability domain**:

| Shareability  | Meaning                           | Cache coherency scope                   |
|---------------|-----------------------------------|-----------------------------------------|
| Non-Shareable | Local to one CPU                  | No coherency required with other agents |
| Inner Shareable | Within the Inner Share domain   | All CPUs on the same cluster/SoC        |
| Outer Shareable | Including I/O masters           | Across the whole system                 |

For kernel code and data mapped with `MT_NORMAL`, the `SH` field in the PTE
(or inherited from `TCR_EL1`) is set to Inner Shareable. This means cache
coherency is maintained by hardware across all cores — no explicit `dc civac`
is needed between CPU0 writing data and CPU1 reading it.

For device memory, shareability is meaningless (device memory ignores caching).

---

## 6. MAIR and the Boot Sequence — Timeline

```
primary_entry
    │
    ├── __cpu_setup  ← WRITES MAIR_EL1 = MT_DEVICE_nGnRnE | MT_DEVICE_nGnRE |
    │                   MT_DEVICE_GRE | MT_NORMAL_NC | MT_NORMAL | MT_NORMAL_TAGGED
    │                   (must be done BEFORE MMU enable)
    │
    └── __primary_switch
            │
            ├── __enable_mmu    ← MMU ON. All accesses now use MAIR attributes
            │
            └── __pi_early_map_kernel
                    │
                    └── maps kernel sections with AttrIdx=4 (MT_NORMAL)
                        maps FDT with AttrIdx=4 (MT_NORMAL)
                        maps device regions with AttrIdx=0 (MT_DEVICE_nGnRnE)
```

**Key ordering requirement:** `MAIR_EL1` MUST be written before `SCTLR_EL1.M`
is set. If the MMU were enabled with MAIR_EL1 = 0 (all device-nGnRnE), the
kernel would execute correctly but every memory access would go through strict
device ordering. The first data cache operation that assumed write-back behavior
would produce incorrect results.

---

## 7. Debugging MAIR Issues

If you suspect MAIR is misconfigured (e.g., after a custom `__cpu_setup`
modification):

```bash
# In crash or gdb, on a running kernel:
(crash) rd -8 <addr_of_MAIR_EL1_save>   # If saved to thread_info

# On target hardware with JTAG:
read MAIR_EL1                            # Should show 0x0000F0FF440C0400

# Expected value breakdown:
# Byte 0 (Attr0): 0x00 = nGnRnE
# Byte 1 (Attr1): 0x04 = nGnRE
# Byte 2 (Attr2): 0x0C = GRE
# Byte 3 (Attr3): 0x44 = Normal NC
# Byte 4 (Attr4): 0xFF = Normal WB ← most accesses use this
# Byte 5 (Attr5): 0xF0 = Normal Tagged (MTE)
# Byte 6 (Attr6): 0x00 = unused
# Byte 7 (Attr7): 0x00 = unused
```

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
MAIR_EL1 (Memory Attribute Indirection Register, EL1) is an 8-entry lookup table encoded in a 64-bit register. Each page-table descriptor contains a 3-bit AttrIdx field (0-7) that indexes into MAIR_EL1 to obtain the actual memory attributes. Attributes include:
- Device-nGnRnE (strictly ordered, no gather/reorder/early-write): for MMIO.
- Device-nGnRE: for most device registers.
- Normal Non-cacheable: for DMA buffers or uncached mappings.
- Normal Inner/Outer Write-Back Cacheable: for RAM (kernel code and data).
The CPU uses these attributes to decide whether to use the cache, how to order accesses, and whether to issue speculative fetches.

### Kernel Perspective (Linux ARM64)
Linux defines MAIR indices as MT_DEVICE_nGnRnE (0), MT_DEVICE_nGnRE (1), MT_DEVICE_GRE (2), MT_NORMAL_NC (3), MT_NORMAL (4), MT_NORMAL_WT (5) in arch/arm64/include/asm/memory.h. MAIR_EL1 is programmed in __cpu_setup before the MMU is enabled. Every ioremap() call uses the Device MAIR index; vmalloc() and the linear map use MT_NORMAL.

### Memory Perspective (ARMv8 Memory Model)
The ARMv8 memory model distinguishes Device memory (strictly ordered, no speculation) from Normal memory (can be cached, reordered, prefetched). MAIR_EL1 is the bridge between the page-table descriptor (which carries only a 3-bit index) and the actual memory type that the bus interface and cache controllers use. A misconfigured MAIR (e.g., mapping DRAM as Device-nGnRnE) causes catastrophic performance loss; mapping MMIO as Normal causes undefined behavior because the cache may absorb writes that must reach the device.