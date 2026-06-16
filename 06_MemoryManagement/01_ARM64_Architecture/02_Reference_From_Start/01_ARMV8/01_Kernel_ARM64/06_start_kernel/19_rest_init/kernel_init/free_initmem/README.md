# `free_initmem()` — Reclaim Boot-Only Memory

## Purpose

Frees the `.init.text` and `.init.data` sections of the kernel, which contain code and data only needed during boot. This typically reclaims 1–4 MB of memory.

## Source File

`mm/init-mm.c` / architecture-specific

```c
void free_initmem(void)
{
    free_initmem_default(POISON_FREE_INITMEM);
}

void __ref free_initmem_default(int poison)
{
    extern char __init_begin[], __init_end[];
    
    poison_init_mem(__init_begin, __init_end - __init_begin);
    
    if (!machine_is_protected_virtualization())
        set_memory_nx((unsigned long)__init_begin,
                      (__init_end - __init_begin) >> PAGE_SHIFT);
    
    free_reserved_area(__init_begin, __init_end,
                       poison, "unused kernel");
}
```

## What is `__init`?

The `__init` attribute tells the linker to place the function in the `.init.text` section:

```c
// This function is discarded after boot:
void __init setup_arch(char **cmdline_p) { ... }

// This data is discarded after boot:
static char __initdata my_boot_config[1024];

// This causes a kernel symbol to be placed in .init.text:
module_init(my_driver_init)   // Only if compiled as built-in, not module
```

## Kernel Sections Layout

```
vmlinux ELF sections:
.text           ← permanently resident kernel code
.data           ← kernel data (globals, etc.)
.bss            ← uninitialized data
.init.text      ← boot code (freed after boot) ← __init functions
.init.data      ← boot data (freed after boot) ← __initdata
.initcall*.init ← initcall function pointers
.con_initcall*  ← console init calls
.init.setup     ← __setup() and early_param() table
```

## Memory Savings

```
kernel: Freeing unused kernel image (initmem) memory: 2884K
```

Typical savings:
- Minimal kernel: ~1 MB
- Full desktop kernel: ~3-5 MB
- Large server kernel: ~5-8 MB

## Poison Value

`POISON_FREE_INITMEM` (0xCC on most architectures) is written to freed memory. This means any code that accidentally references freed init memory will execute `INT3` (0xCC = breakpoint on x86) and crash, rather than silently running garbage code.

## After `free_initmem()`

You cannot call any `__init` function after this point. The kernel enforces this:

```c
// If you try:
call_after_free_initmem();   // → page fault at 0x000... (freed memory)
```

## Cross-references

- [Parent: kernel_init](../README.md)
