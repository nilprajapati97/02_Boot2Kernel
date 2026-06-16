# ARM64 Early Boot: `kasan_early_init` and `mov x0, x20`

## Code snippet

```asm
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
    bl  kasan_early_init
#endif
    mov x0, x20
```

## One-line explanation

This code conditionally initializes KASAN during early ARM64 kernel boot, then restores the CPU boot-mode value into `x0` so it can be passed to the next function call.

---

## Where this appears

This code appears during early ARM64 Linux boot, around the `__primary_switched` stage.

At this point:

- The MMU is already enabled.
- The kernel is executing using virtual addresses.
- Early boot assembly is transitioning toward normal C kernel initialization.
- Some important boot-time values are still being preserved in registers such as `x20`.

---

## What is KASAN?

KASAN means **Kernel Address Sanitizer**.

It is a debugging feature used to detect memory bugs inside the Linux kernel, such as:

- Out-of-bounds access
- Use-after-free
- Invalid memory access
- Stack/global memory corruption

KASAN is mainly used during kernel development and debugging. It is usually not enabled in production builds because it adds memory and runtime overhead.

---

## Why the `#if` condition exists

```c
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
```

This means the call to `kasan_early_init` is included only if the kernel was built with one of these KASAN modes enabled.

### `CONFIG_KASAN_GENERIC`

This is the generic software-based KASAN mode.

It uses shadow memory to track whether kernel memory is valid or poisoned.

### `CONFIG_KASAN_SW_TAGS`

This is software tag-based KASAN.

It is used on ARM64 and relies on memory tagging concepts, but implemented in software rather than requiring full hardware memory tagging support.

If neither option is enabled, this code is removed at compile time.

So in a normal non-KASAN build, the boot flow skips this call completely.

---

## Instruction: `bl kasan_early_init`

```asm
bl kasan_early_init
```

`bl` means **Branch with Link**.

It performs a function call:

```c
kasan_early_init();
```

The CPU jumps to `kasan_early_init`, and the return address is saved in `x30`, also called the link register.

When `kasan_early_init` finishes, execution returns to the next instruction.

---

## What does `kasan_early_init` do?

At a high level, `kasan_early_init` prepares KASAN shadow memory very early in boot.

KASAN cannot detect memory bugs unless it has shadow memory ready.

Conceptually:

```text
Kernel memory address  --->  KASAN shadow memory
```

The shadow memory records whether parts of kernel memory are valid or poisoned.

For example:

```text
Normal kernel memory:
[ valid object ][ redzone ][ freed memory ]

KASAN shadow memory:
[ accessible ][ poisoned ][ poisoned ]
```

When the kernel accesses memory, KASAN checks the corresponding shadow memory to detect invalid accesses.

---

## Why is this done so early?

KASAN must be initialized before the kernel starts using many memory-management and allocator paths.

If KASAN starts too late, early memory bugs may go undetected.

So during early boot, the kernel sets up enough KASAN infrastructure before moving further into normal initialization.

This is especially important for:

- Early page-table setup
- Boot memory allocator usage
- Kernel stack checking
- Static kernel memory checking
- Detecting bugs before `start_kernel()` fully runs

---

## Important register detail

After the optional KASAN call, the code does:

```asm
mov x0, x20
```

This is important because ARM64 uses `x0` as the first function argument.

But `bl kasan_early_init` may modify caller-saved registers such as `x0`.

So the kernel reloads the needed value from `x20` into `x0`.

---

## What is in `x20`?

In this boot path, `x20` preserves the CPU boot mode.

The boot mode tells the kernel whether the CPU entered Linux at a particular ARM exception level, commonly EL1 or EL2.

Conceptually:

```text
x20 = boot CPU mode
```

This value must be passed to later boot code, such as:

```asm
bl set_cpu_boot_mode_flag
```

So before that call, the kernel prepares the argument:

```asm
mov x0, x20
```

Meaning:

```c
set_cpu_boot_mode_flag(x20);
```

---

## Why not keep the value directly in `x0`?

Because `x0` is caller-saved according to the ARM64 calling convention.

That means when a function is called, the called function is allowed to overwrite `x0`.

So the kernel keeps important long-lived boot data in a more stable register such as `x20`, then copies it into `x0` only when needed as a function argument.

This is a very common low-level assembly pattern:

```asm
preserve important value in x20
call optional function
restore argument into x0
call next function
```

---

## CPU-level view

The CPU executes this as:

```text
If KASAN is enabled:
    call kasan_early_init

Then:
    x0 = x20
```

So the final guaranteed state is:

```text
x0 contains boot mode
```

regardless of whether KASAN was enabled or disabled.

---

## Memory-management view

This snippet is connected to early memory safety.

If KASAN is enabled, the kernel prepares shadow memory before continuing deeper into boot.

This helps catch bugs in memory accesses during early initialization.

Without this initialization, KASAN checks would not have correct shadow memory state.

---

## Board/platform view

On platforms such as NVIDIA Jetson, NVIDIA server platforms, ARM development boards, or AMD systems using ARM-based firmware paths, early boot must correctly initialize debugging and memory-detection features without breaking the boot register state.

This snippet shows two important responsibilities:

1. Initialize optional memory debugging infrastructure.
2. Preserve and restore critical boot information for the next initialization step.

---

## Interview-ready explanation

You can explain it like this:

> This code conditionally calls `kasan_early_init` if the kernel was built with KASAN support, either generic KASAN or software tag-based KASAN. KASAN is the Kernel Address Sanitizer, used to detect memory bugs such as out-of-bounds access and use-after-free. Since this is early boot, KASAN needs to set up its shadow memory before normal kernel initialization continues. After that call, the code executes `mov x0, x20` because `x0` is the first argument register in the ARM64 calling convention and may have been clobbered by the function call. The boot mode is preserved in `x20`, so it is copied back into `x0` before the next function uses it, such as `set_cpu_boot_mode_flag`.

---

## Short interview answer

> If KASAN is enabled, the kernel initializes early KASAN support before continuing boot. Then it moves `x20` into `x0` because `x20` holds the preserved CPU boot mode, and `x0` is used to pass the first argument to the next function. This ensures the boot mode is correctly passed even if the KASAN call modified caller-saved registers.

---

## Best filename

Recommended filename:

```text
arm64_kasan_early_init_explained.md
```

Other good options:

```text
linux_arm64_kasan_boot_init.md
arm64_primary_switched_kasan.md
arm64_early_boot_kasan_notes.md
```

