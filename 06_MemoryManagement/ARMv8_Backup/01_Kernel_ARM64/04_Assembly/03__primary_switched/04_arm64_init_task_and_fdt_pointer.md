`str_l x21, __fdt_pointer, x5` means:

**Save the bootloader-provided Device Tree Blob pointer into the global kernel variable `__fdt_pointer`.**

In interview language:

```asm
str_l   x21, __fdt_pointer, x5
```

is storing the value in `x21` into memory at the address of `__fdt_pointer`, using `x5` as a temporary register.

### What is in `x21`?

On ARM64 Linux boot, the bootloader passes the **FDT pointer** in `x0`.

FDT means **Flattened Device Tree**. It describes the hardware layout of the board:

```text
CPU information
RAM banks
UART
interrupt controller
timers
reserved memory
chosen bootargs
initrd location
NVIDIA-specific devices
```

Early boot code preserves that pointer, and by the time this line runs, `x21` contains the FDT physical address.

So:

```asm
x21 = physical address of the Device Tree Blob
```

### What is `__fdt_pointer`?

`__fdt_pointer` is a global kernel variable used to remember where the device tree is located.

Conceptually:

```c
phys_addr_t __fdt_pointer;
```

After this instruction:

```c
__fdt_pointer = x21;
```

So later kernel code can find the device tree and parse board information.

### What is `str_l`?

Like `adr_l`, `str_l` is not a real ARM64 instruction. It is an assembler macro.

It usually expands into something like:

```asm
adr_l   x5, __fdt_pointer
str     x21, [x5]
```

Meaning:

1. Compute the address of `__fdt_pointer`
2. Store `x21` into that address

`x5` is just a temporary scratch register used to hold the address.

### Why is this important?

The kernel needs the FDT later to understand the board.

For an NVIDIA platform, the device tree may describe things like:

```text
memory layout
GIC interrupt controller
UART console
PCIe controllers
GPU-related nodes
IOMMU/SMMU
reserved carveouts
clocks and resets
boot arguments
```

Without saving this pointer, the kernel could lose access to the hardware description passed by firmware or U-Boot.

### Memory perspective

This instruction writes into kernel memory:

```text
Before:
__fdt_pointer = unknown / 0

x21 = address of FDT blob

After:
__fdt_pointer = address of FDT blob
```

Diagram:

```text
x21
 │
 │ contains FDT physical address
 ▼
+----------------------+
| Device Tree Blob     |
| /memory              |
| /chosen              |
| /cpus                |
| /soc                 |
+----------------------+

__fdt_pointer
 │
 ▼
stores same address
```

### CPU perspective

The CPU is doing a normal store:

```asm
str x21, [address_of___fdt_pointer]
```

It does **not** parse the device tree here.

It only saves the pointer so C code later can parse it.

### Interview-ready explanation

You can say:

> `str_l x21, __fdt_pointer, x5` saves the preserved FDT physical address into the global variable `__fdt_pointer`. The bootloader passes the device tree pointer to the kernel, and Linux must keep it because later boot code parses the FDT to discover RAM, CPUs, interrupts, reserved memory, boot arguments, and board-specific devices. `str_l` is an assembler macro: it uses `x5` as a temporary register to calculate the address of `__fdt_pointer`, then stores `x21` there. This line does not parse the device tree; it only preserves the pointer for later kernel initialization.
