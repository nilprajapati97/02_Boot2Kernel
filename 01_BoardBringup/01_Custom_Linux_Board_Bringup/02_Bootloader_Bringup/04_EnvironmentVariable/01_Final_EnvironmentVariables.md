✅ 4. Environment Variables
==============================================================================================================================
🔹 Goal:    
        Set up your boot flow, file paths, and kernel command-line.

Key Variables:
-----------------------
01.     setenv bootargs 'console=ttyMSM0,115200 root=/dev/mmcblk0p2 rw rootwait'
02.     setenv bootcmd 'mmc dev 0; load mmc 0:1 0x80000000 zImage; load mmc 0:1 0x86000000 dtb; bootz 0x80000000 - 0x86000000'
03.     saveenv


Optional Settings:
-------------------
01.     ip=dhcp → Enable network boot
02.     bootdelay=3 → Countdown before auto boot
03.     fdt_high → Set if dtb overlaps with kernel addr
04.     initrd_high → Optional for initrd/uInitrd

💡 Tip: Store in eMMC, NAND, or SPI Flash as per your design.
