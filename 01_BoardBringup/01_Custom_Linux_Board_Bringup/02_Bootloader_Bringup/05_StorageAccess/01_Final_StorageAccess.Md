✅ 5. Storage Access
==============================================================================================================================
🔹 Goal:
        Ensure read/write to boot media is functional

Features:
---------------------------------------
01. eMMC/SD → use mmc dev, mmc write, fatload
02. NAND → Requires UBI or raw flash handling
03. SPI NOR → sf probe, sf read, etc.

💻 Example:
--------------------------------------------
            mmc dev 0
            fatls mmc 0:1
            load mmc 0:1 0x80000000 zImage