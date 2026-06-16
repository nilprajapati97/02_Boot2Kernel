# W^X (Write XOR Execute) Policy: ARM64 Implementation

**Category**: Access Permissions & Execute Never  
**Targeted**: ARM, Qualcomm, NVIDIA, AMD

---

## 1. What is W^X?

```
W^X (Write XOR Execute) is a security policy:
  A memory page can be EITHER writable OR executable — NEVER both simultaneously.
  
  Writable AND executable (W+X) is dangerous because:
    1. Attacker writes malicious code into the page (exploit payload)
    2. Attacker redirects execution to that page
    3. CPU executes attacker's code from that writable page
    
    This is the classic "shellcode injection" attack.
    W^X breaks it: if the page is writable, it cannot be executed.

ARM64 enforcement via PTE bits:
  Writable: AP[2] = 0 (AP[2:1] = 0b00 or 0b01)
  Executable (user): UXN = 0
  W^X requires: if AP[2]=0 (writable), then UXN=1 (not executable)
               if UXN=0 (executable), then AP[2]=1 (read-only, not writable)

Linux kernel W^X enforcement:
  User space: mprotect() enforces W^X (cannot add EXEC to writable page)
  Kernel space: mark_rodata_ro() makes text RO, data NX (PXN=1)
```

---

## 2. User Space W^X Enforcement

```
Kernel config: CONFIG_STRICT_W_XOR_X (part of general hardening)

mprotect() flow with W^X check (mm/mprotect.c):
  do_mprotect_pkey(addr, len, prot, pkey)
  
  Security check:
    if (prot & PROT_EXEC) && vm_file &&
       (prot & PROT_WRITE) &&
       security_file_mprotect() returns error:
      return -EACCES;
  
  Personality-based override:
    Older applications may need W+X (emulators, some JIT implementations)
    READ_IMPLIES_EXEC personality flag allows this (less secure)
    
  SELinux/AppArmor can enforce W^X per-domain:
    deny execheap, execstack, execmem rules

mmap() with W^X:
  mmap(NULL, size, PROT_WRITE|PROT_EXEC, MAP_ANON|MAP_PRIVATE, -1, 0)
  → Kernel may deny if W^X strict mode, or return mapping with either W or X
  
  Correct JIT pattern:
    // Step 1: Map writable, not executable
    buf = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    // Step 2: Write code to buffer
    memcpy(buf, code, code_size);
    // Step 3: Flush D-cache to PoU, invalidate I-cache (see Category 05/08)
    flush_icache_range(buf, buf + code_size);
    // Step 4: Make executable (removes write permission)
    mprotect(buf, size, PROT_READ|PROT_EXEC);
    // Step 5: Execute
    ((void(*)())buf)();
    
  This is the CORRECT two-phase approach:
    Phase 1: PROT_WRITE, !PROT_EXEC → UXN=0 (user can read/write)
    Phase 2: PROT_EXEC, !PROT_WRITE → UXN=0 (user can read/execute)
    Never both simultaneously.
```

---

## 3. Kernel Space W^X

```
Linux kernel enforces W^X for its own code and data:

After mark_rodata_ro() (called from kernel_init_freeable()):
  .text section:
    AP[2:1] = 0b10 (kernel read-only) → cannot be written by kernel
    PXN = 0 → kernel can execute
    UXN = 1 → user cannot execute
    
  .rodata section:
    AP[2:1] = 0b10 (read-only)
    PXN = 1 (cannot execute from rodata — it's data)
    UXN = 1
    
  .data, .bss sections:
    AP[2:1] = 0b00 (kernel read-write)
    PXN = 1 (cannot execute from data — W^X)
    UXN = 1

  init sections (freed after boot):
    .init.text: was PXN=0 during boot, freed/unmapped after
    .init.data: was PXN=1 during boot, freed/unmapped after

Kernel text protection verification:
  CONFIG_DEBUG_RODATA_TEST: verifies .rodata cannot be written
  /sys/kernel/debug/kernel_page_tables: shows PTE attributes for each section
  /proc/kallsyms: shows addresses, cross-reference with page tables

Kernel module W^X:
  Module text (.text): PXN=0 (executable) + read-only after load
  Module data: PXN=1 (non-executable)
  set_memory_ro() / set_memory_exec() helpers for module permission management
```

---

## 4. SCTLR_EL1.WXN: Writable Implies Execute Never

```
WXN (Writable imply XN) is a hardware enforcement bit:
  SCTLR_EL1[19] = WXN

  WXN = 0: Normal operation — UXN/PXN bits control execution
  WXN = 1: Any writable page automatically becomes non-executable
            Even if UXN=0 or PXN=0 in PTE, a writable page gets XN enforced
            by hardware (override of PTE execute permission)

Effect:
  If AP[2:1]=0b00 (writable) and WXN=1:
    → Effectively UXN=1 AND PXN=1 (neither EL0 nor EL1 can execute)
    Regardless of what UXN/PXN bits say
    
  If AP[2:1]=0b10 or 0b11 (read-only) and WXN=1:
    → Normal UXN/PXN bits apply (read-only pages can still be executable)

Linux usage:
  CONFIG_ARM64_SW_TTBR0_PAN and CONFIG_RODATA_FULL_DEFAULT_ENABLED
  may optionally enable WXN for strongest W^X enforcement
  
  Default: WXN=0 (software manages W^X via PTE bits)
  Why not always WXN=1? 
    Some code paths legitimately need read/write + execute during setup
    (e.g., boot trampoline, module loading before finalization)
    WXN=1 would fault those pages, requiring careful ordering of RO-marking

Samsung Knox / Qualcomm:
  SCTLR_EL1.WXN=1 enabled in security-hardened Android kernels
  Combined with KASLR and CFI for defense in depth
```

---

## 5. W^X and Common Bypass Techniques

```
Since W^X is widely enforced, attackers use alternative techniques:

1. Return-Oriented Programming (ROP):
   Find "gadgets": short code sequences ending in RET in existing .text pages
   Chain gadgets to compute desired operations
   Never needs writable+executable: uses existing .text (executable, not writable)
   Defense: CFI (Control Flow Integrity) — validates branch/call targets

2. Jump-Oriented Programming (JOP):
   Similar to ROP but uses indirect jump gadgets instead of RET
   Defense: BTI (Branch Target Identification, ARMv8.5-A)
             BTI marks valid indirect branch targets in .text
             CPU faults if indirect branch targets non-BTI instruction

3. Data-Only Attacks:
   No code execution needed — modify data structures (function pointers, etc.)
   Change return address to existing function with right arguments
   Defense: Shadow Stack (ARMv9.3-A GCS), PAC for stack integrity

4. mprotect() Race Condition (TOCTOU):
   Thread A: maps page WRITE, writes shellcode
   Thread A: calls mprotect → page becomes EXEC
   Thread B: tries to execute the page in the race window
   Defense: no good architectural defense; application-level mitigations
   
5. Shared library overwrite:
   If a shared library is mapped rw (e.g., during ld.so relro setup):
   Overwrite PLT/GOT entry before RELRO makes it read-only
   Defense: FULL RELRO (mprotect PLT/GOT read-only early)
```

---

## 6. Interview Questions & Answers

**Q1: Explain exactly how ARM64 enforces W^X. What bits are involved, and what prevents a program from having a writable-executable page?**

ARM64 enforces W^X through a combination of two PTE mechanisms. First, **write permission** is controlled by `AP[2:1]`: if `AP[2]=0`, the page is writable; if `AP[2]=1`, it's read-only. Second, **execute permission** is controlled by `UXN` (bit[54]) for user space and `PXN` (bit[55]) for kernel: `UXN=0` means executable, `UXN=1` means no execution allowed.

W^X enforcement: when `AP[2]=0` (writable), Linux policy ensures `UXN=1` and `PXN=1`. When `UXN=0` (executable), Linux ensures `AP[2]=1` (read-only). These constraints are enforced in `mprotect()` — if you request `PROT_WRITE | PROT_EXEC` simultaneously, the kernel either rejects it (with `CONFIG_STRICT_W_XOR_X`) or prevents the combination in the PTE.

Additionally, `SCTLR_EL1.WXN=1` can be set to make the hardware automatically treat all writable pages as non-executable, providing hardware-level W^X enforcement regardless of the software-set `UXN`/`PXN` bits.

---

## 7. Quick Reference

| PTE State | AP[2] | UXN | PXN | W^X compliant? |
|---|---|---|---|---|
| Writable, not executable | 0 | 1 | 1 | YES (data page) |
| Read-only, executable | 1 | 0 | 0 | YES (code page) |
| Writable, executable | 0 | 0 | 0 | NO (violation!) |
| Read-only, not executable | 1 | 1 | 1 | YES (const data) |

| Linux section | AP[2] | PXN | UXN | W^X |
|---|---|---|---|---|
| .text (after rodata_ro) | 1 (RO) | 0 | 1 | YES |
| .rodata | 1 (RO) | 1 | 1 | YES |
| .data / .bss | 0 (RW) | 1 | 1 | YES |
| User text | 1 (RO) | 1 | 0 | YES |
| User data | 0 (RW) | 1 | 1 | YES |
