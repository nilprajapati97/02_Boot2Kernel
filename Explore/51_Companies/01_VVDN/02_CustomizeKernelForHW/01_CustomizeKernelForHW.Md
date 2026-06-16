# How to **customize & configure Linux kernels and device drivers** — an engineer-level BSP playbook

Nice — below is a practical, step-by-step guide you can use as a BSP engineer when you need to adapt kernels and drivers to new SoCs / boards. I cover goals, workflows, concrete commands/recipes/examples (Kbuild, Kconfig, Device Tree, out-of-tree modules, Yocto module recipe), testing/CI, debugging, signing, and maintenance. I’ll cite kernel/Yocto reference docs for the heavy claims.

---

## Goals you must hit as a BSP engineer

1. Build a kernel that boots the board reliably (SPL → U-Boot → kernel). ([Kernel Documentation][1])
2. Expose board hardware (serial, mmc, ethernet, display, sensors) using proper Device Tree and driver binding. ([Kernel Documentation][2], [Kernel.org][3])
3. Minimize out-of-tree patches (upstream when possible) and package drivers properly for the BSP build system (e.g., Yocto). ([Kernel Documentation][1], [Yocto Project Documentation][4])
4. Provide reproducible builds, signed images/modules if required, and CI that validates boot + core functionality.

---

## 1) High-level workflow (concise)

1. Pick kernel baseline (vendor LTS vs mainline LTS).
2. Create a branch in kernel repo: `git checkout -b meta/<board>/linux-5.x-myboard`.
3. Start with a minimal `defconfig` (vendor or `make ARCH=arm defconfig`), enable early console. Use `make menuconfig` / `make nconfig` to tweak. ([Kernel Documentation][5])
4. Add/modify Device Tree (.dts/.dtsi) for board wiring; follow kernel bindings. ([Kernel.org][3], [Kernel Documentation][2])
5. Implement drivers: in-tree if possible, otherwise out-of-tree module packaged by build system. ([Kernel Documentation][6])
6. Build, deploy, test on target; iterate until boot + services stable.
7. Upstream clean patches where feasible; keep minimal local patch queue.

---

## 2) Kernel config (Kconfig) — quick, practical

* Tools: `make menuconfig`, `make nconfig`, `scripts/config`, or `kconfig-merge` for CI. `Kconfig` files control feature flags. ([Kernel.org][7], [Kernel Documentation][8])
* Typical commands:

  * Start from vendor defconfig:
    `make ARCH=arm CROSS_COMPILE=arm-none-eabi- <vendor>_defconfig`
  * Edit: `make ARCH=arm menuconfig` (interactive).
  * Build: `make -j$(nproc) ARCH=arm CROSS_COMPILE=... zImage dtbs modules`.
  * Create a minimal config by preserving only modules you need: `make localmodconfig` (on a running kernel with target modules loaded). See admin guide for helpers. ([Kernel.org][9])
* CI tip: store a committed `.config` or generate it from `fragments/` and `scripts/kconfig/merge_config.sh` so builds are reproducible.

---

## 3) Device Tree (DTS) — wiring the board for Linux

* Device Tree defines hardware topology (clocks, resets, pinctrl, interrupts, compatible strings). Always follow the device-tree bindings in kernel docs. Use overlays when you need to modify base DTB without changing SoC trees. ([Kernel.org][3], [Kernel Documentation][2])
* Minimal example (SPI node):

```dts
&spi1 {
    status = "okay";
    spidev@0 {
        compatible = "spidev";
        reg = <0>;
        spi-max-frequency = <10000000>;
    };
};
```

* Validation: `dtc -I dtb -O dts <your.dtb>` to inspect, then `dmesg` / `sysfs` to confirm driver binding.

---

## 4) Drivers — in-tree vs out-of-tree

* **In-tree** (preferred): easier long-term maintenance, upstreamable, built with kernel tree. Add `Kconfig` entries and `Makefile` lines so your driver can be `y` (built-in) or `m` (module). Follow kernel style & submit patches upstream. ([Kernel Documentation][1])
* **Out-of-tree** (when upstreaming isn’t possible): build as external module using kbuild. Use the recommended kbuild module Makefile pattern and the kernel docs for building external modules. Example `Makefile`:

```makefile
obj-m := mydrv.o
KDIR := /lib/modules/$(shell uname -r)/build

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
```

Build instructions and portability rules are in the kernel docs. ([Kernel Documentation][6])

---

## 5) Example: add a Kconfig + driver skeleton (in-tree)

* `drivers/misc/Kconfig`:

```kconfig
config MYDRV
    tristate "My Board Driver"
    depends on SPI
    help
      Support for the MyBoard peripheral.
```

* `drivers/misc/Makefile`:

```makefile
obj-$(CONFIG_MYDRV) += mydrv.o
```

* Implement `mydrv.c` following kernel driver patterns (probe/remove, devm\_ helpers, proper locking).

---

## 6) Packaging a kernel module in Yocto (practical)

Let Yocto handle kernel modules via `module.bbclass`. Minimal recipe:

```bitbake
SUMMARY = "out-of-tree mydrv kernel module"
LICENSE = "GPLv2"
SRC_URI = "git://git.example.com/mydrv.git;branch=main"
SRCREV = "abcdef"
S = "${WORKDIR}/git"
inherit module
KERNEL_MODULE_AUTOLOAD = "mydrv"
```

This produces `kernel-module-mydrv` packages and ensures proper placement in the image. ([Yocto Project Documentation][4], [Toradex Community][10])

---

## 7) Debugging & validation checklist

* **Early boot logs**: ensure UART console prints during SPL/U-Boot and kernel boot (`dmesg`).
* **Driver probe failures**: inspect `dmesg` for `"probe of <name> failed with -..."`
* **Sysfs & device nodes**: check `/sys/bus/*`, `/sys/class/*`, and `/dev`.
* **Module tools**: `insmod` / `rmmod` / `modprobe` / `depmod -a`.
* **Trace & performance**: ftrace, perf, iostat, `trace-cmd`/`kernelshark`.
* **Crash analysis**: enable `crashkernel` and use `makedumpfile` + `crash` for post-mortem. (Use CI to capture boot logs automatically.)
* **Reproducible tests**: boot + service smoke tests, run peripheral I/O tests (SD card read/write, Ethernet iperf, camera capture).

---

## 8) Module signing & secure boot

* If secure boot is required, sign the kernel and modules. The kernel supports module signature verification; set `CONFIG_MODULE_SIG` and provide keys to `scripts/sign-file` / `kmodsign`. Manage keys and include verification in bootloader policy (EFI shim / U-Boot verified boot). (See kernel kbuild/modules docs for signing details.) ([Kernel Documentation][6])

---

## 9) Patch workflow & upstreaming (best practice)

* Keep local patches minimal and well-documented in a patch queue managed with `git` (use branches).
* Use `git format-patch` and `git send-email` or a PR workflow matching the kernel community guidelines. Read the kernel docs on submitting patches and maintainers. Upstreaming reduces long-term maintenance cost and security debt. ([Kernel Documentation][1])

---

## 10) CI / automation you should implement

* Build the kernel + modules for target in CI matrix (different defconfigs, kernel versions, cross toolchains).
* Run automated dtb compile checks and `dtc` validation. ([Kernel.org][3])
* Flash a QEMU image or hardware-in-the-loop node and capture serial logs; fail CI on kernel oops, panics, or missing device probe.
* Auto-generate SBOM for image + kernel modules and run SCA scans (track CVEs for kernel and modules).

---

## 11) Long-term maintenance: versioning & backports

* Pin a supported kernel branch for each product (LTS with CVE backports). Maintain a small patch queue and rebase regularly. Automate compile & boot regression tests on each rebase. Track MTTR for kernel CVEs and enforce SLAs.

---

## Quick reference links (authoritative)

* Linux kernel kbuild / Kconfig language. ([Kernel.org][7])
* Device Tree bindings & usage (how to write bindings). ([Kernel.org][3], [Kernel Documentation][2])
* Building external (out-of-tree) kernel modules (kbuild modules). ([Kernel Documentation][6])
* Yocto classes & packaging (module.bbclass guidance). ([Yocto Project Documentation][4])
* Kernel documentation (development & maintainers handbook). ([Kernel Documentation][1])

---

## Want it hands-on?

I can **immediately** produce any of these (pick one) and drop it in this chat:

* A **concrete patch example**: Kconfig + Makefile + driver skeleton for an in-tree driver (ready to `git apply`).
* An **out-of-tree module + Yocto recipe** (complete `Makefile`, `mydrv.c`, and `mydrv.bb`) you can drop into `meta-myboard`.
* A **CI job (GitHub Actions)** that builds kernel, compiles DTBs, builds modules and runs a QEMU boot smoke test.

Pick one and I’ll generate the files and exact commands you can run.

[1]: https://docs.kernel.org/?utm_source=chatgpt.com "The Linux Kernel documentation"
[2]: https://docs.kernel.org/devicetree/usage-model.html?utm_source=chatgpt.com "Linux and the Devicetree - The Linux Kernel documentation"
[3]: https://www.kernel.org/doc/Documentation/devicetree/bindings/?utm_source=chatgpt.com "Index of /doc/Documentation/devicetree/bindings/"
[4]: https://docs.yoctoproject.org/ref-manual/classes.html?utm_source=chatgpt.com "5 Classes — The Yocto Project ® 5.2.999 documentation"
[5]: https://docs.kernel.org/kbuild/kconfig.html?utm_source=chatgpt.com "Configuration targets and editors - The Linux Kernel documentation"
[6]: https://docs.kernel.org/kbuild/modules.html?utm_source=chatgpt.com "Building External Modules - The Linux Kernel documentation"
[7]: https://www.kernel.org/doc/html/next/kbuild/kconfig-language.html?utm_source=chatgpt.com "Kconfig Language — The Linux Kernel documentation"
[8]: https://docs.kernel.org/kbuild/kconfig-macro-language.html?utm_source=chatgpt.com "Kconfig macro language - The Linux Kernel documentation"
[9]: https://www.kernel.org/doc/Documentation/admin-guide/README.rst?utm_source=chatgpt.com "Documentation-admin-guide-README.rst - The Linux Kernel Archives"
[10]: https://community.toradex.com/t/best-practices-for-adding-kernel-module-recipes/27919?utm_source=chatgpt.com "Best practices for adding kernel-module recipes? - Technical Support"
