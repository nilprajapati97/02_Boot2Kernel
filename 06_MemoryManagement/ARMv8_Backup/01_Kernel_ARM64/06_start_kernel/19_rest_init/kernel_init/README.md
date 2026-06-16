# `kernel_init()` — PID 1: The Init Process

## Purpose

Runs as PID 1. Completes all remaining kernel initialization that requires a process context, then transitions from kernel space to userspace by `execve()`-ing the init program (`/sbin/init`, `/init`, or `/bin/sh`).

## Source File

`init/main.c`

```c
static int __ref kernel_init(void *unused)
{
    int ret;
    
    // Wait for kthreadd to be ready:
    wait_for_completion(&kthreadd_done);
    
    // Mark this thread as a "kernel thread" that will exec:
    kernel_init_freeable();
    
    // All remaining init functions have run.
    // Now become a userspace process:
    
    // Free __init memory (functions/data only needed during boot):
    free_initmem();
    
    // Mark the system as initialized:
    system_state = SYSTEM_RUNNING;
    numa_default_policy();
    
    rcu_end_inkernel_boot();
    
    do_sysctl_args();
    
    // Try to exec the init program:
    if (ramdisk_execute_command) {
        ret = run_init_process(ramdisk_execute_command);
        if (!ret)
            return 0;
        pr_err("Failed to execute %s (error %d)\n",
               ramdisk_execute_command, ret);
    }
    
    // Fallback sequence:
    if (execute_command) {
        ret = run_init_process(execute_command);
        if (!ret)
            return 0;
        panic("Requested init %s failed (error %d).",
              execute_command, ret);
    }
    
    // Last resort:
    if (!try_to_run_init_process("/sbin/init") ||
        !try_to_run_init_process("/etc/init") ||
        !try_to_run_init_process("/bin/init") ||
        !try_to_run_init_process("/bin/sh"))
        return 0;
    
    panic("No working init found.  "
          "Try passing init= option to kernel. "
          "See Linux Documentation/admin-guide/init.rst for guidance.");
}
```

## `kernel_init_freeable()` — Long Initialization

See [kernel_init_freeable/README.md](kernel_init_freeable/README.md) for this major sub-function which:
1. Waits for all CPUs to come online (`smp_init()`)
2. Runs all `do_initcalls()` (all built-in driver and subsystem init)
3. Mounts the root filesystem (`prepare_namespace()`)

## `free_initmem()` — Reclaim Boot Memory

```c
void free_initmem(void)
{
    free_initmem_default(POISON_FREE_INITMEM);
}
```

The kernel is compiled with `__init` sections:
```c
void __init my_boot_function(void) { ... }
// → placed in .init.text section

static int __initdata my_boot_variable = 42;
// → placed in .init.data section
```

After `free_initmem()`, these sections are freed back to the page allocator — typically several hundred KB to a few MB.

```
kernel: Freeing unused kernel image (initmem) memory: 2884K
```

## `run_init_process()`

```c
static int run_init_process(const char *init_filename)
{
    const char *const *p;
    
    argv_init[0] = init_filename;
    pr_info("Run %s as init process\n", init_filename);
    
    return kernel_execve(init_filename,
                         argv_init,
                         envp_init);
}
```

`kernel_execve()` transforms the kernel thread (PID 1) into a user process:
1. Loads the ELF binary from the root filesystem
2. Sets up a user address space (`mm_struct`)
3. Sets up user stack
4. Returns to userspace at the ELF entry point

**After `execve()`, PID 1 is no longer a kernel thread. It is `/sbin/init` running in userspace.**

## Init Program Options

| Init | Description |
|------|-------------|
| `systemd` | Modern init (default on most distros) |
| `sysvinit` | Traditional SysV init |
| `busybox` | Embedded systems |
| `runit` | Minimal service supervision |
| `openrc` | Gentoo's init |
| `initramfs /init` | Early userspace for pivot_root |

## Boot Command-Line Override

```bash
# Override init program:
init=/bin/sh          # Emergency single-user shell
init=/usr/sbin/init   # Custom path
rdinit=/init          # Override for initramfs
```

## Cross-references

- [Phase overview](../README.md)
- [`kernel_init_freeable/`](kernel_init_freeable/README.md) — major sub-function
- [`free_initmem`](free_initmem/README.md) — boot memory reclaim
