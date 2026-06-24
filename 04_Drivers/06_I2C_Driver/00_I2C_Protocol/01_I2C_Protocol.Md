# I2C (Inter-Integrated Circuit) Protocol – Complete Detailed Explanation

I2C is a **synchronous serial communication protocol** developed by NXP Semiconductors (originally Philips) for communication between integrated circuits on the same board.

It is one of the most commonly asked topics in Linux Device Driver, Embedded Systems, AMD, NVIDIA, Qualcomm, Intel, and Automotive interviews.

---

# 1. Why I2C Was Introduced?

Before I2C:

* Parallel communication required many wires.
* UART requires TX/RX pair for each device.
* SPI requires multiple chip-select lines.

Suppose a microcontroller needs to communicate with:

* EEPROM
* RTC
* Temperature Sensor
* Accelerometer

SPI would require:

```
SCLK
MOSI
MISO
CS1
CS2
CS3
CS4
```

Total many wires.

I2C reduces everything to only:

```
SCL (Clock)
SDA (Data)
```

Regardless of the number of devices.

---

# 2. Where is I2C Used?

## Embedded Systems

* EEPROM
* RTC
* Sensors
* ADC/DAC

## Mobile Phones

* Touch Controllers
* Power Management IC
* Battery IC
* Camera Control

## Linux Systems

Example:

```bash
i2cdetect -y 0
```

shows devices connected on I2C bus.

## Automotive

* Temperature sensors
* PMIC
* Dashboard electronics

---

# 3. When Should I Use I2C?

Use I2C when:

✅ Few Mbps not required

✅ Distance is small

✅ Many devices on same bus

✅ Pin count must be reduced

Avoid I2C when:

❌ Very high speed required

❌ Long distance communication

❌ Heavy data transfer

Then SPI is preferred.

---

# 4. How I2C Works?

Only 2 wires:

```
SCL  -> Clock
SDA  -> Data
```

Bus contains:

```
Master
Slave
Slave
Slave
```

Example:

```
          SDA
Master -------- Sensor

          SCL
Master -------- Sensor
```

---

# 5. Master and Slave

Master:

* Generates clock
* Starts transfer
* Stops transfer

Slave:

* Responds when addressed

Example:

```
CPU -----> Sensor
```

CPU = Master

Sensor = Slave

---

# 6. Open Drain Concept

Most important interview question.

I2C lines are NOT driven high.

Devices only pull line LOW.

External pull-up resistor makes line HIGH.

```
 VCC
  |
 [R]
  |
 SDA ---- Device
```

Why?

Because many devices share same line.

If one drives HIGH and another LOW:

```
SHORT CIRCUIT
```

could occur.

Hence:

```
LOW  -> actively driven
HIGH -> pull-up resistor
```

---

# 7. Idle Bus Condition

Bus idle:

```
SCL = HIGH
SDA = HIGH
```

Waveform:

```
SCL ─────────────── HIGH

SDA ─────────────── HIGH
```

---

# 8. START Condition

Communication begins with START.

Definition:

```
SDA HIGH -> LOW
while SCL HIGH
```

Waveform:

```
SCL ────────────────

SDA ─────┐
         └─────────
```

At that moment:

```
START detected
```

Every slave starts listening.

---

# 9. STOP Condition

Definition:

```
SDA LOW -> HIGH
while SCL HIGH
```

Waveform:

```
SCL ────────────────

SDA _________┐
             └──────
```

Bus becomes free.

---

# 10. Address Frame

After START:

Master sends address.

### 7-bit Address

```
A6 A5 A4 A3 A2 A1 A0
```

followed by:

```
R/W bit
```

Total:

```
7 + 1 = 8 bits
```

Example:

```
1010000 + 0
```

0 = Write

1 = Read

---

# 11. Complete Write Transaction

Suppose:

```
Slave Address = 0x50
Data = 0xAA
```

Sequence:

```
START

Address + Write

ACK

Data

ACK

STOP
```

---

# Waveform

```
SCL  _|¯|_|¯|_|¯|_|¯|_

SDA  Addr Addr Addr ACK Data ACK
```

---

# 12. ACK and NACK

After every 8 bits:

Receiver sends ACK.

ACK:

```
9th clock
SDA = LOW
```

Waveform:

```
Bit1 Bit2 ... Bit8 ACK

SDA               __
                 |
                 |
```

Meaning:

```
Data received successfully
```

NACK:

```
SDA HIGH
```

Meaning:

```
Not received
```

or

```
No device present
```

---

# 13. Read Transaction

Master wants data.

Sequence:

```
START

Address + Read

ACK

Slave sends data

Master ACK

STOP
```

Waveform:

```
Master -> Address

Slave  -> Data
```

---

# 14. Repeated START

Very important interview topic.

Instead of STOP:

Master issues another START.

```
START
Address Write
ACK

Repeated START

Address Read
ACK

Data
STOP
```

Used heavily in:

* EEPROM
* Sensors

---

# 15. Arbitration

Multiple masters can exist.

Example:

```
Master1
Master2
```

both start simultaneously.

---

Suppose:

Master1 sends:

```
1
```

Master2 sends:

```
0
```

Since LOW dominates:

```
Bus = 0
```

Master1 detects mismatch.

Master1 loses arbitration.

Master2 continues.

---

Interview Question:

**Why is I2C called non-destructive arbitration?**

Because loser stops transmitting without corrupting data.

---

# 16. Clock Stretching

Slave may be busy.

It holds SCL LOW.

```
Master wants next clock

Slave keeps SCL LOW
```

Master waits.

```
Slave releases SCL
```

Communication resumes.

Used by:

* Slow ADC
* EEPROM
* Sensors

---

# 17. I2C Speeds

| Mode       | Speed    |
| ---------- | -------- |
| Standard   | 100 Kbps |
| Fast       | 400 Kbps |
| Fast Plus  | 1 Mbps   |
| High Speed | 3.4 Mbps |
| Ultra Fast | 5 Mbps   |

---

# 18. Linux Kernel I2C Stack

Application:

```c
read()
write()
ioctl()
```

↓

I2C Core

↓

I2C Adapter Driver

↓

I2C Controller Hardware

↓

SCL/SDA

↓

Slave Device

Architecture:

```text
User Space

   |
   v

I2C Device Driver

   |
   v

I2C Core

   |
   v

I2C Adapter

   |
   v

Hardware Controller
```

---

# 19. Linux Driver Structures

### i2c_adapter

Represents controller.

```c
struct i2c_adapter
{
};
```

Example:

* DesignWare I2C
* Qualcomm I2C
* NVIDIA I2C

---

### i2c_client

Represents slave.

```c
struct i2c_client
{
};
```

Example:

```
EEPROM
RTC
Sensor
```

---

### i2c_driver

Represents device driver.

```c
struct i2c_driver
{
    .probe
    .remove
};
```

---

# 20. Internal Linux I2C Flow

When Device Tree is parsed:

```dts
i2c@12340000 {
    temp@48 {
        compatible = "ti,tmp102";
        reg = <0x48>;
    };
};
```

Kernel creates:

```c
i2c_client
```

Then:

```c
i2c_register_driver()
```

↓

```c
i2c_device_match()
```

↓

```c
probe()
```

called.

Flow:

```text
Device Tree

     |
     v

i2c_client

     |
     v

i2c_driver

     |
     v

Match

     |
     v

probe()
```

---

# Complete I2C Timing Diagram

```text
Idle

SCL  ─────────────────────────────

SDA  ─────────────────────────────


START

SCL  ─────────────────────────────

SDA  ───────┐
            └────────────────────


Address + Write

SCL  _|¯|_|¯|_|¯|_|¯|_|¯|_

SDA   1 0 1 0 0 0 0 0


ACK

SCL  _|¯|_

SDA  _____


Data

SCL  _|¯|_|¯|_|¯|_|¯|_

SDA   1 0 1 0 1 0 1 0


ACK

SCL  _|¯|_

SDA  _____


STOP

SCL  ─────────────────────────

SDA  _________┐
              └──────────────
```

# Top Interview Questions

1. Why are pull-up resistors required in I2C?
2. Why is I2C open-drain?
3. Difference between ACK and NACK?
4. What is clock stretching?
5. What is repeated START?
6. How does arbitration work?
7. Why does LOW dominate HIGH?
8. Difference between SPI and I2C?
9. How does Linux create `i2c_client`?
10. Explain complete flow from Device Tree to `probe()`.
11. What happens if two masters transmit simultaneously?
12. Why is I2C slower than SPI?
13. How is bus recovery performed when SDA is stuck LOW?
14. What happens internally in `i2c_transfer()`?
15. Explain `i2c_adapter`, `i2c_client`, and `i2c_driver` in detail.

These are commonly asked in Linux kernel driver interviews at companies such as AMD, NVIDIA, Qualcomm, and Intel.
