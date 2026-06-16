✅ 3. Console Output (Early UART)
==============================================================================================================================
🔹 Goal:
        Get debug output ASAP during SPL and U-Boot boot stages.

Steps:
01.     Configure base address and clock for UART (CONFIG_DEBUG_UART_BASE, etc.)
02.     Check baudrate matches PC (typically 115200)
03.     U-Boot early printf is enabled via:
                CONFIG_DEBUG_UART=y
                CONFIG_DEBUG_UART_MSM=y   // For Qualcomm or SoC-specific

📌 Tip: Use early_printf() or puts() to debug SPL issues before RAM init.