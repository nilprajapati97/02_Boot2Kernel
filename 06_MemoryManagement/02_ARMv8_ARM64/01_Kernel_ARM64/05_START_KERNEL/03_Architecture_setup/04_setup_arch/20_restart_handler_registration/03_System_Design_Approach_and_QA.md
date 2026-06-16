# restart_handler_registration — System Design Approach and Q&A

## 1. Why a Notifier Chain?

The restart notifier chain (`restart_handler_list`) is a `blocking_notifier_head`. The design rationale:

1. **Multiple subsystems want a chance to reset** — PSCI, EFI, board watchdog
2. **Priority ordering** — try the most reliable method first (PSCI at EL3 with full hardware control), then EFI, then board watchdog
3. **Return NOTIFY_STOP to short-circuit** — the first handler that successfully issues reset can return `NOTIFY_STOP` so lower-priority handlers don't run redundantly
4. **Separation of concerns** — the ARM core restart code doesn't need to know about PSCI or EFI; each subsystem registers its own handler

---

## 2. Dependency Graph

```
[machine descriptor populated] (via DT match or ATAG)
        │
[arm_efi_init()]
  └── EFI runtime services mapped (may register EFI restart handler)
        │
[setup_arch(): if (mdesc->restart) arm_pm_restart = mdesc->restart]
  └── arm_pm_restart function pointer set
        │
[register_restart_handler(&arm_restart_nb)]
  └── arm_restart_nb added to restart_handler_list (priority 128)
        │
[psci_dt_init() — called next in setup_arch()]
  └── If PSCI available → psci_sys_reset_nb registered (priority 129)
        │
[User: reboot() syscall]
  └── blocking_notifier_call_chain(&restart_handler_list)
        ├── PSCI (129): SMC → ATF → hardware reset → [STOP]
        ├── arm_restart (128): skipped (PSCI already stopped chain)
        └── EFI (70): skipped
```

---

## 3. What Is register_restart_handler()?

```c
/* kernel/reboot.c */
static BLOCKING_NOTIFIER_HEAD(restart_handler_list);

int register_restart_handler(struct notifier_block *nb)
{
    return blocking_notifier_chain_register(&restart_handler_list, nb);
}
```

This adds `nb` to the doubly-linked list of notifier blocks, sorted by priority (highest first). Thread-safe via semaphore in `blocking_notifier_chain_register()`.

---

## 4. Design Alternatives

**Alternative 1: Single function pointer**
```c
void (*machine_restart_func)(enum reboot_mode, const char *);
```
Problem: Only one handler can be registered. When PSCI is added, must override the function pointer. Loses the original board handler. Hard to compose multiple reset mechanisms.

**Alternative 2: List of function pointers**
```c
void (*restart_handlers[])(enum reboot_mode, const char *);
```
Problem: No priority ordering, no way for a handler to say "I succeeded, stop trying others." The notifier chain already implements exactly this pattern generically — no need to reinvent.

**Chosen: Blocking notifier chain** — reuses generic infrastructure, allows arbitrary number of handlers, priority-ordered, interruptible-by-NOTIFY_STOP.

---

## 5. System Design Q&A

**Q: What is the difference between a blocking_notifier and atomic_notifier?**
> `blocking_notifier_chain_register()` uses a semaphore (mutex), so it can only be called from process context (can sleep). It's used for restart/reboot notifications where the callers are in process context (sys_reboot). `atomic_notifier_chain_register()` uses a spinlock — safe from interrupt context, but handlers cannot sleep. Restart handlers need to do MMIO (watchdog register writes) which may sleep or need process context, so blocking notifier is correct. Audio sample-rate change notifications, memory hotplug notifications — those use atomic notifiers because they can fire from interrupt context.

**Q: What ensures the restart handler isn't called during normal operation, only at reboot?**
> `kernel_restart()` calls `blocking_notifier_call_chain(&restart_handler_list, ...)` — this is the ONLY place in the kernel that calls this chain. `kernel_restart()` itself is only called from `kernel_power_off()`, `kernel_halt()`, or `sys_reboot()` with `LINUX_REBOOT_CMD_RESTART`. Normal drivers don't have access to the `restart_handler_list` notifier head (it's `static` in `kernel/reboot.c`). The API is only: `register_restart_handler()` (add a handler) and `unregister_restart_handler()` (remove one). Drivers can register handlers but cannot trigger the chain — that's intentionally restricted.

**Q: Why is arm_restart_nb given priority 128, not 255 or 0?**
> Priority 128 is "medium" — it leaves room for higher-priority handlers (PSCI at 129-255, EFI at 70-128) to override it. PSCI is registered at 129 by `psci_dt_init()` which runs AFTER `register_restart_handler()` in `setup_arch()`. By using 128 for the board-level handler, PSCI (when available) automatically takes precedence without needing to unregister the board handler. If priority were 0, future hardware-level handlers couldn't preempt it. If 255, the board handler would prevent PSCI/EFI from running. 128 is the "board default" priority that's easy to override.
