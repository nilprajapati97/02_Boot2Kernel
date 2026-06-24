# Q20 — The Linux Device Model: device, bus, driver, kobject, sysfs

> **Subsystem:** Driver core · **Files:** `drivers/base/`, `include/linux/device.h`, `include/linux/kobject.h`
> **Interviewer is really probing:** Do you understand the **unifying object model** beneath all
> drivers — how `kobject`/`sysfs` give structure, and how **bus matching** drives probe/remove?

---

## TL;DR Cheat Sheet

- The **device model** is a **unified hierarchy** representing all hardware/drivers, exposed to
  userspace through **sysfs** (`/sys`). It answers "what devices exist, how are they related, which
  driver owns each, and what can I tune."
- **Core objects:**
  - **`kobject`** — the base "object": a name, a **refcount** (`kref`), a parent, and a
    **sysfs directory**. Embedded in bigger structs (the kernel's OO via `container_of`).
  - **`struct device`** — a device instance; lives on a **bus**, has a **driver**, a parent, and
    sysfs attributes.
  - **`struct bus_type`** — a kind of bus (platform, PCI, USB, I2C…). Owns **`match()`** (device↔
    driver) and **`probe()`** glue.
  - **`struct device_driver`** — a driver; advertises what it supports (id/of/acpi tables), provides
    `probe`/`remove`.
  - **`struct class`** — a **functional** grouping (`/sys/class/net`, `/sys/class/block`) regardless
    of physical bus.
- **Matching → binding:** when a device or driver registers, the **bus's `match()`** pairs them; on
  a match the core calls the driver's **`probe()`**; unbinding calls **`remove()`**.
- **sysfs** is the userspace face: directories = kobjects, files = **attributes** (read/write a
  value), symlinks = relationships. Backs **udev** (device nodes) and tunables.
- **kobject refcounting** (`kobject_get/put` via `kref`) controls object lifetime and is the reason
  hot-unplug/teardown is memory-safe.

---

## The Question

> Explain the Linux device model: `struct device`, `bus`, `driver`, `kobject`, sysfs, and the
> probe/remove lifecycle.

---

## Why the device model exists

Before 2.6, device handling was ad hoc and per-subsystem. Three needs drove a **unified model**:

1. **A single coherent view of hardware.** Modern systems are deeply **hierarchical** (a USB device
   on a hub on a controller on PCIe on a bridge…). Power management, hotplug, and
   suspend/resume need to walk this tree in the **right order** (you can't suspend a parent before
   its children). A uniform `struct device` tree encodes those relationships.

2. **Userspace visibility & control.** Tools (`udev`, `lsusb`, `ethtool`, monitoring) need to
   **enumerate** devices, learn their properties, and **tune** them without bespoke ioctls per
   subsystem. **sysfs** gives a uniform `/sys` filesystem reflecting the device tree, with
   **attribute files** for inspection/control and **uevents** for hotplug notification.

3. **Driver binding & lifetime.** A generic mechanism to **match drivers to devices** (across buses
   that discover differently — PCI by ID, DT by `compatible`, USB by descriptors) and to manage
   **refcounted lifetime** so removing a device while it's in use is safe.

The elegant core idea: a tiny base object — the **`kobject`** — provides **naming, a refcount, a
parent pointer, and a sysfs directory**. Everything else (`device`, `bus`, `driver`, `class`)
**embeds** a kobject and adds semantics. This is the kernel's **object-oriented infrastructure**
built in C via **`container_of`**.

---

## When the model is exercised

- **Registration:** a bus driver discovers a device (PCI scan, DT populate Q19, USB enumerate) and
  calls `device_register()`; a driver calls `driver_register()`.
- **Binding:** the bus **`match()`** runs whenever a device or driver appears → `probe()` on match.
- **Userspace events:** every add/remove emits a **uevent** → udev creates `/dev` nodes, loads
  modules, applies rules.
- **Power management:** suspend/resume and **runtime PM** walk the device tree using parent/child
  ordering.
- **Hot-unplug:** `device_unregister()` → `remove()` → refcounts drop → memory freed when last
  reference goes.

---

## Where in the kernel

```
drivers/base/core.c     <- struct device, device_register/add, sysfs glue
drivers/base/bus.c      <- bus_type, match, bind/unbind, /sys/bus/*
drivers/base/driver.c   <- device_driver, driver_register
drivers/base/dd.c       <- "driver <-> device" binding: really_probe(), __device_attach
drivers/base/class.c    <- struct class, /sys/class/*
lib/kobject.c           <- kobject, kset, kref refcounting, sysfs dirs
fs/sysfs/               <- sysfs filesystem implementation
include/linux/device.h, kobject.h, device/driver.h, device/bus.h
```

---

## How it works — mechanics

### 1. kobject: the base object

A `kobject` provides four things: a **name** (its sysfs dir name), a **`kref`** (atomic refcount), a
**parent** (its position in the sysfs tree), and a **`kobj_type`** (`ktype`) defining its
**`sysfs_ops`** and the **`release()`** callback run when the refcount hits zero. Bigger structs
embed it:

```
struct device { ... struct kobject kobj; ... };   /* a device IS-A kobject (+ more) */
```
You navigate from the kobject back to the device with **`container_of`**. The **`kref`** is why the
model is memory-safe: as long as anyone holds a reference (`get_device`/`kobject_get`), the object
**won't be freed**, even across hot-unplug.

### 2. The hierarchy (devices, buses, classes)

- **`/sys/devices/...`** — the **physical** device tree (parent/child by topology).
- **`/sys/bus/<bus>/devices` and `/devices/drivers`** — devices and drivers **per bus**, with
  **symlinks** to the real objects in `/sys/devices`.
- **`/sys/class/<class>/...`** — **functional** grouping (all network interfaces under
  `/sys/class/net`, all block devices under `/sys/class/block`) independent of which bus they're on.

So one physical NIC appears in `/sys/devices/pci.../...`, is symlinked under `/sys/bus/pci/devices`,
and under `/sys/class/net/eth0` — three views of one `struct device`.

### 3. Bus matching → probe (the binding engine)

This is the heart (`drivers/base/dd.c`):
1. A **device** registers on a bus, or a **driver** registers for a bus.
2. The bus's **`match(dev, drv)`** decides compatibility — PCI compares **vendor/device IDs**, the
   platform bus compares **`of_match_table` `compatible`** (Q19), USB compares descriptors.
3. On a match, **`really_probe()`** sets `dev->driver`, runs bus pre-probe glue, then calls the
   driver's **`probe(dev)`**.
4. `probe()` initializes the device and registers it with its subsystem (netdev, input, char/misc
   Q17, etc.). Success ⇒ bound (visible in sysfs as a `driver` symlink). Failure ⇒ unwind (devm
   cleanup, Q19).
5. **Unbind/remove:** writing to `/sys/.../unbind`, module unload, or hot-unplug calls
   **`remove(dev)`**, detaches the driver, and drops references.

### 4. sysfs attributes — inspect & tune

Each device can expose **attributes**: a file in its sysfs dir with **`show`** (read) and
**`store`** (write) callbacks. This is how userspace reads status and tweaks knobs without ioctls:

```c
static ssize_t speed_show(struct device *d, struct device_attribute *a, char *buf)
{   struct my_dev *m = dev_get_drvdata(d);
    return sysfs_emit(buf, "%u\n", m->speed); }
static ssize_t speed_store(struct device *d, struct device_attribute *a,
                           const char *buf, size_t len)
{   struct my_dev *m = dev_get_drvdata(d); u32 v;
    if (kstrtou32(buf, 0, &v)) return -EINVAL;
    m->speed = v; return len; }
static DEVICE_ATTR_RW(speed);   /* creates "speed" file, 0644 */
```
**Rule:** **one value per file**, text format, no complex ABIs (that's what sysfs is *for* — `procfs`
and ioctl are for other things). Group attributes with `attribute_group` and register via
`devm_device_add_groups()`.

### 5. uevents & udev

Add/remove/change emit a **uevent** (a netlink message with `ACTION`, `DEVPATH`, `SUBSYSTEM`,
modalias…). **udev** in userspace reacts: create `/dev/<node>` (using the device's **`dev_t`** Q17),
set permissions, load the matching module (via **`MODULE_DEVICE_TABLE`** modalias), run rules. This
is the bridge from the in-kernel model to `/dev`.

### 6. Power management ordering

Because the device tree encodes **parent/child**, the PM core suspends **children before parents**
and resumes **parents before children** — and **runtime PM** (`pm_runtime_*`) autosuspends idle
devices. The model's hierarchy is what makes correct ordering possible (Qualcomm runtime-PM topic).

---

## Diagrams

### Object relationships

```mermaid
flowchart TD
    KO[kobject: name + kref + parent + sysfs dir] --> DEV[struct device embeds kobject]
    DEV -->|on a| BUS[struct bus_type: match(), probe glue]
    DEV -->|bound to| DRV[struct device_driver: probe/remove + id tables]
    DEV -->|grouped by| CLS[struct class: /sys/class/*]
    BUS --> SYS["/sys/bus/<bus>"]
    DEV --> SYSD["/sys/devices/..."]
    CLS --> SYSC["/sys/class/..."]
```

### Bind lifecycle

```mermaid
flowchart TD
    R1[device_register] --> M{bus match() finds a driver?}
    R2[driver_register] --> M
    M -- yes --> PB["really_probe() -> driver.probe(dev)"]
    PB -- ok --> BOUND["bound: sysfs driver symlink, subsystem registered"]
    PB -- fail --> UNW[devm cleanup, leave unbound]
    BOUND --> UNB["unbind / unplug / rmmod -> driver.remove(dev)"]
    UNB --> PUT["put references; release() when kref hits 0"]
```

### sysfs views of one device

```
/sys/devices/pci0000:00/0000:00:1f.6/        <- physical
/sys/bus/pci/devices/0000:00:1f.6 ──symlink──┘
/sys/class/net/eth0 ─────────────────symlink─┘   (functional view)
```

---

## Annotated C

```c
/* The base object: refcount + sysfs presence + type. */
struct kobject {
    const char        *name;
    struct kobject    *parent;
    struct kset       *kset;
    const struct kobj_type *ktype;   /* sysfs_ops + release() */
    struct kref        kref;         /* lifetime: free when -> 0 */
};

/* A device instance. */
struct device {
    struct device   *parent;         /* topology -> PM ordering */
    struct kobject   kobj;           /* IS-A kobject */
    const struct bus_type *bus;      /* which bus */
    struct device_driver  *driver;   /* bound driver (NULL = unbound) */
    void            *driver_data;    /* dev_set/get_drvdata() */
    struct device_node *of_node;     /* DT backing node (Q19) */
    dev_t            devt;           /* for /dev node creation (Q17) */
};

/* A bus: the matchmaker. */
struct bus_type {
    const char *name;
    int (*match)(struct device *dev, struct device_driver *drv); /* device<->driver */
    int (*probe)(struct device *dev);
    int (*remove)(struct device *dev);
};

/* container_of: recover the containing struct from an embedded kobject/device. */
struct my_dev *m = container_of(kobj, struct my_dev, dev.kobj);
```

> The whole model is **"embed a kobject, use `container_of`, refcount with `kref`."** If you can
> explain that sentence and the **match→probe→remove** lifecycle, you understand the device core.

---

## Company Angle

- **All companies:** this underpins *every* driver — expect it as the conceptual glue behind Q17/Q19.
  Be able to explain **probe/remove**, **sysfs attributes**, and **container_of/kref**.
- **Qualcomm:** **runtime PM** + power-domain ordering using the device tree/hierarchy; sysfs knobs
  for clocks/regulators; platform bus matching (Q19).
- **Google (observability):** sysfs as the **stable userspace ABI** for monitoring/control; udev
  rules; class-based enumeration; uevent-driven automation at fleet scale.
- **NVIDIA/AMD:** PCI bus matching (vendor/device IDs), hot-plug, refcount safety on device removal
  while userspace holds references (GPU/NVMe).

---

## War Story

*"A GPU-style PCI driver hit a **use-after-free** during hot-unplug under load: userspace still held
an open fd / mmap when the device was removed, and `remove()` freed the driver's private struct while
an in-flight ioctl was still using it → crash. The root cause was **bypassing the device model's
refcounting**: the code freed its object in `remove()` instead of tying lifetime to the
**`kobject`/`kref`**. The fix was to take a **device reference** (`get_device`) for the duration of
each userspace operation and release it (`put_device`) on completion, and let the **`release()`**
callback free the object only when the **last reference** dropped. After that, unplug-with-open-fd was
safe: `remove()` tears down hardware access, but memory survives until the final `put`. The
interviewer's point: **the device model's `kref` lifetime is the contract** — `remove()` ≠ `free()`;
the object dies when its refcount hits zero, which is exactly what makes hot-unplug memory-safe."*

---

## Interviewer Follow-ups

1. **What is a kobject?** The base kernel object: name, parent, **kref refcount**, and a **sysfs
   directory**; embedded in `device`/`bus`/`driver` to give them structure and lifetime.

2. **device vs bus vs driver?** `device` = an instance; `bus_type` = a kind of bus that **matches**
   devices to drivers and provides probe glue; `device_driver` = code that drives matching devices.

3. **What is sysfs and what are attributes?** A RAM filesystem mirroring the kobject/device tree;
   **attributes** are files with `show`/`store` callbacks — one value per file — for inspection and
   tuning.

4. **Walk the probe lifecycle.** Device/driver registers → bus `match()` → on match `really_probe()`
   → driver `probe()` (acquire resources, register subsystem) → bound; unbind/unplug → `remove()`.

5. **How does matching differ per bus?** PCI by vendor/device ID, platform by DT `compatible`
   (`of_match_table`), USB by descriptors — each bus implements `match()`.

6. **Why kref/refcounting?** So an object isn't freed while still referenced (open fds, in-flight
   ops, hot-unplug); `release()` frees it only when the count hits zero — `remove() != free()`.

7. **What is `struct class` for?** Functional grouping in `/sys/class` (net, block, tty) independent
   of physical bus; also drives `/dev` node creation (Q17).

8. **How does udev fit in?** Kernel emits **uevents** on add/remove; udev creates `/dev` nodes, sets
   permissions, autoloads modules via modalias, and runs rules.

9. **How is PM ordering derived?** From the `device->parent` hierarchy: suspend children before
   parents, resume in reverse; runtime PM autosuspends idle devices.

---

## 30-Minute Talk Track

| Min | Cover |
|-----|-------|
| 0–4 | Why a unified model: coherent HW view, userspace visibility, binding/lifetime |
| 4–8 | kobject: name/kref/parent/sysfs dir; embedding + container_of (C-style OO) |
| 8–12 | Object set: device, bus_type, device_driver, class; the three sysfs views |
| 12–17 | Bus matching engine: match() per bus → really_probe → probe/remove |
| 17–21 | sysfs attributes: show/store, one-value-per-file, attribute groups |
| 21–25 | uevents + udev → /dev nodes, modalias autoload |
| 25–28 | kref lifetime safety; PM/runtime-PM ordering from the hierarchy |
| 28–30 | War story (hot-unplug use-after-free; remove != free) + summary |
