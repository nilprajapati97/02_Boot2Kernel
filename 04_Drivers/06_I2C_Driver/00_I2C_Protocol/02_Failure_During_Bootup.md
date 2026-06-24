# Debugging I2C Failure During Bootup (Linux Kernel)

This is a very common interview question in Linux Kernel, BSP, AMD, NVIDIA, Qualcomm, Intel, Automotive, and Embedded Linux roles.

---

# Step 1: Understand the Complete Boot Flow

During boot:

```text
BootROM
   ↓
Bootloader (U-Boot)
   ↓
Kernel Start
   ↓
Device Tree Parsing
   ↓
I2C Controller Driver Probe
   ↓
I2C Adapter Registration
   ↓
I2C Device Creation
   ↓
I2C Device Driver Probe
```

Failure can happen at any stage.

---

# Step 2: Identify What Exactly Failed

Ask:

### Case 1

Controller driver probe failed?

```text
I2C Controller Not Working
```

Example:

```text
i2c_designware
i2c_qcom_geni
tegra_i2c
```

---

### Case 2

Controller probed successfully but slave not detected?

```text
No ACK from device
```

---

### Case 3

Device detected but driver probe failed?

```text
tmp102_probe() failed
```

---

### Case 4

Bus stuck?

```text
SDA Low forever
```

---

# Step 3: Check Kernel Logs

First command:

```bash
dmesg | grep -i i2c
```

Examples:

```text
i2c_designware 12340000.i2c: controller timed out
```

or

```text
i2c 0: adapter registered
```

or

```text
probe of 0-0048 failed
```

---

# Step 4: Verify Controller Driver Probe

Add logs inside:

```c
probe()
{
    pr_info("I2C Controller Probe\n");
}
```

Check:

```text
probe called ?
```

If not called:

Possible issues:

* DT issue
* compatible mismatch
* controller disabled

---

# Step 5: Check Device Tree

Example:

```dts
i2c0: i2c@12340000 {
        compatible = "snps,designware-i2c";
        status = "okay";
};
```

Verify:

### Compatible

```text
Driver compatible
        ==
DT compatible
```

Example:

```text
snps,designware-i2c
```

must match OF table.

---

### Status

Bad:

```dts
status = "disabled";
```

Good:

```dts
status = "okay";
```

---

# Step 6: Verify Controller Clock

Most common boot failure.

Driver may fail:

```text
Unable to get clock
```

Check:

```dts
clocks = <&gcc GCC_I2C_CLK>;
```

Kernel log:

```text
clk_get failed
```

Inside probe:

```c
clk_prepare_enable()
```

must succeed.

---

# Step 7: Verify Reset Control

Many SoCs keep controller in reset.

Example:

```dts
resets = <&gcc GCC_I2C_RESET>;
```

Probe may fail if:

```text
reset not deasserted
```

Check:

```c
reset_control_deassert()
```

---

# Step 8: Verify Pinmux

One of the biggest reasons.

Controller may probe successfully.

But bus never works.

---

Example:

Expected:

```text
GPIO10 -> SDA
GPIO11 -> SCL
```

Actual:

```text
GPIO10 -> GPIO
GPIO11 -> GPIO
```

Bus dead.

---

Check:

```bash
cat /sys/kernel/debug/pinctrl/*/pinmux-pins
```

Verify:

```text
I2C function selected
```

---

# Step 9: Verify Pull-up Resistors

Most common hardware issue.

Remember:

```text
I2C is open-drain
```

Without pull-up:

```text
SDA = LOW
SCL = LOW
```

forever.

---

Measure:

```text
SDA
SCL
```

using oscilloscope.

Idle state should be:

```text
SDA HIGH
SCL HIGH
```

---

Expected:

```text
3.3V
```

or

```text
1.8V
```

depending on board.

---

# Step 10: Check Bus Stuck Condition

Oscilloscope:

```text
SDA = LOW
```

forever.

Example:

```text
Sensor crashed
```

and holding SDA low.

Waveform:

```text
SCL ─────────────

SDA ____________
```

---

Linux reports:

```text
Bus Busy
```

or

```text
Transfer Timeout
```

---

# Step 11: Verify I2C Adapter Registration

Check:

```bash
ls /sys/class/i2c-adapter
```

Example:

```text
i2c-0
i2c-1
i2c-2
```

---

No adapter?

Controller probe failed.

---

# Step 12: Use i2cdetect

Check slave visibility.

```bash
i2cdetect -y 0
```

Expected:

```text
48
50
68
```

Example:

```text
0x48 -> TMP102
0x50 -> EEPROM
0x68 -> RTC
```

---

No device?

Possible:

* Power missing
* Reset issue
* Wrong address
* Pull-up issue
* Wiring issue

---

# Step 13: Dynamic Debug

Enable I2C debug.

```bash
echo 'file drivers/i2c/* +p' \
> /sys/kernel/debug/dynamic_debug/control
```

Observe:

```text
START
Address
ACK
STOP
```

transactions.

---

# Step 14: Enable Tracepoints

Check:

```bash
trace-cmd record -e i2c
```

View:

```bash
trace-cmd report
```

Example:

```text
i2c_write
i2c_read
i2c_reply
```

---

# Step 15: Check Probe Deferral

Very common.

Boot log:

```text
probe defer
```

or

```text
-EPROBE_DEFER
```

Example:

```text
PMIC not ready
Clock not ready
Regulator not ready
```

Driver retries later.

---

# Step 16: Check Regulator

Many sensors require power.

Example:

```dts
vdd-supply = <&pmic_ldo3>;
```

Inside probe:

```c
regulator_enable()
```

Verify:

```text
Voltage present?
```

Measure physically.

---

# Step 17: Verify Actual Transaction

Suppose reading register:

```c
i2c_smbus_read_byte_data(client, 0x10);
```

Expected sequence:

```text
START
ADDR+W
ACK

REG=0x10
ACK

RESTART

ADDR+R
ACK

DATA
NACK

STOP
```

Check on logic analyzer.

---

# Step 18: Use Logic Analyzer

Best debugging tool.

Observe:

```text
START
Address
ACK
Data
STOP
```

---

Example Failure:

```text
START

0x48

NACK
```

Meaning:

```text
No device responding
```

---

Example:

```text
START

0x48

ACK

0x10

NACK
```

Meaning:

```text
Device rejected register
```

---

# Step 19: Decode Timeout Errors

Example log:

```text
i2c transfer timed out
```

Internally:

```text
Master generated transaction

No completion interrupt

Timeout occurred
```

Check:

* IRQ registered?
* IRQ fired?
* Controller state machine?

---

# Step 20: Interrupt Debug

Check:

```bash
cat /proc/interrupts
```

Example:

```text
45 i2c0
```

Transfer count should increase.

---

No increment?

Possible:

```text
IRQ not connected
IRQ disabled
Wrong DT interrupt
```

---

# Step 21: Read Controller Registers

Example:

```bash
devmem
```

or

```bash
debugfs
```

Check:

### Status Register

```text
BUS_BUSY
TX_EMPTY
RX_FULL
ARB_LOST
```

---

Example:

```text
ARB_LOST = 1
```

Meaning:

```text
Arbitration lost
```

---

# Step 22: Common Bootup Failure Checklist

| Check      | Expected     |
| ---------- | ------------ |
| DT status  | okay         |
| Compatible | Match driver |
| Clock      | Enabled      |
| Reset      | Released     |
| Pinmux     | I2C mode     |
| Regulator  | ON           |
| IRQ        | Working      |
| SDA        | High idle    |
| SCL        | High idle    |
| Adapter    | Registered   |
| Device     | ACKing       |
| Probe      | Called       |

---

# Real Interview Scenario

**Question:** Sensor at address 0x48 is not probing during boot. How do you debug?

### Answer Flow

1. Check `dmesg`
2. Verify controller probe
3. Verify adapter registration
4. Run `i2cdetect`
5. Check Device Tree address
6. Verify power regulator
7. Verify clock/reset
8. Verify pinmux
9. Probe SDA/SCL using oscilloscope
10. Capture I2C transaction using logic analyzer
11. Check ACK/NACK
12. Read controller status registers
13. Verify interrupt count
14. Enable dynamic debug and tracepoints
15. Confirm driver's `probe()` execution

This systematic approach usually finds **95% of I2C boot-time failures** in Linux-based embedded systems.


For AMD, NVIDIA, Qualcomm, Intel, Samsung, and Linux Kernel interviews, simply saying "check dmesg and i2cdetect" is usually not enough.

They often ask:

> "What exactly happens internally? What registers do you check? What waveforms do you expect? What does each failure indicate?"

Let's go deep.

---

# Question 1:

# Sensor at 0x48 is not detected during boot. How do you debug?

## Step 1: Verify Controller Probe

Boot flow:

```text
start_kernel()
    |
driver_init()
    |
platform_driver_register()
    |
i2c controller probe()
```

Check:

```bash
dmesg | grep i2c
```

Expected:

```text
i2c_designware 12340000.i2c: initialized
```

If missing:

```text
probe() never executed
```

Possible causes:

### DT mismatch

Driver:

```c
static const struct of_device_id dw_i2c_of_match[] = {
{
.compatible = "snps,designware-i2c"
}
};
```

DT:

```dts
compatible = "snps,dw-i2c";
```

Mismatch.

Probe never called.

---

# Question 2:

# Controller probe called but adapter not created.

Internally:

```c
i2c_add_adapter()
```

creates:

```text
/sys/class/i2c-adapter/i2c-0
```

Check:

```bash
ls /sys/class/i2c-adapter
```

No adapter means:

```text
probe failed before i2c_add_adapter()
```

---

Possible failures:

### Clock failure

```c
clk = devm_clk_get()
```

returns:

```text
-ENOENT
```

Kernel:

```text
unable to get clock
```

---

### Reset failure

```c
reset_control_deassert()
```

failed.

Controller remains in reset.

---

# Question 3:

# Why can i2cdetect show no device even though wiring is correct?

Because ACK is missing.

Transaction:

```text
START

0x48 + W

ACK expected
```

Waveform:

```text
Master:

SDA
1 0 0 1 0 0 0 0

9th clock

Slave should pull LOW
```

If ACK absent:

```text
9th clock
SDA remains HIGH
```

NACK.

Possible reasons:

### Device not powered

### Wrong address

### Reset pin active

### Clock not running inside chip

### SDA/SCL swapped

---

# Question 4:

# How do you identify power issue?

Measure:

```text
VDD
```

using scope.

Example:

```text
Expected 1.8V

Actual 0V
```

Sensor dead.

---

Linux side:

```dts
vdd-supply = <&pmic_ldo3>;
```

Driver:

```c
regulator_enable(vdd);
```

Verify:

```bash
cat /sys/kernel/debug/regulator/regulator_summary
```

---

# Question 5:

# SDA stuck LOW. Explain in detail.

Normal idle:

```text
SCL HIGH

SDA HIGH
```

Fault:

```text
SCL HIGH

SDA LOW
```

Waveform:

```text
SCL ────────────

SDA ___________
```

Bus busy forever.

---

Why?

### Scenario 1

Slave reset occurred mid-transfer.

Example:

```text
Master sent 5 bits

Sensor reset
```

Sensor thinks transaction incomplete.

Keeps SDA LOW.

---

### Scenario 2

Clock lost.

Slave waiting for clocks.

Holds SDA LOW.

---

Linux reports:

```text
bus busy
```

or

```text
timeout
```

---

Recovery:

```text
Generate 9 clocks manually
```

Waveform:

```text
SCL

_|¯|_|¯|_|¯|_|¯|_
```

Slave shifts out pending data.

Releases SDA.

---

# Question 6:

# Why 9 clocks fix stuck bus?

Internally slave contains shift register:

```text
D7 D6 D5 D4 D3 D2 D1 D0
```

Needs:

```text
8 bits
+
ACK bit
```

Total:

```text
9 clocks
```

After last bit:

```text
FSM returns IDLE
```

SDA released.

---

# Question 7:

# How do you debug arbitration loss?

Multi-master:

```text
CPU
MCU
```

both start.

CPU sends:

```text
1
```

MCU sends:

```text
0
```

Bus:

```text
0
```

because LOW wins.

CPU reads:

```text
expected = 1

actual = 0
```

Hardware sets:

```text
ARB_LOST
```

register.

---

DesignWare register:

```text
IC_TX_ABRT_SOURCE
```

contains:

```text
ARB_LOST
```

---

# Question 8:

# Transfer timeout. Explain deeply.

Application:

```c
i2c_transfer()
```

Driver:

```c
wait_for_completion_timeout()
```

Flow:

```text
START

Address

Data

Interrupt expected
```

But:

```text
IRQ never arrives
```

Timeout.

---

Root causes:

### Wrong interrupt number

DT:

```dts
interrupts = <35>;
```

actual:

```text
36
```

---

### Interrupt masked

Register:

```text
INTR_MASK
```

wrong.

---

### Hardware hung

FSM stuck.

---

Check:

```bash
cat /proc/interrupts
```

Before:

```text
45: 100 i2c0
```

After transfer:

```text
45: 100 i2c0
```

No increment.

IRQ issue.

---

# Question 9:

# How do you debug clock stretching?

Master releases SCL.

Slave holds LOW.

Waveform:

```text
Master:
     HIGH

Actual:
     LOW
```

Scope:

```text
SCL ___
```

for long duration.

---

Linux sees:

```text
transfer timeout
```

if stretch exceeds timeout.

---

Possible causes:

### Slow EEPROM

### Sensor firmware hang

### Hardware issue

---

# Question 10:

# Probe called but sensor driver not probed.

Controller working.

Adapter working.

Device exists.

Still:

```text
probe() not called
```

---

Check DT:

```dts
tmp102@48 {
compatible = "ti,tmp102";
reg = <0x48>;
};
```

Driver:

```c
static const struct of_device_id tmp102_ids[] = {
{
.compatible = "ti,tmp102"
}
};
```

Mismatch?

```text
probe never happens
```

---

# Question 11:

# How do you verify actual I2C transaction?

Best answer in interview:

### Use Logic Analyzer

Capture:

```text
START
ADDR
ACK
DATA
ACK
STOP
```

Example:

```text
START

0x48

NACK
```

Means:

```text
Physical device not responding
```

---

Example:

```text
START

0x48

ACK

0x10

ACK

RESTART

0x49

ACK

DATA

STOP
```

Perfect transaction.

---

# Question 12:

# If I2C fails only during boot but works later?

This is a favorite Qualcomm/NVIDIA interview question.

Most likely:

```text
Probe ordering issue
```

Examples:

### Regulator not ready

```text
-EPROBE_DEFER
```

### PMIC not initialized

### Clock provider not ready

### GPIO expander not probed

### Runtime PM race

---

Check:

```bash
dmesg | grep defer
```

---

# Question 13:

# What registers do you inspect in hardware?

Common registers:

### STATUS

```text
BUS_BUSY
RX_FULL
TX_EMPTY
```

---

### ENABLE

```text
Controller enabled?
```

---

### TX_ABRT_SOURCE

```text
No ACK
Arbitration loss
```

---

### RAW_INTR_STAT

```text
RX_DONE
STOP_DET
TX_ABRT
```

---

### SCL_COUNT

Clock generation.

---

# Interview Closing Answer

When asked:

> "I2C device is not detected during boot. Explain your debugging methodology."

A senior-level answer is:

```text
1. Verify controller probe.
2. Verify adapter registration.
3. Verify DT compatibility.
4. Verify clocks and resets.
5. Verify regulators.
6. Verify pinmux.
7. Verify SDA/SCL idle levels.
8. Run i2cdetect.
9. Capture bus using logic analyzer.
10. Check ACK/NACK.
11. Inspect controller status registers.
12. Verify interrupt generation.
13. Check arbitration loss and timeout registers.
14. Analyze probe ordering and EPROBE_DEFER.
15. Perform bus recovery if SDA is stuck low.
```
