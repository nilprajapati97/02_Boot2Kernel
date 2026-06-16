	stp	x29, x30, [sp, #-16]!
	mov	x29, sp

	Technical aspect only:

- `stp x29, x30, [sp, #-16]!`
  - pre-indexed store pair
  - allocates 16 bytes on the current stack frame
  - writes:
    - `[sp + 0] = old x29`
    - `[sp + 8] = old x30`
  - maintains mandatory AArch64 16-byte stack alignment
  - saves:
    - `x29` = caller frame pointer
    - `x30` = link register / return address

- `mov x29, sp`
  - establishes the new frame pointer
  - `x29` now points to the frame record just pushed on stack

What this forms:

- standard AArch64 frame record
- frame layout is:

```text
sp/x29 -> saved previous x29
         saved previous x30
```

Technical purpose:

- creates a canonical call frame for `__primary_switched`
- allows nested `bl` calls without losing current return linkage
- makes unwinders/backtrace logic able to walk this frame
- conforms to AAPCS64 frame-chain convention

Why needed here:

- after this point `__primary_switched` calls multiple functions:
  - `set_cpu_boot_mode_flag`
  - `kasan_early_init`
  - `finalise_el2`
  - `start_kernel`
- each `bl` overwrites `x30`
- therefore current `x30` must be preserved on stack before further calls
- `x29` is updated so this function becomes a valid node in the frame chain

Net effect:

- stack gets a real function frame
- current function becomes unwindable
- return address becomes durable across subsequent subroutine calls
- control-flow metadata moves from only registers into stack-backed state