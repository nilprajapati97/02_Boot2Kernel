Nice — here’s a practical, **step-by-step** checklist you can follow to debug an SPI driver from scratch. I’ll assume you have access to the board (serial console, JTAG), a logic analyzer / oscilloscope, and the Linux kernel source (or your RTOS code). Follow these steps in order — each one narrows the problem rapidly.

# Quick tools & info to gather first

* **Datasheet** of the SPI slave and SPI master IP (timing, mode, reset/power requirements).
* **Schematic / pinout** (which MCU pins are SPI, which CS line).
* Tools: serial console, JTAG, logic analyzer (Saleae-like), oscilloscope, `git` + kernel build, `dmesg`, `strace`, `trace-cmd`/`ftrace` (optional), `spidev_test` or Python `spidev`.

---

## Step-by-step debug flow

### 1) Hardware sanity — power, reset, wiring

* Verify power rails, reset pins, decoupling. Confirm slave not held in reset.
* Check wiring: SCLK, MOSI, MISO, CS (and any HOLD/WP for SPI flash). Ensure correct pull-ups/pull-downs if required.
* If possible, power-cycle after any wiring change.

### 2) Pinmux / GPIO / Pin configuration

* On SoC, confirm the pins are muxed to SPI master, not GPIO. Wrong pinmux is the most common issue.
* On Linux: verify device tree pinctrl node exists and is `status = "okay"` for the SPI controller.

### 3) Confirm SPI master/driver presence in OS

* On Linux:

```bash
dmesg | grep -i spi
ls /sys/bus/spi/devices
```

* If the master driver didn’t register, the problem is at device-tree/board bringup (pinctrl, clocks, reset).

### 4) Device tree / platform setup

* Verify the SPI controller node and child device node (slave) exist and have correct `reg` (chip select), `spi-max-frequency`, `compatible`.
  Example (DT):

```dts
&spi1 {
    status = "okay";
    flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0>; /* CS 0 */
        spi-max-frequency = <50000000>;
    };
};
```

### 5) Minimal probe logging in your driver

* Add `dev_info()`/`pr_info()` at probe entry to ensure probe runs. If probe not called — DT or driver registration is wrong.
* Example:

```c
static int my_spi_probe(struct spi_device *spi)
{
    dev_info(&spi->dev, "my_spi_probe: cs=%d, maxhz=%u\n",
             spi->chip_select, spi->max_speed_hz);
    ...
}
```

### 6) Basic userspace smoke test (spidev)

* Temporarily bind the slave to `spidev` or use `spidev_test`:

```bash
# If spidev is enabled
python3 -c "import spidev; s=spidev.SpiDev(); s.open(0,0); s.max_speed_hz=500000; print(s.xfer2([0x9F,0,0,0]))"
```

* If loopback (MOSI->MISO) returns identical data, master is working.

### 7) Do a loopback test (hardware)

* Connect MOSI → MISO physically (with CS inactive) and run the spidev test to verify master timing and bit order.
* If loopback fails: check mode (CPOL/CPHA), bit order, clock generation.

### 8) Check mode, polarity, phase, bit-order & CS polarity

* Confirm correct `spi->mode` (0..3), `bits_per_word`, `lsb_first`. Wrong mode is a frequent cause.
* Also check whether CS is active-low or active-high (use `.controller_data` or device tree `cs-gpios` if CS is GPIO-driven).

### 9) Observe waveforms with logic analyzer / scope

* Capture a transfer and verify:

  * CS goes low before clocking.
  * Clock frequency matches `max_speed_hz`.
  * Data valid edge matches CPHA/CPOL (Mode 0: sample on rising, idle low).
  * MISO is tri-stated when slave not driving.
* If data looks shifted, try changing mode or bit order.

### 10) Validate SPI messages in driver code (kernel)

* Typical transfer:

```c
u8 tx[] = {0x9F,0,0,0}, rx[4];
struct spi_transfer t = { .tx_buf = tx, .rx_buf = rx, .len = 4 };
struct spi_message m;
spi_message_init(&m);
spi_message_add_tail(&t, &m);
ret = spi_sync(spi, &m);
```

* Check return of `spi_sync()` and inspect `rx[]`. Add `dev_dbg()` prints.

### 11) DMA / cache coherence (if using DMA)

* If transfers use DMA, ensure buffers are DMA-safe:

  * Use `dma_alloc_coherent()` or `dma_map_single()`/`dma_unmap_single()` as appropriate.
  * Clean/Invalidate CPU cache before/after DMA.
* Symptoms: garbage or partial data only when using large transfer sizes.

### 12) Interrupts & /proc/interrupts

* Check `/proc/interrupts` to see if SPI IRQs fire during transfers (if controller uses IRQs).
* If IRQ not firing, check controller init, `request_irq()` and interrupt routing.

### 13) Kernel-level tracing & dynamic debug

* Enable debug prints in your driver: `dev_dbg()`, `dev_err()`; then control with `dynamic_debug`:

```bash
echo 'file drivers/spi/my_driver.c +p' > /sys/kernel/debug/dynamic_debug/control
dmesg -w
```

* Use `trace-cmd`/`ftrace` to capture SPI tracepoints if available:

```bash
trace-cmd record -e spi
trace-cmd report
```

### 14) Test with known-good slave or example commands

* Try reading JEDEC ID from SPI flash (`0x9F`) — many examples exist. If a known-good slave responds, your master and wiring are correct.

### 15) Register-level debug (last resort)

* Read SPI controller hardware registers (via `devmem` or registers mapped in kernel) to check status, FIFOs, and error flags per IP manual. Use JTAG if necessary.

---

## Common problems & quick fixes

* **No probe / no device node:** DT pinctrl/clock/reset wrong → fix DT.
* **Garbage on MISO:** Wrong mode (CPOL/CPHA) or wrong bit-order.
* **Correct pattern in logic analyzer but kernel reads wrong:** cache/DMA mapping issue.
* **CS toggles weirdly / missing between transfers:** set `.cs_change` properly or use separate transfers.
* **Slave needs wake-up or specific delay:** add `delay_usecs` or `delay_ms` before/after transfers.
* **Partial transfers:** FIFO thresholds or DMA length limits — check controller datasheet.

---

## Handy commands & files

```bash
dmesg | tail -n 200
ls /sys/bus/spi/devices
cat /proc/interrupts | grep spi
# Enable dynamic debug for your file
echo 'file drivers/spi/my_driver.c +p' > /sys/kernel/debug/dynamic_debug/control
```

---

## Minimal userspace test (C)

```c
// compile: gcc -o spitest spitest.c
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
int main(){
  int fd = open("/dev/spidev0.0", O_RDWR);
  uint8_t tx[] = {0x9F,0,0,0}, rx[4];
  struct spi_ioc_transfer tr = { .tx_buf=(unsigned long)tx, .rx_buf=(unsigned long)rx, .len=4 };
  ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
  close(fd);
  return 0;
}
```

---

## Final checklist (fast)

1. Power / reset / wiring OK.
2. Pinmux and clocks enabled.
3. SPI master driver registered (`dmesg`, `/sys/bus/spi`).
4. DT slave node correct.
5. Loopback works.
6. Logic analyzer: correct mode & timing.
7. `spi_sync` returns 0 and rx data expected.
8. DMA/cache handled if used.
9. Add debug prints & tracepoints to confirm control flow.

---

If you want, I can:

* Generate a **one-page debug checklist** you can print, or
* Produce a **kernel driver minimal template** with debug hooks and DMA-safe transfers, or
* Walk through **a specific failing waveform** if you upload analyzer screenshots or raw capture. Which would help most next?


👌 Got it — since you asked for a **one-page debug checklist you can print and keep next to you while debugging SPI**, here’s a compact version.

---

# 🛠 SPI Driver Debug Checklist (One-Pager)

### 1. Hardware Setup

* [ ] Verify **power rails / reset** for the slave.
* [ ] Check **pin wiring** (MOSI, MISO, SCLK, CS, WP/HOLD if any).
* [ ] Confirm **pinmux** → pins set to SPI, not GPIO.
* [ ] Oscilloscope: CS toggles? SCLK present when transfer starts?

### 2. OS / Driver Bring-Up

* [ ] `dmesg | grep spi` → master registered?
* [ ] `ls /sys/bus/spi/devices` → slave visible?
* [ ] Device Tree: `compatible`, `reg=<cs>`, `spi-max-frequency`, `cs-gpios`.
* [ ] Add `dev_info()` in probe — confirm probe executes.

### 3. Quick Tests

* [ ] Loopback MOSI→MISO test (`spidev_test` or Python `spidev`).
* [ ] Try **known-good slave command** (e.g. SPI NOR `0x9F` for JEDEC ID).
* [ ] Verify **SPI mode** (CPOL/CPHA), **bit order**, **chip-select polarity**.

### 4. Logic Analyzer / Scope

* [ ] CS goes **low before clocking**.
* [ ] Clock frequency ≈ `spi->max_speed_hz`.
* [ ] Data stable on correct edge (Mode 0: sample rising, idle low).
* [ ] MOSI matches expected TX; MISO not floating.

### 5. Driver-Side Checks

* [ ] Use `spi_sync()` with debug prints:

  ```c
  dev_dbg(&spi->dev, "TX=%*ph RX=%*ph\n", len, tx, len, rx);
  ```
* [ ] Handle **cs\_change / delay\_usecs** if slave requires gaps.
* [ ] If using DMA → ensure buffers are **DMA-safe** (`dma_alloc_coherent` or `dma_map_single`).
* [ ] Clean/Invalidate caches when sharing with DMA.

### 6. Interrupts / Kernel Tools

* [ ] `/proc/interrupts` → IRQ count increasing?
* [ ] Enable **dynamic debug**:

  ```bash
  echo 'file drivers/spi/*.c +p' > /sys/kernel/debug/dynamic_debug/control
  ```
* [ ] Use `trace-cmd record -e spi` to capture transfers.

### 7. Common Fixes

* Garbage data → Wrong mode/bit order.
* No probe → DT mismatch or missing clocks/resets.
* Data shifts / missing bits → CS polarity/timing issue.
* Works in PIO, fails in DMA → cache/coherence issue.

---

📌 **Golden Rule:**
Always start from **power + wiring → pinmux → probe → loopback → logic analyzer → driver buffers → DMA/cache**. Don’t jump into code before HW signals are verified.

---

Do you want me to now also prepare the **kernel driver minimal template with built-in debug hooks + DMA-safe transfer example**, so you can directly drop it into your kernel tree for testing?


Do you want me to now also prepare the kernel driver minimal template with built-in debug hooks + DMA-safe transfer example, so you can directly drop it into your kernel tree for testing?