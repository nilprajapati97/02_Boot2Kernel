# U-Boot & Bring-up Templates for `myvendor/myboard`

This document contains a set of minimal, annotated templates to start a U-Boot bring-up for a custom board when vendor BSP is not available. **Edit the annotated placeholders** (UART base, timer base, DDR timings, SoC-specific register addresses) before building or testing on hardware.

---

## Files included

* `hello_uart/hello_uart.c` — bare-metal UART-only test (load to on-chip SRAM via JTAG)
* `u-boot/board/myvendor/myboard/board.c` — minimal board hooks (early init, dram\_init, board\_init)
* `u-boot/board/myvendor/myboard/spl.c` — SPL skeleton (prints to UART, calls DDR init)
* `u-boot/configs/myboard_defconfig` — minimal defconfig enabling SPL + serial + timer
* `u-boot/arch/arm64/dts/myvendor-myboard.dts` — minimal Device Tree (memory + chosen + UART)
* `u-boot/myboard.its` — simple FIT description for kernel+dtb (optional later)
* `u-boot/board/myvendor/myboard/Kconfig` — minimal Kconfig
* `u-boot/board/myvendor/myboard/Makefile` — build Makefile
* `u-boot/board/myvendor/myboard/MAINTAINERS` — maintainer note

Each file below has **clear TODO markers** where you must replace values with real hardware information.

---

## 1) `hello_uart/hello_uart.c` — Bare-metal SRAM UART test

```c
/* hello_uart.c
 * Minimal bare-metal UART test to run from on-chip SRAM/IRAM
 * Build with: aarch64-linux-gnu-gcc -nostdlib -Ttext=0x00000000 -o hello.elf hello_uart.c
 * Then use objcopy to binary and load with JTAG to SRAM address that BootROM will jump to.
 */

#define UART0_BASE 0x01C28000UL /* TODO: REPLACE with guessed/known UART base */
#define UART_THR   (UART0_BASE + 0x00)
#define UART_LSR   (UART0_BASE + 0x14)
#define UART_LSR_THRE (1 << 5)

static inline void mmio_write32(unsigned long addr, unsigned int val) {
    *(volatile unsigned int *)addr = val;
}
static inline unsigned int mmio_read32(unsigned long addr) {
    return *(volatile unsigned int *)addr;
}

static void putc(char c) {
    while (!(mmio_read32(UART_LSR) & UART_LSR_THRE)) ;
    mmio_write32(UART_THR, (unsigned int)c);
}

void puts_hello(const char *s) {
    while (*s) putc(*s++);
}

void main(void) {
    /* If UART needs explicit init (baud divisor, LCR), add it here. */
    puts_hello("Hello from SRAM - bare-metal test\n");
    for (;;) ;
}
```

**Notes:**

* Replace `UART0_BASE` with the actual base. If unknown, try values from similar SoC family.
* Link address (`-Ttext=0x00000000`) must match where BootROM jumps; change as needed.
* Use JTAG/OpenOCD to load and run this binary initially.

---

## 2) `u-boot/board/myvendor/myboard/board.c`

```c
/* SPDX-License-Identifier: GPL-2.0+ */
#include <common.h>
#include <init.h>
#include <asm/global_data.h>
#include <asm/arch/clock.h>   /* TODO: create or map to your SoC clock header */

DECLARE_GLOBAL_DATA_PTR;

int board_early_init_f(void)
{
    /* TODO: Implement clock enable and early pinmux for UART here.
     * Example calls (you must implement):
     *   soc_enable_uart_clock();
     *   board_uart_pinmux_init();
     */
    return 0;
}

int dram_init(void)
{
    /* TODO: Implement DDR controller init in spl.c or arch layer.
     * As a placeholder, set a guessed RAM size. Replace with actual detection.
     */
    gd->ram_size = 512UL * 1024 * 1024; /* TODO: set actual RAM size */
    return 0;
}

int board_init(void)
{
    /* Put boot parameters at a sane place in RAM */
    gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + 0x100;
    return 0;
}

int board_late_init(void)
{
    /* Optional: init Ethernet, PMIC, etc. */
    return 0;
}
```

**Notes:**

* Replace placeholders with SoC-specific functions. For early bring-up, having UART init here helps if SPL didn't fully set it up.

---

## 3) `u-boot/board/myvendor/myboard/spl.c` — SPL skeleton

```c
/* SPDX-License-Identifier: GPL-2.0+ */

#include <common.h>
#include <spl.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/ddr.h>

void board_init_f(ulong dummy)
{
    /* 1) Setup minimal clocks required for UART and PLL */
    /* TODO: implement soc_clock_init() according to SoC TRM */
    soc_clock_init();

    /* 2) Early UART pinmux + init */
    early_uart_pinmux_init();
    early_uart_init();
    puts("SPL: early UART init done\n");

    /* 3) DDR init: program controller & PHY (if exists) */
    if (ddr_init()) {
        puts("SPL: DDR init failed\n");
        hang();
    }
    puts("SPL: DDR init done\n");

    /* 4) Load full u-boot from configured boot device into RAM */
    /* This uses U-Boot SPL helper to probe storage; implement storage read if needed. */
    spl_boot_device_t boot_dev = spl_boot_device();
    /* spl_load_image() is platform helper; many targets use MMC raw read, SPI-NOR, etc. */
    spl_load_image();
}
```

**Notes:**

* `soc_clock_init`, `early_uart_pinmux_init`, `early_uart_init`, and `ddr_init` are SoC/board-specific and must be implemented in the `arch/` or `board/` code.
* Initially you can stub `ddr_init()` to return success only after confirming DDR is not used; but to load full U-Boot, DDR must be functional.

---

## 4) `u-boot/configs/myboard_defconfig` (minimal)

```text
# Minimal myboard_defconfig
CONFIG_ARM=y
CONFIG_SPL=y
CONFIG_SPL_LIBCOMMON_SUPPORT=y
CONFIG_SPL_LIBGENERIC_SUPPORT=y
CONFIG_SPL_BOARD_INIT=y
CONFIG_SYS_TEXT_BASE=0x80000000
CONFIG_SYS_SDRAM_BASE=0x80000000
CONFIG_NR_DRAM_BANKS=1
CONFIG_SYS_INIT_SP_ADDR=0x80100000

# Serial console / debug uart (update base in header or config)
CONFIG_BAUDRATE=115200
CONFIG_CONS_INDEX=1

# Minimal shell and commands
CONFIG_CMDLINE=y
CONFIG_HUSH_PARSER=y
CONFIG_SYS_PROMPT="myboard> "
CONFIG_CMD_BDI=y
CONFIG_CMD_MEMORY=y
CONFIG_CMD_BOOTD=y
CONFIG_CMD_RUN=y

# Keep features small for bring-up
CONFIG_DM_SERIAL=y

# Device tree support
CONFIG_OF_LIBFDT=y
CONFIG_DEFAULT_DEVICE_TREE="myvendor-myboard"
```

**Notes:**

* `CONFIG_SYS_TEXT_BASE` and `CONFIG_SYS_SDRAM_BASE` must match your memory map.
* You may need to add SoC-specific Kconfig items when integrating into real U-Boot.

---

## 5) `u-boot/arch/arm64/dts/myvendor-myboard.dts` — Minimal DT

```dts
/dts-v1/;

/ {
    compatible = "myvendor,myboard";
    model = "MyVendor MyBoard";

    chosen {
        bootargs = "console=ttyS0,115200"; /* TODO: set correct console name */
    };

    memory@80000000 {
        device_type = "memory";
        reg = <0x0 0x80000000 0x0 0x20000000>; /* TODO: size = 512MB example */
    };

    uart0: serial@01c28000 {
        compatible = "ns16550a"; /* adjust to SoC driver if different */
        reg = <0x01c28000 0x1000>; /* TODO: replace with actual UART base & size */
        interrupts = <0 29 4>;
        status = "okay";
    };
};
```

**Notes:**

* Replace `reg` and `interrupts` values with SoC-correct entries. Use your SoC's `.dtsi` as reference.

---

## 6) `u-boot/myboard.its` — Simple FIT wrapper (optional)

```its
/dts-v1/;

/ {
    description = "Simple FIT for myboard";

    images {
        kernel {
            description = "Linux kernel Image.gz";
            data = /incbin/("arch/arm64/boot/Image.gz");
            type = "kernel";
            arch = "arm64";
            os = "linux";
            compression = "gzip";
            load = <0x80080000>;
            entry = <0x80080000>;
        };
        fdt {
            description = "Device Tree Blob";
            data = /incbin/("arch/arm64/boot/dts/myvendor-myboard.dtb");
            type = "flat_dt";
            arch = "arm64";
            compression = "none";
        };
    };

    configurations {
        default = "conf";
        conf {
            kernel = "kernel";
            fdt = "fdt";
        };
    };
};
```

**Notes:**

* Useful later to produce a single ITB (Image Tree Blob) containing kernel+dtb for U-Boot.

---

## 7) `u-boot/board/myvendor/myboard/Kconfig`

```Kconfig
menu "myvendor myboard"

config TARGET_MYBOARD
    bool "Enable support for myvendor myboard"

endmenu
```

---

## 8) `u-boot/board/myvendor/myboard/Makefile`

```makefile
obj-y += board.o

# Add SPL object when needed
obj-$(CONFIG_SPL) += spl.o
```

---

## 9) `u-boot/board/myvendor/myboard/MAINTAINERS`

```
M: Your Name <you@example.com>
S: Board maintainers
F: board/myvendor/myboard/
```

---

## Build & Test Quick Steps

1. Build hello test and verify UART via JTAG:

   ```bash
   aarch64-linux-gnu-gcc -nostdlib -Ttext=0x00000000 -o hello.elf hello_uart.c
   aarch64-linux-gnu-objcopy -O binary hello.elf hello.bin
   # Use OpenOCD / JTAG to write hello.b
   ```
