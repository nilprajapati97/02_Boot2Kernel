# GCC Clock Framework — Kernel Driver

## Overview

The kernel's **Common Clock Framework (CCF)** manages all clocks in the system. On SDM660, the primary clock driver is the **GCC (Global Clock Controller)** driver, which registers all SoC clock branches, PLLs, and muxes discovered from the device tree.

---

## Clock Framework Architecture

```
┌────────────────────────────────────────────────────────┐
│           Linux Common Clock Framework (CCF)            │
│                                                        │
│  ┌──────────────────────────────────────────────────┐  │
│  │  Consumer API (used by peripheral drivers)        │  │
│  │                                                  │  │
│  │  clk_get()        → Get clock reference          │  │
│  │  clk_prepare()    → Enable parent clocks         │  │
│  │  clk_enable()     → Gate on                      │  │
│  │  clk_set_rate()   → Change frequency             │  │
│  │  clk_disable()    → Gate off                     │  │
│  │  clk_put()        → Release reference            │  │
│  └──────────────────────────┬───────────────────────┘  │
│                             │                          │
│  ┌──────────────────────────▼───────────────────────┐  │
│  │  Clock Core (kernel/drivers/clk/clk.c)           │  │
│  │                                                  │  │
│  │  struct clk_hw ← each clock is registered        │  │
│  │  Parent tracking, rate calculation, ref counting  │  │
│  │  Propagation: enable/rate changes up/down tree    │  │
│  └──────────────────────────┬───────────────────────┘  │
│                             │                          │
│  ┌──────────────────────────▼───────────────────────┐  │
│  │  Provider Drivers (SoC-specific)                  │  │
│  │                                                  │  │
│  │  drivers/clk/qcom/gcc-sdm660.c     ← GCC driver │  │
│  │  drivers/clk/qcom/mmcc-sdm660.c    ← MMCC       │  │
│  │  drivers/clk/qcom/gpucc-sdm660.c   ← GPU CC     │  │
│  │  drivers/clk/qcom/rpmcc-sdm660.c   ← RPM clocks │  │
│  └──────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────┘
```

---

## GCC Driver Probe

```c
/* drivers/clk/qcom/gcc-sdm660.c */

/* Clock definitions — every clock is described */
static struct clk_branch gcc_blsp1_qup3_i2c_apps_clk = {
    .halt_reg = 0x1f008,          /* CBCR register offset */
    .halt_check = BRANCH_HALT,
    .clkr = {
        .enable_reg = 0x1f008,
        .enable_mask = BIT(0),
        .hw.init = &(struct clk_init_data){
            .name = "gcc_blsp1_qup3_i2c_apps_clk",
            .parent_names = (const char *[]){
                "blsp1_qup3_i2c_apps_clk_src",
            },
            .num_parents = 1,
            .flags = CLK_SET_RATE_PARENT,
            .ops = &clk_branch2_ops,
        },
    },
};

/* Rate control clock source (RCG) */
static struct clk_rcg2 blsp1_qup3_i2c_apps_clk_src = {
    .cmd_rcg_reg = 0x1f000,
    .mnd_width = 0,
    .hid_width = 5,
    .parent_map = gcc_parent_map_1,
    .freq_tbl = ftbl_blsp1_qup_i2c_apps_clk_src,
    .clkr.hw.init = &(struct clk_init_data){
        .name = "blsp1_qup3_i2c_apps_clk_src",
        .parent_names = gcc_parent_names_1,
        .num_parents = 3,
        .ops = &clk_rcg2_ops,
    },
};

/* Frequency table */
static const struct freq_tbl ftbl_blsp1_qup_i2c_apps_clk_src[] = {
    F(19200000, P_XO, 1, 0, 0),
    F(50000000, P_GPLL0_OUT_MAIN, 12, 0, 0),
    { }
};

/* Probe function — registers all clocks */
static int gcc_sdm660_probe(struct platform_device *pdev)
{
    /* Map GCC registers */
    regmap = qcom_cc_map(pdev, &gcc_sdm660_desc);
    
    /* Register all PLLs */
    /* Register all RCGs (rate control groups) */
    /* Register all branch clocks */
    /* Register all muxes */
    
    return qcom_cc_really_probe(pdev, &gcc_sdm660_desc, regmap);
}

static const struct of_device_id gcc_sdm660_match_table[] = {
    { .compatible = "qcom,gcc-sdm660" },
    { }
};
```

---

## Clock Tree Walkthrough: I2C for BMI160

```
When I2C QUP3 driver probes:
─────────────────────────────
1. i2c_qup_probe() calls:
   clk = devm_clk_get(&pdev->dev, "core");
   → DT lookup: clocks = <&gcc GCC_BLSP1_QUP3_I2C_APPS_CLK>;
   → Returns clk reference to gcc_blsp1_qup3_i2c_apps_clk

2. clk_prepare_enable(clk)
   → CCF walks up the tree:
   
   gcc_blsp1_qup3_i2c_apps_clk (branch/gate)
     ├── enable: set bit 0 of register 0x1f008
     └── parent: blsp1_qup3_i2c_apps_clk_src (RCG)
           ├── set rate to 50 MHz (from freq table)
           └── parent: gpll0_out_main (PLL)
                 ├── enable GPLL0 if not already on
                 └── parent: xo (19.2 MHz crystal)

   Result: 19.2 MHz → GPLL0 (600 MHz) → /12 → 50 MHz → gate ON
   I2C controller gets 50 MHz source clock
   QUP divides internally for 400 KHz SCL

3. When I2C transfer completes:
   clk_disable_unprepare(clk)
   → Gate OFF, GPLL0 stays on (other users)
```

---

## Clock DT Bindings

```dts
/* SoC-level: clock controller */
gcc: clock-controller@100000 {
    compatible = "qcom,gcc-sdm660";
    reg = <0x100000 0x94000>;
    #clock-cells = <1>;      /* One cell: clock ID */
    #reset-cells = <1>;      /* One cell: reset ID */
};

/* Consumer: I2C controller referencing clocks */
i2c_3: i2c@78b7000 {
    clocks = <&gcc GCC_BLSP1_QUP3_I2C_APPS_CLK>,
             <&gcc GCC_BLSP1_AHB_CLK>;
    clock-names = "core", "iface";
    /* "core" = functional clock, "iface" = bus access clock */
};
```

### Clock ID Constants

```c
/* include/dt-bindings/clock/qcom,gcc-sdm660.h */
#define GCC_BLSP1_AHB_CLK              4
#define GCC_BLSP1_QUP1_I2C_APPS_CLK    8
#define GCC_BLSP1_QUP1_SPI_APPS_CLK    9
#define GCC_BLSP1_QUP2_I2C_APPS_CLK    10
#define GCC_BLSP1_QUP3_I2C_APPS_CLK    12  /* ← BMI160 I2C */
#define GCC_BLSP1_UART1_APPS_CLK       14
#define GCC_SDCC1_APPS_CLK             42
#define GCC_UFS_AXI_CLK                68
#define GCC_USB30_MASTER_CLK           72
```

---

## RPM Clocks

Some clocks are managed by RPM (not GCC directly):

```dts
rpmcc: clock-controller {
    compatible = "qcom,rpmcc-sdm660";
    #clock-cells = <1>;
};

/* Example: XO clock from RPM */
&some_device {
    clocks = <&rpmcc RPM_SMD_XO_CLK_SRC>;
};
```

---

## Debugging Clocks

```bash
# Clock summary (all clocks)
adb shell cat /sys/kernel/debug/clk/clk_summary

# Example output:
#                                  enable  prepare  rate
# clock                           count   count    
# ---------------------------------------------------------
# xo_board                        3       3        19200000
#    gpll0                         1       1        600000000
#       blsp1_qup3_i2c_apps_src   1       1        50000000
#          gcc_blsp1_qup3_i2c_clk 1       1        50000000

# Specific clock rate
adb shell cat /sys/kernel/debug/clk/gcc_blsp1_qup3_i2c_apps_clk/clk_rate
# 50000000

# Enable count
adb shell cat /sys/kernel/debug/clk/gcc_blsp1_qup3_i2c_apps_clk/clk_enable_count
# 1

# Find orphan clocks (parents not registered)
adb shell cat /sys/kernel/debug/clk/orphans
```

---

## Related Documents

- [../02_XBL_Secondary_Bootloader/03_Clock_Tree_Setup.md](../02_XBL_Secondary_Bootloader/03_Clock_Tree_Setup.md) — XBL clock init
- [02_Device_Tree_Processing.md](02_Device_Tree_Processing.md) — Clock DT nodes
- [06_Regulator_Framework.md](06_Regulator_Framework.md) — Power + clock dependencies
