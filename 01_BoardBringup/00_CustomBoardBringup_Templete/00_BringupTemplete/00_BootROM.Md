In embedded systems, the BootROM (also called ROM code or primary bootloader inside the SoC) doesn’t “decide dynamically” in a smart way like software—it follows **predefined rules set by the chip manufacturer**.

Here’s how it works in practice 👇

---

## 🔧 1. Where is this behavior defined?

The boot source selection is defined by **hardware + ROM firmware**, specifically:

### ✅ a) BootROM code (inside the SoC)

* This is **hardcoded by the silicon vendor** (e.g., TI, NXP, ST, Qualcomm).
* You **cannot modify it**.
* It contains a **boot sequence / priority list** like:

  ```
  1. eMMC
  2. SD card
  3. SPI NOR
  4. NAND
  5. UART (fallback)
  ```

---

### ✅ b) Boot configuration pins (BOOT pins / straps)

* On reset, the BootROM reads **GPIO pins or strap pins**.
* These pins tell it:

  * Which boot device to try
  * Or which **boot mode** to use

📌 Example:

* `BOOT_MODE[2:0] = 001` → boot from eMMC
* `BOOT_MODE[2:0] = 010` → boot from SD
* `BOOT_MODE[2:0] = 100` → boot from SPI NOR

👉 These pins are wired on your **custom board**, often via:

* Pull-up / pull-down resistors
* DIP switches (for development boards)

---

### ✅ c) eFuse / OTP memory (optional override)

* Some SoCs allow storing boot config in:

  * eFuse
  * OTP (One-Time Programmable memory)

👉 This can:

* Override boot pins
* Lock boot device permanently (for production/security)

---

## 🔄 2. Typical Boot Flow

1. Power ON / Reset
2. BootROM executes (from internal ROM)
3. BootROM reads:

   * Boot pins (or eFuse)
4. Based on that:

   * Selects boot device (SD/eMMC/SPI/NAND/UART)
5. Tries to load:

   * First-stage bootloader (FSBL, e.g., SPL, MLO)
6. If fails:

   * Moves to next device (fallback order)

---

## 🧠 3. For Custom Board Bring-up

You control boot source mainly via:

### ✔ Hardware design

* Set correct **BOOT pins**
* Ensure proper pull-up/pull-down values
* Route storage interfaces correctly

### ✔ Documentation (VERY important)

You must check:

* **SoC Technical Reference Manual (TRM)**
* **Booting Guide / Application Notes**

These documents specify:

* Boot pin mapping
* Boot priority
* Supported devices
* Image format required

---

## ⚠️ Key Insight

👉 The decision is **NOT in your code**
👉 It is **NOT in Linux / U-Boot initially**
👉 It is **fixed in BootROM + configured by hardware pins/eFuse**

---

## 🧩 Example (Real-world style)

For a typical SoC:

* Boot pins say: `SD boot`
* BootROM:

  * Initializes SD controller
  * Looks for a specific sector (e.g., sector 0x200)
  * Loads SPL into SRAM
* SPL then:

  * Initializes DRAM
  * Loads full bootloader (e.g., U-Boot)

---

For **Qualcomm Snapdragon 660 (SDM660)**, the boot flow is quite different from TI/NXP-style SoCs you might be used to. There are **no simple BOOT pins selecting SD/eMMC/SPI** like typical MCU/MPU designs.

Instead, Qualcomm uses a **fixed boot architecture driven by BootROM + fuses + storage type (mostly eMMC/UFS)**.

Let’s break it down specifically for **board bring-up perspective** 👇

---

# 🔥 1. SDM660 BootROM (PBL) – What it actually does

After reset:

### Step 1: BootROM execution (PBL)

* CPU jumps to **internal ROM (Primary Bootloader – PBL)**
* Reads configuration from:

  * **QFPROM (eFuse)**
  * Security state (Secure Boot, keys, debug enable) ([squadri][1])

👉 Important:

* These fuses **do NOT select SD vs SPI like typical SoCs**
* They mainly control:

  * Secure boot
  * Root of trust
  * Debug permissions

---

### Step 2: Boot device selection (VERY IMPORTANT)

For SDM660:

👉 BootROM is **hardwired to boot from:**

* **eMMC (primary)**
* or **UFS (in some designs)**

📌 There is **NO general-purpose SPI-NOR / NAND / SD boot option exposed** like i.MX or AM335x.

✔ That’s why in fastboot you often see:

```
variant: SDM EMMC
```

(from real devices) — meaning BootROM expects eMMC layout.

---

### Step 3: Load next stage (XBL / SBL)

BootROM:

* Initializes minimal eMMC/UFS controller
* Loads:

  * **XBL (eXtensible Bootloader)** or older **SBL1**
* From **fixed sectors/partitions in eMMC**

Then:

* Verifies signature (if secure boot enabled) ([squadri][1])

---

# 🔄 2. Full SDM660 Boot Chain

Typical sequence:

```
1. BootROM (PBL)  ← internal ROM
2. XBL / SBL1     ← from eMMC (boot partition)
3. XBL Loader / ABL (UEFI)
4. TrustZone (QSEE)
5. ABL loads boot.img
6. Linux Kernel
7. Android userspace
```

📌 From source:

* BootROM → loads bootloader from eMMC/UFS and authenticates it ([squadri][1])

---

# ⚙️ 3. Where is “boot source” decided in SDM660?

Here’s the key difference vs embedded MPU:

## ❌ NOT via boot pins (like TI/NXP)

* No BOOT[0:3] style pin mux for media selection

## ✅ Instead:

### 1. Hardware design decision

* You physically connect:

  * eMMC → primary boot device
  * (or UFS)

👉 That’s your “boot selection”

---

### 2. BootROM internal logic (fixed)

* Always tries:

  ```
  Primary: eMMC/UFS
  Fallback: EDL (Emergency Download mode via USB)
  ```

---

### 3. eFuse (QFPROM)

* Controls:

  * Secure boot enable
  * Authentication keys
* NOT typical media mux

---

# 🔌 4. What about UART / USB boot?

Yes — but different concept:

### EDL mode (Emergency Download)

* If boot fails or forced:

  * BootROM enters **EDL (Qualcomm HS-USB 9008 mode)**

👉 This is your “UART/USB fallback”

Used for:

* Flashing via tools (QPST, QFIL)

---

# 🧠 5. Pin Configuration (Important for bring-up)

Unlike MCU-style BOOT pins:

### What matters in SDM660:

#### ✅ eMMC interface pins (SDC1)

From DTS:

* `sdc1_clk`
* `sdc1_cmd`
* `sdc1_data[0..7]` ([Android Git Repositories][2])

👉 These must be:

* Correctly routed
* Proper pull-ups
* Correct voltage (1.8V / 3.0V)

---

#### ✅ Power rails

* eMMC VCC / VCCQ must be valid
* PMIC sequencing must be correct

---

#### ✅ Reset & PON logic

* PMIC triggers boot
* No “boot strap pins” like other SoCs

---

# ⚠️ 6. Critical Bring-up Insights (SDM660)

This is where most people get confused:

### 🚫 You CANNOT:

* Boot from SD card directly
* Boot from SPI NOR directly
* Select boot device using GPIO straps

---

### ✅ You MUST:

1. Populate **eMMC (or UFS)**
2. Flash proper partitions:

   * `xbl`
   * `abl`
   * `tz`
   * `rpm`
3. Ensure:

   * Correct partition layout (GPT)
   * Signed images (if secure boot)

---

### 🔧 If eMMC fails:

* Device goes to:

  * **EDL mode (USB recovery)**

---

# 🧩 7. Mental Model (Very Important)

For SDM660:

👉 Think like this:

| Traditional SoC              | SDM660     |
| ---------------------------- | ---------- |
| Boot pins select media       | ❌ No       |
| BootROM scans multiple media | ❌ No       |
| Flexible boot (SD/SPI/NAND)  | ❌ No       |
| Fixed boot device            | ✅ eMMC/UFS |
| Recovery mode                | ✅ USB EDL  |

---

# 🚀 Final Summary

👉 For **SDM660 board bring-up**:

* BootROM is **fixed and not configurable**
* Boot source is:
  ✔ **Hardwired to eMMC/UFS**
* No boot pins for media selection
* Only fallback:
  ✔ **EDL (USB download mode)**

---

Great question—this part is **crucial for production devices and security**, and also one of the easiest places to make irreversible mistakes.

Let’s go deep but keep it practical for **board bring-up + productization** 👇

---

# 🔐 What are eFuse / OTP in SoCs?

### ✅ eFuse (Electronic Fuse)

* Physical **one-time programmable bits inside the chip**
* Blown using a high voltage pulse
* Once programmed:

  * ❌ Cannot be erased
  * ❌ Cannot be changed
  * ✅ Only read afterward

---

### ✅ OTP (One-Time Programmable memory)

* Similar idea, different implementation:

  * Anti-fuse, flash-based OTP, etc.
* Same property:

  * **Write once → permanent**

---

# 🧠 Why do SoCs use eFuse / OTP?

They act as **non-volatile hardware configuration storage** for things like:

### 🔹 Boot configuration

* Boot device selection (eMMC / NAND / SPI / SD)
* Boot priority order

### 🔹 Security

* Secure boot enable
* Public key hashes
* JTAG disable
* Debug lock

### 🔹 Device identity

* MAC address
* Serial number
* Encryption keys

---

# 🔄 Boot Pins vs eFuse — Who Wins?

This is the key concept:

### 🥊 Priority logic inside BootROM:

```
if (eFuse boot config is programmed):
    use eFuse settings
else:
    read BOOT pins
```

👉 So:

* eFuse can **override hardware pins**
* Useful when you want to **lock behavior permanently**

---

# 🔒 Why Override Boot Pins?

## 1. Prevent unauthorized boot sources

During development:

* You may boot from SD card or UART

In production:

* You **must NOT allow**:

  * Booting from external SD
  * Loading unsigned code via UART

👉 So you:

* Burn eFuse to force:

  ```
  Boot only from eMMC
  ```

---

## 2. Enable Secure Boot

Typical flow:

1. Burn:

   * Public key hash into eFuse
2. Enable:

   * Secure boot bit in eFuse

Now BootROM:

* Loads bootloader
* Verifies signature using fused key
* If invalid → ❌ boot fails

👉 This prevents:

* Tampered firmware
* Malware injection

---

## 3. Disable Debug Access (JTAG/UART)

* During development → enabled
* In production → disabled via eFuse

👉 Prevents:

* Reverse engineering
* Memory dumping
* Key extraction

---

# ⚙️ What Exactly Gets Stored?

Typical eFuse fields (varies by SoC):

### 🔹 Boot Configuration

* Boot device (eMMC / NAND / SPI)
* Boot mode (raw / filesystem)
* Bus width, speed

### 🔹 Security Bits

* Secure boot enable
* Root of trust key hash
* Anti-rollback version

### 🔹 Access Control

* JTAG disable
* UART download disable

---

# 🧪 Example Scenario (Realistic)

### During Development:

* Boot pins = SD card
* You flash bootloader on SD
* Debug via UART

---

### During Production:

You program eFuse:

```
BOOT_DEVICE = eMMC
SECURE_BOOT = ENABLED
JTAG = DISABLED
UART_BOOT = DISABLED
```

Now:

* BootROM ignores SD pins ❌
* Only boots from eMMC ✅
* Only signed images allowed ✅
* No debug access ✅

---

# ⚠️ Risks & Gotchas

## ❌ 1. Irreversible Programming

* Once eFuse is burned → **no undo**
* Wrong config = **bricked device**

---

## ❌ 2. Locking Yourself Out

Example:

* Disable UART boot
* eMMC image is broken

👉 Result:

* No recovery path 😬

---

## ❌ 3. Secure Boot Misconfiguration

* Wrong key hash → nothing boots
* Device permanently unusable

---

# 🛠️ Best Practices (VERY important)

### ✅ 1. Validate everything before burning eFuse

* Boot flow works reliably
* Storage device stable
* Bootloader signed and verified

---

### ✅ 2. Keep a recovery path (initially)

* Don’t disable UART/USB boot too early
* Use development boards for testing

---

### ✅ 3. Use staged programming

1. Test without eFuse
2. Program non-critical fuses
3. Finally lock security

---

### ✅ 4. Follow SoC documentation strictly

Each vendor defines:

* Fuse map
* Programming sequence
* Voltage/timing requirements

---

# 🧩 Mental Model

Think of it like this:

* **BOOT pins** = temporary configuration (like BIOS settings)
* **eFuse/OTP** = permanently burned firmware policy (like hardware lockdown)

---

# 🚀 Final Summary

👉 eFuse/OTP allows you to:

* Override boot pins
* Permanently select boot device
* Enforce secure boot
* Disable debug access

👉 BootROM always trusts eFuse first (if programmed)

👉 It is powerful—but dangerous if misused

---

If you want, I can walk you through a **real SoC example (like i.MX, TI AM62x, STM32MP1)** and show:

* Exact fuse registers
* Boot override behavior
* Safe bring-up → production transition flow
