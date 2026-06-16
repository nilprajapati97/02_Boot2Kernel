# Hierarchical Permission Overrides: PXNTable, XNTable, APTable

**Category**: Access Permissions & Execute Never  
**Targeted**: ARM, Qualcomm

---

## 1. Hierarchical Permission Overview

```
ARM64 page table has three levels of descriptors before the leaf (page/block):
  L0 (PGD): Table descriptor → L1 (PUD)
  L1 (PUD): Table descriptor → L2 (PMD)
  L2 (PMD): Table descriptor → L3 (PTE) or Block descriptor

Each TABLE descriptor (non-leaf) can carry permission restriction bits
that OVERRIDE or RESTRICT the leaf descriptor's permissions:

Table descriptor bits[63:59]:
  Bit[59] = PXNTable: Override PXN for all pages in this subtree
  Bit[58] = XNTable:  Override XN/UXN for all pages in this subtree
  Bit[62] = APTable[1]: Override AP[1] for all pages in subtree
  Bit[61] = APTable[0]: Override AP[0] for all pages in subtree (actually AP[2])
  Bit[60] = NSTable: Override NS bit for all pages in subtree

Important rule:
  Table descriptor overrides can only RESTRICT permissions, never GRANT them.
  
  If PXNTable=1: All leaves have effective PXN=1 (cannot execute at EL1)
                 even if the leaf PTE has PXN=0
  If PXNTable=0: Leaf PTE PXN bit applies (leaf decides)
  
  You CANNOT set PXNTable=0 to grant execute permission to a leaf that has PXN=1.
  The hierarchical override is unidirectional: restrict-only.
```

---

## 2. Table Descriptor Bit Layout

```
Table Descriptor (64-bit), Stage 1, pointing to next-level table:

63 62 61 60 59 58  51      12  11   2  1  0
┌──┬──┬──┬──┬──┬──┬──────────┬──────┬─────┐
│NG│AP│AP│NS│PX│XN│ Reserved │ Table│ 1 1 │
│  │T1│T0│T │ T│ T│          │ Addr │     │
└──┴──┴──┴──┴──┴──┴──────────┴──────┴─────┘

Bit[63] = nG (not applicable for table descriptors)
Bit[62] = APTable[1]
Bit[61] = APTable[0]
Bit[60] = NSTable
Bit[59] = PXNTable
Bit[58] = XNTable (UXNTable for EL1/EL0 translations)
Bits[47:12] = Table Base Address (physical address of next-level table)
Bits[1:0] = 0b11 (table descriptor encoding)

APTable encoding:
  APTable[1:0] = 0b00: No effect, leaf AP bits apply
  APTable[1:0] = 0b01: Deny user access (effective AP[1]=0 for all leaves)
  APTable[1:0] = 0b10: Read-only (effective AP[2]=1 for all leaves)
  APTable[1:0] = 0b11: Read-only + deny user (RO + no EL0 access)
```

---

## 3. Linux Kernel Usage of Hierarchical Permissions

```
Linux primarily uses leaf-level PTE permissions.
Table-level overrides are used in specific optimization scenarios:

Kernel linear map (TTBR1_EL1):
  PGD/PUD table entries for kernel direct map:
    PXNTable = 0 (let leaf PXN control)
    XNTable = 0 (let leaf UXN control)
  
  However, conceptually equivalent:
    All kernel PUD entries covering the kernel linear map
    could set PXNTable=1 to prevent any user-space
    mapping from ever being kernel-executable

  In practice:
    Linux does NOT set PXNTable widely — uses per-leaf PXN instead
    More granular control preferred

KPTI (Kernel Page Table Isolation):
  In KPTI, user-mode trampoline page tables have minimal kernel mappings
  Table entries for the trampoline pgd use XNTable/PXNTable
  to restrict execution to only the trampoline stub pages

Module space:
  set_memory_ro() / set_memory_exec() work on leaf pages
  No table-level overrides used for modules (modules are small, leaf-level OK)

ARM Trusted Firmware (ATF):
  Stage 2 table descriptors use XNTable=1 for device PA ranges:
    All GPAs (Guest Physical Addresses) mapping to MMIO use XNTable=1 at PMD level
    Any leaf block mapping below this automatically gets XN=1
    Efficient: one PMD setting protects entire 2MB device region
```

---

## 4. HPD and Table Override Interaction

```
HPD (Hierarchical Permission Disable) in TCR_EL1:
  TCR_EL1.HPD0[41]: If 1, table permissions for TTBR0 are DISABLED
  TCR_EL1.HPD1[42]: If 1, table permissions for TTBR1 are DISABLED

Effect of HPD:
  HPD=0 (default): PXNTable, XNTable, APTable, NSTable all ACTIVE
    Table descriptor overrides apply to all leaf pages below
    
  HPD=1: PXNTable, XNTable, APTable, NSTable IGNORED
    Only leaf PTE bits control permissions
    Bits [62:59] in table descriptors become available for PBHA
    (Page-Based Hardware Attributes — hardware-use)

Why would you set HPD=1?
  PBHA use: want to use PTE bits [62:59] for hardware-defined purposes
            (cache partitioning, QoS, encryption tags)
  Security tradeoff: lose hierarchical permission protection
  
  Linux default: HPD=0 (table overrides active, PBHA not used)
  
  ARM recommends: leave HPD=0 for security
  Some Qualcomm SoC features that use PBHA require HPD=1
    → Must carefully audit all table descriptors have correct leaf permissions
    → Cannot rely on table-level PXNTable as security shortcut

When HPD=1 is dangerous:
  If any leaf PTE accidentally has PXN=0 on a data page:
    HPD=0 → parent PXNTable=1 would catch it (enforce PXN=1)
    HPD=1 → no table override → kernel executes from data page → security hole
```

---

## 5. APTable for Access Restriction

```
APTable[1:0] in table descriptor restricts leaf AP bits:

APTable[1:0] = 0b01 (deny user access):
  Effective AP[1]=0 for all leaves below this table
  All pages in this subtree: EL0 has no access
  Use case: kernel-only tables (TTBR1 tables already don't serve EL0)
  
APTable[1:0] = 0b10 (read-only):
  Effective AP[2]=1 (read-only) for all leaves
  All pages become read-only regardless of leaf AP bits
  Use case: after initial setup, lock a large kernel memory region read-only
  
APTable[1:0] = 0b11 (read-only + deny user):
  Both: EL0 no access AND all pages read-only
  Most restrictive: only EL1 can read, no one can write or EL0 access
  Use case: kernel read-only protected data regions

NSTable (bit[60]):
  Controls Non-Secure attribute for all pages in subtree
  NSTable=1 → all leaf PA outputs are NS (non-secure) regardless of leaf NS bit
  NSTable=0 → leaf NS bits apply
  Used in TrustZone partitioning to mark entire page subtrees as non-secure
```

---

## 6. Interview Questions & Answers

**Q1: If a PMD table descriptor has PXNTable=1, but a leaf PTE has PXN=0, which takes effect?**

**PXNTable=1 takes effect** — the table descriptor wins. Hierarchical permissions can only **restrict**, never grant. If `PXNTable=1` at the PMD (L2) level, then ALL leaf PTEs reachable through that PMD effectively have `PXN=1`, regardless of their individual `PXN` bit setting. The leaf's `PXN=0` is overridden. This is a "restriction cascade": setting a permission restriction at a higher table level propagates down to all pages below it.

The reverse is NOT true: if the PMD has `PXNTable=0`, the leaf's `PXN` bit is the authority — a leaf with `PXN=1` will still be non-executable even though the table didn't restrict it.

**Q2: When would you use APTable vs per-leaf AP bits for access control?**

APTable is efficient for coarse-grained access control over large address ranges. For example, to make an entire 1GB kernel heap region read-only, you can set `APTable=0b10` on the single PGD entry covering that range, instead of iterating through thousands of PTEs. The table-level setting applies atomically to all leaf pages below. It's also useful for bulk lockdown after initialization (lock a PUD after module loading completes). Per-leaf AP bits provide fine-grained control — needed when different pages in the same table have different permissions (e.g., within a kernel module: `.text` is RO+exec, `.data` is RW+noexec). In practice, Linux primarily uses per-leaf permissions for flexibility, reserving table-level overrides for security-critical bulk lockdowns.

---

## 7. Quick Reference

| Table bit | Effect | Override direction |
|---|---|---|
| PXNTable=1 | All leaves: EL1 cannot execute | Restrict only |
| XNTable=1 | All leaves: EL0 cannot execute | Restrict only |
| APTable=0b01 | All leaves: EL0 no access | Restrict only |
| APTable=0b10 | All leaves: read-only | Restrict only |
| NSTable=1 | All leaves: NS output | Override |

| HPD setting | Table overrides | Effect |
|---|---|---|
| HPD=0 (default) | ACTIVE | PXNTable/XNTable/APTable apply |
| HPD=1 | DISABLED | Bits[62:59] become PBHA |
