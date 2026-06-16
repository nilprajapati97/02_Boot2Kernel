# Day 01 — Project Skeleton & First Boot

> **Goal**: Get an empty AArch64 ELF kernel loaded by QEMU `virt` to a known-good idle loop, with a working cross-compile build system.
>
> **Why today**: Nothing else works without a build that produces a properly linked ELF at the correct load address and an entry point that QEMU can jump to. We establish the foundation — Makefile, linker script, `head.S`, and a one-line `kmain` — so every subsequent day plugs into a working `make && ./tools/qemu-run.sh` loop.

---

## 1. Background Theory

### 1.1 What QEMU virt gives us
- `qemu-system-aarch64 -M virt -cpu cortex-a72 -kernel image.elf` loads the ELF, sets `PC = entry_point`, hands us:
  - `x0` = physical address of a Flattened Device Tree (FDT/DTB) blob.
  - `x1, x2, x3` = 0 (reserved).
  - CPU in **EL2** by default (with `-cpu cortex-a72`), MMU off, caches off, interrupts masked.
  - RAM at `0x4000_0000`; convention is to load the kernel at `0x4008_0000` (512 KiB above RAM base, leaving room for DTB).
- All cores are released simultaneously — we must park secondaries early using `MPIDR_EL1` to avoid races.

### 1.2 AArch64 registers we touch today
| Register | Purpose |
|---|---|
| `MPIDR_EL1` | CPU affinity; bits[7:0] = Aff0 ≈ CPU id |
| `CurrentEL` | Bits[3:2] = current EL (2 = EL2, 1 = EL1) |
| `SP_EL1` / `SP` | Stack pointer |
| `x0`–`x3` | Boot args from QEMU |
| `x29` (FP), `x30` (LR) | Frame/return — kept zero initially |

### 1.3 ELF & linker script essentials
- Load address ≠ link address (yet — that comes Day 7). Today both = `0x4008_0000`.
- Sections in load order: `.text`, `.rodata`, `.data`, `.bss`. Provide `__bss_start`/`__bss_end` symbols for clearing.

---

## 2. Design

### 2.1 Directory layout created today
```
nkernel/
├── Makefile
├── linker.ld
├── arch/arm64/boot/head.S
├── kernel/main.c
├── include/kernel/types.h
├── tools/qemu-run.sh
└── docs/day01-boot.md
```

### 2.2 Boot flow diagram
```
QEMU                                Our code
─────                               ────────
load ELF @ 0x40080000
PC = _start, x0 = DTB phys
        │
        ▼
  head.S:_start ───► mrs MPIDR_EL1
                     test Aff0 == 0?
                       │      └─ secondaries: wfe; b 1b (park)
                       ▼
                     adrp/add SP = __stack_top
                     bl  bss_clear
                     mov x19, x0           ; preserve DTB
                     mov x0,  x19          ; pass to kmain
                     bl  kmain
                     b   .                 ; safety halt
```

---

## 3. Implementation Steps

### 3.1 `linker.ld`
```ld
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : ALIGN(4096) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) { *(.rodata .rodata.*) }
    .data   : ALIGN(4096) { *(.data   .data.*)   }

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(4096);
    __stack_bottom = .;
    . += 0x4000;                 /* 16 KiB boot stack */
    __stack_top = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
```

### 3.2 `arch/arm64/boot/head.S`
```asm
/* arch/arm64/boot/head.S — first instructions executed by QEMU */
    .section ".text.boot","ax"
    .global _start
_start:
    /* Park secondary CPUs */
    mrs     x1, MPIDR_EL1
    and     x1, x1, #0xFF              // Aff0
    cbnz    x1, .Lpark

    /* Set up boot stack */
    adrp    x1, __stack_top
    add     x1, x1, :lo12:__stack_top
    mov     sp, x1

    /* Clear BSS */
    adrp    x1, __bss_start
    add     x1, x1, :lo12:__bss_start
    adrp    x2, __bss_end
    add     x2, x2, :lo12:__bss_end
1:  cmp     x1, x2
    b.ge    2f
    str     xzr, [x1], #8
    b       1b
2:
    /* x0 still holds DTB phys from QEMU; pass through */
    bl      kmain

    /* Should never return */
3:  wfe
    b       3b

.Lpark:
    wfe
    b       .Lpark
```

### 3.3 `kernel/main.c`
```c
/* kernel/main.c */
#include <kernel/types.h>

void kmain(u64 dtb_phys)
{
    (void)dtb_phys;          /* Day 3 uses it */
    for (;;)
        asm volatile("wfe");
}
```

### 3.4 `include/kernel/types.h`
```c
#ifndef _KERNEL_TYPES_H
#define _KERNEL_TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long       u64;
typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef signed long         s64;

typedef u64                 uintptr_t;
typedef u64                 size_t;
typedef s64                 ssize_t;

#define NULL                ((void *)0)
#define BIT(n)              (1UL << (n))

#endif
```

### 3.5 `Makefile`
```make
CROSS   ?= aarch64-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

CFLAGS  := -ffreestanding -nostdlib -nostartfiles -mgeneral-regs-only \
           -fno-stack-protector -fno-pic -mcmodel=large \
           -O2 -g -Wall -Wextra -Werror \
           -Iinclude

ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -T linker.ld

OBJS := build/arch/arm64/boot/head.o \
        build/kernel/main.o

build/nkernel.elf: $(OBJS) linker.ld
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

build/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

clean:
	rm -rf build

run: build/nkernel.elf
	./tools/qemu-run.sh

-include $(OBJS:.o=.d)
.PHONY: clean run
```

### 3.6 `tools/qemu-run.sh`
```bash
#!/usr/bin/env bash
set -euo pipefail
exec qemu-system-aarch64 \
    -M virt,gic-version=2 -cpu cortex-a72 -smp 1 -m 512M \
    -nographic -kernel build/nkernel.elf \
    "$@"
```
Make executable: `chmod +x tools/qemu-run.sh`.

---

## 4. Files Created Today

| Path | Purpose |
|---|---|
| `Makefile` | Build orchestration |
| `linker.ld` | ELF layout, symbols, load addr |
| `arch/arm64/boot/head.S` | `_start`, secondary park, BSS clear |
| `kernel/main.c` | `kmain()` stub |
| `include/kernel/types.h` | Fixed-width types |
| `tools/qemu-run.sh` | QEMU launch script |

---

## 5. ARM64 Cheat-Sheet (Day 1)

```
adrp Xn, sym    ; Xn = page address of sym (PC-relative ±4 GiB)
add  Xn, Xn, :lo12:sym  ; add 12-bit offset within page
mrs  Xn, SYSREG ; move system register into general
wfe             ; wait for event (low-power halt)
cbnz Xn, lbl    ; branch if Xn != 0
```

---

## 6. Pitfalls

1. **Forgetting `.text.boot` linker placement** → `_start` not at image base; QEMU jumps into garbage.
2. **Stack misalignment**: SP must be 16-byte aligned at function entry. `__stack_top` aligned to 4 KiB satisfies this.
3. **`mcmodel=large`**: required because the higher-half kernel (Day 7) uses 64-bit addresses; using it from Day 1 avoids relinking surprises later.
4. **DTB pointer clobbered**: do not touch `x0` between `_start` and `bl kmain` — that's our boot arg.
5. **Caches/MMU off**: any data written before MMU enable must be visible to subsequent reads — fine while single-core EL2, but remember when enabling SMP.

---

## 7. Verification

### Build
```bash
make
file build/nkernel.elf      # ELF 64-bit LSB executable, ARM aarch64
```

### Run
```bash
make run
# Expect: QEMU enters, hangs silently. Ctrl-A X to quit.
```

### GDB sanity check
```bash
./tools/qemu-run.sh -s -S &
gdb-multiarch build/nkernel.elf
(gdb) target remote :1234
(gdb) info reg pc        # should be 0x40080000
(gdb) info reg x0        # non-zero (DTB phys)
(gdb) si 20              # step a few instrs; sp loaded; bss cleared
(gdb) bt
```
**Success marker (CI)**: `gdb` shows `pc == 0x40080000` at break, then `kmain` reachable.

---

## 8. Stretch Goals

- Generate `build/nkernel.bin` via `objcopy -O binary` (for raw boot on real hardware).
- Add `make debug` target that runs QEMU `-s -S` and launches GDB in tmux.
- Print `_start` address via `nm build/nkernel.elf | grep _start`.

---

## 9. References

- ARM ARM §D1.6 (Exception model), §G1 (AArch64 system registers).
- QEMU `hw/arm/virt.c` — memory map constants.
- Linux `arch/arm64/kernel/head.S` (read for inspiration; do not copy).
