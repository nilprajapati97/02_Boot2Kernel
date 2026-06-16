Awesome—let’s build you a **battle-tested, end-to-end playbook** for debugging & troubleshooting kernel panics, driver failures, hangs, performance stalls, and “it just doesn’t probe” mysteries on embedded Linux. Use this as both a **real workflow** and an **interview answer**.

---

# 0) Golden Rules (before you touch anything)

* **Make it reproducible**: exact kernel tag, config, DTB, rootfs, bootargs, board rev.
* **Collect evidence first**: don’t reboot until you’ve grabbed logs, stacks, traces.
* **Change one variable at a time**: config, patch, bootarg, device power—keep diffs tiny.
* **Keep two builds**: `debug` (instrumented) and `release` (minimal).

---

# 1) Universal Triage Flow (applies to any failure)

**A. Capture logs early & reliably**

* Boot args: `loglevel=7 ignore_loglevel printk.time=1`
* Bring up a console ASAP: `earlycon` (SoC UART) + `earlyprintk` (legacy)
* If console is unreliable: **netconsole**, **pstore/ramoops**, or **serial over USB**
* Android/UEFI boards: enable **pstore** and check `/sys/fs/pstore/`

**B. Snapshot the system on failure**

* Magic SysRq: enable `CONFIG_MAGIC_SYSRQ` → `echo 1 > /proc/sys/kernel/sysrq`

  * `echo t > /proc/sysrq-trigger` → dump all task stacks
  * `echo w > /proc/sysrq-trigger` → show blocked tasks
  * `echo l > /proc/sysrq-trigger` → backtrace of all CPUs
* Watchdogs/NMI: keep **NMI watchdog** on for hard lockups

**C. Make symbols meaningful**

* Kernel config: `CONFIG_DEBUG_INFO_DWARF5=y`, `CONFIG_GDB_SCRIPTS=y`
* Disable KASLR for symbol stability: add `nokaslr` to bootargs
* Keep exact `vmlinux`, `System.map`, and module `.ko` files used on target

---

# 2) Kernel Panics & Oops: turn the trace into code lines

**What to collect**

* Full oops/panic backtrace, RIP/PC, process name, CPU, taint flags
* The **top frame** and two frames below often locate the bug

**Decoding steps**

```bash
# From the build tree that produced the image
# 1) Symbolize a module address
addr2line -e drivers/mydrv.ko 0x<offset-from-oops>

# 2) Symbolize a core kernel address
addr2line -e vmlinux 0xffffffff8abc1234

# 3) Use gdb with kernel scripts for richer context
gdb vmlinux
(gdb) add-symbol-file drivers/mydrv.ko 0x<load-addr>
(gdb) l *0xffffffff8abc1234
```

**Common root causes & quick checks**

* **NULL deref**: uninitialized `priv`, missing `of_match_table`, failed `devm_*` alloc; audit error paths.
* **Use-after-free / double free**: add **KASAN** (`CONFIG_KASAN` + `kasan=on`) → repro and read report.
* **Stack overflow**: large on-stack arrays in probes/IRQs; move to heap or static.
* **Bad copy\_to/from\_user**: check bounds & `access_ok()`.
* **BUG/WARN splats**: trust them—read the assert; often lock misuse or invalid state.

**When panics are intermittent**

* Turn on **KFENCE** (cheap corruption detector) or **KMSAN/KCSAN** (data races), start with KFENCE for production-like overhead.

---

# 3) Hangs, Freezes, Lockups (no panic)

**Symptoms**

* Serial stops, ping drops, LEDs frozen, but no oops.

**Immediate actions**

* SysRq `t`, `w`, `l` dumps
* Add boot args: `nmi_watchdog=1` `hung_task_panic=1` `rcupdate.rcu_cpu_stall_timeout=60`

**Tools**

* **Lockdep** (`CONFIG_PROVE_LOCKING`, `CONFIG_LOCK_DEBUGGING_SUPPORT`) → deadlocks
* **RCU stall detector** (`CONFIG_RCU_TRACE`, boot logs show the stuck CPU)
* **ftrace** function graph:

  ```bash
  echo function_graph > /sys/kernel/debug/tracing/current_tracer
  echo 1 > /sys/kernel/debug/tracing/tracing_on
  # repro, then:
  cat /sys/kernel/debug/tracing/trace > /tmp/fg.txt
  ```
* **trace-cmd + KernelShark** for nicer visuals

**Typical causes**

* Interrupt handler waiting on sleeping lock (don’t sleep in hardirq)
* Workqueue never scheduled (IRQ affinity/mask)
* PM runtime deadlock (runtime PM usage count imbalance)
* Clock/regulator off → peripheral reads hang bus

---

# 4) Driver Probe Failures (device never appears or binds)

**Checklist**

1. **DT/ACPI matching**

   * `compatible` string matches driver `of_match_table`?
   * `status = "okay";` present? Node not disabled?
2. **Power & clocks**

   * `regulator_summary` and `clk_summary` in debugfs show rails/clks enabled?
   * Reset lines deasserted? `reset-gpios` ok?
3. **Pinmux/pinctrl**

   * Correct function & bias? `pinctrl-0` applied at boot?
4. **Bus addressing**

   * I²C `reg = <0xNN>` correct? device responds on the bus?
   * SPI `mode` (CPOL/CPHA) and `max_speed_hz` sane?
5. **Interrupts**

   * Right IRQ type (edge/level)? `request_irq()` errors?
   * GPIO → IRQ mapping correct? Pinmux set to IRQ mode?
6. **DMA**

   * `dma_set_mask_and_coherent()` matches hardware width?
   * Bounce buffer churn? Update `dma-ranges` in DT if needed.
7. **Probe deferral**

   * `-EPROBE_DEFER` loop? Missing supplier (clk/reg/phy)? Fix DT order or use fw\_devlink.

**Commands & debugfs**

```bash
# DT live view:
ls -R /proc/device-tree/soc/.../mydev
# Clocks:
cat /sys/kernel/debug/clk/clk_summary
# Regulators:
cat /sys/kernel/debug/regulator/regulator_summary
# GPIO/pin:
cat /sys/kernel/debug/gpio
cat /sys/kernel/debug/pinctrl/*/pins
```

**Dynamic debug to see driver paths**

```bash
# Build driver with pr_debug/dev_dbg
echo 'module mydrv +p' > /sys/kernel/debug/dynamic_debug/control
dmesg -w
```

---

# 5) Bus-Specific Rapid Triage

**I²C**

* `i2cdetect -y <bus>` (careful on live systems) → address visible?
* Pull-ups present? Clock stretching? Lower bus speed to test.
* If NACKs: verify power rail and reset timing in DT.

**SPI**

* Check `mode` and `bits_per_word`; try slower `max_speed_hz`.
* Some controllers need CS delay → `cs-gpios`, `spi-cs-high`, `spi,cs-setup-ns`.

**MMC/SD**

* Boot args `mmc_core.clock_gating=0` for debug
* Signal voltage switching (3.3V→1.8V) failures show as init timeouts; lock to HS to test.

**PCIe**

* `lspci -vv` (if available); MSI vs INTx issues; try `pci=nomsi`.
* Check PERST#/CLKREQ#/REFCLK; LTSSM state via controller debugfs.

**USB**

* `usbmon` traces: `modprobe usbmon` then `cat /sys/kernel/debug/usb/usbmon/*u`
* Power budget & VBUS; autosuspend quirks: `usbcore.autosuspend=-1`.

**Ethernet/PHY**

* `ethtool -S eth0`, `ethtool --phy-statistics`
* PHY address in DT, RGMII delay (`tx/rx-internal-delay-ps`) match board routing?

**DRM/Display**

* `drm.debug=0x1e` boot arg; use `modetest` to list connectors/modes.

---

# 6) Memory, Concurrency & Data Races

**Enable the right sanitizers (pick based on symptom)**

* **KASAN**: heap out-of-bounds, UAF (dev/test builds)
* **KFENCE**: low-overhead corruption guard (prod-like)
* **UBSAN**: undefined behavior
* **KCSAN**: data races
* **LOCKDEP**: deadlocks
* **SLUB debug**: `slub_debug=FZPU` boot arg, `page_poison=1`
* **HW errata**: confirm cacheability / barriers for MMIO (`readl_relaxed` vs `readl`)

**Barriers & ordering**

* Verify `dma_wmb()/dma_rmb()` around DMA descriptors
* IRQ handler vs threaded handler: don’t sleep in hardirq

---

# 7) Performance & Latency (when it’s “slow” not “broken”)

**Measure before tuning**

```bash
perf top                    # hotspots
perf record -g -- <workload>
perf report
```

* Tracepoints + eBPF (`bpftrace`, `trace-cmd`) for kernel events
* Scheduler: check preemption model (PREEMPT/RT), IRQ affinity
* I/O: for flash use `noop`/`mq-deadline`; mount `noatime`
* CPUfreq governor (`performance` for latency tests)
* Networking: GRO/LRO, NAPI budget, offloads

---

# 8) Boot-time Failures (no userspace yet)

**Make the kernel talk early**

* Boot args: `earlycon`, `initcall_debug`, `no_console_suspend`, `ignore_loglevel`
* See slow inits with `initcall_debug` timestamps
* If filesystem is the blocker: test with **initramfs** to isolate storage

**U-Boot/SPL**

* Turn on verbose for storage init; try raw partition boot to bypass FS parsing
* If board hangs before kernel: use **JTAG** (Lauterbach/PLS) to halt and inspect clocks/resets/DDR init

---

# 9) Post-mortem Crash Dumps (kdump/kexec)

**Set up once on dev boards—huge time saver**

1. Reserve memory: boot arg `crashkernel=256M` (size per SoC)
2. Install `kexec-tools`, configure `kdump` kernel/initramfs
3. On panic, 2nd kernel collects `/proc/vmcore` → save with `makedumpfile`
4. Analyze:

   ```bash
   crash vmlinux /path/to/vmcore
   # 'bt', 'ps', 'files', 'kmem', 'dev -d', etc.
   ```

---

# 10) Git Bisection (when a specific commit broke it)

```bash
git bisect start
git bisect bad v6.6.45          # failing tag
git bisect good v6.6.42          # last known good
# Provide a fast, scripted test that returns 0=good, 1=bad
while git bisect run ./run_repro.sh; do :; done
git bisect reset
```

* Keep the repro minimal (boot, run a smoke test, check dmesg for signature)

---

# 11) Instrumentation You’ll Reuse A Lot

**Dynamic debug**

```bash
echo 'file drivers/mydrv/*.c +p' > /sys/kernel/debug/dynamic_debug/control
```

**Tracepoints (example)**

```bash
# Find available tracepoints for a subsystem
ls /sys/kernel/debug/tracing/events/net/
# Enable a set
echo 1 > /sys/kernel/debug/tracing/events/net/netif_rx/enable
```

**Event-driven probes**

* **kprobes/kretprobes** via `tracefs` or bpftrace:

  ```bash
  bpftrace -e 'kprobe:mydrv_tx { printf("%s %d\n", comm, pid); }'
  ```

---

# 12) Hardware-adjacent sanity checks (when the driver looks fine)

* **Power rails**: measure with DMM/scope; check power-good timing vs DT constraints
* **Clocks**: verify frequency on a test pad if available
* **Reset**: see that reset is deasserted before register access
* **Signals**: logic analyzer for I²C/SPI/UART timing
* **EMI/Signal integrity**: reduce bus speed to see if errors vanish → layout/timing hint

---

# 13) Ready-to-use Checklists

**Crash/Oops checklist**

* [ ] Save full dmesg/oops
* [ ] Symbolize with `addr2line` (module & vmlinux)
* [ ] Turn on KASAN/KFENCE if memory bug suspected
* [ ] Reproduce with `nokaslr`, `DEBUG_INFO`, same DTB
* [ ] Minimal repro; then bisect if needed

**Probe-fail checklist**

* [ ] DT compatible/status/reg/interrupts/clocks/resets
* [ ] Power/clk/reset debugfs summaries
* [ ] Pinctrl/GPIO direction & pull
* [ ] Bus enumerates? (i2cdetect/usbmon/lspci)
* [ ] Dynamic debug logs from driver probe path
* [ ] -EPROBE\_DEFER loop resolved (supplier ready)

**Hang/lockup checklist**

* [ ] SysRq t/w/l captured
* [ ] Lockdep enabled → deadlock report?
* [ ] RCU stall logs
* [ ] ftrace function\_graph around suspect code
* [ ] IRQ context misuse checked

---

# 14) Interview-style summary (30-second version)

> “I first make the issue reproducible and capture evidence early (earlycon, netconsole, pstore). For panics I symbolize the backtrace to source with `addr2line/gdb`, and I enable the right debug configs (KASAN/KFENCE/LOCKDEP) based on symptoms. For hangs I use SysRq + ftrace function-graph, lockdep, and RCU stall detector. For driver probe issues I validate DT matching, power/clock/reset, pinmux, interrupts, and DMA masks, using debugfs summaries and dynamic-debug. If the failure is a regression, I automate a `git bisect` with a minimal repro script. For severe crashes I rely on kdump to analyze `vmcore` with `crash`. I only change one variable per iteration and keep a debug vs release build for fast turnaround.”

---

If you want, I can tailor this to **your SoC (e.g., Qualcomm SDM660)** with **exact bootargs, relevant debugfs paths, and typical subsystem flags** you’ll need (MMC/SDHCI, IPA/Net, RPMh clocks, SLPI/remoteproc).
