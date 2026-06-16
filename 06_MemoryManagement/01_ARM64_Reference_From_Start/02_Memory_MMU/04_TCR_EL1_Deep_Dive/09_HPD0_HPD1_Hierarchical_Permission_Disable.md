# HPD0 / HPD1: Hierarchical Permission Disable

**Category**: Translation Control Register Deep Dive  
**Targeted**: ARM, Qualcomm

---

## 1. Hierarchical Permission Controls: Background

ARM64 page table descriptors contain two types of permission bits:

1. **Page/Block descriptors** (leaf entries): `AP[7:6]`, `PXN[53]`, `UXN[54]`  
   → Control access to the final mapped physical page

2. **Table descriptors** (non-leaf, intermediate entries): `APTable[62:61]`, `PXNTable[59]`, `XNTable[58]`, `NSTable[60]`  
   → Override / restrict permissions for ALL subsequent levels in the hierarchy

```
Hierarchical permission: A table entry at L0/L1/L2 can restrict what 
child tables (and ultimately pages) are allowed to do, regardless of 
what the leaf descriptor says.

Example:
  L2 table descriptor has PXNTable=1
  → ALL pages reachable through this L2 table are non-executable at EL1
  → Even if a leaf page PTE has PXN=0 (should be executable)
  → PXNTable OVERRIDES PXN at the leaf level

This allows a single bit at a high table level to lock down a large
VA range (e.g., 1GB mapped through an L1 table) without modifying
every individual leaf PTE.
```

---

## 2. Hierarchical Permission Fields in Table Descriptors

```
Table descriptor (L0/L1/L2) bits[63:59]:

  Bit[60] = NSTable:  0=Secure, 1=Non-Secure output PA
  Bit[61] = APTable[0]: Access Permissions override
  Bit[62] = APTable[1]: Access Permissions override
  Bit[59] = XNTable (UXN for L0/L1; Execute-Never at EL0)
            or XNTable (some ARM revisions combine PXN+XN)
  Bit[58] = PXNTable: Privileged Execute-Never override

Combination effects (when HPD=0, hierarchical perms ACTIVE):

APTable[1:0] override:
  0b00 = No effect (leaf AP bits used as-is)
  0b01 = Access from EL0 denied (overrides leaf AP to prevent EL0 access)
  0b10 = Entire range is read-only (overrides any writable leaf entries)
  0b11 = EL0 access denied AND entire range is read-only

PXNTable=1: All pages in this subtree are non-executable at EL1
  → Even if leaf PTE has PXN=0 → treated as PXN=1

XNTable=1 (UXNTable): All pages non-executable at EL0
  → Even if leaf PTE has UXN=0 → treated as UXN=1

NSTable: Affects Secure/Non-Secure PA selection (TrustZone context)
```

---

## 3. HPD0 / HPD1 Fields

```
TCR_EL1.HPD0 (bit[41]):
  0 = Hierarchical permission overrides ARE active for TTBR0 (user) region
      APTable, PXNTable, XNTable bits in intermediate table descriptors
      can restrict permissions of child entries
  1 = Hierarchical permission overrides are DISABLED for TTBR0 region
      APTable, PXNTable, XNTable bits in TTBR0 table descriptors are IGNORED
      Only leaf descriptor AP/PXN/UXN bits determine access rights

TCR_EL1.HPD1 (bit[42]):
  0 = Hierarchical permission overrides ARE active for TTBR1 (kernel) region
  1 = Hierarchical permission overrides are DISABLED for TTBR1 region

Linux default: HPD0=0, HPD1=0 (hierarchical permissions ARE active)
  This means table-level APTable/PXNTable/XNTable DO work.
```

---

## 4. When HPD=1 Would Be Used

```
HPD=1 (hierarchical permissions disabled) use cases:

1. Legacy OS Compatibility:
   Some older 32-bit ARM OS ports that were migrated to 64-bit may not
   set APTable/PXNTable bits correctly (they leave them as 0 from ARM32).
   On ARM64 with HPD=0, these table-level bits would be interpreted as:
     APTable=0b00 → no override (harmless)
   But if a legacy OS accidentally sets these bits → unexpected restriction
   HPD=1 on TTBR0 side can protect against accidental table-level permissions.

2. Hypervisor Simplification:
   A hypervisor may want guests to have full control of their own page tables
   without hierarchical permission constraints.
   Setting HPD=1 in the guest's TCR lets guest page table hierarchy work
   without table-descriptor overrides interfering.

3. Testing/Debugging:
   If you're debugging a permission fault and suspect a table-level APTable
   bit is incorrectly set, temporarily setting HPD=1 isolates whether the
   fault comes from leaf descriptor or table descriptor.

4. Performance micro-optimization (marginal):
   With HPD=1, the hardware PTW doesn't need to accumulate permission bits
   across all levels → slightly simpler logic.
   In practice, the performance difference is negligible.

Linux reasoning for HPD=0 (default):
   Linux actively USES hierarchical permissions for security:
   - Kernel linear map: PXNTable=1 at top-level → entire linear map is 
     non-executable at EL1 (enforcing W^X for kernel data)
   - Module/vmalloc region: managed carefully with PXN
   - Table-level permissions provide defense-in-depth
```

---

## 5. Practical Example: Kernel Linear Map Protection

```
Kernel linear map (all physical memory mapped as kernel data):
  VA range: 0xFFFF_8000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF (128 TB)
  
  Linux sets PGD/PUD entries for the linear map with PXNTable=1:
    All physical memory mapped in the linear map is non-executable at EL1
    Even if a leaf PTE accidentally has PXN=0, the table-level PXNTable=1 forces PXN=1
  
  This provides defense against:
    - Kernel JIT bugs that accidentally enable execute permission on data
    - Use-after-free exploits trying to execute freed memory

  With HPD1=1 (hierarchical perms disabled):
    The PXNTable=1 in the PGD would be IGNORED
    The linear map could become executable if any leaf PTE has PXN=0
    → Security regression: an attacker could potentially execute data pages
  
  Therefore: Linux NEVER sets HPD1=1.
```

---

## 6. HPD Interaction with W^X Policy

```
W^X (Write XOR Execute) is the security policy:
  A page should never be simultaneously Writable AND Executable.

Enforcement:
  Software W^X: checked at mmap/mprotect time (in kernel mm code)
  Hardware W^X: enforced via PXN/UXN bits in page descriptors

Hierarchical permissions STRENGTHEN W^X:
  XNTable=1 at a table covering the writable data region ensures
  no page in that region can be executable, even if someone modifies
  a leaf PTE to set execute permission.

With HPD=1 (overrides disabled):
  XNTable is ignored → software W^X check is the only enforcement
  If software W^X check has a bug → execute permission may be granted
  
  Security implication: HPD=0 provides an additional hardware layer
  against W^X violations. HPD=1 weakens this protection.
```

---

## 7. Interview Questions & Answers

**Q1: What are hierarchical permissions in ARM64 page tables, and what does HPD do?**

ARM64 table descriptors (non-leaf entries at L0/L1/L2) carry `APTable[62:61]`, `PXNTable[59]`, and `XNTable[58]` bits that **override and restrict** permissions for all child entries in the subtree. For example, `PXNTable=1` in an L1 table makes all pages under that L1 entry non-executable at EL1, regardless of individual leaf PTE `PXN` bits. `HPD0/HPD1` in TCR_EL1 (bits[41:42]) **disables** these hierarchical overrides when set to 1. With `HPD=1`, only the leaf descriptor AP/PXN/UXN bits matter. Linux always uses `HPD=0` (overrides active) to benefit from table-level security enforcement (e.g., non-executable linear map via `PXNTable`).

**Q2: From a security perspective, why should HPD be kept at 0 (default)?**

`HPD=0` allows hierarchical permission bits in table descriptors to provide **defense-in-depth** for W^X enforcement. The kernel sets `PXNTable=1` and `XNTable=1` at high-level table entries covering the linear map and writable data regions. Even if a bug in the memory management code incorrectly sets a leaf PTE's `PXN=0` (making a data page executable), the table-level `PXNTable=1` overrides it, preventing execution. With `HPD=1`, this safety net disappears — the table-level protection is ignored. An exploit that manipulates leaf PTEs to grant execute permission would succeed. Therefore `HPD=1` weakens kernel memory protection and should never be used in production Linux.

---

## 8. Quick Reference

| Field | Bit | Default | Effect when 1 |
|---|---|---|---|
| HPD0 | [41] | 0 | Disable hierarchical perms for TTBR0 |
| HPD1 | [42] | 0 | Disable hierarchical perms for TTBR1 |

| Table descriptor bit | Overrides | Direction |
|---|---|---|
| APTable[62:61] | Leaf AP[7:6] | Can restrict, never grant |
| PXNTable[59] | Leaf PXN[53] | Can add PXN=1, never remove |
| XNTable[58] | Leaf UXN[54] | Can add UXN=1, never remove |
| NSTable[60] | Output PA security | Can force NS, never force S |

Key rule: hierarchical permissions can only **restrict** (add restrictions), never **grant** (remove restrictions). APTable can make things more restricted but never more permissive than the table-level setting.
