# `console_init()` — Console Driver Registration

## Purpose

Registers all built-in console drivers and flushes the printk ring buffer to all registered consoles. After this call, kernel log messages appear on the configured console (VGA screen, serial port, etc.).

## Source File

`kernel/printk/printk.c`

```c
void __init console_init(void)
{
    int ret;
    initcall_t *call;
    
    // Call all console_initcall() registered drivers:
    call = __con_initcall_start;
    while (call < __con_initcall_end) {
        (*call)();
        call++;
    }
}
```

## Console Architecture

### Dual Buffering

```
printk("hello")
    → formatted into ring buffer (always)
    → if console registered:
        → flush to console immediately
    → if no console registered:
        → message stays in ring buffer
        → flushed when console_init() registers one
```

### `struct console`

```c
struct console {
    char    name[16];
    void    (*write)(struct console *, const char *, unsigned);
    int     (*read)(struct console *, char *, unsigned);
    struct  tty_driver *(*device)(struct console *, int *);
    void    (*unblank)(void);
    int     (*setup)(struct console *, char *);
    int     (*exit)(struct console *);
    int     (*match)(struct console *, char *name, int idx, char *options);
    short   flags;
    short   index;
    int     cflag;
    uint    ispeed;
    uint    ospeed;
    void    *data;
    struct  console *next;
};
```

## Built-in Console Drivers

Registered via `console_initcall()`:

| Console | Description | Source |
|---------|-------------|--------|
| `vga16fb` / `con_init` | VGA text mode | `drivers/video/console/` |
| `uart` / `ttyS` | Serial port (8250) | `drivers/tty/serial/8250/` |
| `hvc` | Hypervisor console (Xen, KVM) | `drivers/tty/hvc/` |
| `netconsole` | Network console | `drivers/net/netconsole.c` |
| `ttyprintk` | printk to tty | `drivers/tty/ttyprintk.c` |

## Console Selection

The active console is selected by `console=` cmdline:

```bash
console=ttyS0,115200    # Serial at 115200 baud
console=tty0            # First VGA console
console=tty1            # Second VGA console
console=hvc0            # Hypervisor console
```

Multiple `console=` entries can be specified — messages go to all of them.

## Before and After

Before `console_init()`:
```
Boot message → ring buffer only (not visible)
```

After `console_init()`:
```
Boot message → ring buffer + VGA screen + serial (if configured)
```

The ring buffer holds the accumulated messages, so the full boot log appears on-screen after console_init() runs.

## `earlycon` vs `console_init()`

| | earlycon | console_init |
|--|---------|--------------|
| When | After `parse_early_param()` (Phase 3) | Phase 13 |
| Mechanism | Direct hardware write | Full driver |
| Capabilities | Basic output only | Full tty, input, etc. |
| Use | Debug early boot | Normal operation |

## Cross-references

- [Phase overview](../README.md)
- `setup_log_buf()`: [../../04_memory_management/setup_log_buf/README.md](../../04_memory_management/setup_log_buf/README.md) — the ring buffer being flushed
