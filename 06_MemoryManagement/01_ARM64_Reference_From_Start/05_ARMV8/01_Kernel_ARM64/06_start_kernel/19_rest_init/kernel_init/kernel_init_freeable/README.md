# `kernel_init_freeable()` — Final Kernel Setup in PID 1

## Purpose

Performs all kernel initialization that requires a process context (can sleep, can allocate memory, can wait for events). This is called from `kernel_init()` (PID 1) before transitioning to userspace. It is the largest single function in the boot sequence.

## Source File

`init/main.c`

```c
static noinline void __init kernel_init_freeable(void)
{
    utsname_setup();
    wait_for_completion(&kthreadd_done);
    
    init_mm_internals();
    
    rcu_init_tasks_generic();
    do_pre_smp_initcalls();    // Run __initcall0 (early_initcall)
    
    lockup_detector_init();
    
    smp_init();               // Bring up secondary CPUs!
    sched_init_smp();         // Enable SMP scheduling
    
    padata_init();
    page_alloc_init_late();
    
    /* Initialize the rest of the kernel: */
    do_basic_setup();          // Run all initcalls (modules, drivers)
    
    kunit_run_all_tests();
    
    wait_for_initramfs();
    console_on_rootfs();
    
    if (!initrd_start || dry_run_security_modules())
        init_eaccess(ramdisk_execute_command);
    
    /* Open the /dev/console: */
    if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
        pr_err("Warning: unable to open an initial console.\n");
    
    (void) sys_dup(0);   // stdin = fd 0
    (void) sys_dup(0);   // stdout = fd 1  
    (void) sys_dup(0);   // stderr = fd 2
    
    /*
     * check if there is an early userspace init, and
     * if yes, let it do the rest of the init.
     * Mount the root filesystem:
     */
    prepare_namespace();
}
```

## Sub-Functions Overview

| # | Function | Description |
|---|----------|-------------|
| 1 | [`smp_init()`](smp_init/README.md) | Bring all secondary CPUs online |
| 2 | [`do_basic_setup()`](do_basic_setup/README.md) | Run all `initcall` levels (0–7) |
| 3 | [`prepare_namespace()`](prepare_namespace/README.md) | Mount root filesystem |

## Console Setup

```c
// Open /dev/console and dup to 0/1/2:
sys_open("/dev/console", O_RDWR, 0)  // fd 0 = stdin
sys_dup(0)                            // fd 1 = stdout
sys_dup(0)                            // fd 2 = stderr
```

This sets up the standard streams for the init process.

## `do_pre_smp_initcalls()`

Runs `early_initcall()` functions registered at level 0. These are functions that must run before SMP is enabled.

## `lockup_detector_init()`

Starts the watchdog timers that detect:
- **Hard lockup**: CPU stuck in interrupt handler > 10s
- **Soft lockup**: CPU stuck in kernel > 120s (CONFIG_LOCKUP_DETECTOR)

## Timeline: Before and After `kernel_init_freeable()`

```
Before:
  - 1 CPU online
  - No drivers loaded
  - No root filesystem
  - No userspace

After:
  - All CPUs online
  - All built-in drivers loaded
  - Root filesystem mounted
  - /dev/console open
  - Ready for execve(/sbin/init)
```

## Cross-references

- [Parent: kernel_init](../README.md)
- [`smp_init/`](smp_init/README.md)
- [`do_basic_setup/`](do_basic_setup/README.md)
- [`prepare_namespace/`](prepare_namespace/README.md)
