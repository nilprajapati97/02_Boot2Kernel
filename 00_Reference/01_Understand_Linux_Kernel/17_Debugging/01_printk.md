# 01 — printk

## 1. What is printk?

`printk()` is the **kernel's primary logging function** — equivalent to printf for userspace.

Messages go to:
- Kernel ring buffer (circular, queried via `dmesg`)
- `/dev/kmsg` (read by syslog/systemd-journald)
- Serial console (if configured)

---

## 2. Log Levels

```c
/* include/linux/kern_levels.h */
#define KERN_EMERG    "0"  /* System is unusable */
#define KERN_ALERT    "1"  /* Action must be taken immediately */
#define KERN_CRIT     "2"  /* Critical conditions */
#define KERN_ERR      "3"  /* Error conditions */
#define KERN_WARNING  "4"  /* Warning conditions */
#define KERN_NOTICE   "5"  /* Normal but significant condition */
#define KERN_INFO     "6"  /* Informational */
#define KERN_DEBUG    "7"  /* Debug-level messages */
```

---

## 3. Usage

```c
/* Raw printk with level */
printk(KERN_INFO "mydriver: initialized at %p\n", addr);
printk(KERN_ERR  "mydriver: error %d\n", ret);

/* Preferred shorthand macros: */
pr_info("mydriver: device found, id=%d\n", id);
pr_warn("mydriver: timeout occurred\n");
pr_err("mydriver: failed to allocate memory\n");
pr_debug("mydriver: value=%d\n", val);   /* Compiled out unless DEBUG defined */

/* Device-context logging (includes device name): */
dev_info(&dev, "Probed successfully\n");
dev_err(&dev, "Failed with error %d\n", ret);
dev_dbg(&dev, "register value: 0x%x\n", reg);
dev_warn(&dev, "Retry attempt %d\n", n);
```

---

## 4. Rate Limiting

```c
/* Avoid log flooding from high-frequency events */
printk_ratelimited(KERN_WARNING "mydrv: interrupt flood!\n");
pr_warn_ratelimited("too many errors\n");

/* Or with explicit control: */
static DEFINE_RATELIMIT_STATE(rl, 5 * HZ, 10);  /* 10 msgs per 5s */
if (__ratelimit(&rl))
    pr_warn("error occurred\n");
```

---

## 5. Reading the Ring Buffer

```bash
# Print kernel messages:
dmesg
dmesg | tail -50
dmesg -T             # Human-readable timestamps
dmesg -l err,warn    # Filter by level
dmesg -w             # Watch mode (follow)

# Ring buffer: controls
cat /proc/kmsg       # Read ring buffer (blocks)
cat /dev/kmsg        # Raw with metadata

# Clear:
dmesg -C
```

---

## 6. Console Log Level

```bash
# Messages at level < console_loglevel appear on console
cat /proc/sys/kernel/printk
# 4  4  1  7
# Current MinimumDefault BootDefault

# Set to show INFO and above on console:
echo "7 4 1 7" > /proc/sys/kernel/printk
```

---

## 7. printk in Performance-Critical Code

```c
/* pr_debug is compiled out unless DEBUG is defined — zero overhead */
#define DEBUG
#include <linux/kernel.h>

/* Or per-file: */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt   /* Add module name prefix */
```

---

## 8. Oops / Panic Output

```
BUG: unable to handle kernel NULL pointer dereference at 0000000000000010
PGD 0 P4D 0
Oops: 0002 [#1] SMP PTI
...
RIP: 0010:mydriver_write+0x42/0x90 [mydriver]
...
Call Trace:
 vfs_write+0xb8/0x1a0
 ksys_write+0x68/0xe0
 do_syscall_64+0x57/0x130
```

---

## 9. Source Files

| File | Description |
|------|-------------|
| `kernel/printk/printk.c` | Core printk implementation |
| `include/linux/printk.h` | All printk/pr_* macros |
| `include/linux/dev_printk.h` | dev_err/dev_info/dev_dbg |

---

## 10. Related Topics
- [06_Dynamic_Debug.md](./06_Dynamic_Debug.md)
- [04_ftrace.md](./04_ftrace.md)
