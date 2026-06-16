Nice — here’s a **practical, real-world, engineer-level checklist** (step-by-step flow) you can use when **collaborating with hardware engineers during board bring-up**. It’s organised by phase, includes exact actions, test checks, artifacts to exchange, templates you can copy, common failure modes and mitigations, and small scripts/snippets you can drop into CI or run on a bench. No hand-holding fluff — full engineer checklist.

# Purpose

This checklist turns a schematic + bare board into a reproducible, tested BSP image. It focuses on collaboration: what to ask hardware for, what to hand back, how to triage HW vs SW issues, and how to record everything so the next engineer (or QA) can reproduce your results.

---

# Phase 0 — Prep (before the board arrives)

Actions (BSP + HW collaboration)

* Request and archive from HW team:

  * Full schematic (PDF + native CAD), BOM, PCB rev (silkscreen & rev codes), mechanical drawings, connector pinouts.
  * DDR memory part numbers, PMIC datasheets, oscillator specs, power sequencing tables, testpoint map, JTAG header pinout.
  * Expected default boot device (eMMC/SD/UFS/USB/Network) and secure-boot requirements.
* Create a board artifact folder in your repo: `/boards/<board-name>/` and version it (git).
* Prepare build environment:

  * Toolchain & cross-compiler pinned (exact commits/versions).
  * Yocto/Buildroot layer skeleton ready: `meta-<board>`, shell scripts for building images.
  * Flash tooling & image layout (partition map).
* Make test plan & acceptance criteria (who signs off on Day-N).
* Ask HW for an example “golden board” or at least one reference board to compare signals when troubleshooting.

Deliverables to request from hardware:

* Schematic PDFs, labeled PCB image, testpoint map, DDR params, PMIC config table, connector pinout.
* Serial number / board revision sticker format.

Exit criteria for Phase 0:

* You can build a baseline image for the SoC (even if it will not boot yet).
* Flash/serial/JTAG tools installed and smoke test scripts ready.

---

# Phase 1 — Board receipt & physical verification

Checklist (first inspection)

* Visual inspection:

  * Check silkscreen revision matches ordering.
  * Look for visible shorts, solder bridges, missing components.
* Basic continuity checks:

  * Multimeter: verify power rails exist and are not shorted to ground before powering.
* Inventory:

  * Label board with unique ID, record serial/board rev in repo.
* Connectors & connectors orientation: ensure test cables match pinouts.

Record a short "board arrival" form (example fields later).

If the board powers off / smells burnt → stop, record, return to HW team.

---

# Phase 2 — Bench setup (tools + basic connections)

Essential tools you must have ready (BSP + HW collaboration)

* USB-to-UART adapter (PL2303/FTDI), logic level matched to board.
* JTAG adapter (OpenOCD compatible or Lauterbach) with correct target adapter config.
* Multimeter, bench PSU adjustable, oscilloscope, logic analyzer / I2C/SPI bus analyzer.
* Flash programmer (if SPI/NOR or external flash needs programmer).
* USB mass storage cable, SD card writer.
* Serial terminal tools: `screen`, `picocom`, `minicom`, `cu`.
* Optional: USB Ethernet gadget / serial-over-USB.

Actions

* Hook UART to debug UART with correct voltage levels (1.8V/3.3V).
* Connect JTAG and verify that the JTAG chain enumerates (use OpenOCD or vendor tool).
* Power with bench PSU limited current (e.g., set current limit low for first power-up).
* Verify oscillator / clock presence on scope if available.

Exit criteria:

* Stable power rails at expected voltages.
* UART responds or at least shows preboot activity.
* JTAG target detectable.

---

# Phase 3 — Day-0 bring-up: Boot visibility & basic HW health

Objective: get a serial console and see any ROM/SPL/Loader output.

Step-by-step:

1. Power the board while serial terminal open (typical settings 115200,8,N,1). Record first bytes.

   * Command: `screen /dev/ttyUSB0 115200` or `picocom -b 115200 /dev/ttyUSB0`
   * If nothing appears: confirm UART pins, voltage levels, TX/RX crossover, pull-ups, and pinctrl in schematic.
2. If no serial output:

   * Check power rails are present with multimeter.
   * Measure oscillator clock on scope (main crystal).
   * Use JTAG to halt CPU and read system registers (check Reset vector).
3. If boot ROM or SPL outputs appear:

   * Capture full serial log to file: `cat /dev/ttyUSB0 > serial-<boardid>.log` (or use `screen -L`).
4. If you see SPL/U-Boot prompt → success! Document U-Boot version.
5. If stuck early (no SPL), ask HW to run a pre-programmed golden SPL image and/or provide test harness outputs (oscilloscope snapshots).

Key debug points to collaborate with HW:

* Share oscilloscope screenshots of clock/reset lines.
* HW engineer to verify PMIC power sequencing and that GPIO resets are pulled to correct levels.

Exit criteria:

* Either (A) serial boot ROM/loader output captured, or (B) JTAG can access CPU and you have electrical trace data to escalate.

---

# Phase 4 — Bootloader bring-up (SPL → U-Boot)

Goal: get U-Boot prompt and ability to load kernels.

Checklist

* Ensure early console in SPL (add `CONFIG_SPL_SERIAL_SUPPORT` if building).
* Use a known-good U-Boot build (from vendor or your tree) and flash to the appropriate boot partition.
* Verify that U-Boot environment can read storage: `mmc list`, `mmc read`, `fatls mmc 0`.

  * U-Boot commands: `mmc dev 0`, `fatload mmc 0 0x80000000 zImage`, `bootz 0x80000000 - 0x82000000`
* If flash is not accessible:

  * Check PHY/SD/eMMC power rails, card detect pin, eMMC CLK trace.
  * Use oscilloscope/logic analyzer to check clock toggles on SD/eMMC line during `mmc` ops.
* Add safe recovery paths: serial console, USB mass storage boot, or SD-card boot.

Deliverables for HW team from this phase:

* U-Boot log, boot order, environment used.
* If boot fails due to pinmux/power → patch suggestion for schematic or board wiring.

Exit criteria:

* U-Boot prompt visible AND U-Boot can load a kernel image from the expected boot device.

---

# Phase 5 — Kernel bring-up & device tree

Goal: kernel boots and IO basic drivers probe.

Checklist

* Start with a minimal kernel `defconfig` with early console and basic drivers (UART, mmc, network).
* Add device tree (`.dts`) prepared from schematic. Ensure:

  * `compatible` strings match drivers.
  * clocks, resets, pinctrl nodes are correct.
  * regulator voltages and consumers listed.
* Build `zImage/Image` + `dtb` and boot from U-Boot.

  * Useful commands in U-Boot: `bootz` / `bootm`.
* Capture kernel boot log: `dmesg` or serial capture. Save `serial-boot.log`.
* Look for driver probe lines and errors:

  * `dmesg | grep -i -E "(error|failed|panic|cannot|unhandled)"`
  * `dmesg | grep -i probe`
* If a driver fails to bind:

  * Verify `compatible` and `reg` match hardware.
  * Verify clocks/resets are toggled (some drivers query clocks during probe).
  * HW: check physical reset and clocks on scope.
* For memory failures:

  * Use JTAG to read DDR controller regs or run a U-Boot `md` memory test.
  * Compare DDR init code with board DDR timing from HW team.

Deliverables to hardware:

* Kernel `dmesg` output (annotated with timestamps + failing lines).
* DT source / diff showing adjustments.

Exit criteria:

* Kernel prints the version, mounts rootfs (or at least can read rootfs device) and shows no critical panics.

---

# Phase 6 — Rootfs & userspace verification

Goal: have a minimal functional rootfs and interactive shell.

Checklist

* Boot a minimal rootfs (busybox/systemd).
* Ensure `init` runs and you get a login prompt (serial or network).
* Verify basic commands:

  * `mount`, `ls /dev`, `ip link`, `ifconfig` / `ip a`, `/bin/sh`.
* Provide SDK/sysroot to application devs (Yocto `populate_sdk`).
* Test flashing and recovery modes.

Deliverables:

* Rootfs image(s), toolchain tarball, rootfs manifest (SBOM), flash instructions.

Exit criteria:

* Login prompt accessible and rootfs minimal smoke tests pass.

---

# Phase 7 — Peripheral-by-peripheral bring-up

For each peripheral (Storage, Ethernet, USB, Display, Camera, Audio, Sensors):

Use a repeated micro-checklist:

1. **Observe dmesg** for probe success.

   * `dmesg | grep <driver|device>`
2. **Check sysfs** entries: `/sys/class/<device>`, `/sys/bus/<bus>/devices/...`
3. **Run functional test**:

   * eMMC: `dd if=/dev/zero of=/tmp/test bs=1M count=4 oflag=direct` (be careful—use a test partition).
   * Ethernet: `ip link set eth0 up && dhclient eth0`, `iperf3` test.
   * USB: `lsusb`, `dmesg` on device attach.
   * I2C sensors: use `i2cdetect -y <bus>` then `i2cget/i2cset` or the sensor's userspace tool.
   * SPI: use `spidev_test` or the device's sample app.
4. **If failing**:

   * Check power rails for that peripheral.
   * Check resets & interrupts (are they routed?).
   * Check pinctrl/pinmux settings in DTS.
   * Collaborate: HW to probe signals (SDA/SCL, CLK, DATA) with logic analyzer/oscilloscope.

Document test results in a per-peripheral spreadsheet: pass/fail, logs, mitigation.

---

# Phase 8 — Power management & thermal

Checklist

* Validate PMIC configuration (voltage levels under load).
* Test suspend/resume:

  * `echo mem > /sys/power/state` then wake with RTC or GPIO.
* Test CPU frequency scaling and governor behavior (`cpufreq-info`, `cpupower`).
* Validate thermal throttling events in `dmesg`.
* If failures:

  * HW: check wakeup sources & reset behavior.
  * SW: check wakeup IRQs in DTS & IRQ polarity.

---

# Phase 9 — Automated tests, CI & regression

Set up automation as you go:

* **CI build**: build bootloader, kernel, image on every PR.
* **Smoke test runner**: an agent attached to a DUT that:

  * Flashes image
  * Captures serial log for N seconds
  * Runs basic checks (boot prompt, mount, network)
  * Returns pass/fail and artifacts
* **Daily regression**: flash and run long-running soak tests (burn-in).
* **SBOM & SCA**: auto-generate license manifest and run vulnerability scan.

Example smoke test checks (pseudo):

* Boot timeout: serial sees "U-Boot" within X sec and "Linux version" within Y sec.
* No kernel panic/oops in logs.
* Rootfs mounted, `systemd` services started.

---

# Phase 10 — Triage & escalation process (how to decide HW vs SW)

Triage checklist (run on every new failure)

1. Reproduce issue on **second board**. If not reproducible → likely HW/assembly.
2. Swap to known-good golden board: if passes → suspect board HW.
3. If issue reproducible across boards → likely SW.
4. Collect full artifacts to attach to ticket:

   * Serial logs (full), kernel config, kernel image + dtb, U-Boot env, /proc and /sys dumps, `mount` output, oscilloscope screenshots, JTAG core dumps.
5. Create an issue with clear reproduction steps and expected vs actual.
6. HW/SW sync: triage meeting with both engineers, assign owner, define actions and timeline.

Escalation steps:

* If board hardware fault (shorts, missing component): HW -> manufacturing.
* If design error (wrong resistor value, pin swap): change request and update schematics (treat as ECO).
* If SW needs to adapt to HW (edge-case clock/reset): patch kernel/DTB and plan upstreaming.

---

# Deliverables & Handover (what BSP must deliver back to HW & product)

* Tagged git repo with:

  * `meta-<board>` layer, kernel branch, U-Boot branch.
  * Patches and `README` for apply/flash.
* Release artifacts:

  * Signed images, SDK/toolchain, SBOM (SPDX/CycloneDX), flash scripts, partition layout.
* Test artifacts:

  * Serial logs, kernel `dmesg`, bench test results (power, thermal), soak test reports.
* Runbook:

  * Day-0 checklist, serial/U-Boot commands, hardware test points, recovery procedure.
* Issue list with statuses, root cause, and compensating controls (if any).

Exit criteria for board bring-up:

* U-Boot prompt + kernel boot to login reliably on target hardware.
* All required production peripherals pass their functional tests.
* SBOM generated, images signed, and SDK ready for application devs.

---

# Reproducible templates (copy/paste)

### Board arrival form (example)

```
Board ID: <board-serial>
PCB rev: <rev>
Received date: <YYYY-MM-DD>
Schematic version: <vX.Y>
Testpoints provided: YES/NO (attach map)
Golden board available: YES/NO
Initial power measured rails: VDD_CORE=1.2V, VDD_IO=3.3V, ...
Serial port: /dev/ttyUSB0 @ 115200
JTAG: adapter=<model>, chain OK: YES/NO
Physical defects: <notes/photos>
Initial status: [POWER_OK | NO_SERIAL | NO_JTAG | OTHER]
```

### Minimal bug / failure ticket template

```
Title: [BOOT|STORAGE|ETH|OTHER] <short summary>
Board ID: <id>
Repro steps:
1) Connect UART at 115200
2) Power on
3) Observe
Expected:
Observed:
Artifacts attached:
- serial.log
- u-boot env (fw_printenv)
- kernel config (.config)
- dtb/dts
- oscilloscope screenshots
- JTAG core dump (if any)
Priority: [P0,P1,P2]
Assigned: <name>
```

---

# Small helpful scripts & snippets

**Capture serial to file (Linux)**

```bash
# replace /dev/ttyUSB0 and choose baud as needed
tee serial.log < /dev/ttyUSB0 &
# or use screen logging: screen -L /dev/ttyUSB0 115200
```

**Basic smoke test script (safe, non-destructive)**

```bash
#!/bin/bash
LOG=serial-$(date +%Y%m%d-%H%M%S).log
# assumes /dev/ttyUSB0
timeout 300 cat /dev/ttyUSB0 > $LOG &
sleep 10
# check for bootloader or kernel markers in the log after timeout
sleep 310
if grep -q "U-Boot" $LOG || grep -q "Linux version" $LOG; then
  echo "BOOT MARKER FOUND"
else
  echo "BOOT MARKER NOT FOUND - FAIL"
fi
# Save log to artifact store
```

(Expand this in CI: fail job if markers not found.)

**Quick kernel log checks**

```bash
dmesg | egrep -i "panic|oops|failed|error"
dmesg | egrep -i "probe|bind|registered"
```

---

# Common pitfalls & mitigations (short)

* No early UART: make UART first priority in SPL/U-Boot. Always.
* Wrong pinmux: confirm pinctrl entries against silicon datasheet.
* eMMC/SD not detected: check power domain & CLK presence on scope.
* Out-of-tree module breaks on kernel bump: minimize OOT code, upstream quickly.
* Binary blobs with wrong ABI: prefer building from source; if binary, record exact toolchain used.

---

# Communication & process rules (so collaboration doesn’t stall)

* Always attach logs & exact steps in tickets. Don’t assume “I saw it” — every failure must be reproducible from artifacts.
* Daily 15–30 min standups while bring-up is active (HW + BSP + QA). Keep them short and action-oriented.
* For any HW change (ECO) require a new schematic/PCB document and new test vectors; freeze SW unless approved.
* Mark a single owner for each failing item (avoid ping-pong).

---

# KPIs to track for each bring-up

* Boot success rate (flashed boards that boot to login / total boards).
* Time to first UART output after power-on (shorter is better).
* Number of HW ECOs required after first prototype.
* % of functional tests automated and passing.

---

If you want, I’ll now:

* generate a **ready-to-use Day-0 runbook** for a specific SoC (e.g., i.MX8 / Qualcomm SDM660 / RK3399) with exact U-Boot commands, kernel builds and DT fragments; **or**
* create a **GitHub Actions CI pipeline** that builds U-Boot + kernel + image and runs the smoke test above against a QEMU DUT (or hardware lab agent).

Pick one and I’ll produce the full files and commands immediately.
