# vgacon_register_screen — Detailed Design Bottom-To-Top Flow

## 1. The Code

```c
/* arch/arm/kernel/setup.c */
#if defined(CONFIG_VT) && defined(CONFIG_VGA_CONSOLE)
    conswitchp = &vga_con;
    vgacon_register_screen(&vgacon_screen_info);
#endif
```

This registers VGA console support. On most ARM systems, this code never runs because `CONFIG_VGA_CONSOLE` is not set — ARM SoCs use framebuffer or serial consoles, not VGA.

---

## 2. When Is VGA Console Available on ARM?

VGA console (`CONFIG_VGA_CONSOLE`) requires:
1. A VGA-compatible graphics controller (ISA VGA or PCI VGA)
2. The VGA BIOS text mode (or emulation)
3. The classic 0xB8000 text buffer in physical address space

ARM systems that might have VGA:
- ARM-based desktops with discrete GPU (very rare)
- ARM x86 hybrid boards with legacy VGA pass-through
- Virtual machines (QEMU with `-vga std` and `arm-virt` machine)
- ARM servers with BMC (Baseboard Management Controller) providing virtual VGA

For embedded ARM boards (Raspberry Pi, i.MX6, etc.), this code is always `#ifdef`'d out.

---

## 3. struct screen_info and vgacon_screen_info

```c
/* include/uapi/linux/screen_info.h */
struct screen_info {
    __u8  orig_x;           /* cursor X position at boot */
    __u8  orig_y;           /* cursor Y position */
    __u16 ext_mem_k;        /* extended memory (KB) */
    __u16 orig_video_page;  /* video page */
    __u8  orig_video_mode;  /* video mode (0x03 = 80x25 text) */
    __u8  orig_video_cols;  /* columns (80) */
    __u8  flags;
    __u8  unused2;
    __u16 orig_video_ega_bx;
    __u8  orig_video_lines; /* rows (25) */
    __u8  orig_video_isVGA; /* 1 if VGA */
    __u16 orig_video_points;/* character height in scanlines */
    /* EFI framebuffer info: */
    __u16 lfb_width;        /* linear framebuffer width */
    __u16 lfb_height;       /* linear framebuffer height */
    __u16 lfb_depth;        /* bits per pixel */
    __u32 lfb_base;         /* framebuffer physical address */
    __u32 lfb_size;         /* framebuffer size */
    __u16 cl_magic, cl_offset;
    __u16 lfb_linelength;
    __u8  red_size, red_pos;
    __u8  green_size, green_pos;
    __u8  blue_size, blue_pos;
    __u8  rsvd_size, rsvd_pos;
    __u16 vesapm_seg, vesapm_off;
    __u16 pages, vesa_attributes;
    __u32 capabilities;
    __u32 ext_lfb_base;
    __u8  _reserved[2];
} __attribute__((packed));
```

`vgacon_screen_info` is a global `struct screen_info` populated by the bootloader or UEFI firmware with the console's initial state.

---

## 4. conswitchp: The Console Switch Pointer

```c
/* drivers/tty/vt/vt.c */
const struct consw *conswitchp;
```

This is the initial console type. At `vt_console_init()` time, the virtual terminal subsystem uses `conswitchp` to initialize the first console:

```c
/* drivers/tty/vt/vt.c */
void __init vt_console_init(void)
{
    if (conswitchp)
        do_take_over_console(conswitchp, 0, MAX_NR_CONSOLES - 1, 1);
}
```

Options for `conswitchp`:
- `&vga_con` — classic VGA text mode (80x25)
- `&dummy_con` — no console (headless)
- `&fb_con` — framebuffer console (ARM SoCs)

---

## 5. vgacon_register_screen()

```c
/* drivers/video/console/vgacon.c */
void vgacon_register_screen(struct screen_info *si)
{
    if (!si)
        return;

    /* Store screen_info for later use */
    vgacon_screen_info = si;

    /* Check if this is a real VGA mode (not EFI framebuffer) */
    if (!vgacon_yres || !vgacon_xres) {
        vgacon_yres = si->orig_video_lines;
        vgacon_xres = si->orig_video_cols;
    }
}
```

The registered `screen_info` provides the initial text dimensions and cursor position when the VGA console initializes.

---

## 6. For ARM in Practice: conswitchp = &dummy_con

On most ARM boards without VGA:

```c
/* drivers/tty/vt/vt.c */
#ifdef CONFIG_VT
const struct consw *conswitchp = &dummy_con;   /* default */
#endif
```

ARM's `setup_arch()` either:
1. (If `CONFIG_VGA_CONSOLE`) sets `conswitchp = &vga_con`
2. (Otherwise) leaves it as `&dummy_con`
3. Later: framebuffer driver registers via `do_take_over_console(&fb_con, ...)`

---

## 7. Interview Q&A

**Q1: What is the difference between a serial console and a VGA console?**
> A **serial console** (`CONFIG_SERIAL_CONSOLE`) uses a UART (Universal Asynchronous Receiver-Transmitter) — the kernel sends characters as 8N1 serial data at a fixed baud rate (115200 typical). Requires a serial cable or USB-UART adapter. Works from very early boot (before any graphics), widely used in embedded/server ARM systems. A **VGA console** (`CONFIG_VGA_CONSOLE`) uses the VGA hardware text mode: a memory-mapped text buffer at physical address 0xB8000 where each character cell is 2 bytes (ASCII + color attribute). Requires a VGA-compatible graphics controller and a monitor. On ARM, serial is standard; VGA console is the exception. ARM64 UEFI systems may also use `CONFIG_EFI_FB_CONSOLE` — EFI GOP framebuffer.

**Q2: What is CONFIG_VT (virtual terminals) and what does it provide?**
> `CONFIG_VT` enables the Linux **virtual terminal** subsystem — the infrastructure for `/dev/tty1` through `/dev/tty63`, `Ctrl+Alt+F1` switching between ttys, and the `agetty`/login prompt on the console. VT provides a common abstraction over different physical consoles (VGA, framebuffer, serial). Without VT, Linux can still have a serial console (via `CONFIG_SERIAL_CONSOLE` early console), but there's no tty-switching, no `/dev/ttyN`, no `ngetty`. Most embedded ARM boards that only have serial output disable `CONFIG_VT` to save kernel size. Server ARM64 boards with BMC may enable it for remote console access.

**Q3: On ARM embedded boards, what replaces VGA console?**
> ARM embedded boards typically use one of: (1) **earlycon / earlyprintk**: UART-based very-early console before subsystem init (just writes bytes to UART register). (2) **Serial console**: Full tty-based serial console via `CONFIG_SERIAL_8250` or SoC-specific UART driver, specified as `console=ttyS0,115200n8` in cmdline. (3) **Framebuffer console**: `CONFIG_FB_SIMPLE` (simple-framebuffer DT node) or `CONFIG_DRM_PANEL` for LCD panels — uses `fb_con`. (4) **No console**: Headless embedded systems may boot without any console, logging only to kernel ring buffer (accessed via `dmesg` if a network connection is available later).
