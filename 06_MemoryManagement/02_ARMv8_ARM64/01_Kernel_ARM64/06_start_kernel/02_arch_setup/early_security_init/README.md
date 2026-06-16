# `early_security_init()` — LSM Framework Initialization

## Purpose

Initializes the Linux Security Module (LSM) framework by processing the ordered list of security modules and calling each module's `init()` callback. This must happen before `setup_arch()` because some architecture-level operations (e.g., EFI secure boot) may call into LSM hooks.

## Source File

`security/security.c`

```c
int __init early_security_init(void)
{
    struct lsm_info *lsm;

#define call_void_hook(FUNC, ...)               \
    do { for (lsm = __start_lsm_info; lsm < __end_lsm_info; lsm++) { \
        if (lsm->init)                          \
            lsm->init();                        \
    } } while (0)

    for (lsm = __start_early_lsm_info;
         lsm < __end_early_lsm_info; lsm++) {
        if (!lsm->enabled)
            continue;
        lsm->init();
    }
    return 0;
}
```

## The LSM Architecture

Linux supports stacking multiple security modules simultaneously (since kernel 4.2). Each LSM:
- Is registered in the `__lsm_info` ELF section via `DEFINE_LSM()`
- Has an `init()` function called here
- Registers hook implementations into the `security_hook_heads` structure

The hook dispatch is done by iterating the hook list and calling each registered handler.

## LSMs with `early_init = true`

Only LSMs explicitly marked `LSM_FLAG_EARLY_BOOT` are initialized here. As of recent kernels, this includes:
- **lockdown** — restricts certain operations based on secure boot state
- **yama** — ptrace scope restrictions
- **loadpin** — restricts which filesystems modules can be loaded from (needed before module loading)

## Why Before `setup_arch()`?

`setup_arch()` on x86 checks EFI Secure Boot status and may call `security_lock_kernel_down()`. If the LSM framework isn't initialized, this call panics.

## Pre-conditions

- `__start_early_lsm_info` and `__end_early_lsm_info` linker symbols must be valid
- The `security_hook_heads` structure must be zeroed (`.bss`)

## Post-conditions

- Early LSMs are initialized and their hooks are registered
- `security_add_hooks()` has been called for early LSMs
- Subsequent `security_*()` calls are safe

## IRQ State

IRQs **disabled** — purely data structure initialization.

## Key Data Structures

| Symbol | Type | Purpose |
|--------|------|---------|
| `security_hook_heads` | `struct security_hook_heads` | Per-hook linked lists of registered handlers |
| `lsm_info` | `struct lsm_info` | LSM descriptor (name, flags, init function) |
| `__start_early_lsm_info` | linker symbol | Start of early LSM info section |

## Kconfig Dependencies

- `CONFIG_SECURITY`: Master switch for LSM framework
- `CONFIG_SECURITY_LOCKDOWN_LSM_EARLY`: Makes lockdown an early LSM
- `CONFIG_YAMA`: Enables yama LSM

## Cross-references

- [Phase overview](../README.md)
- `security_init()` — full LSM init: [../../11_security_randomness/security_init/README.md](../../11_security_randomness/security_init/README.md)
