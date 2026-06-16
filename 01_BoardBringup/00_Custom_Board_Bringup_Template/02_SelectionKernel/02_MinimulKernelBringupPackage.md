Done — I created a detailed, ready-to-use **Minimal Linux Kernel Bring-up Package** as a canvas document titled **“Minimal Linux Kernel Bring-up Package for Custom Board”**. It includes the minimal `myboard.dts` template, kernel config checklist, BusyBox initramfs build steps, U-Boot boot commands, troubleshooting tips, and next steps.

Open the document (titled *Minimal Linux Kernel Bring-up Package for Custom Board*) in the canvas panel to view and copy the files.

Would you like me to now:

* (A) Pre-fill the `myboard.dts` and initramfs script with your SoC’s actual addresses (tell me SoC name, RAM base/size, UART base, UART IRQ), or
* (B) Generate a shell script that automatically builds kernel + dtb + initramfs and produces `initramfs.cpio.gz`, or
* (C) Produce a FIT image example and U-Boot `uEnv.txt` for SD-card boot?

Pick A, B or C (or ask for a combination) and I’ll generate it right away.



# Minimal Linux Kernel Bring-up Package — Custom Board

**Goal:** get a Linux kernel + minimal BusyBox initramfs booting to a shell on UART on a completely custom board that has no vendor BSP. This package contains ready-to-use templates, commands, and a short workflow you can follow and adapt.

---

## What this package contains

1. Step-by-step workflow (pick kernel, prepare build env, build kernel/dtb, build initramfs).
2. `myboard.dts` — minimal device tree template (CPU, memory, timer, uart). **You must fill platform-specific addresses/interrupts.**
3. Minimal kernel config checklist and commands to produce a workable config.
4. `initramfs` build script and BusyBox-based init: `init`, `/etc/inittab` and basic device nodes.
5. U-Boot boot commands to load kernel + dtb + initramfs (via mmc/tftp/serial).
6. Troubleshooting checklist and next steps.

---

## Prerequisites (host)

* Linux host (Debian/Ubuntu recommended)
* Cross toolchain (example for ARM64): `gcc-aarch64-linux-gnu` or Linaro toolchain
* Required packages:

  ```bash
  sudo apt update
  sudo apt install build-essential git libncurses-dev bc bison flex libssl-dev u-boot-tools device-tree-compiler qemu-user-static rsync cpio
  ```
* Source trees: Kernel (recommended LTS, e.g., `git clone --depth=1 --branch v6.6 https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git`) and BusyBox.

---

## Workflow (high level)

1. Identify SoC/arch (ARMv7/ARM64/RISC-V) and pick matching kernel arch.
2. Prepare minimal Device Tree (`myboard.dts`) from template below — set `reg` values for memory and UART addresses.
3. Prepare minimal kernel config: start from `defconfig` for the arch, enable earlyprintk and initramfs support.
4. Build kernel + dtb.
5. Create initramfs (BusyBox) and produce `initramfs.cpio.gz`.
6. Boot using U-Boot: either pass `initramfs` via FIT image or load kernel and initramfs into memory and `booti`/`bootz` with `ramdisk_addr_r` set.

---

## 1) Minimal Device Tree template (`arch/arm64/boot/dts/myvendor/myboard.dts`)

> **IMPORTANT:** Replace placeholder addresses and interrupts with values for your SoC. The examples use typical ARM64 conventions (memory base 0x80000000). If your SoC is ARM32, adapt to `arch/arm/...` folder and 32-bit types.

```dts
/dts-v1/;
/ {
    model = "myvendor myboard";
    compatible = "myvendor,myboard", "arm,pl011";

    chosen {
        bootargs = "console=ttyS0,115200 earlyprintk root=/dev/ram0 rw init=/bin/sh";
    };

    aliases {
        serial0 = &uart0;
    };

    memory@80000000 {
        device_type = "memory";
        reg = <0x80000000 0x40000000>; /* 1GB at 0x80000000 - CHANGE to your RAM base/size */
    };

    soc {
        #address-cells = <1>;
        #size-cells = <1>;
        compatible = "simple-bus";
        ranges;

        uart0: serial@00000000 {
            compatible = "arm,pl011"; /* change if your UART is different */
            reg = <0x00000000 0x1000>; /* CHANGE: base address, size */
            interrupts = <0 33 4>;    /* CHANGE: IRQ for UART */
            clock-frequency = <24000000>;
        };

        timer {
            compatible = "arm,armv8-timer";
        };

        /* Add minimal syscon/clock nodes only if necessary for the UART/console. */
    };
};
```

### Notes on DTS

* **Memory node**: Must be accurate for kernel to map RAM. If this is wrong kernel may crash early.
* **UART**: Console node must match the `console=` in `bootargs`.
* If you don't know IRQ numbers or exact addresses, use the closest reference SoC DTS and search for UART/node names — then update addresses from PCB silks or SoC TRM.

---

## 2) Minimal kernel config

Start from an arch default, then enable the following options (menuconfig names shown roughly):

* `CONFIG_SERIAL_CORE` / `Serial drivers`
* `CONFIG_MAGIC_SYSRQ` (optional)
* `CONFIG_EARLY_PRINTK` (very helpful if early logs missing)
* `CONFIG_ARM` / `CONFIG_ARM64` as appropriate
* `CONFIG_CMDLINE_BOOL` / provide `bootargs`
* `CONFIG_INITRAMFS_SOURCE` (or build an external `initramfs.cpio.gz` and pass via U-Boot)
* `CONFIG_DEVTMPFS` and `CONFIG_DEVTMPFS_MOUNT` (so /dev populated)
* Minimal filesystems: `TMPFS`, `SQUASHFS` (if needed later)

### Example quick flow (ARM64):

```bash
cd linux-stable
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
make defconfig                 # starts with a basic config
# Now enable options interactively or use scripts to patch .config
make menuconfig               # enable: Early printk, initramfs support, serial drivers
```

You can also set `CONFIG_INITRAMFS_SOURCE` to a host path for your BusyBox root and the kernel build will embed it automatically.

---

## 3) Build kernel and dtb

```bash
make -j$(nproc) Image dtbs
# Output: arch/arm64/boot/Image  and arch/arm64/boot/dts/myvendor/myboard.dtb
```

If you prefer zImage or uImage use appropriate `make` targets and `mkimage`.

---

## 4) Build a BusyBox initramfs (rootfs) — minimal

Create a build directory (e.g., `rootfs/`) with this structure:

```
rootfs/
├── bin
├── sbin
├── etc
├── proc
├── sys
├── dev
├── tmp
└── init
```

**Minimal `init` (executable, mode 755)**

```sh
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo
echo "--- initramfs booted ---"
/sbin/mdev -s || true
/bin/sh
```

**Build steps:**

1. Build BusyBox (static recommended for early bring-up):

```bash
git clone https://git.busybox.net/busybox
cd busybox
make defconfig
# enable static build: menuconfig -> BusyBox Settings -> Build Options -> Build BusyBox as a static binary (CONFIG_STATIC=y)
make -j$(nproc) CROSS_COMPILE=aarch64-linux-gnu- install CONFIG_PREFIX=../rootfs
```

2. Populate `/dev` minimal nodes (mknod) and ensure `/init` is present. Example helper script:

```bash
cd ../rootfs
mkdir -p dev proc sys tmp
sudo mknod -m 666 dev/console c 5 1
sudo mknod -m 666 dev/null c 1 3
cp ../init ./init
chmod +x ./init
```

3. Create cpio.gz initramfs:

```bash
cd rootfs
find . | cpio -H newc -o | gzip > ../initramfs.cpio.gz
```

You can also use `fakeroot` to avoid sudo for device node creation.

---

## 5) Booting from U-Boot

Two common methods:

### A) Load kernel Image + dtb + initramfs into memory and boot

```
# from U-Boot prompt
mmc rescan
fatload mmc 0:1 ${kernel_addr_r} Image
fatload mmc 0:1 ${fdt_addr_r} myboard.dtb
fatload mmc 0:1 ${ramdisk_addr_r} initramfs.cpio.gz
setenv bootargs 'console=ttyS0,115200 root=/dev/ram0 rw init=/bin/sh'
booti ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
```

Make sure `kernel_addr_r`, `fdt_addr_r`, `ramdisk_addr_r` are correct memory addresses free for use (chosen by your board/U-Boot). Example values: `0x40080000`, `0x43000000`, `0x44000000` — **ADJUST** per your RAM map.

### B) Create a FIT image that embeds kernel, dtb, ramdisk

* Build a FIT source and use `mkimage -f` to create a single image U-Boot can load and boot atomically. Useful for stability.

---

## 6) Troubleshooting checklist

* No output on UART: check baud rate, correct TX/RX wiring, pinmux in SPL, clock gating for UART.
* Kernel panics early: verify memory node in DTS is correct. Wrong memory base/size crashes kernel during mm allocation.
* Initramfs doesn't run: ensure `/init` is executable and correct interpreter; if using static BusyBox, dynamic loader is not needed.
* No /dev/console: create `/dev/console` node with mknod before packing initramfs.

---

## 7) Next steps after first shell

1. Add MMC/eMMC driver in kernel & test SD card rootfs.
2. Add network drivers and enable TFTP/NFS for faster iteration.
3. Replace static BusyBox with a buildroot image for a more complete rootfs.
4. Iterate DTS: add mmc, ethernet PHY, i2c, spi nodes as you bring each peripheral up.

---

## 8) Files to customize before building

* `myvendor/myboard.dts` (memory base/size, uart address, irq)
* Kernel `defconfig` changes (enable appropriate SoC drivers)
* `init` script (optional automations)
* U-Boot env addresses (`kernel_addr_r`, `fdt_addr_r`, `ramdisk_addr_r`)

---

If you want, I can now:

* Generate a ready-to-use `myboard.dts` file filled with placeholders for your specific addresses (you supply SoC name + memory base + UART base + IRQ),
* Provide a patch/script that automatically builds kernel + dtb + initramfs and creates `initramfs.cpio.gz`,
* Or produce a FIT image example and `uEnv.txt` for SD card boot.

Tell me which of the three you want next and paste SoC/DDR/UART addresses if you'd like me to pre-fill them.
