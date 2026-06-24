# I2C in Linux — Rapid-Fire Interview Q&A

Short, sharp answers you can say out loud. Grouped from protocol → kernel → debug.

---

## A. Protocol basics

**Q: What is I2C and why is it used?**
A two-wire (SDA + SCL), synchronous, multi-master serial bus for connecting
low-speed on-board peripherals (sensors, EEPROMs, RTCs, PMICs) to a CPU. Cheap:
only two pins regardless of how many devices.

**Q: Why are the lines open-drain with pull-ups?**
Devices can only pull a line LOW; the pull-up restores HIGH. This gives
**wired-AND** behavior, which enables multi-master arbitration and clock
stretching without bus contention or damage from two drivers fighting.

**Q: How does a single transfer look on the wire?**
START → 7-bit address + R/W bit → (ACK) → 8-bit data + ACK/NACK (repeated) →
STOP. Data may only change while SCL is LOW; START/STOP are the special cases
where SDA moves while SCL is HIGH.

**Q: What is a repeated START?**
A new START issued without an intervening STOP — used to switch direction
(write register pointer, then read) while keeping the bus, so another master
can't jump in between.

**Q: What is clock stretching?**
A slow slave holds SCL LOW to pause the master until it's ready. The master must
wait. It's the main reason I2C timing is flexible.

**Q: How is multi-master arbitration resolved?**
By wired-AND: a master that drives HIGH but reads LOW lost arbitration and backs
off. The winner is unaware; no data is corrupted.

**Q: 7-bit vs 10-bit addressing?**
7-bit gives 112 usable addresses (most common). 10-bit (flag `I2C_M_TEN`) exists
but is rare.

**Q: SMBus vs I2C?**
SMBus is a stricter Intel-defined subset: defined timeouts (25–35 ms), min clock
(10 kHz), fixed transaction types (read/write byte/word/block, process call),
optional PEC (CRC-8). Most register chips speak SMBus-style ops.

---

## B. Linux architecture

**Q: Describe the Linux I2C architecture.**
Four players decoupled by the **I2C core**:
1. **Adapter/bus driver** (`i2c_adapter` + `i2c_algorithm`) — knows the SoC
   controller HW, implements `master_xfer`.
2. **I2C core** — routing, locking, SMBus emulation, matching, sysfs, /dev,
   DT/ACPI enumeration.
3. **Client/device driver** (`i2c_driver`) — drives one chip type, registers a
   `.probe`.
4. **Client** (`i2c_client`) — one physical chip instance on a bus at an address.

**Q: Why split adapter and client drivers?**
Decoupling. Any client driver works on any controller and vice-versa, because
both talk only to the core via standard structures. Write the LM75 driver once;
it runs on every SoC.

**Q: What's the single most important adapter callback?**
`i2c_algorithm.master_xfer(adap, msgs, num)` — processes an array of `i2c_msg`,
issues (repeated) STARTs and one STOP, returns the number of messages on success
or a negative errno.

**Q: What does `i2c_transfer()` do?**
The core's central entry point. Takes the per-bus `bus_lock`, calls
`__i2c_transfer()` → `adap->algo->master_xfer()`, applies retries/timeout, then
unlocks. Guarantees exclusive bus access per transaction.

**Q: How does a client driver actually read/write?**
Either raw `i2c_transfer()` with `i2c_msg[]`, the convenience wrappers
`i2c_master_send/recv()`, or SMBus helpers `i2c_smbus_read_byte_data()` etc. Many
modern drivers use **regmap-i2c** (`regmap_read/write`).

**Q: What is `i2c_msg`?**
One segment of a transfer: `{addr, flags, len, buf}`. `flags & I2C_M_RD` means
read. An array of them is one transaction; the core inserts repeated STARTs
between segments.

**Q: How is a driver matched to a device?**
The bus type's `match` tries, in order: **device-tree `compatible`** vs
`of_match_table` → **ACPI** → **`id_table`** (`i2c_device_id`). On match the core
calls `.probe(client)`.

**Q: How are I2C devices discovered/instantiated?**
Several ways: **Device Tree** child nodes (dominant on ARM), **ACPI**
(`_HID`/`_CRS` on x86), legacy **board info** (`i2c_register_board_info`),
explicit **`i2c_new_client_device()`**, user-space **`new_device`** sysfs, or
**auto-detect** (`.detect` + `address_list`, discouraged).

**Q: What happens in `.probe()`?**
Verify the chip is really present (read an ID/WHO_AM_I register), allocate
per-device state (`devm_kzalloc`), `i2c_set_clientdata()`, initialize the chip,
and register with the relevant subsystem (hwmon/iio/rtc/input/regmap).

**Q: What is `i2c_check_functionality()` for?**
An adapter advertises capabilities via `functionality()` (`I2C_FUNC_I2C`,
`I2C_FUNC_SMBUS_BYTE_DATA`, …). A client checks it before using ops the adapter
might not support — important because some controllers are SMBus-only.

**Q: If an adapter only implements `master_xfer`, can clients use SMBus calls?**
Yes — the core **emulates** SMBus on top of `master_xfer`
(`i2c_smbus_xfer_emulated`). The reverse isn't true: a pure-SMBus controller
can't do arbitrary raw I2C.

**Q: `i2c_add_adapter()` vs `i2c_add_numbered_adapter()`?**
First assigns a dynamic bus number; second requests a fixed `adap.nr` (when you
need a stable `/dev/i2c-N`). Both trigger enumeration of DT child chips.

**Q: How does locking work?**
The core holds `adap->bus_lock` (an rt_mutex) around each transaction, so only
one transfer happens on a physical bus at a time. There's also adapter-level
muxing support (`I2C_LOCK_ROOT_ADAPTER`/`SEGMENT`) for I2C muxes.

---

## C. User space & debugging

**Q: How do you reach I2C from user space?**
Through `i2c-dev`: open `/dev/i2c-N`, `ioctl(I2C_SLAVE, addr)`, then
`read()`/`write()` or `ioctl(I2C_RDWR/I2C_SMBUS, …)`. Tools: `i2cdetect`,
`i2cget`, `i2cset`, `i2cdump`.

**Q: `i2cdetect -y 1` shows `UU` at an address. Meaning?**
A kernel driver is already bound to that device (in use). `--` means nothing
responded; a hex number means a device ACKed and is free.

**Q: A device isn't probing. How do you debug?**
Check `dmesg`; confirm DT `compatible`/`reg` match the driver; `i2cdetect` to see
if the chip ACKs at all; verify pull-ups/power/pinmux; check
`/sys/bus/i2c/devices/` and `/sys/bus/i2c/drivers/<name>/`; confirm
`i2c_check_functionality`; scope SDA/SCL if hardware is suspect.

**Q: Bus is stuck LOW / “arbitration lost” / device hung. Fix?**
A slave may be mid-transfer holding SDA. Recovery: master toggles SCL up to 9
clocks to flush the slave, then issues STOP. Linux supports this via
`i2c_bus_recovery_info` / `i2c_recover_bus()`. GPIO bit-bang recovery is common.

**Q: Where does the real code live in the kernel?**
`drivers/i2c/i2c-core-base.c`, `i2c-core-smbus.c`, `i2c-dev.c`;
`drivers/i2c/busses/*` (adapters, e.g. `i2c-imx.c`, `i2c-designware-*`);
headers `include/linux/i2c.h`, `include/uapi/linux/i2c.h`, `i2c-dev.h`.

---

## D. Gotchas interviewers love

- **`master_xfer` return value** is the *number of messages*, not bytes; the core
  checks `== num`.
- **Open-drain means you cannot drive HIGH** — forgetting pull-ups is the #1 HW
  bug (bus stays low or floats).
- **Address is 7-bit in the API** (`client->addr`), but on the wire it's shifted
  left and OR'd with the R/W bit. Mixing up "8-bit address" from a datasheet vs
  the 7-bit Linux address is a classic mistake.
- **Repeated START vs STOP+START**: building one `i2c_msg[2]` keeps the bus
  (atomic read-after-pointer); two separate `i2c_transfer` calls release the bus
  in between and can race with other masters.
- **SMBus block vs I2C block** read are different (SMBus block has a leading
  length byte). Pick the right helper.
- **Don't sleep in atomic context**: most adapters can sleep (IRQ-driven); for
  callers in atomic context there's `i2c_transfer` vs the atomic transfer path
  (`I2C_M_...`, `i2c_smbus_xfer` in `i2c_atomic`/`transfer_atomic`).
- **`devm_*`** in probe auto-frees on remove/probe-failure — preferred for
  leak-free drivers.

---

## E. 30-second whiteboard: read a 16-bit register

```c
u8 reg = 0x00, buf[2];
struct i2c_msg msgs[2] = {
    { .addr = c->addr, .flags = 0,        .len = 1, .buf = &reg },
    { .addr = c->addr, .flags = I2C_M_RD, .len = 2, .buf = buf  },
};
if (i2c_transfer(c->adapter, msgs, 2) != 2)
    return -EIO;
val = (buf[0] << 8) | buf[1];   /* big-endian */
```
"Write the register pointer, repeated START, read two bytes, one STOP — done in a
single locked transaction."
