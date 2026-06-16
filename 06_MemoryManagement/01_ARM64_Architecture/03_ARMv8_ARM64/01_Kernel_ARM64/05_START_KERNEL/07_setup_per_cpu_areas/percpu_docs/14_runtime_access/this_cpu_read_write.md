# `this_cpu_read()` / `this_cpu_write()` — ARM32 vs ARM64 Assembly Comparison

## Source Reference
- `include/linux/percpu-defs.h:250` — `this_cpu_ptr()`
- `arch/arm/include/asm/percpu.h:27` — ARM32 `__my_cpu_offset()`
- `arch/arm64/include/asm/percpu.h:32` — ARM64 `__kern_my_cpu_offset()`
- `include/asm-generic/percpu.h:98` — `this_cpu_read()` generic path

---

## The 3-Instruction Per-CPU Access Pattern

Both ARM32 and ARM64 implement per-CPU variable access in **3 instructions**:

```
Instruction 1: Read per-CPU offset from hardware register
Instruction 2: Add template variable offset to per-CPU offset
Instruction 3: Load/store the variable at the computed address
```

---

## Side-by-Side Assembly Comparison

### Scenario: `this_cpu_read(my_counter)` where `my_counter` is `int`

#### ARM32 (non-SMP / UP system, without `.alt.smp.init` patching)

```asm
; Compiled from: this_cpu_read(my_counter)
; where my_counter template address = 0xC0A12004 (example)

; Instruction 1: Read TPIDRPRW = __per_cpu_offset[current_cpu]
mrc   p15, 0, r2, c13, c0, 4     ; r2 = __per_cpu_offset[cpu]

; Instruction 2: Load address of my_counter's copy for this CPU
; (compiler may encode as ldr with base + TPIDRPRW offset)
ldr   r0, [r2, #<my_counter_offset>]   ; r0 = *(base + offset)

; Or the compiler might emit:
add   r2, r2, #<page_offset>           ; r2 += compile-time offset
ldr   r0, [r2]                         ; r0 = *r2
```

#### ARM32 (SMP system, after `.alt.smp.init` patching)

```asm
; Before patching (UP kernel with ALT_SMP entry):
mov   r2, #0                          ; UP: offset always 0

; After boot patching (SMP kernel or SMP-capable UP with alt patch):
mrc   p15, 0, r2, c13, c0, 4     ; reads TPIDRPRW
```

#### ARM64 (non-VHE)

```asm
; Compiled from: this_cpu_read(my_counter)

; Instruction 1: Read tpidr_el1 = __per_cpu_offset[current_cpu]
mrs   x8, tpidr_el1               ; x8 = __per_cpu_offset[cpu]

; Instruction 2+3: Load from CPU's copy of my_counter
; The compiler typically merges add+load:
ldr   w0, [x8, #<my_counter_offset>]  ; w0 = *(x8 + offset)
```

#### ARM64 (VHE, after `ALTERNATIVE()` patching)

```asm
; Instruction 1: Read tpidr_el2 (was patched from tpidr_el1 at boot)
mrs   x8, tpidr_el2               ; x8 = __per_cpu_offset[cpu]

; Instruction 2+3: Load (same as non-VHE):
ldr   w0, [x8, #<my_counter_offset>]
```

---

## Detailed: ARM32 `this_cpu_inc(my_counter)` (Read-Modify-Write)

```asm
; this_cpu_inc(my_counter) on ARM32:

; Instruction 1: Get per-CPU base
mrc   p15, 0, r3, c13, c0, 4     ; r3 = per-CPU offset

; Instruction 2: Compute address of this CPU's counter
add   r2, r3, #<percpu_base_offset>   ; r2 = percpu_base_page
add   r2, r2, #<within_page_offset>   ; r2 += variable offset within page

; Instruction 3: Load current value
ldr   r0, [r2]                    ; r0 = current value

; Instruction 4: Increment
add   r0, r0, #1                  ; r0++

; Instruction 5: Store back
str   r0, [r2]                    ; store incremented value

; Note: No lock needed — this CPU is the only writer for its own copy!
; Even if interrupted, the interrupt handler would use the same per-CPU
; data (it's on the same CPU), so no corruption.
```

---

## Detailed: ARM64 `this_cpu_inc(my_counter)` (Read-Modify-Write)

```asm
; this_cpu_inc(my_counter) on ARM64:

; Instruction 1: Get per-CPU base
mrs   x2, tpidr_el1              ; x2 = per-CPU offset

; Instruction 2: Compute address
add   x2, x2, #<my_counter_offset>   ; x2 = &this_cpu(my_counter)

; Instruction 3: Load + increment + store using LSE (if available)
; With ARMv8.1 LSE (Large System Extensions):
ldadd w0, w1, [x2]              ; atomic add (but per-CPU doesn't need this!)

; Without LSE (plain load-store):
ldr   w0, [x2]                  ; load
add   w0, w0, #1                ; increment
str   w0, [x2]                  ; store
```

---

## `arch_raw_cpu_ptr()` — The Architecture Hook

```c
/* This is how the architecture plugs into the generic percpu code: */

/* ARM32: arch/arm/include/asm/percpu.h */
#define arch_raw_cpu_ptr(ptr)   \
    SHIFT_PERCPU_PTR(ptr, __my_cpu_offset)

/* ARM64: arch/arm64/include/asm/percpu.h */
#define arch_raw_cpu_ptr(ptr)   \
    SHIFT_PERCPU_PTR(ptr, __my_cpu_offset)
/* __my_cpu_offset = __kern_my_cpu_offset() = mrs tpidr_el1/el2 */

/* Both architectures:
 * SHIFT_PERCPU_PTR(ptr, offset) = (typeof(*ptr) *)(ptr + offset)
 *                                = RELOC_HIDE(ptr, offset)
 */
```

---

## Addressing Modes: How the Compiler Combines Instructions

### ARM32 Addressing Capability

ARM32 LDR/STR supports:
- Base + 12-bit unsigned immediate: `ldr r0, [r1, #0x123]`
- Base + shifted register: `ldr r0, [r1, r2, lsl #2]`

For per-CPU access, the compiler computes:
```
effective_address = TPIDRPRW_value + (template_addr - __per_cpu_start)
```
If the offset fits in 12 bits, the add and load merge into one LDR instruction.
If not, multiple ADD + LDR instructions.

### ARM64 Addressing Capability

ARM64 LDR/STR supports:
- Base + 12-bit scaled unsigned offset: `ldr w0, [x1, #0x10]` (scales by operand size)
- Base + 64-bit sign-extended register: `ldr w0, [x1, x2, sxtx]`
- Base + register shift: `ldr w0, [x1, x2, lsl #2]`

For per-CPU access:
```
effective_address = tpidr_el1_value + (template_addr - __per_cpu_start)
```
ARM64 LDR can often fold the add into the addressing mode directly.

---

## Preemption Safety: Why These Accesses Are Preemption-Dependent

### The "Q" Constraint Issue (Both ARM32 and ARM64)

```c
/* ARM64 __kern_my_cpu_offset: */
asm volatile(ALTERNATIVE("mrs %0, tpidr_el1", ...)
             : "=r"(off)
             : "Q"(*(unsigned long *)NULL));
/* The "Q" constraint is an artificial memory input.
 * It tells the compiler:
 *   "Treat this as if it reads from memory address NULL"
 * This prevents the compiler from moving the MRS instruction
 * above any preceding memory stores.
 *
 * Without "Q": compiler could hoist MRS above a preempt_disable()
 * call, reading stale per-CPU offset.
 */
```

### The Preemption Window

```c
/* DANGEROUS (without preempt disable): */
void *ptr = this_cpu_ptr(&my_var);  /* reads tpidr_el1 on CPU 0 */
                                     /* <-- task migrates to CPU 1 here! */
*ptr = 42;                           /* writes to CPU 0's copy on CPU 1 */
                                     /* WRONG! ptr points to CPU 0's data */

/* SAFE: */
preempt_disable();                   /* OR: disable_irq, spin_lock, etc. */
void *ptr = this_cpu_ptr(&my_var);  /* reads tpidr_el1 on current CPU */
*ptr = 42;                           /* current CPU cannot change */
preempt_enable();
```

---

## Complete Reference: `this_cpu_*` Operation Expansion

| Operation | What it expands to | Preempt needed? |
|---|---|---|
| `this_cpu_ptr(&var)` | `SHIFT_PERCPU_PTR(&var, __my_cpu_offset)` | Yes (caller) |
| `this_cpu_read(var)` | `*this_cpu_ptr(&var)` | Yes (caller) |
| `this_cpu_write(var, v)` | `*this_cpu_ptr(&var) = v` | Yes (caller) |
| `this_cpu_add(var, v)` | `*this_cpu_ptr(&var) += v` | Yes (caller) |
| `this_cpu_inc(var)` | `*this_cpu_ptr(&var) += 1` | Yes (caller) |
| `get_cpu_var(var)` | `preempt_disable(); *this_cpu_ptr(&var)` | Builtin |
| `put_cpu_var(var)` | `preempt_enable()` | Releases |
| `raw_cpu_ptr(&var)` | Same but no debug assertions | Yes (caller) |
| `__this_cpu_read(var)` | Preemption-unsafe (caller must be in irq ctx) | No |

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| How many instructions to access a per-CPU var? | 3: mrs/mrc + add + ldr/str |
| ARM32 read instruction? | `mrc p15, 0, Rd, c13, c0, 4` |
| ARM64 read instruction? | `mrs Xd, tpidr_el1` (or `tpidr_el2`) |
| Is per-CPU access atomic? | No locking needed — only one CPU writes its own copy |
| Can compiler hoist mrs above preempt_disable? | Prevented by "Q" constraint in __kern_my_cpu_offset |
| What is arch_raw_cpu_ptr? | Arch hook that calls SHIFT_PERCPU_PTR with hardware register |
| What does RELOC_HIDE prevent? | Type-based aliasing analysis reordering the pointer computation |
| Can ARM64 fold add into LDR? | Yes — base + offset addressing mode merges add + load |
