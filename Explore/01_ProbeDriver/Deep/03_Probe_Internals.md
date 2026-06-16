# How `probe()` Works Internally — Kernel Source-Level Deep Dive

> **Companion to:** [../02_Probe_DesignDoc.md](../02_Probe_DesignDoc.md), Section 6.
> **Focus:** The Linux Driver Model internals (`drivers/base/`) that turn a registered
> device + driver into a call to your `driver->probe()`.
> **Kernel baseline:** Modern 5.x / 6.x. Some symbol names changed across versions
> (e.g. `call_driver_probe()` was factored out around v5.13); the *flow* is stable even
> where a helper's name differs.

---

## Table of Contents

1. [Scope & the Two Entry Paths](#1-scope--the-two-entry-paths)
2. [Source Map — Where Each Function Lives](#2-source-map--where-each-function-lives)
3. [Driver-Side Registration Path](#3-driver-side-registration-path)
4. [Device-Side Registration Path](#4-device-side-registration-path)
5. [Matching — `driver_match_device` → `platform_match`](#5-matching--driver_match_device--platform_match)
6. [The Core — `driver_probe_device` → `really_probe`](#6-the-core--driver_probe_device--really_probe)
7. [Dispatch — `call_driver_probe`](#7-dispatch--call_driver_probe)
8. [Return-Value Handling](#8-return-value-handling)
9. [The Deferred-Probe Engine](#9-the-deferred-probe-engine)
10. [Asynchronous Probe](#10-asynchronous-probe)
11. [Locking Model](#11-locking-model)
12. [Failure & Unwind Path Inside `really_probe`](#12-failure--unwind-path-inside-really_probe)
13. [sysfs Effects of Binding](#13-sysfs-effects-of-binding)
14. [Master Diagram — Both Paths Merging](#14-master-diagram--both-paths-merging)
15. [Function Reference Table](#15-function-reference-table)
16. [Interview Cues — Internals](#16-interview-cues--internals)

---

## 1. Scope & the Two Entry Paths

Binding can be triggered from **either side** of the device model, and both converge on
the same core routine, `driver_probe_device()`:

| Trigger | Starting function | Meaning |
| --- | --- | --- |
| **Device appears** (DT parsed, hotplug, `platform_device_register`) | `device_add()` → `bus_probe_device()` | "A new device arrived — is there a driver for it?" |
| **Driver appears** (`insmod`, built-in init, `platform_driver_register`) | `driver_register()` → `driver_attach()` | "A new driver arrived — are there devices it can claim?" |

```text
DEVICE-FIRST                         DRIVER-FIRST
device_add()                         driver_register()
   ↓                                    ↓
bus_add_device()                     bus_add_driver()
   ↓                                    ↓
bus_probe_device()                   driver_attach()
   ↓                                    ↓
device_initial_probe()               __driver_attach()   (per device)
   ↓                                    ↓
        └──────────────┬─────────────────┘
                       ↓
            driver_probe_device()        ← convergence point
                       ↓
                really_probe()
                       ↓
              call_driver_probe()
                       ↓
                driver->probe()          ← YOUR probe runs
```

The key insight: **registration order does not matter.** Whichever party (device or
driver) appears second drives the match, and the deferred-probe engine (Section 9) heals
any remaining ordering gaps.

---

## 2. Source Map — Where Each Function Lives

| File | Responsibility |
| --- | --- |
| `drivers/base/dd.c` | "Driver ↔ Device" core: `driver_attach`, `__driver_attach`, `driver_probe_device`, `really_probe`, `call_driver_probe`, deferred-probe engine. |
| `drivers/base/bus.c` | Bus plumbing: `bus_add_device`, `bus_probe_device`, `bus_add_driver`, `driver_match_device`. |
| `drivers/base/core.c` | `device_add` and device lifecycle. |
| `drivers/base/platform.c` | Platform bus: `platform_match`, `platform_drv_probe` wrapper. |
| `drivers/of/device.c` | `of_driver_match_device` / `of_match_device` (DT compatible matching). |

> `dd.c` literally stands for **"device driver"** binding — it is the heart of this
> document.

---

## 3. Driver-Side Registration Path

When a driver registers (e.g. via `module_platform_driver()` →
`platform_driver_register()` → `driver_register()`):

### `driver_register()`
* Validates the driver, checks for duplicate registration.
* Calls `bus_add_driver()`.

### `bus_add_driver()`
* Links the driver into the bus's driver list (`bus->p->klist_drivers`).
* Creates the driver's sysfs directory under `/sys/bus/<bus>/drivers/<name>/`.
* If the bus permits autoprobe, calls **`driver_attach()`**.

### `driver_attach()`
* Iterates **every device** already on the bus via `bus_for_each_dev()`, invoking
  `__driver_attach()` as the per-device callback.

### `__driver_attach()`
For each candidate device it:
1. Calls `driver_match_device()` (Section 5). No match → skip the device.
2. On `-EPROBE_DEFER` from match, records the device for later.
3. Takes the device lock (and parent lock if needed — Section 11).
4. Calls **`driver_probe_device()`** to attempt the actual bind.
5. For asynchronous drivers, schedules `__driver_attach_async_helper()` instead
   (Section 10).

---

## 4. Device-Side Registration Path

When a device registers (DT enumeration, hotplug, `platform_device_register()` →
`device_add()`):

### `device_add()` *(core.c)*
* Inserts the device into the device hierarchy and kobject/sysfs tree.
* Emits the `KOBJ_ADD` uevent (this is the modalias that lets userspace `udev`/`modprobe`
  load a matching module).
* Calls `bus_add_device()` then `bus_probe_device()`.

### `bus_add_device()` *(bus.c)*
* Adds the device to the bus's device list and creates its sysfs links.

### `bus_probe_device()` *(bus.c)*
* If the bus has autoprobe enabled, calls `device_initial_probe()`, which calls
  `__device_attach()`.

### `__device_attach()`
* Walks **every driver** on the bus via `bus_for_each_drv()` with
  `__device_attach_driver()` as the callback.
* `__device_attach_driver()` runs `driver_match_device()` and, on match, calls
  **`driver_probe_device()`** — the same convergence point as the driver-first path.

---

## 5. Matching — `driver_match_device` → `platform_match`

### `driver_match_device()` *(bus.c)*

```text
driver_match_device(drv, dev)
    → if (drv->bus->match)
          return drv->bus->match(dev, drv);   /* bus-specific */
      else
          return 1;                            /* no matcher = always match */
```

For the platform bus, `bus->match` is **`platform_match()`**.

### `platform_match()` *(platform.c)* — precedence order

```text
1. Forced match?        → driver_override   (sysfs "driver_override" wins absolutely)
2. Device Tree match?   → of_driver_match_device()   (compatible strings)
3. ACPI match?          → acpi_driver_match_device()
4. ID table match?      → platform_match_id(pdrv->id_table, pdev)
5. Name match?          → strcmp(pdev->name, drv->name)
```

The first mechanism that succeeds wins. This layered approach is exactly why the **same
driver binary** can bind via DT on an ARM SoC, via ACPI on x86, or via a legacy name on a
board-file platform.

> A return of `-EPROBE_DEFER` from the OF/ACPI layer (e.g. an unresolved supplier in the
> `fwnode` graph) propagates up and routes the device onto the deferred list.

---

## 6. The Core — `driver_probe_device` → `really_probe`

### `driver_probe_device()` *(dd.c)*
* Guards against probing a device that is not present / already bound.
* Handles runtime-PM gating of the parent.
* Calls **`really_probe()`** and interprets its return code (Section 8).

### `really_probe()` *(dd.c)* — the heart

`really_probe()` performs the ordered sequence that actually binds the pair:

1. **Atomic claim:** sets `dev->driver = drv` (the device is now *tentatively* owned).
2. **devres group open:** opens a `devres` group so every `devm_*` allocation made inside
   probe can be released as a unit on failure.
3. **Driver sysfs links:** calls `driver_sysfs_add()` to create the
   `/sys/bus/<bus>/drivers/<drv>/<dev>` symlink and the reverse `device/driver` link.
4. **Pre-probe hooks:** `pinctrl_bind_pins()` (default pin states), `dma_configure()`
   (set up DMA / IOMMU for the device).
5. **Bus notifier:** emits `BUS_NOTIFY_BIND_DRIVER`.
6. **Dispatch:** calls **`call_driver_probe()`** (Section 7), which actually invokes the
   driver's `probe()`.
7. **On success (0):**
   * Emits `BUS_NOTIFY_BOUND_DRIVER`.
   * Adds the device to the driver's bound list (`klist_devices`).
   * Records `bound_time` and enables sysfs `bind`/`unbind` semantics.
8. **On failure:** jumps to the unwind labels (Section 12).

> Everything between "set `dev->driver`" and "BOUND_DRIVER" is the *binding transaction*.
> If any step fails, `really_probe()` rolls it back so the system looks untouched.

---

## 7. Dispatch — `call_driver_probe`

`call_driver_probe()` *(dd.c, factored out ~v5.13)* chooses **who** to call:

```text
call_driver_probe(dev, drv)
    → if (dev->bus->probe)
          ret = dev->bus->probe(dev);     /* bus wrapper, e.g. platform_drv_probe */
      else if (drv->probe)
          ret = drv->probe(dev);          /* direct driver callback */
```

For the platform bus, `dev->bus->probe` is the wrapper `platform_drv_probe()`, which:

1. Calls `dev_pm_domain_attach()` (power domains).
2. Invokes the **driver's** `pdrv->probe(pdev)` — *this is your `mydev_probe()`*.
3. If your probe returns `-EPROBE_DEFER`, detaches the PM domain again so retry starts
   clean.

`call_driver_probe()` then maps known sentinel errors and returns the result up to
`really_probe()`.

---

## 8. Return-Value Handling

The value **your** `probe()` returns is interpreted at this layer:

| Return | Interpretation | Action taken |
| --- | --- | --- |
| `0` | Success — device bound | Keep `dev->driver`, commit devres, fire `BOUND_DRIVER`, add to bound list. |
| `-EPROBE_DEFER` | Dependency not ready | Roll back, call `driver_deferred_probe_add()` to queue for retry (Section 9). |
| `-ENODEV` / `-ENXIO` | Device not actually present | Treated as a *quiet* non-fatal mismatch; clean unbind, no error spam. |
| Any other `< 0` | Hard probe failure | Full unwind (Section 12), log the error, leave device **unbound**. |

`really_probe()` clears `dev->driver` back to `NULL` on every non-success path, so a
failed probe never leaves a half-bound device.

---

## 9. The Deferred-Probe Engine

`-EPROBE_DEFER` is what makes registration order irrelevant. Mechanism (all in `dd.c`):

### Enqueue
* `driver_deferred_probe_add(dev)` puts the device on the global
  `deferred_probe_pending_list` (guarded by `deferred_probe_mutex`).

### Trigger / retry
* `driver_deferred_probe_trigger()` is called whenever **a new driver or device
  registers** (i.e., the world changed, so deferrals might now succeed).
* It moves the pending list to an active list and schedules
  `deferred_probe_work_func()` on a workqueue.
* That worker re-runs `bus_probe_device()` / `device_attach()` for each deferred device —
  re-entering the same `driver_probe_device()` core.

### Observability
* debugfs exposes the current backlog:

```bash
cat /sys/kernel/debug/devices_deferred
```

* Each line is a device still waiting; the optional reason string (when drivers call
  `dev_err_probe()`) tells you *which* supplier it is blocked on.

### `dev_err_probe()` — the idiomatic helper

```c
return dev_err_probe(dev, PTR_ERR(priv->clk), "failed to get clock\n");
```

This logs **only** when the error is *not* `-EPROBE_DEFER` (avoiding boot-log spam from
expected deferrals), records the deferral reason for debugfs, and returns the same error
code — collapsing three lines of boilerplate into one.

---

## 10. Asynchronous Probe

To shorten boot, a driver can opt into asynchronous probing:

```c
.driver = {
    .name       = "mydev",
    .probe_type = PROBE_PREFER_ASYNCHRONOUS,
},
```

Internals:

* In the driver-first path, `__driver_attach()` detects the async preference and, instead
  of probing inline, calls `async_schedule_dev(__driver_attach_async_helper, dev)`.
* `__driver_attach_async_helper()` later runs `driver_probe_device()` from an async
  worker thread.
* The kernel tracks an **async cookie** per scheduled probe; `wait_for_device_probe()`
  (and late-init synchronization) block until all async probes settle, so subsystems that
  need "all devices bound" still get a consistent point.

`probe_type` values:

| Value | Behavior |
| --- | --- |
| `PROBE_DEFAULT_STRATEGY` | Synchronous unless the bus/kernel opts into async globally. |
| `PROBE_PREFER_ASYNCHRONOUS` | Probe scheduled on an async worker. |
| `PROBE_FORCE_SYNCHRONOUS` | Always inline/synchronous (use when ordering matters). |

---

## 11. Locking Model

`really_probe()` and its callers run under careful locking to keep binding atomic:

* **`device_lock(dev)`** — held across the whole probe attempt so two threads can't probe
  the same device, and `unbind` can't race a `bind`.
* **Parent lock** — for devices that require it (`dev->parent`), the parent is locked
  first to preserve a consistent lock order and protect parent-child bring-up.
* **`deferred_probe_mutex`** — guards the deferred lists.
* **Async cookies** — order async probes relative to synchronization points without
  holding a global lock.

> Because `probe()` runs holding `device_lock(dev)`, you must **never** try to bind/unbind
> the *same* device from inside its own probe — it would deadlock.

---

## 12. Failure & Unwind Path Inside `really_probe`

When `call_driver_probe()` returns an error, `really_probe()` walks its unwind labels (the
`probe_failed:` region) in reverse order of setup:

```text
probe_failed:
   1. dma_cleanup()                  /* undo dma_configure() */
   2. device_unbind_cleanup() / devres_release_all(dev)
                                      /* free every devm_* allocation from this probe */
   3. driver_sysfs_remove(dev)       /* remove the drivers/<drv>/<dev> symlink */
   4. dev_set_drvdata(dev, NULL)     /* clear driver private pointer */
   5. dev->driver = NULL             /* release tentative ownership */
   6. pinctrl_bind_pins() rollback   /* release pin state */
   7. BUS_NOTIFY_DRIVER_NOT_BOUND    /* tell listeners binding failed */
```

This is precisely why drivers that use `devm_*` helpers need **no manual cleanup** on
probe failure: step 2 (`devres_release_all`) frees the entire devres group opened in step
2 of Section 6. Anything you allocated *without* devres (e.g. a bare
`clk_prepare_enable`) you must unwind yourself before returning the error.

---

## 13. sysfs Effects of Binding

A successful probe leaves visible, queryable state under `/sys`:

| Path | Meaning |
| --- | --- |
| `/sys/bus/<bus>/drivers/<drv>/<dev>` | Symlink: this device is bound to this driver. |
| `/sys/devices/.../<dev>/driver` | Reverse symlink to the bound driver. |
| `/sys/bus/<bus>/drivers/<drv>/bind` | Write a device name to force-bind (re-run probe). |
| `/sys/bus/<bus>/drivers/<drv>/unbind` | Write a device name to unbind (run `remove()`). |
| `/sys/devices/.../<dev>/driver_override` | Force a specific driver, bypassing match tables. |

The presence (or absence) of the `driver` symlink under a device is the single fastest way
to answer *"did this device bind?"* during debugging.

---

## 14. Master Diagram — Both Paths Merging

```text
   DEVICE-FIRST                                 DRIVER-FIRST
device_add() [core.c]                       driver_register() [bus.c→dd.c]
   │                                              │
bus_add_device() [bus.c]                     bus_add_driver() [bus.c]
   │                                              │
bus_probe_device() [bus.c]                   driver_attach() [dd.c]
   │                                              │
device_initial_probe()                       bus_for_each_dev → __driver_attach() [dd.c]
   │  __device_attach() [dd.c]                    │
   │  bus_for_each_drv → __device_attach_driver   │
   └───────────────┬──────────────────────────────┘
                   ▼
        driver_match_device() [bus.c]
                   ▼
          bus->match → platform_match() [platform.c]
            (override→OF→ACPI→id→name)
                   │
        match? ──no──► (skip / try next)
          │yes
          │      -EPROBE_DEFER ──► driver_deferred_probe_add() ─┐
          ▼                                                     │
   driver_probe_device() [dd.c] ◄───── deferred_probe_work_func ┘ (retried on
          ▼                                  any new dev/drv registration)
   really_probe() [dd.c]
     set dev->driver / open devres / sysfs link
     pinctrl_bind_pins / dma_configure
     BUS_NOTIFY_BIND_DRIVER
          ▼
   call_driver_probe() [dd.c]
          ▼
   platform_drv_probe() [platform.c]  (PM domain attach)
          ▼
   driver->probe()  ◄── YOUR mydev_probe()
          ▼
   ┌── 0 ──────────────► commit: BOUND_DRIVER, bound list, bound_time
   ├── -EPROBE_DEFER ──► deferred list (loop above)
   └── other <0 ───────► probe_failed: devres_release_all, clear driver, NOT_BOUND
```

---

## 15. Function Reference Table

| Function | File | One-line purpose |
| --- | --- | --- |
| `device_add()` | core.c | Register a device; emit uevent; kick off device-first probe. |
| `bus_add_device()` | bus.c | Link device into the bus and sysfs. |
| `bus_probe_device()` | bus.c | Start autoprobe for a newly added device. |
| `__device_attach()` | dd.c | Walk all drivers to find one for this device. |
| `driver_register()` | dd.c | Register a driver and trigger driver-first attach. |
| `bus_add_driver()` | bus.c | Link driver into the bus; create sysfs; call `driver_attach`. |
| `driver_attach()` | dd.c | Iterate all devices for a newly added driver. |
| `__driver_attach()` | dd.c | Per-device match + probe (or schedule async). |
| `driver_match_device()` | bus.c | Dispatch to the bus-specific match routine. |
| `platform_match()` | platform.c | Platform precedence: override → OF → ACPI → id → name. |
| `driver_probe_device()` | dd.c | Convergence point; guards + calls `really_probe`. |
| `really_probe()` | dd.c | The binding transaction (devres, sysfs, dispatch, commit/unwind). |
| `call_driver_probe()` | dd.c | Choose `bus->probe` vs `driver->probe`; map errors. |
| `platform_drv_probe()` | platform.c | Platform wrapper; PM-domain attach; calls your `probe`. |
| `driver_deferred_probe_add()` | dd.c | Queue a device that returned `-EPROBE_DEFER`. |
| `driver_deferred_probe_trigger()` | dd.c | Re-run deferred probes after the device model changes. |
| `dev_err_probe()` | core.c | Log+record deferral reason; quiet on `-EPROBE_DEFER`. |

---

## 16. Interview Cues — Internals

* **Q: Name the convergence point of both probe paths.** `driver_probe_device()` →
  `really_probe()`; device-first arrives via `bus_probe_device`, driver-first via
  `driver_attach`.
* **Q: Which file holds the binding core?** `drivers/base/dd.c` ("device driver").
* **Q: What does `really_probe()` set before calling your probe?** `dev->driver`, opens a
  devres group, creates the sysfs driver↔device links, runs pinctrl/dma_configure, fires
  `BUS_NOTIFY_BIND_DRIVER`.
* **Q: How is `-EPROBE_DEFER` retried?** Device goes on the deferred list;
  `driver_deferred_probe_trigger()` re-schedules a workqueue whenever a new device/driver
  registers.
* **Q: How does `devm_*` cleanup happen on failure?** `really_probe()`'s `probe_failed`
  path calls `devres_release_all()` on the group it opened.
* **Q: What lock is held during probe?** `device_lock(dev)` (plus parent lock when
  required) — so never bind/unbind the same device from within its own probe.
* **Q: Where does the bus get a say?** `bus->match` (matching) and `bus->probe`
  (`platform_drv_probe` wrapper that attaches PM domains before your callback).
* **Q: Why is registration order irrelevant?** The second party to register drives the
  match, and the deferred-probe engine resolves any remaining dependency ordering.
