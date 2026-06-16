Short answer up front: you **cannot** safely and reliably produce a full U-Boot port *without* at least some SoC information (TRM / boot ROM behaviour / memory map / UART base / pinmux /clk domains / DDR controller registers).
But you *can* make steady, practical progress from zero by following an ordered, evidence-driven workflow: find what you *can* discover, validate with hardware (UART/JTAG), then incrementally add SPL → U-Boot → Kernel. Below is a complete, from-scratch recipe you can follow right now (no more questions asked) — with specific commands, file locations, code sketches, debug steps, and fallback options when documentation is missing.

I’ll split it into phases and give concrete actionable tasks and snippets you can copy-paste.

---

# Phase 0 — Gather anything you can (even if vendor gave “nothing”)

You need to discover or infer these critical facts. Try all possible sources:

1. SoC part number stamped on the chip (silkscreen / marking).
2. Board schematic, silkscreen, connector pinouts, or photos of the PCB.
3. Any on-board EEPROM/ID device, labeled jumpers, or boot-mode pins.
4. Search the web for the SoC family or board clones (use similar board code).
5. If nothing else, get JTAG wiring and try to read the SoC ID registers (or ask hardware team to tell you the SoC part).

Why: the TRM (technical reference manual) and DDR datasheet are required for correct clock/DDR/UART programming. If you truly have zero documentation, you must rely on hardware analysis (JTAG, logic analyzer, reading ROM behavior).

---

# Phase 1 — Tools & environment (install these now)

```bash
# Basic tools
sudo apt-get install git build-essential libncurses5-dev bison flex python3 \
  gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf device-tree-compiler \
  u-boot-tools tipxdm

# Optional but essential: JTAG utilities
sudo apt-get install openocd
# Install cross toolchains if not above (or use Linaro releases)
```

* `openocd` + vendor JTAG adapter (Segger/JLink/FT2232) — indispensable if you lack BootROM download options.
* `u-boot-tools` provides `mkimage`, `loadb` helpers.

---

# Phase 2 — Learn the SoC boot ROM expectations

You must learn:

* Where BootROM reads the first stage (SD/eMMC, SPI-NOR, NAND, UART).
* The expected image format or raw location (e.g., first sector of eMMC, fixed flash offset).
* The entry address for SPL in on-chip SRAM/IRAM and maximum SPL size.

If you don’t have TRM:

* Use JTAG to halt CPU on reset and read boot ROM status registers or the boot configuration pins to infer boot device.
* Probe boot pins with a multimeter/oscilloscope to see activity at reset.
* Try common defaults (many devboards use SD/SDMMC or QSPI).

---

# Phase 3 — Start small: bare-metal UART “Hello” in on-chip SRAM

Goal: get any text out of the board. This proves you can execute code and see serial output.

1. Find likely UART physical base addresses (look at SoC family reference or similar boards).
2. Create a small C file that initializes UART registers directly and prints a message. No RTOS, no DDR.

Example `hello_uart.S`/`hello_uart.c` (conceptual — adapt base addresses):

```c
/* hello_uart.c — minimal early test; compile as bare metal */
#define UART0_BASE 0x01C28000  // example; replace with guessed/known base
#define UART_THR   (UART0_BASE + 0x00)
#define UART_LSR   (UART0_BASE + 0x14)
#define UART_LSR_THRE (1<<5)

static inline void putc(char c) {
    while (!(*(volatile unsigned int *)UART_LSR & UART_LSR_THRE)) {}
    *(volatile unsigned int *)UART_THR = c;
}

void main(void) {
    const char *s = "Hello from SRAM!\n";
    while (*s) putc(*s++);
    for (;;) {}
}
```

* Compile with a cross-compiler and link to SRAM address the BootROM would jump to.
* Use `openocd`/JTAG to load this binary to that SRAM address and set PC to it. If you get output, you now know UART base/baud — huge win.

If BootROM supports xmodem/ymodem over UART, you can `loadb` the binary with U-Boot on a host — otherwise JTAG is the reliable path.

---

# Phase 4 — Implement minimal SPL (in U-Boot framework)

Once UART is validated, write a tiny SPL that runs from on-chip SRAM/IRAM and does minimum tasks:

* enable clocks needed for UART (if required)
* init UART (if not set by ROM)
* print early messages
* (optionally) init DDR — or at least verify DDR after you implement it
* load full U-Boot into DDR (or directly jump to a simpler U-Boot)

## Where to put files (U-Boot source)

```
u-boot/
  board/myvendor/myboard/spl.c
  configs/myboard_defconfig
  board/myvendor/myboard/board.c
```

## Minimal SPL skeleton (u-boot style)

```c
/* board/myvendor/myboard/spl.c */
#include <common.h>
#include <spl.h>
#include <asm/io.h>

void board_init_f(ulong dummy)
{
    /* 1. Very early: configure UART clock/pinmux if necessary */
    early_uart_clock_enable();
    early_uart_pinmux();

    /* 2. Init UART so we can print */
    early_serial_init();
    puts("SPL: Hello UART\n");

    /* 3. (Optional) Init DDR controller here:
     *    ddr_init() -> initialise DRAM controller registers based on DDR datasheet
     */
    ddr_init();

    /* 4. If DDR initialized, load U-Boot from storage into DDR */
    spl_load_image_from_storage();
}
```

* Add `CONFIG_SPL` related Kconfig options in your defconfig (I gave examples earlier).
* `early_serial_init()` and `ddr_init()` are SoC-specific functions — implement them in `arch/arm/mach-<soc>/` or board folder as stubs until replaced by real values.

---

# Phase 5 — DDR bring-up — the hardest bit

You must program the DDR controller with timing parameters that match the attached DRAM IC. Steps:

1. Get DDR chip part (stamped on IC) → download its datasheet (timings: tCL, tRCD, tRP, tRAS, tRFC, refresh interval).
2. Get SoC DDR controller register map (from TRM).
3. Compose DDR init sequence: clock setup (PLLs), memory controller config (timing, mode registers), PHY training or static timings.
4. Run a memory test immediately after DDR init (modulo caches) — write pattern to large region and read back.

If DDR info not available:

* Try to find board clones or similar reference designs that used same DRAM/SoC.
* Some vendors publish "DDR init binaries" used by their bootroms — these can sometimes be reused.

**Memory test code** (very simple):

```c
volatile uint32_t *p = (uint32_t *)0x80000000;
for (uint32_t i = 0; i < 0x10000; ++i) p[i] = i ^ 0xA5A5A5A5;
for (uint32_t i = 0; i < 0x10000; ++i)
  if (p[i] != (i ^ 0xA5A5A5A5)) puts("DDR TEST FAIL\n");
puts("DDR test OK\n");
```

---

# Phase 6 — Build and flash full U-Boot

Once SPL can init DDR reliably, build U-Boot proper:

```bash
export CROSS_COMPILE=aarch64-linux-gnu-
make myboard_defconfig
make -j$(nproc)
```

* You’ll produce `u-boot-spl.bin` and `u-boot.bin` (or a FIT image).
* Use the write method the SoC expects: copy to SD card in specific offset, flash to SPI-NOR at offset, or use `dd` to create bootable SD image.

If you still don’t know boot device:

* Try writing to SD card (common), place `u-boot-spl.bin` in first sectors and insert card — see UART output.
* Use JTAG to load SPL + U-Boot into RAM and jump into it for testing.

---

# Phase 7 — Device Tree / Kernel handoff

Create a minimal Device Tree that describes only what you need for bring-up:

* memory node (size, base)
* chosen.bootargs (console)
* serial node (with correct base and freq)
* simple timer/GIC if required

Build kernel and DTB:

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
# customize .config (enable initramfs or modules as needed)
make -j$(nproc) Image dtbs
```

Use initramfs (BusyBox) as rootfs for first boots to avoid storage complexity.

---

# Debugging checklist & techniques (use these constantly)

* UART prints: first indicator of progress. If nothing prints, fix pinmux/clock/UART address.
* JTAG: read/write registers live, step CPU, inspect DDR controller writes.
* Oscilloscope/logic analyzer: observe clock outputs on DDR or UART TX pin, SD card activity.
* Use `dmesg`/kernel log to find device-tree mismatch for peripheral drivers.
* Build small tests (memory test, GPIO toggle) to verify subsystems incrementally.

---

# Fallbacks when documentation is absent

1. **Find similar SoC** implementations (same family) — reuse their DDR/clock snippets as a template.
2. **Use binary blobs**: some vendors supply DDR init blobs you can call from SPL (licensed).
3. **Ask hardware team** for minimal info: UART base, DDR part number, and boot-mode strap values — these three facts accelerate everything. (If allowed, state this in a one-line request you can send the HW team: “Please share SoC part, boot-mode strapping, and DRAM part number.”)

---

# Concrete files & snippets I recommend you create now

* `board/myvendor/myboard/board.c` — minimal board hooks (board\_early\_init\_f, dram\_init, board\_init).
* `board/myvendor/myboard/spl.c` — SPL skeleton that prints to UART.
* `configs/myboard_defconfig` — minimal settings (SPL enabled, UART base macro, simple prompt).
* `arch/arm/mach-<soc>/clock.c` — basic clock enable functions (stubs initially).
* `arch/arm/mach-<soc>/ddr.c` — DDR init function (start with stub that returns fail until you implement).
* `u-boot.its` — simple FIT describing SPL + U-Boot + device tree for easier flashing.

If you want, I will generate those file templates (SPL + board.c + defconfig + simple DTS skeleton + build commands) tuned to a generic ARM64 SoC and include comments showing exactly which values you must replace (UART base, timer base, DDR addresses). Tell me to “generate templates” and I’ll drop them in the chat now.

---

# Final practical guidance (priority list)

1. Get UART working in SRAM (bare-metal) — immediate measurable win.
2. Get SPL to print messages — then expand SPL to initialize DDR.
3. Memory test DDR; when OK, load full U-Boot to DDR and iterate.
4. Create minimal DT for console + memory and boot kernel via U-Boot with initramfs.
5. Add MMC/FLASH/ETH once basic boot works.

---

If you want immediate deliverables, I can **generate the minimal working file templates now**:

* `hello_uart.c` bare-metal test,
* `board/myvendor/myboard/spl.c` skeleton,
* `board/myvendor/myboard/board.c` skeleton,
* `configs/myboard_defconfig` minimal,
* `myboard.dts` minimal chosen + memory + serial node,

All annotated with exact places you must edit (UART base, timer, DDR timings). Say “Generate templates” and I’ll produce them right away.


Below are **minimal, working skeletons** you can drop into a U-Boot-style tree (or adapt for standalone bare-metal). I’ve **annotated every place you must edit** for your SoC/board.

---

# 1️⃣ `hello_uart.c` — bare-metal SRAM test

```c
/* hello_uart.c — minimal UART poll TX test
 *
 * Build (example for ARMv7-A, adjust toolchain/flags):
 *   arm-none-eabi-gcc -nostdlib -nostartfiles -T linker.ld -o hello.elf hello_uart.c
 *   arm-none-eabi-objcopy -O binary hello.elf hello.bin
 *
 * Load to SRAM via JTAG/UART downloader and jump to _start.
 */

/* ===== EDIT THESE ===== */
#define UART0_BASE   0x01C28000  // <-- CHANGE: your UART base from TRM
#define UART_CLK_HZ  24000000    // <-- CHANGE: input clock to UART
#define BAUDRATE     115200      // <-- CHANGE if needed

/* 16550-compatible offsets (verify for your SoC!) */
#define UART_THR   (UART0_BASE + 0x00) /* write */
#define UART_RBR   (UART0_BASE + 0x00) /* read  */
#define UART_DLL   (UART0_BASE + 0x00)
#define UART_DLM   (UART0_BASE + 0x04)
#define UART_LCR   (UART0_BASE + 0x0C)
#define UART_LSR   (UART0_BASE + 0x14)
#define UART_FCR   (UART0_BASE + 0x08)

#define UART_LSR_THRE (1 << 5)

static inline void writel(unsigned int v, unsigned int addr) {
    *(volatile unsigned int *)addr = v;
}
static inline unsigned int readl(unsigned int addr) {
    return *(volatile unsigned int *)addr;
}

/* Optional: minimal init if BootROM/SPL didn’t do it */
static void uart_init(void) {
    /* ===== EDIT if your UART differs =====
     * Assume 16550: set divisor, 8N1, enable FIFO
     */
    unsigned int divisor = UART_CLK_HZ / (16 * BAUDRATE);

    /* Enable access to DLL/DLM */
    writel(0x80, UART_LCR);
    writel(divisor & 0xFF, UART_DLL);
    writel((divisor >> 8) & 0xFF, UART_DLM);

    /* 8 data bits, no parity, 1 stop */
    writel(0x03, UART_LCR);

    /* Enable FIFO, clear RX/TX */
    writel(0x07, UART_FCR);
}

static inline void putc(char c) {
    while (!(readl(UART_LSR) & UART_LSR_THRE)) {}
    writel((unsigned int)c, UART_THR);
}

static void puts(const char *s) {
    while (*s) {
        if (*s == '\n') putc('\r');
        putc(*s++);
    }
}

/* Simple entry; your linker should set reset vector to _start */
void _start(void) {
    /* If clocks/pinmux aren’t already set, you MUST do it here:
     * ===== EDIT: enable UART clock + pinmux =====
     */

    uart_init(); /* comment out if already initialized */

    puts("Hello from SRAM!\n");

    for (;;) {}
}
```

> ⚠️ If you see nothing: check **clock enable + pinmux** first, then base address.

---

# 2️⃣ `board/myvendor/myboard/spl.c` — SPL skeleton

```c
/* SPL early init skeleton */

#include <common.h>
#include <spl.h>

/* ===== EDIT: include SoC headers for clocks, pinmux, DDR ===== */

void spl_board_init(void)
{
    /* Called after basic SPL init */

    /* 1) Enable clocks needed for UART, MMC, DDR */
    // clock_init();  // <-- IMPLEMENT

    /* 2) Setup pinmux for UART (for early prints) */
    // pinmux_init_uart(); // <-- IMPLEMENT

    /* 3) Init UART for debug (optional if early console is enabled) */
    // preloader_console_init();

    /* 4) Init DRAM (critical) */
    // dram_init(); // <-- IMPLEMENT (DDR timings!)

    /* 5) Init boot device (eMMC/SD/SPI) */
    // mmc_initialize(NULL); // or spi/nand init
}

/* If your platform needs custom boot device selection */
u32 spl_boot_device(void)
{
    /* ===== EDIT: return correct boot device enum ===== */
    // return BOOT_DEVICE_MMC1;
    return BOOT_DEVICE_NONE;
}
```

---

# 3️⃣ `board/myvendor/myboard/board.c` — main U-Boot board file

```c
#include <common.h>
#include <init.h>

/* ===== EDIT: include SoC-specific headers ===== */

int board_init(void)
{
    /* Set boot params address if needed */
    // gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + 0x100;

    /* Init peripherals if not done earlier */
    // pinmux_init_all();
    // clock_init_peripherals();

    return 0;
}

int dram_init(void)
{
    /* U-Boot DRAM size detection or fixed size */
    /* ===== EDIT: set your RAM size ===== */
    gd->ram_size = 512 * 1024 * 1024ULL; // e.g., 512MB
    return 0;
}

int board_late_init(void)
{
    /* Optional: environment setup */
    // env_set("bootcmd", "run distro_bootcmd");
    return 0;
}
```

---

# 4️⃣ `configs/myboard_defconfig` — minimal

```config
CONFIG_ARM=y                      # or ARCH for your CPU
CONFIG_TARGET_MYBOARD=y

CONFIG_SPL=y
CONFIG_SPL_TEXT_BASE=0x00000000   # <-- EDIT: SRAM address for SPL
CONFIG_SPL_MAX_SIZE=0x00020000    # <-- EDIT

CONFIG_DEFAULT_DEVICE_TREE="myboard"

CONFIG_DEBUG_UART=y
CONFIG_DEBUG_UART_BASE=0x01C28000   # <-- EDIT UART base
CONFIG_DEBUG_UART_CLOCK=24000000    # <-- EDIT clock
CONFIG_DEBUG_UART_ANNOUNCE=y

CONFIG_BAUDRATE=115200

CONFIG_DM=y
CONFIG_OF_CONTROL=y

CONFIG_MMC=y
CONFIG_MMC_SDHCI=y                # or your controller

CONFIG_SERIAL=y
CONFIG_SYS_NS16550=y              # if 16550-compatible
```

---

# 5️⃣ `arch/arm/dts/myboard.dts` — minimal device tree

```dts
/dts-v1/;

/ {
    model = "MyVendor MyBoard";
    compatible = "myvendor,myboard";

    #address-cells = <1>;
    #size-cells = <1>;

    chosen {
        stdout-path = &uart0;   // <-- ensures console output
    };

    memory@40000000 {
        device_type = "memory";
        reg = <0x40000000 0x20000000>; // <-- EDIT: base + size (e.g., 512MB)
    };

    uart0: serial@01c28000 {
        compatible = "ns16550a";  // <-- EDIT if different
        reg = <0x01c28000 0x400>; // <-- EDIT base/size
        clock-frequency = <24000000>; // <-- EDIT
        current-speed = <115200>;
        status = "okay";
    };
};
```

---

# 🧭 What You MUST Customize (Checklist)

### 🔴 Critical (won’t boot without this)

* UART base address
* UART clock
* Pinmux setup
* DRAM init (DDR timings!)
* SPL load address

---

### 🟡 Board-specific

* Boot device (eMMC / SD / SPI)
* Device tree memory size
* Peripheral enables

---

### 🟢 Nice-to-have

* Debug UART
* Environment variables
* Extra drivers

---

# 🚀 Suggested Bring-up Order

1. ✅ Run `hello_uart.c` from SRAM → confirm UART works
2. ✅ Add clock + pinmux → stable output
3. ✅ Bring up SPL → print early logs
4. ✅ Add DDR init → verify memory
5. ✅ Load full U-Boot
6. ✅ Boot kernel via DTS

---

If you tell me your exact SoC (e.g., Allwinner, TI AM62x, NXP i.MX6), I can **replace all placeholders with real register values and working init code** so you can boot much faster.
