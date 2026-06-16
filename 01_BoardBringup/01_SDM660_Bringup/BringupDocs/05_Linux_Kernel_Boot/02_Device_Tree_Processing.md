# Device Tree Processing

## Overview

The **Device Tree (DT)** is a data structure that describes the hardware to the Linux kernel. On Qualcomm SDM660, the DT describes every SoC peripheral, PMIC, board-level device, pin configuration, clock relationship, and interrupt mapping. The kernel parses the DTB (Device Tree Blob) received from ABL to discover and configure hardware.

---

## Device Tree Flow

```
ABL passes DTB physical address in x0 register
    │
    ▼
head.S: Saves DTB pointer, maps DTB into virtual address space
    │
    ▼
start_kernel() → setup_arch() → setup_machine_fdt()
    │
    ├── unflatten_device_tree()
    │   └── Convert binary DTB → in-memory tree structure
    │       └── struct device_node linked list/tree
    │
    ├── of_irq_init()
    │   └── Initialize interrupt controllers (GIC)
    │
    ├── of_clk_init()
    │   └── Initialize clock providers (GCC, MMCC, GPUCC)
    │
    └── of_platform_populate()
        └── Create platform_device for each DT node
            └── Triggers driver probe() matching
```

---

## SDM660 DT Source File Hierarchy

```
arch/arm64/boot/dts/qcom/
├── sdm660.dtsi                    # SoC-level base (all SDM660 boards)
│   ├── CPU nodes                   # 8 cores, big.LITTLE
│   ├── Memory-mapped peripherals   # I2C, SPI, UART, USB, etc.
│   ├── Interrupt controller (GIC)  # GIC-v3 configuration
│   ├── Clock controllers           # GCC, MMCC, GPUCC
│   ├── IOMMU (SMMU)               # System MMU config
│   └── Reserved memory regions     # TZ, SMEM, PIL regions
│
├── sdm660-pinctrl.dtsi            # All TLMM pin configurations
│   └── Pin groups: i2c, spi, uart, etc.
│
├── sdm660-regulator.dtsi          # PMIC regulator definitions
│   ├── PM660 regulators (S1-S6, L1-L19)
│   └── PM660L regulators (S1-S5, L1-L12)
│
├── pm660.dtsi                     # PM660 PMIC peripherals
│   ├── ADC channels
│   ├── GPIO controller
│   └── RTC, charger, etc.
│
├── sdm660-mtp.dtsi                # MTP (Mobile Test Platform) board
│   ├── Board-specific enables
│   ├── Sensor nodes
│   └── Display panel config
│
└── sdm660-mtp.dts                 # Final DTS (compiled to DTB)
    ├── /dts-v1/;
    ├── #include "sdm660.dtsi"
    ├── #include "sdm660-mtp.dtsi"
    └── / { model = "SDM660 MTP"; };
```

---

## Key DT Node Examples

### CPU Nodes

```dts
cpus {
    #address-cells = <2>;
    #size-cells = <0>;

    cpu-map {
        cluster0 {  /* Gold (Performance) */
            core0 { cpu = <&CPU0>; };
            core1 { cpu = <&CPU1>; };
            core2 { cpu = <&CPU2>; };
            core3 { cpu = <&CPU3>; };
        };
        cluster1 {  /* Silver (Efficiency) */
            core0 { cpu = <&CPU4>; };
            core1 { cpu = <&CPU5>; };
            core2 { cpu = <&CPU6>; };
            core3 { cpu = <&CPU7>; };
        };
    };

    CPU0: cpu@0 {
        device_type = "cpu";
        compatible = "arm,kryo";
        reg = <0x0 0x0>;
        enable-method = "psci";
        capacity-dmips-mhz = <1024>;  /* Big core */
        clock-frequency = <2208000000>;
        next-level-cache = <&L2_0>;
    };

    CPU4: cpu@100 {
        device_type = "cpu";
        compatible = "arm,kryo";
        reg = <0x0 0x100>;
        enable-method = "psci";
        capacity-dmips-mhz = <670>;   /* Little core */
        clock-frequency = <1843200000>;
        next-level-cache = <&L2_1>;
    };
};
```

### I2C Controller (for BMI160)

```dts
i2c_3: i2c@78b7000 {
    compatible = "qcom,i2c-qup-v2.2.1";
    reg = <0x78b7000 0x600>;
    interrupts = <GIC_SPI 97 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&gcc GCC_BLSP1_QUP3_I2C_APPS_CLK>,
             <&gcc GCC_BLSP1_AHB_CLK>;
    clock-names = "core", "iface";
    clock-frequency = <400000>;  /* 400 KHz Fast Mode */
    pinctrl-names = "default", "sleep";
    pinctrl-0 = <&i2c_3_active>;
    pinctrl-1 = <&i2c_3_sleep>;
    status = "okay";

    bmi160@68 {
        compatible = "bosch,bmi160";
        reg = <0x68>;
        interrupt-parent = <&tlmm>;
        interrupts = <23 IRQ_TYPE_EDGE_RISING>;
        vdd-supply = <&pm660_l13>;
        vddio-supply = <&pm660_l9>;
    };
};
```

### GIC (Interrupt Controller)

```dts
intc: interrupt-controller@17a00000 {
    compatible = "arm,gic-v3";
    reg = <0x17a00000 0x10000>,     /* GICD (Distributor) */
          <0x17b00000 0x100000>,    /* GICR (Redistributor) */
          <0x17a60000 0x2000>,      /* GICC (CPU Interface) */
          <0x17a70000 0x1000>,      /* GICH */
          <0x17a80000 0x2000>;      /* GICV */
    interrupt-controller;
    #interrupt-cells = <3>;  /* <type, number, flags> */
    interrupts = <GIC_PPI 9 IRQ_TYPE_LEVEL_HIGH>;
};
```

---

## DT → Driver Matching

```
Device Tree Node                      Kernel Driver
────────────────                      ────────────
compatible = "qcom,i2c-qup-v2.2.1"   ──►  drivers/i2c/busses/i2c-qup.c
                                           .compatible = "qcom,i2c-qup-v2.2.1"

compatible = "arm,gic-v3"            ──►  drivers/irqchip/irq-gic-v3.c
                                           .compatible = "arm,gic-v3"

compatible = "bosch,bmi160"          ──►  drivers/iio/imu/bmi160/bmi160_i2c.c
                                           .compatible = "bosch,bmi160"
```

### Matching Process

```
1. of_platform_populate() walks DT tree
2. For each node with compatible property:
   ├── Create struct platform_device
   ├── Search all registered platform_driver.of_match_table
   └── If match found → call driver.probe()

3. I2C/SPI child devices:
   ├── I2C adapter driver (i2c-qup) probes first
   ├── i2c_qup_probe() registers I2C adapter
   ├── of_i2c_register_devices() scans child nodes
   └── For "bosch,bmi160" → creates i2c_client → bmi160 driver probes
```

---

## DT Compilation

```bash
# Compile DTS → DTB
dtc -I dts -O dtb -o sdm660-mtp.dtb sdm660-mtp.dts

# Or via kernel build system
make ARCH=arm64 dtbs

# Decompile DTB (for debugging)
dtc -I dtb -O dts -o output.dts sdm660-mtp.dtb

# Verify DTB contents
fdtdump sdm660-mtp.dtb | less
```

---

## Debugging Device Tree Issues

```bash
# View entire DT from running device
adb shell ls /sys/firmware/devicetree/base/

# Check a specific node
adb shell cat /sys/firmware/devicetree/base/soc/i2c@78b7000/status
# Output: "okay" or "disabled"

# Check compatible string
adb shell cat /sys/firmware/devicetree/base/soc/i2c@78b7000/compatible
# Output: qcom,i2c-qup-v2.2.1

# Check if driver matched
adb shell ls /sys/bus/i2c/devices/
# 3-0068 → Bus 3, address 0x68 (BMI160)

# View all platform devices created from DT
adb shell ls /sys/bus/platform/devices/ | grep 78b7
# 78b7000.i2c
```

---

## Related Documents

- [01_Early_Assembly_Boot.md](01_Early_Assembly_Boot.md) — head.S maps DTB
- [03_GCC_Clock_Framework.md](03_GCC_Clock_Framework.md) — Clock DT nodes
- [04_Pinctrl_TLMM.md](04_Pinctrl_TLMM.md) — Pin configuration DT
- [05_GIC_Interrupt_Controller.md](05_GIC_Interrupt_Controller.md) — Interrupt mapping DT
