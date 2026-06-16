✅ 6. TFTP/NFS Boot Support
-------------------------------
🔹 Goal: Load kernel/DTB over network without flashing

U-Boot Config:
-----------------
CONFIG_CMD_NET
CONFIG_CMD_DHCP
CONFIG_CMD_TFTPBOOT
CONFIG_CMD_NFS
CONFIG_DM_ETH

Typical Flow:  sh
--------------------------------------
setenv serverip 192.168.1.100
setenv ipaddr 192.168.1.50
tftpboot 0x80000000 zImage
tftpboot 0x86000000 dtb
bootz 0x80000000 - 0x86000000

🧪 Useful for kernel testing or early bring-up without re-flashing every time.
