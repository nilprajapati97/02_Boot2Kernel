✅ 7. USB Mass Storage (UMS) Mode
==============================================================================================================================
🔹 Goal: Let U-Boot expose eMMC/SD as a USB drive to PC

Config: c
------------------------------
CONFIG_CMD_USB_MASS_STORAGE
CONFIG_USB_GADGET
CONFIG_USB_GADGET_DUALSPEED
CONFIG_USB_GADGET_DOWNLOAD

Use: sh
----------------------------
ums 0 mmc 0


🔌 Connect USB to PC → Board appears as mass storage → Flash zImage/DTB directly