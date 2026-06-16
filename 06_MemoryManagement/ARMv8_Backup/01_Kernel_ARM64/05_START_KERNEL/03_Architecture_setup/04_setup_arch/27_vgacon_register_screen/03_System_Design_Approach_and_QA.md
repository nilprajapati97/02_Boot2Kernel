# vgacon_register_screen — System Design Approach and Q&A

## 1. The Console Abstraction Layer

Linux separates console hardware from the virtual terminal subsystem via two abstraction layers:

```
User space: read/write /dev/tty1, /dev/console
                ↑
VT subsystem (drivers/tty/vt/vt.c)
  Manages character cells, cursor, color attributes
  Uses consw (console switch) operations
                ↑
consw implementations:
  vga_con   → writes to VGA text buffer (0xB8000)
  fb_con    → writes to framebuffer pixels (via bitmapped font)
  dummy_con → discards all output (headless)
  prom_con  → OpenFirmware PROM console (SPARC)
```

The `conswitchp` pointer selects which `consw` is used at VT init time. This is the **compile-time default** — drivers can change it at runtime via `do_take_over_console()`.

---

## 2. Why vgacon_register_screen() Is Called Early (setup_arch)

The VGA console initialization chain:

```
setup_arch() → vgacon_register_screen(&vgacon_screen_info)
  └── stores screen_info: columns=80, rows=25, video_mode=3
        │
start_kernel() → console_init()
  └── tty_register_driver(&vt_driver) → vt_console_init()
        └── do_take_over_console(&vga_con, 0, MAX_NR_CONSOLES-1, 1)
              └── vgacon_init() → uses stored vgacon_screen_info
                    └── maps 0xB8000 → ioremapped VGA text buffer
                    └── sets up 80×25 text console
```

`vgacon_register_screen()` must run before `console_init()` to provide the initial screen dimensions. If not called first, `vgacon_init()` would use zero columns/rows and fail to set up the text console.

---

## 3. Dependency Graph

```
[Early boot: BIOS/UEFI sets VGA mode 3 (80x25 text)]
  └── Physical VGA registers: text mode active, 0xB8000 = text buffer
        │
[Bootloader: screen_info populated]
  └── vgacon_screen_info.orig_video_cols = 80
  └── vgacon_screen_info.orig_video_lines = 25
  └── vgacon_screen_info.orig_video_mode = 3
        │
[setup_arch()]
  └── vgacon_register_screen(&vgacon_screen_info)
        └── stores pointer, sets up vgacon_xres/yres
        │
[start_kernel() → console_init()]
  └── vt_console_init() → do_take_over_console(&vga_con)
        └── vgacon_init()
              → ioremaps 0xB8000 (VGA text buffer)
              → console output goes to VGA screen
              → /dev/tty1 is the VGA console
```

---

## 4. System Design Q&A

**Q: What is the difference between early console (earlycon) and the VGA/VT console?**
> **Early console (earlycon)**: Registered before `setup_arch()`, uses `printk()` with a simple UART or console write callback. No VT subsystem, no character cell abstraction, no `/dev/tty`. Just writes bytes to a register. Set up via `early_param("earlycon", ...)`. **VGA/VT console**: Full virtual terminal with cursor positioning, ANSI escape codes, color, multiple TTY switching, `/dev/tty1`-`/dev/tty63`. Registered in `console_init()` (called from `start_kernel()` after `setup_arch()`). VGA console requires the full VT subsystem (`CONFIG_VT`). Most embedded systems use earlycon during boot (for very early messages) and then switch to a serial or framebuffer VT console after VT init.

**Q: Why does ARM use framebuffer console instead of VGA console?**
> VGA text mode (0xB8000 text buffer) is a PC-specific hardware feature that dates to the IBM PC. It requires: (1) ISA bus or PCI VGA-compatible hardware with the 0xB8000 memory-mapped text buffer, (2) BIOS INT 10h or similar to set text mode, (3) Character ROM in VGA hardware for rendering font. ARM SoCs have their own display controllers (DSI/HDMI/LVDS) with pixel-based framebuffers — no 0xB8000 text buffer. ARM systems that use UEFI (most ARM64) get a linear pixel framebuffer from EFI GOP — simpledrm or efifb provides framebuffer console on top of this. The font is rendered in software by `fbcon` using a built-in bitmap font. This is more flexible but requires the ARM display controller to be initialized first.

**Q: How does the console transfer from earlycon (during setup_arch) to VT console (after start_kernel)?**
> Linux has a `console_drivers` list. `register_console(&earlycon)` adds the earlycon to the head. `printk()` iterates all registered consoles and calls each one's `write()` callback. When `console_init()` runs (after `setup_arch()`), `register_console(&vga_console)` or `register_console(&uart_console)` is called. For the regular console driver, `CON_BOOT` flag is not set — this triggers `unregister_console()` for any `CON_BOOT`-flagged consoles (like earlycon). The transition is seamless: one `printk()` call in earlycon, next `printk()` call goes to VT console. The transition is visible in dmesg as "console [tty0] enabled, bootconsole [pl011] disabled."
