# 06 — Module Parameters

## 1. What are Module Parameters?

Module parameters allow runtime configuration **without recompiling**:
- Passed at load time: `insmod mymod.ko debug=1`
- Or via `modprobe`: `modprobe mymod debug=1`
- Or written to `/sys/module/<name>/parameters/<param>` at runtime

---

## 2. Declaring Parameters

```c
#include <linux/moduleparam.h>

/* int parameter */
static int debug = 0;
module_param(debug, int, 0644);    /* name, type, permissions */
MODULE_PARM_DESC(debug, "Enable debug output (0=off, 1=on)");

/* string parameter */
static char *device_name = "default";
module_param(device_name, charp, 0444);
MODULE_PARM_DESC(device_name, "Device name string");

/* unsigned long */
static unsigned long timeout_ms = 1000;
module_param(timeout_ms, ulong, 0644);

/* bool */
static bool enable_feature = true;
module_param(enable_feature, bool, 0644);

/* Array of ints */
static int ports[MAX_PORTS];
static int num_ports;
module_param_array(ports, int, &num_ports, 0444);
MODULE_PARM_DESC(ports, "Array of port numbers");
```

---

## 3. Parameter Types

| Type | C type | Description |
|------|--------|-------------|
| `bool` | `bool` | `y/n/1/0` |
| `invbool` | `bool` | Inverted bool |
| `int` | `int` | Integer |
| `uint` | `unsigned int` | Unsigned int |
| `long` | `long` | Long int |
| `ulong` | `unsigned long` | Unsigned long |
| `short` | `short` | Short int |
| `ushort` | `unsigned short` | Unsigned short |
| `charp` | `char *` | String (copied from user) |

---

## 4. Permissions (sysfs visibility)

```c
/* permissions = 0 → not visible in /sys */
module_param(internal, int, 0);

/* Read-only from userspace: */
module_param(version, int, 0444);

/* Read-write (can be changed at runtime): */
module_param(debug, int, 0644);  /* root r/w, others r */
```

---

## 5. Using at Load Time

```bash
# insmod:
insmod mymod.ko debug=1 timeout_ms=5000

# modprobe (reads /etc/modprobe.d/):
modprobe mymod debug=1

# Verify:
cat /sys/module/mymod/parameters/debug
# 1

# Change at runtime (if writable):
echo 0 > /sys/module/mymod/parameters/debug
```

---

## 6. Complete Example

```c
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>

static int log_level = 1;
module_param(log_level, int, 0644);
MODULE_PARM_DESC(log_level, "Log verbosity: 0=quiet, 1=normal, 2=verbose");

static bool enable_x = false;
module_param(enable_x, bool, 0644);
MODULE_PARM_DESC(enable_x, "Enable feature X");

static int __init mymod_init(void)
{
    if (log_level >= 1)
        pr_info("mymod: init, log_level=%d, enable_x=%d\n",
                log_level, enable_x);
    return 0;
}

static void __exit mymod_exit(void)
{
    pr_info("mymod: exit\n");
}

module_init(mymod_init);
module_exit(mymod_exit);
MODULE_LICENSE("GPL");
```

---

## 7. Source Files

| File | Description |
|------|-------------|
| `include/linux/moduleparam.h` | module_param macros |
| `kernel/params.c` | Parameter parsing and sysfs |

---

## 8. Related Topics
- [05_Module_Loading.md](./05_Module_Loading.md)
- [03_sysfs.md](./03_sysfs.md)
