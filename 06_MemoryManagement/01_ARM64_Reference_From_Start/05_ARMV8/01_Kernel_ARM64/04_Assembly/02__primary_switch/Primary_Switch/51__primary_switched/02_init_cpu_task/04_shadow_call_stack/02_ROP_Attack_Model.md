# ROP Attack Model — Why SCS Protects the Kernel

## Classic Stack Buffer Overflow

```c
// Vulnerable kernel function (hypothetical):
void parse_packet(char *buf, size_t len) {
    char local[64];
    memcpy(local, buf, len);  // BUG: len not bounded! Overflow possible!
    // ... process local ...
}
```

Stack layout before exploit:
```
[sp+0]   local[0..63]        ← 64 bytes
[sp+64]  saved_x29 (fp)
[sp+72]  saved_x30 (lr)      ← return to parse_packet's caller
```

After exploit (attacker sends 200-byte buf):
```
[sp+0]   AAAAAAAAAAAA...     ← overwrites local[0..63]
[sp+64]  BBBBBBBB             ← overwrites saved fp
[sp+72]  0xdeadbeef           ← HIJACKED return address!
```

When `parse_packet` returns:
```asm
ldp x29, x30, [sp, #64]  → x30 = 0xdeadbeef
ret                        → PC = 0xdeadbeef → ROP gadget!
```

---

## ROP Chain Example — Escalating Privilege

A ROP chain on a kernel without SCS:

```
GADGET 1 (at kernel VA 0xffffd000):
    ldr x0, [sp]     // load address of credential struct
    ret              // → GADGET 2

GADGET 2 (at kernel VA 0xffffe000):
    str xzr, [x0, #OFFSET_UID]  // zero out UID (root!)
    str xzr, [x0, #OFFSET_GID]
    ret              // → return to "normal" code (cover tracks)

NORMAL CODE (at kernel VA 0xffff0000):
    // attacker-chosen return point in kernel
    // appears as legitimate return
```

The attacker constructs this chain by:
1. Analyzing the kernel image to find usable gadgets
2. Crafting the overflow payload to set up the gadget chain

---

## How SCS Defeats This Attack

With SCS enabled:
```asm
// parse_packet's compiler-generated prologue:
str    x30, [x18], #8           // PUSH LR to shadow stack
stp    x29, x30, [sp, #-80]!    // also save to normal stack (for AAPCS)
mov    x29, sp

// ... function body ...

// parse_packet's compiler-generated epilogue:
ldp    x29, x30, [sp], #80      // restore from normal stack
// At this point: x30 = 0xdeadbeef (attacker's value!)
// But then:
ldr    x30, [x18, #-8]!         // OVERWRITE x30 from shadow stack!
// x30 = original LR (legitimate caller address)
// The attacker's 0xdeadbeef is discarded
ret                              // returns to the CORRECT caller
```

The ROP chain never executes because `ret` uses the SHADOW stack's return address,
not the corrupted normal stack's return address.

---

## The `x18` Protection Model

For SCS to work, `x18` itself must not be corruptible. How is it protected?

1. **Not in memory during kernel execution:** `x18` is a CPU register — attackers
   can't write to it via a memory overflow.

2. **Never written by generated code:** `-ffixed-x18` tells the compiler to never
   allocate `x18` for any variable. No `mov x18, xN` instructions appear in
   compiler-generated code.

3. **Saved/restored during context switch:** When the OS switches tasks, `x18` is
   saved in the outgoing `task_struct` and the incoming task's `x18` is restored.
   The save/restore happens in dedicated assembly code (`cpu_switch_to`), not
   compiler-generated code.

4. **Validated on return from exception:** Some kernels check that `x18` has not
   been tampered with when returning from an exception handler.

---

## Attack Scenario Requiring SCS Bypass

An attacker targeting SCS would need to:

1. Find a way to WRITE to the shadow call stack region — requires a separate
   arbitrary-write vulnerability (usually harder than a stack overflow)
2. OR find a way to overwrite `x18` itself — requires a register write, which
   means code execution (circular dependency)
3. OR find a function that does NOT use SCS (assembly code, or
   `__noscs` annotated functions) — limited target surface

This layered defense is why SCS significantly raises the bar for exploitation.

---

## `__noscs` — Functions Excluded from SCS

Some kernel assembly functions explicitly opt out of SCS:

```c
// include/linux/compiler_attributes.h
#define __noscs __attribute__((no_sanitize("shadow-call-stack")))
```

Used sparingly for:
- Early boot code that runs before SCS is initialized
- Architecture-specific assembly that manages `x18` explicitly
- Some interrupt entry/exit paths (where x18 is swapped with another value)

`scs_load_current` itself is called from `init_cpu_task` — which is pure assembly.
The call from `__primary_switched` to `start_kernel` is the FIRST SCS-protected
function call in the kernel's lifetime.

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