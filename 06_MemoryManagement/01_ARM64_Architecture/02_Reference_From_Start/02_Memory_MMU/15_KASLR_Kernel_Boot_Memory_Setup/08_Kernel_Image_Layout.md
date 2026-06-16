# ARM64 Kernel Image Layout Deep Dive

Category: KASLR and Kernel Boot Memory Setup  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Kernel image layout defines how executable code and data are arranged from boot image through runtime mapping.

Why interviewers ask:
- Layout knowledge explains boot crashes, relocation behavior, and memory protections.
- It connects linker script design, section permissions, and early MM setup.

Main elements:
- image header and entry
- text, rodata, data, bss
- init-only sections reclaimed later
- placement of initrd and early metadata

---

## 2. ARM64 Hardware Detail

### 2.1 Image and execution expectations

ARM64 boot protocol expects:
- 64-bit execution state
- aligned image placement constraints
- entry semantics matching architecture boot ABI

Hardware implications:
- instruction fetch permissions for text
- data non-executable policy for writable segments
- page-table permissions aligned with section intent

### 2.2 Section protection model

Desired runtime protection split:
- text: read and execute
- rodata: read-only and non-executable for data use
- data and bss: read-write and non-executable

This relies on page-table attribute transitions after early boot setup.

---

## 3. Linux Kernel Implementation

### 3.1 Linker script and symbols

Kernel linker script defines section ordering and exported boundaries.
Common boundary symbols include conceptual markers for:
- start of text
- end of text and rodata
- start and end of bss
- overall image end

These markers are used by early memory setup and protection routines.

### 3.2 Early entry flow

Early assembly path does:
1. initial environment checks
2. temporary table setup
3. transition into C runtime initialization

Image layout assumptions are hard-coded in this path for relocation and mapping.

### 3.3 init sections lifecycle

Init sections store one-time boot code and data:
- used during initialization only
- released after init completes to recover memory

This is an important optimization for kernel memory footprint.

### 3.4 initrd placement interactions

initrd is provided by firmware or bootloader and must:
- avoid overlap with kernel image and reserved early structures
- remain mapped and accessible until userspace handoff stage

---

## 4. Hardware-Software Interaction

Typical mapping progression:
1. Bootloader places kernel image and optional initrd.
2. Kernel enters with early minimal mappings.
3. Final page tables map section ranges with intended permissions.
4. Late init frees init-only ranges and tightens protections.

Security outcome:
- writable regions become non-executable.
- code regions become read-only after relocation or patching windows close.

---

## 5. Interview Q and A

Q1: Why separate text, rodata, and data in layout?
To enforce least-privilege memory permissions and improve exploit resistance.

Q2: What is the benefit of freeing init sections?
Reclaims memory after boot and reduces long-term kernel footprint.

Q3: Why are linker symbols critical in early boot?
They provide exact boundaries needed for mapping and protection logic.

Q4: Can initrd overlap kernel sections?
It must not. Overlap can corrupt boot artifacts and cause unpredictable failures.

Q5: Where does kernel image layout policy mostly live?
In architecture boot code plus linker script definitions.

Q6: How does layout relate to KASLR?
Randomization changes base placement, but section-relative ordering and protection goals remain consistent.

---

## 6. Pitfalls and Gotchas

- Misinterpreting physical placement versus virtual runtime addresses.
- Forgetting late permission tightening after early writable phases.
- Incorrect section alignment breaking large mapping opportunities.
- Overlooking initrd constraints during custom bootloader integration.
- Assuming all debug builds preserve production protection policy.

---

## 7. Quick Reference Table

| Section type | Typical runtime permission |
|---|---|
| text | read plus execute |
| rodata | read-only |
| data | read plus write |
| bss | read plus write |
| init text and data | temporary, freed after init |

| Layout artifact | Why it matters |
|---|---|
| linker symbols | precise mapping boundaries |
| image header | bootloader and firmware compatibility |
| initrd placement | early userspace handoff integrity |
