1. Pre-Bootloader Setup
======================================================================================================================================

🔧  Pre-Bootloader Setup
==============================================================================================
| Step                       | Description                                                   |
| ---------------------------|---------------------------------------------------------------|
| ✅ Schematic Review        | Verify SoC, power rails, clocks, PMIC, peripherals.           |
| ✅ Clock Tree Plan         | Ensure proper oscillator and PLL routing.                     |
| ✅ DDR Layout + Config     | Confirm memory topology, impedance matching, and termination. |
| ✅ JTAG/UART Access        | Confirm availability for low-level debug.                     |
| ✅ Power-On Reset Sequence | Match with SoC datasheet timing.                              |

📁 Objective
==========================
Prepare all hardware elements such that when the SoC is powered, the BootROM (read-only) can successfully:
    Detect the boot media (eMMC/NAND/UFS)
    Load the bootloader
    Initialize basic DDR and peripherals

Without completing these steps correctly, you won’t even reach U-Boot or ABL logs.

🧱 Step-by-Step Breakdown
=====================================================================================================================================

✅ 1. Schematic Review
=========================
🔍 What:
        Review SoC schematic design and reference design against the vendor's hardware design guidelines.

🎯 Purpose:
            To catch issues like:
            Incorrect power rail voltages
            Missing pull-ups/pull-downs
            Wrong level shifters on I/O lines
            Peripheral miswiring (e.g., SD/eMMC/MMC, USB, PMIC)

💡 Key Review Points:
            PMIC → SoC power domain mapping
            Boot source pins: e.g., BOOT_MODE[1:0]
            eMMC/SDIO signal routing
            Clock input routing to SoC (32kHz, 19.2MHz, 26MHz)
            UART/JTAG connectivity for debug access

🛠 Example:
            On SDM660, the eMMC supply must match the VDDCX domain. GPIOs controlling reset should have proper level shifters when  interfacing with external power chips.


✅ 2. Clock Tree Plan
====================================================================================================================================
🔍 What:
            Map out the oscillator inputs, clock multiplexers, and PLLs inside SoC and board.

🎯 Purpose:
            Ensure SoC receives valid clock signals for BootROM to execute, DDR to be initialized, and interfaces to function.

💡 Key Considerations:
            SoC requires ref clk (26 MHz) for main PLL lock.
            32.768 kHz crystal for RTC or PMIC wakeups.
            USB PHY needs separate ref clk in some designs.

🧠 Deep Debug Tip:
            Missing or unstable oscillator → BootROM hang → no serial logs → need to scope crystal with oscilloscope.


✅ 3. DDR Layout + Configuration
====================================================================================================================================
🔍 What:
        Review DDR physical layout and apply correct timing settings in DDR training or PHY initialization code.

🎯 Purpose:
            DDR must be stable during pre-loader stage (XBL/ABL/LK), else bootloader will crash or hang silently.

💡 Key Checklist:
            Follow length matching for DQ, DQS, CLK lines.
            Ensure termination resistors exist as per SoC DDR PHY spec.
            Confirm VTT supply for termination.
            Run simulation (IBIS) for DDR SI (Signal Integrity).

📐 Example:
            For LPDDR4 on SDM660:
            Verify impedance matching (typically 40–50 ohms).
            Check fly-by topology and termination resistors at DRAM end.

🧠 DDR Debug Tip:
            Use memtester, stressapptest, or DDR stress tools after bootloader runs to validate memory under load.

✅ 4. JTAG/UART Access
=====================================================================================================================================
🔍 What:
            Ensure you have low-level debug access in case SoC fails before bootloader stage.

🎯 Purpose:
            JTAG → For stepping through BootROM or XBL
            UART → For early bootloader logs, ATF, or crash info

💡 Connectivity Must Haves:
            UART0 or UART_DBG accessible on board headers
            JTAG with proper pull-ups, TRST/nTRST, and isolation
            Use of Lauterbach or PLS debugger to connect to DAP

🛠 Practical:
            Configure JTAG chain and test connectivity using BSDL or TRACE32 script:

            SYStem.CONFIG SDM660
            SYStem.UP

✅ 5. Power-On Reset (POR) Sequence
===================================================================================================================================
🔍 What:
            Validate that all power domains come up in correct order with correct timing and meet minimum reset specifications.

🎯 Purpose:
            If the power sequence is violated, the SoC can get stuck in reset or fail to enter BootROM stage.

💡 Sequence Includes:
            Power to Always-On domains (e.g., VDD_AON)
            PMIC power OK asserted
            Main VDD_CPU / VDD_DDR
            Clock stable
            RESET_N released after debounce
            Boot pins latched correctly

🧠 Debug Tip:
            Use oscilloscope or logic analyzer to probe PMIC rails, RESET_N, and clocks during cold boot.
            Delays between rails are often in microseconds, but extremely important

🧭 Final Checklist: "Am I Ready for Bootloader Stage?"
==========================================================
| ✅ Checkpoint                         | Status |
| ------------------------------------- | ------ |
| PMIC Power Sequence Verified          | ✅     |
| Clock Inputs Validated                | ✅     |
| DDR Topology Matched                  | ✅     |
| UART/JTAG Confirmed                   | ✅     |
| Boot Source Configured                | ✅     |
| Pull-ups/Pull-downs on critical lines | ✅     |


🔚 End Result of Pre-Bootloader Phase
==========================================================

If all above steps are successful:
01. SoC exits reset
02. BootROM runs from internal mask ROM
03. BootROM finds the boot media (eMMC/UFS/SD)
04. Loads first-stage loader (e.g., XBL for Qualcomm) into SRAM
05.  UART log begins → you enter Bootloader Phase

