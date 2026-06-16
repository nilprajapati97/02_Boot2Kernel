# vgacon_register_screen — ARM32 vs ARM64 Design Details

## 1. ARM32: VGA Console Possible But Rare

ARM32 has the VGA console `#ifdef` guard in `setup_arch()`:

```c
/* arch/arm/kernel/setup.c */
#if defined(CONFIG_VT) && defined(CONFIG_VGA_CONSOLE)
    conswitchp = &vga_con;
    vgacon_register_screen(&vgacon_screen_info);
#endif
```

ARM32 systems where VGA is possible:
- QEMU `virt` machine with `-device VGA` option
- ARM-based industrial PCs with PCI slot + VGA card
- Some Armada (Marvell) boards with PCIe + GPU

Most production ARM32 SoC boards (BCM2836, i.MX6, Allwinner) never build with `CONFIG_VGA_CONSOLE`.

---

## 2. ARM64: EFI Framebuffer Is the Standard

ARM64 `setup_arch()` does NOT have `vgacon_register_screen()`. ARM64 systems standardize on:

**EFI GOP (Graphics Output Protocol) framebuffer:**

```c
/* arch/arm64/kernel/setup.c — no vgacon call */
/* EFI GOP framebuffer is handled in efi_init() / efi_fb_setup() */

/* drivers/firmware/efi/efi-init.c */
void __init efi_init(void)
{
    ...
    /* screen_info is filled by EFI stub */
    screen_info.lfb_base = ...;
    screen_info.lfb_width = ...;
    screen_info.lfb_height = ...;
    screen_info.lfb_depth = ...;
}
```

After EFI init, `CONFIG_EFI_FB_CONSOLE` or `CONFIG_SIMPLEDRM` provides a framebuffer console from the UEFI-established linear framebuffer.

---

## 3. screen_info Source: ARM32 vs ARM64

### ARM32: screen_info from DT or Firmware

```c
/* arch/arm/kernel/atags_compat.c */
/* Old ATAG_VIDEOLFB populates screen_info for framebuffer boards */
tag_videolfb(tag)
    screen_info.lfb_base = tag->lfb_base;
    screen_info.lfb_width = tag->lfb_width;
    ...
```

For VGA (x86 legacy style), `screen_info.orig_video_mode = 3` (80x25 text).

### ARM64: screen_info from UEFI Stub

```c
/* drivers/firmware/efi/libstub/arm64-stub.c */
/* EFI stub runs before kernel proper */
/* Sets up screen_info from EFI GOP protocol */
screen_info = (struct screen_info){
    .orig_video_isVGA = VIDEO_TYPE_EFI,
    .lfb_width  = mode->info->horizontal_resolution,
    .lfb_height = mode->info->vertical_resolution,
    .lfb_depth  = 32,
    .lfb_base   = (unsigned long)fb_addr,
    .lfb_size   = fb_size,
    ...
};
```

ARM64 EFI framebuffer is pixel-based (e.g., 1920×1080×32bpp), not character-based like VGA.

---

## 4. Console Stack on ARM32 vs ARM64

### ARM32 Typical Console Stack

```
Physical UART hardware
    ↑
8250/PL011 serial driver (CONFIG_SERIAL_8250, CONFIG_SERIAL_AMBA_PL011)
    ↑
Serial console (ttyS0, ttyAMA0)
    ↑
/dev/console → user space init/getty
```

OR with framebuffer:

```
LCD controller / HDMI (configured by DRM/KMS driver)
    ↑
Framebuffer (/dev/fb0)
    ↑
fbcon (CONFIG_FRAMEBUFFER_CONSOLE)
    ↑
Virtual terminal (/dev/tty1)
    ↑
User space
```

### ARM64 Standard Console Stack (UEFI system)

```
UEFI GOP framebuffer (linear, pixel-based)
    ↑
simpledrm / efifb (early, before GPU driver)
    ↑
fbcon / drm_fb_helper
    ↑
Virtual terminal (/dev/tty1)
    ↑
User space getty/login
```

After proper GPU driver loads (e.g., `virtio-gpu`, `vc4`, `amdgpu`):
```
GPU driver takes over framebuffer → DRM/KMS
```

---

## 5. Comparison Table

| Feature | ARM32 | ARM64 |
|---------|-------|-------|
| VGA console in setup_arch() | Yes (#ifdef guarded) | No |
| Standard console type | Serial (UART) | EFI GOP framebuffer |
| screen_info source | ATAGs or DT | UEFI stub / EFI GOP |
| VGA text mode (0xB8000) | Rare (PCI VGA only) | Not used |
| Framebuffer console | Via fbcon + FB driver | Via simpledrm/efifb + fbcon |
| conswitchp default | dummy_con | dummy_con (EFI console set up later) |
| HDMI console | Board-specific DRM | Standard via DRM/KMS |
| CONFIG_VGA_CONSOLE | Rarely enabled | Almost never enabled |
