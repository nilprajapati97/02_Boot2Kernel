# fixmap and Early Virtual Addressing on ARM64 Deep Dive

Category: KASLR and Kernel Boot Memory Setup  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

fixmap provides compile-time fixed virtual slots used during early boot and special kernel operations.

Why it exists:
- Early boot cannot depend on full dynamic allocators or vmalloc.
- Some resources must be mapped at deterministic virtual addresses.
- Entry and low-level code needs stable addresses before full MM setup.

Typical use cases:
- early ioremap windows
- temporary mappings for page-table setup
- fixed vector or trampoline-related slots

---

## 2. ARM64 Hardware Detail

### 2.1 Address-space placement

fixmap occupies a dedicated high virtual range in kernel space.
Properties:
- deterministic slot-to-VA translation
- small, controlled range
- mapped with explicit page-table entries

Conceptual translation:
- fix_to_virt(index) gives fixed VA for slot
- virt_to_fix(va) returns slot index

### 2.2 Early mapping requirements

Before full page tables are online, kernel still needs:
- access to device registers for init
- temporary mapping of physical pages for setup tasks

These mappings must use safe attributes:
- device nGnRE for MMIO windows
- normal memory attributes for RAM-backed temporary mappings

### 2.3 TLB and ordering considerations

Changing fixmap PTEs requires:
- updating page tables atomically with proper barriers
- TLB invalidation for modified fixed slots
- strict sequencing in early boot to avoid stale translations

---

## 3. Linux Kernel Implementation

### 3.1 Core helpers

Key conceptual helpers in ARM64 early mm code:
- set_fixmap
- clear_fixmap
- fix_to_virt and virt_to_fix
- early_ioremap and early_iounmap

Mechanics:
1. Choose free fixmap slot.
2. Install PTE with desired attributes.
3. Use fixed VA.
4. Remove mapping and free slot when done.

### 3.2 Early ioremap window

early_ioremap provides temporary MMIO access before ioremap subsystem is ready.
- backed by a bounded set of fixmap slots
- nested usage depth is limited
- misuse can exhaust slots and break early boot operations

### 3.3 Interaction with trampolines and special mappings

Some secure and transition paths reserve fixed slots for:
- entry trampoline support
- temporary aliasing during early page-table replacement

These slots are architecture-sensitive and must match assembly entry expectations.

---

## 4. Hardware-Software Interaction

Example early MMIO access flow:
1. Kernel needs UART or interrupt controller register access early.
2. Calls early_ioremap with physical register range.
3. Kernel picks fixmap slot and installs device-attribute PTE.
4. Reads or writes through stable fixed VA.
5. Calls early_iounmap and clears slot.

Example table-building flow:
1. Temporary physical page for next-level table is allocated early.
2. Mapped via fixmap slot for initialization.
3. Entries are populated.
4. Slot is cleared once permanent mapping path takes over.

---

## 5. Interview Q and A

Q1: Why not use vmalloc for early boot mappings?
Because vmalloc requires subsystems not fully initialized in earliest boot phases.

Q2: What guarantees fixmap provides?
Deterministic virtual addresses for known slot indices and controlled lifecycle.

Q3: What happens if early_ioremap nesting exceeds limits?
Slot exhaustion can trigger warnings or failures and may block critical early initialization.

Q4: Why are memory attributes important for fixmap mappings?
Wrong attributes can cause ordering issues, device access faults, or incoherent behavior.

Q5: Is fixmap only for early boot?
Mostly early and low-level special use, but fixed slots are also used for specific permanent kernel mechanisms.

Q6: What must happen after changing a fixmap PTE?
Proper barrier and TLB maintenance so CPUs do not use stale translation entries.

---

## 6. Pitfalls and Gotchas

- Reusing a fixmap slot without clear ownership causes hard-to-debug corruption.
- Missing TLB invalidation after remap leads to stale access behavior.
- Mapping MMIO with normal memory attributes can break device semantics.
- Holding early_ioremap mappings too long increases collision risk.
- Assuming fixmap range is large enough for arbitrary temporary allocations.

---

## 7. Quick Reference Table

| Helper | Purpose |
|---|---|
| fix_to_virt | Convert fixed slot index to virtual address |
| virt_to_fix | Convert fixmap virtual address back to slot |
| set_fixmap | Install mapping into fixed slot |
| clear_fixmap | Remove mapping from fixed slot |
| early_ioremap | Temporary early MMIO mapping |
| early_iounmap | Release early MMIO mapping |

| Attribute choice | Typical target |
|---|---|
| Device memory type | MMIO registers |
| Normal memory type | RAM-backed temporary pages |
