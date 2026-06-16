# 06 — Dynamic Debug

## 1. What is Dynamic Debug?

**Dynamic debug** (`dyndbg`) allows enabling/disabling `pr_debug()` and `dev_dbg()` calls **at runtime** without recompiling.

- All `pr_debug()` calls compiled in but **disabled by default**
- Toggle them per-file, per-function, per-line, per-module
- Zero overhead when disabled (single conditional branch, typically predicted correctly)

---

## 2. Config

```bash
CONFIG_DYNAMIC_DEBUG=y
CONFIG_DYNAMIC_DEBUG_CORE=y
```

---

## 3. Control Interface

```bash
# Control file:
/sys/kernel/debug/dynamic_debug/control

# Format: "query flags"
# query: file, func, line, module, format OR combination
# flags: +p (enable print), -p (disable), +f (add function name), +l (add line), +m (add module)
```

---

## 4. Enabling debug Messages

```bash
# Enable ALL pr_debug in a file:
echo "file drivers/usb/core/hub.c +p" > /sys/kernel/debug/dynamic_debug/control

# Enable one function:
echo "func usb_hub_irq +p" > /sys/kernel/debug/dynamic_debug/control

# Enable all messages in a module:
echo "module usbcore +p" > /sys/kernel/debug/dynamic_debug/control

# Enable by line number:
echo "file usb/core/hub.c line 1234 +p" > /sys/kernel/debug/dynamic_debug/control

# Enable everything (all pr_debug everywhere):
echo "module * +p" > /sys/kernel/debug/dynamic_debug/control

# Disable:
echo "module usbcore -p" > /sys/kernel/debug/dynamic_debug/control
```

---

## 5. Adding Extra Info

```bash
# +p = print message
# +f = add function name prefix
# +l = add line number prefix
# +m = add module name prefix
# +t = add thread ID
# All at once:
echo "module mydrv +pflt" > /sys/kernel/debug/dynamic_debug/control

# Output will look like:
# [ 1234.567890] mydrv mydrv_probe:42 Device probed
```

---

## 6. Enable at Boot Time

```bash
# Kernel boot parameter:
dyndbg="file drivers/net/e1000e/netdev.c +p"

# In grub:
GRUB_CMDLINE_LINUX="dyndbg=\"module e1000e +p\""

# For built-in (non-module) code:
# file drivers/acpi/acpica/dbdisply.c +p
```

---

## 7. View Active Debug Messages

```bash
# Show all enabled debug callsites:
grep "=p" /sys/kernel/debug/dynamic_debug/control

# Show all registered callsites (enabled and disabled):
cat /sys/kernel/debug/dynamic_debug/control | wc -l
# ~60000+ (all pr_debug callsites in kernel)

# Example output:
# drivers/usb/core/hub.c:1234 [usbcore]usb_hub_irq =p "hub event %d\n"
#   ^file:line               ^module  ^function   ^enabled ^format
```

---

## 8. In Driver Code

```c
/* Use pr_debug — auto-controlled by dyndbg */
pr_debug("Device %d initialized, reg=0x%x\n", id, reg);

/* Device-specific (includes device name): */
dev_dbg(&pdev->dev, "Probe: irq=%d, base=%p\n", irq, base);

/* For compile-time conditional (override dyndbg): */
#define DEBUG   /* Force all pr_debug on for this file */
#include <linux/kernel.h>
```

---

## 9. Source Files

| File | Description |
|------|-------------|
| `lib/dynamic_debug.c` | Core implementation |
| `include/linux/dynamic_debug.h` | pr_debug, dev_dbg macros |
| `Documentation/admin-guide/dynamic-debug-howto.rst` | Full docs |

---

## 10. Related Topics
- [01_printk.md](./01_printk.md)
- [04_ftrace.md](./04_ftrace.md)
