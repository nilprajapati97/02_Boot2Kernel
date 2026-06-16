Installs the exception vector table into VBAR_EL1
=========================================================

	adr_l	x8, vectors			// load VBAR_EL1 with virtual
	msr	vbar_el1, x8			// vector table address
	isb



What this instruction means:

adr_l x8, vectors computes the virtual address of the kernel exception vector table symbol named vectors and places it in register x8.

Where vectors is defined:

The actual vector table is in entry.S.  
It is 2 KB aligned via .align 11 at entry.S, matching ARM64 architectural requirements for VBAR base alignment.

What this table contains conceptually:

The vectors table has 16 slots:
1. EL1t sync
2. EL1t IRQ
3. EL1t FIQ
4. EL1t SError
5. EL1h sync
6. EL1h IRQ
7. EL1h FIQ
8. EL1h SError
9. EL0 64-bit sync
10. EL0 64-bit IRQ
11. EL0 64-bit FIQ
12. EL0 64-bit SError
13. EL0 32-bit sync
14. EL0 32-bit IRQ
15. EL0 32-bit FIQ
16. EL0 32-bit SError

SYM_CODE_START(vectors)
	kernel_ventry	1, t, 64, sync		// Synchronous EL1t
	kernel_ventry	1, t, 64, irq		// IRQ EL1t
	kernel_ventry	1, t, 64, fiq		// FIQ EL1t
	kernel_ventry	1, t, 64, error		// Error EL1t

	kernel_ventry	1, h, 64, sync		// Synchronous EL1h
	kernel_ventry	1, h, 64, irq		// IRQ EL1h
	kernel_ventry	1, h, 64, fiq		// FIQ EL1h
	kernel_ventry	1, h, 64, error		// Error EL1h

	kernel_ventry	0, t, 64, sync		// Synchronous 64-bit EL0
	kernel_ventry	0, t, 64, irq		// IRQ 64-bit EL0
	kernel_ventry	0, t, 64, fiq		// FIQ 64-bit EL0
	kernel_ventry	0, t, 64, error		// Error 64-bit EL0

	kernel_ventry	0, t, 32, sync		// Synchronous 32-bit EL0
	kernel_ventry	0, t, 32, irq		// IRQ 32-bit EL0
	kernel_ventry	0, t, 32, fiq		// FIQ 32-bit EL0
	kernel_ventry	0, t, 32, error		// Error 32-bit EL0
SYM_CODE_END(vectors)



You can see these entries laid out in entry.S through entry.S.

Why this is needed exactly here:

Before this point, the CPU may still be using reset firmware vectors or undefined state for exception entry.  
After this point, any exception taken at EL1 routes into Linux-owned entry paths in .entry.text.  
Without this setup, the first interrupt or sync exception after MMU-on could branch to wrong memory and crash.

CPU-side technical effect:

1. adr_l loads the base address candidate into x8.
2. msr vbar_el1, x8 programs the architectural exception-vector base register for EL1.
3. isb makes that system register write architecturally visible before any following instruction stream can take exceptions.

That ISB is not optional in practice: it serializes context so exception redirection is guaranteed active before continuing.

Memory-side technical effect:

1. vectors lives in executable kernel text section .entry.text at entry.S.
2. The address loaded is a virtual address, so this must happen after kernel virtual mappings are active.
3. The table must be mapped executable and aligned; both are satisfied by kernel link/layout and section attributes.

Board/platform perspective:

On real hardware, asynchronous events can arrive any time:
- timer interrupt
- external interrupt controller signal
- synchronous abort from bad access
- SError from interconnect/memory system

Once VBAR_EL1 is pointed at vectors, all those events enter Linux’s controlled prologue logic instead of firmware leftovers. This is a core ownership transition from boot firmware world to kernel world.

Why adr_l is used (not absolute move):

It is PC-relative and link-time-relocatable, so it remains valid with kernel relocation/KASLR.  
So the CPU does not “discover” vectors dynamically; linker and relocation provide the correct address, and adr_l reconstructs it at runtime.

One-line summary:

This instruction is the address-setup half of “install Linux exception entry”; together with msr vbar_el1 and isb, it makes the boot CPU start handling all EL1 exceptions through the kernel’s official vector table.


Great question. In this line, “load” means load the address, not load table contents.

How it loads:

1. head.S uses adr_l, which expands to:
- adrp x8, vectors
- add x8, x8, :lo12:vectors

2. That computes the full virtual address of the symbol vectors (defined at entry.S) using PC-relative math:
- adrp gets the page base
- add adds the low 12-bit offset in that page

3. Immediately after, head.S writes x8 into VBAR_EL1:
- msr vbar_el1, x8

4. Then head.S does isb so the new vector base is guaranteed active before execution continues.

Important: no memory read of the vector table happens here. The CPU is only computing an address and programming a system register.

Why load into x8 specifically:

- VBAR_EL1 write needs a GPR source operand, so some x-register must hold the address first.
- x8 is a caller-saved scratch register and is free at this point in this routine.
- Nearby live values are being kept in other registers (for example x20 boot mode, x21 FDT pointer), so x8 is a safe temporary that won’t disturb boot state.
- Architecturally it is not special for VBAR; x9/x10/etc could also work if free. x8 is chosen because it is convenient and non-conflicting in this code path.

Why this is done at all:

- VBAR_EL1 tells the CPU where EL1 exception vectors start.
- Without setting it, interrupts/exceptions could enter wrong handlers.
- After this write, exceptions route into Linux vectors in entry.S.


**3-Instruction Summary**

At head.S, head.S, and head.S, the kernel does three tightly-related things:

1. Compute the virtual address of the Linux exception vector table.
2. Program that address into the EL1 exception-vector base register.
3. Force the CPU to start using that new exception base immediately.

In one sentence:
this is the moment the boot CPU starts taking exceptions through Linux’s own EL1 exception-entry code instead of whatever reset/firmware state existed before.

**The 3 Instructions**

1. adr_l x8, vectors
2. msr vbar_el1, x8
3. isb

The vector table itself starts at entry.S.

**What Each One Does**

**1. adr_l x8, vectors**

This does not load vector contents.
It computes the address of the symbol vectors and places that address into general-purpose register x8.

Technically:
- adr_l is a macro from assembler.h
- it expands to:
  - adrp x8, vectors
  - add x8, x8, :lo12:vectors

Meaning:
- adrp gets the 4 KB page containing vectors, relative to the current PC
- add adds the low 12-bit offset inside that page
- final result: x8 = full virtual address of vectors

Why virtual address?
Because by the time execution reaches head.S, the MMU is already enabled in head.S, and this code is running in the kernel’s virtual address space.

So this instruction is really:
“Find where the kernel’s exception entry table lives in the current virtual mapping.”

**2. msr vbar_el1, x8**

This writes the value in x8 into the architectural system register VBAR_EL1.

What is VBAR_EL1?
VBAR_EL1 = Vector Base Address Register for EL1.

This register tells the CPU:
“When an exception is taken to EL1, the exception entry stubs start at this base address.”

That means every EL1 exception taken after this point will use the vector table rooted at the address stored in VBAR_EL1.

The table is structured exactly as the ARM64 architecture expects:
- 16 vector slots
- each slot is 128 bytes
- grouped by source EL and stack mode
- visible at entry.S through entry.S

These entries cover:
- EL1t sync / irq / fiq / error
- EL1h sync / irq / fiq / error
- EL0 64-bit sync / irq / fiq / error
- EL0 32-bit sync / irq / fiq / error

So writing VBAR_EL1 is the act of installing Linux’s official exception-entry dispatch table.

**3. isb**

ISB means Instruction Synchronization Barrier.

This is not about memory visibility like DMB/DSB.
This is about execution context synchronization.

Why it is needed:
writing a system register like VBAR_EL1 does not necessarily take effect for subsequent instruction fetch / exception behavior immediately unless the architecture says it is synchronized.

The ISB forces the CPU to:
- flush the pipeline context
- recognize the new system-register state
- ensure subsequent exceptions use the new vector base

Without the ISB, there could be a transient window where:
- software thinks VBAR_EL1 is updated
- but the processor might still take an exception using the old context

So ISB closes that window.

**What This 3-Instruction Sequence Achieves As A Unit**

Together, these three instructions mean:

“Install Linux exception entry at EL1, and make that installation active now.”

Before this point:
- the CPU may still be using reset-time exception configuration
- exceptions might still point to firmware vectors, stale vectors, or architecturally unknown state
- taking an IRQ or synchronous fault would be unsafe

After this point:
- any exception taken to EL1 will enter Linux code in entry.S
- the kernel can safely handle:
  - synchronous exceptions
  - interrupts
  - system errors
  - exceptions from EL0
  - exceptions from EL1 itself

**Why It Happens Exactly There**

This is important.

This sequence appears in head.S after:
- the MMU has been enabled
- the initial current task and stack have been established
- the CPU has enough runtime context to survive an exception

It appears before:
- deeper kernel initialization
- general C code in start_kernel
- code paths that may trigger faults or receive interrupts

So this location is intentional.

Why not earlier?
Because earlier:
- kernel virtual mapping may not yet be active
- vectors is a kernel virtual symbol
- VBAR_EL1 must be loaded with a valid executable address in the current translation regime

Why not later?
Because later:
- the CPU may already be exposed to real asynchronous interrupts or synchronous faults
- you want Linux exception entry installed before doing more complex work

So this is placed at the earliest safe point after virtual execution becomes valid and before normal kernel activity begins.

**Why x8 Is Used**

x8 is not special for VBAR_EL1.
Any free general-purpose register could hold the address before the msr.

It is chosen because:
- it is caller-saved / scratch-friendly in this context
- it does not collide with live boot-state registers like x20 and x21
- the code immediately consumes it in the next instruction, so it is a good temporary

So the rule is:
VBAR_EL1 cannot be loaded directly from a symbol; the address must first be in a GPR.
x8 is simply the chosen temporary.

**What The CPU Does Later With VBAR_EL1**

Once VBAR_EL1 points to vectors:
- if an interrupt arrives, hardware computes the appropriate vector offset
- it adds that offset to VBAR_EL1
- it branches to the corresponding entry slot in the vector table

Conceptually:

Exception entry address = VBAR_EL1 + vector_slot_offset

The slot offset depends on:
- source exception level
- stack mode
- exception class group

Then execution enters one of the vector stubs generated by kernel_ventry at entry.S.

Those stubs do the early hardening and register-save work:
- reserve pt_regs space
- handle stack overflow detection
- save registers
- switch context details as needed
- route to the proper handler

So VBAR_EL1 is effectively the root pointer for the whole exception subsystem.

**Advantages**

1. Early correctness
Linux gets control of exceptions as soon as it is safe.

2. Deterministic exception routing
All exceptions now enter the kernel through a known, architecture-compliant table.

3. Separation from firmware
The kernel no longer depends on bootloader or firmware exception setup.

4. Supports both kernel and user exception entry
The installed table includes entries for EL1-originated and EL0-originated exceptions.

5. KASLR-safe and relocation-safe
Using adr_l means the vectors address is computed PC-relatively, so the code still works when the kernel image is relocated.

6. Fast dispatch
VBAR_EL1-based vectoring is direct hardware routing. No software lookup is needed at exception time.

7. Safe activation point
The ISB ensures the change is active exactly when software expects it to be.

**How It Helps The System**

It helps in four main ways.

1. Interrupt handling
If a timer interrupt or GIC interrupt arrives, the CPU knows exactly where Linux’s IRQ entry code begins.

2. Fault handling
If the kernel or user mode triggers a synchronous exception, Linux’s exception-entry path receives control instead of crashing into undefined territory.

3. Security and hardening
The entry code in entry.S includes stack handling, sanitization, overflow checks, and architecture-specific mitigations. None of that works unless VBAR_EL1 points there.

4. Transition to normal kernel life
This is one of the last core architecture handoff steps before the machine can behave like a normal Linux system rather than a bootstrapping CPU.

**Mental Model**

Think of these three instructions as:

- adr_l x8, vectors
  Find Linux’s exception front door.

- msr vbar_el1, x8
  Tell the CPU that this is now the official front door.

- isb
  Make the CPU acknowledge the change immediately.

**Bottom Line**

This 3-instruction block installs and activates the EL1 exception entry root for Linux.

Without it:
- interrupts may go to the wrong place
- faults may be unhandleable
- early kernel execution is unsafe

With it:
- the CPU can safely take exceptions into Linux-managed entry code
- the kernel now owns exception dispatch at EL1
- the machine is much closer to stable normal execution inside start_kernel

If you want, I can do the next step and explain exactly how one specific vector slot, for example EL1h IRQ or EL0 sync, is selected from the vectors table by hardware.