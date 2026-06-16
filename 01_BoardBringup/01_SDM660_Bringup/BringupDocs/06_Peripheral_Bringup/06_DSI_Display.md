# DSI Display

## Overview

SDM660 drives displays via the **DSI (Display Serial Interface)** protocol using the **MDSS (Mobile Display Sub-System)**. The display pipeline includes MDP (Mobile Display Processor) for rendering, DSI controller for protocol handling, and DSI PHY for physical signaling.

---

## Display Pipeline

```
┌──────────────────────────────────────────────────────────────┐
│                    SDM660 Display Pipeline                     │
│                                                              │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐ │
│  │ GPU /    │   │  MDP5    │   │  DSI     │   │  DSI    │ │
│  │ CPU      │──▶│ (Display │──▶│ Ctrl    │──▶│  PHY   │──▶ Panel
│  │ Renders  │   │  Proc)   │   │         │   │        │ │
│  │ to FB    │   │          │   │ Protocol│   │ MIPI   │ │
│  └──────────┘   └──────────┘   └──────────┘   └──────────┘ │
│                                                              │
│  MDP5 @ 0x0C90_0000 (Multimedia Display Subsystem)          │
│  DSI0 @ 0x0C99_4000                                         │
│  DSI1 @ 0x0C99_6000 (for dual-DSI panels)                   │
│  DSI PHY @ 0x0C99_4400                                      │
└──────────────────────────────────────────────────────────────┘
```

---

## Display Bring-Up Sequence

```
1. ABL/LK: Initialize display for splash screen
   ├── Configure DSI PHY PLL (pixel clock)
   ├── Initialize panel (send DSI init commands)
   └── Display OEM logo via simple framebuffer

2. Kernel: DRM/KMS driver takes over
   ├── MDSS driver probes (qcom,mdss_mdp)
   ├── DSI controller driver probes
   ├── DSI PHY driver probes
   ├── Panel driver probes (compatible = "vendor,panel-xxx")
   └── DRM framebuffer replaces LK framebuffer

3. Android: SurfaceFlinger composites UI
   └── Uses HWC (Hardware Composer) HAL → MDP5
```

---

## Device Tree

```dts
mdss_mdp: qcom,mdss_mdp@c900000 {
    compatible = "qcom,mdss_mdp";
    reg = <0x0c900000 0x90000>;
    clocks = <&mmcc MDSS_AHB_CLK>,
             <&mmcc MDSS_AXI_CLK>,
             <&mmcc MDSS_MDP_CLK>;
    clock-names = "iface_clk", "bus_clk", "core_clk";
};

mdss_dsi0: qcom,mdss_dsi@c994000 {
    compatible = "qcom,mdss-dsi-ctrl";
    reg = <0x0c994000 0x400>;
    clocks = <&mmcc MDSS_ESC0_CLK>,
             <&mmcc MDSS_BYTE0_CLK>,
             <&mmcc MDSS_PCLK0_CLK>;
    vdda-supply = <&pm660l_l1>;   /* DSI PHY analog supply */
    vddio-supply = <&pm660_l12>;  /* DSI PHY I/O supply */
};
```

---

## Panel Initialization

DSI panels require an **initialization command sequence** sent as DSI packets:

```
Typical panel init sequence:
────────────────────────────
1. Power on panel supplies (VDD, VDDIO)
2. Assert reset GPIO → delay → deassert
3. Send DSI commands:
   ├── Exit sleep mode (0x11)
   ├── Set display resolution
   ├── Configure gamma correction
   ├── Set pixel format (0x3A, RGB888)
   └── Display on (0x29)
4. Start video/command mode stream
```

---

## Debugging Display

```bash
# DRM info
adb shell cat /sys/class/drm/card0/device/status

# Panel info
adb shell cat /sys/class/graphics/fb0/msm_fb_panel_info

# Display resolution
adb shell wm size

# MDSS debug
adb shell cat /sys/kernel/debug/mdp/stat
```

---

## Related Documents

- [../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md](../05_Linux_Kernel_Boot/03_GCC_Clock_Framework.md) — MMCC display clocks
- [../05_Linux_Kernel_Boot/06_Regulator_Framework.md](../05_Linux_Kernel_Boot/06_Regulator_Framework.md) — Panel power supplies
