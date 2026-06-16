
### New file layout
```
01_Implementation/
  basic_chardriver.c   (existing)
  Makefile             ← NEW — ARM64 cross-compile

qemu_arm64/
  setup_env.sh         ← NEW — one-time setup (run once)
  run_qemu.sh          ← NEW — build .ko + initramfs + launch QEMU
  guest_init.sh        ← NEW — /init inside QEMU (mounts, tests, shell)
```

---

### How to use (from WSL2 Ubuntu)

**Step 1 — One-time setup** (~15–30 min, builds Linux 6.6 kernel + BusyBox):
```bash
cd /mnt/c/My_Workspace/My_Repo/LKI_Arrangement/CharDriver/01_Basic_CharDriver_Registration_VFS
chmod +x qemu_arm64/setup_env.sh
./qemu_arm64/setup_env.sh
```

**Step 2 — Build module + run QEMU** (every time):
```bash
chmod +x qemu_arm64/run_qemu.sh qemu_arm64/guest_init.sh
./qemu_arm64/run_qemu.sh
```

---

### What you'll see inside QEMU

The `guest_init.sh` runs as PID 1 and automatically:

| Step | Action |
|------|--------|
| `[1]` | `insmod basic_chardriver.ko` |
| `[2]` | Dumps `/proc/devices` to confirm major number allocation |
| `[3]` | Verifies `/dev/MyAnilDev` was auto-created by `devtmpfs` |
| `[4]` | `echo "HelloFromARMGuest" > /dev/MyAnilDev` |
| `[5]` | `cat /dev/MyAnilDev` — verifies read/write round-trip |
| `[6]` | `dmesg` filtered to `basic_chrdev` — shows your `pr_info()` lines |
| `[7]` | `rmmod basic_chardriver` |
| `[8]` | Final `dmesg` tail |

After the test, it drops to an interactive BusyBox shell. Exit QEMU with **Ctrl-A then X**.

> **Note:** Scripts have LF line endings on Windows. Before running in WSL2, if you get `bad interpreter` errors, run: `sed -i 's/\r//' qemu_arm64/*.sh`

