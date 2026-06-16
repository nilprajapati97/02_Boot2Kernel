# Module 7 — Device Tree for Cameras

> **Scope:** How a camera pipeline is described in the device tree (DT) so the kernel
> can probe sensors, wire the media graph, and configure clocks/regulators/GPIOs/PHY.
> Covers the OF graph binding (ports/endpoints/remote-endpoint), CSI lane mapping,
> link-frequency, power/clock/reset properties, multi-camera topologies, and real
> Qualcomm + NVIDIA examples. This is the glue that makes Modules 5/6/8/9 actually
> bind together at boot.

---

## 1. Why DT matters for cameras

The kernel needs to know, *before* any driver runs:
- Which sensor is on which I2C bus at what address.
- Which CSI port/lanes the sensor connects to, and the link frequency.
- What clocks, regulators, and GPIOs power and reset the sensor.
- How the media-controller graph should be wired (sensor → CSI → ISP).

All of this is **board-specific** and lives in DT, not in driver code. The V4L2
**fwnode/async** framework (Module 6) reads DT endpoints to bind subdevs into the
media graph. Get the DT wrong and the sensor never probes, or the graph never
completes (`-EPROBE_DEFER` forever).

---

## 2. The OF graph binding: ports, endpoints, remote-endpoint

A camera connection is a point-to-point link described by the **OF graph** binding
(`Documentation/devicetree/bindings/graph.txt`):

```
 sensor node                          CSI receiver node
 ┌──────────────────────┐            ┌──────────────────────┐
 │ port {                │            │ port {               │
 │   ep: endpoint {      │◄──────────►│   csi_ep: endpoint {│
 │     remote-endpoint   │  the link  │     remote-endpoint  │
 │       = <&csi_ep>;    │            │       = <&ep>;       │
 │     data-lanes=<1 2>; │            │     data-lanes=<1 2>;│
 │     link-frequencies; │            │   };                 │
 │   };                  │            │ };                   │
 │ };                    │            └──────────────────────┘
 └──────────────────────┘
```

- **port** — a physical interface on a device (here, the sensor's MIPI output).
- **endpoint** — one connection within a port (a port can have several endpoints for
  multiple remote devices).
- **remote-endpoint** — a phandle pointing at the *other side's* endpoint, forming the
  bidirectional link the kernel matches on.

The kernel matches `remote-endpoint` phandles to build the media graph automatically.

---

## 3. A complete sensor node (IMX219 on a CSI receiver)

```dts
&i2c1 {
    imx219: camera-sensor@10 {
        compatible = "sony,imx219";
        reg = <0x10>;                          /* I2C address */

        /* Clocks: the master/external clock (XCLK/INCK) */
        clocks = <&cam_clk>;
        clock-names = "xclk";
        assigned-clocks = <&cam_clk>;
        assigned-clock-rates = <24000000>;     /* 24 MHz XCLK */

        /* Power rails (regulators) */
        VANA-supply = <&reg_2v8>;              /* analog 2.8V (AVDD) */
        VDIG-supply = <&reg_1v8>;              /* digital 1.8V */
        VDDL-supply = <&reg_1v2>;              /* core 1.2V (DVDD) */

        /* Reset / enable GPIO */
        reset-gpios = <&gpio1 5 GPIO_ACTIVE_HIGH>;

        port {
            imx219_ep: endpoint {
                remote-endpoint = <&csi_in>;
                clock-lanes = <0>;
                data-lanes = <1 2>;            /* 2-lane D-PHY */
                link-frequencies = /bits/ 64 <456000000>;  /* must match sensor PLL */
            };
        };
    };
};
```

Each property feeds a specific framework in the sensor driver (Module 9):
```
DT property            →  kernel API the driver uses
────────────────────────────────────────────────────
clocks/clock-names     →  devm_clk_get(dev, "xclk")
*-supply               →  devm_regulator_get(dev, "VANA")
reset-gpios            →  devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH)
data-lanes             →  v4l2_fwnode_endpoint_parse() → bus.mipi_csi2.num_data_lanes
link-frequencies       →  validated against V4L2_CID_LINK_FREQ menu
remote-endpoint        →  v4l2_async remote subdev matching
```

---

## 4. The CSI receiver side

```dts
csi: csi@1c30000 {
    compatible = "...,mipi-csi2";
    reg = <0x01c30000 0x1000>;
    clocks = <&ccu CLK_CSI>, <&ccu CLK_CSI_MIPI>;
    clock-names = "bus", "mod";
    resets = <&ccu RST_CSI>;
    interrupts = <GIC_SPI 84 IRQ_TYPE_LEVEL_HIGH>;

    ports {
        #address-cells = <1>;
        #size-cells = <0>;

        port@0 {                    /* input from sensor */
            reg = <0>;
            csi_in: endpoint {
                remote-endpoint = <&imx219_ep>;
                clock-lanes = <0>;
                data-lanes = <1 2>;     /* MUST match sensor's data-lanes */
                bus-type = <4>;         /* V4L2_FWNODE_BUS_TYPE_CSI2_DPHY */
            };
        };
    };
};
```

Critical consistency rules:
- `data-lanes` and `link-frequencies` **must match** on both endpoints, or the CSIPHY
  is misconfigured and you get CRC/sync errors (Module 8).
- `bus-type` selects D-PHY vs C-PHY vs parallel; the CSIPHY driver reads it.

---

## 5. Parsing endpoints in the driver (v4l2-fwnode)

```c
struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_DPHY };
struct fwnode_handle *fwep;

fwep = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
ret = v4l2_fwnode_endpoint_alloc_parse(fwep, &ep);

sensor->num_data_lanes = ep.bus.mipi_csi2.num_data_lanes;     /* e.g. 2 */
/* validate link frequency present in our supported menu */
for (i = 0; i < ep.nr_of_link_frequencies; i++)
    if (ep.link_frequencies[i] == IMX219_DEFAULT_LINK_FREQ)
        found = true;

v4l2_fwnode_endpoint_free(&ep);
```

`v4l2_fwnode_endpoint_parse()` decodes `data-lanes`, `clock-lanes`,
`link-frequencies`, and `bus-type` from the endpoint into a struct the driver uses to
program lane count and validate clocking.

---

## 6. Clocks, regulators, GPIO, PHY, power domains

### 6.1 Clocks
```dts
clocks = <&cam_clk>;
clock-names = "xclk";
assigned-clock-rates = <24000000>;
```
Driver: `clk_get` / `clk_set_rate` / `clk_prepare_enable` in the power-on sequence.

### 6.2 Regulators
```dts
VANA-supply = <&reg_2v8>;
```
Driver: `regulator_bulk_get` + `regulator_bulk_enable` with the datasheet ordering
(Module 2 §8). Use `regulator-bulk` for ganged enable.

### 6.3 GPIO (reset/enable/powerdown)
```dts
reset-gpios     = <&gpio1 5 GPIO_ACTIVE_HIGH>;
powerdown-gpios = <&gpio1 6 GPIO_ACTIVE_LOW>;
```
Driver: `devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH)`. **Polarity in DT must match the
schematic** — a flipped `GPIO_ACTIVE_*` is a classic bring-up bug.

### 6.4 PHY
Some SoCs model the D-PHY as a generic PHY:
```dts
phys = <&mipi_dphy 0>;
phy-names = "dphy";
```
Driver: `devm_phy_get` + `phy_init`/`phy_power_on`. On Qualcomm the CSIPHY is its own
CAMSS sub-block instead.

### 6.5 Power domains
```dts
power-domains = <&camcc CAM_CC_PD>;
```
Managed via runtime PM / genpd; the camera subsystem power domain gates the whole
CAMSS/ISP.

---

## 7. Multi-camera device tree

Multiple sensors on multiple CSI ports each get their own endpoint:

```dts
/* CSI bridge with two input ports */
ports {
    port@0 {
        reg = <0>;
        csi0_in: endpoint { remote-endpoint = <&cam0_ep>; data-lanes = <1 2>; };
    };
    port@1 {
        reg = <1>;
        csi1_in: endpoint { remote-endpoint = <&cam1_ep>; data-lanes = <1 2 3 4>; };
    };
};

&i2c1 { cam0: sensor@10 { ... port { cam0_ep: endpoint { remote-endpoint = <&csi0_in>; }; }; }; };
&i2c2 { cam1: sensor@1a { ... port { cam1_ep: endpoint { remote-endpoint = <&csi1_in>; }; }; }; };
```

For **SerDes** (automotive, Module 2 §7), the deserializer is a DT node with multiple
ports: one per remote camera (over the link) plus the CSI output to the SoC. The I2C
addresses are remapped by the SerDes and described in its own subnodes.

---

## 8. Qualcomm camera DT (CAMSS)

Qualcomm CAMSS (Module 11) is described as one big node with sub-blocks and many CSIPHY
ports:

```dts
camss: camss@ac6a000 {
    compatible = "qcom,sdm845-camss";
    reg = <0 0x0ac6a000 0 0x2000>, ... ;   /* csid, vfe, csiphy regions */
    reg-names = "csid0", "csid1", "csid2", "vfe0", "vfe1", "vfe_lite", ...;
    interrupts = <...>;
    interrupt-names = "csid0", "csid1", "vfe0", ...;
    power-domains = <&camcc IFE_0_GDSC>, <&camcc IFE_1_GDSC>, <&camcc TITAN_TOP_GDSC>;
    clocks = <&camcc CAM_CC_*>;
    clock-names = "cpas_ahb", "camnoc_axi", "vfe0_axi", "csiphy0", ...;
    iommus = <&apps_smmu 0x808 0x0>;        /* SMMU stream IDs */

    ports {
        port@0 {
            reg = <0>;
            csiphy0_ep: endpoint {
                clock-lanes = <7>;
                data-lanes = <0 1 2 3>;
                remote-endpoint = <&imx_ep>;
            };
        };
        /* port@1, port@2 ... for other CSIPHYs */
    };
};
```

Notable Qualcomm specifics: `iommus` (SMMU stream IDs for DMA, Module 14),
`power-domains` (per-IFE GDSCs), and the long `clock-names` list (CPAS AHB, CAMNOC AXI,
per-CSIPHY/CSID/VFE clocks). Binding doc:
`Documentation/devicetree/bindings/media/qcom,*-camss.yaml`.

---

## 9. NVIDIA Tegra camera DT

Tegra (Module 12) uses NVCSI + VI with the OF graph too:

```dts
vi@15c10000 {
    compatible = "nvidia,tegra210-vi";
    ...
    ports {
        port@0 {
            reg = <0>;
            vi_in0: endpoint {
                remote-endpoint = <&csi_out0>;
                bus-width = <4>;
            };
        };
    };
};

nvcsi@150c0000 {
    compatible = "nvidia,tegra210-csi";
    channel@0 {
        ports {
            port@0 { csi_in0: endpoint { remote-endpoint = <&imx_out>; data-lanes=<1 2>; }; };
            port@1 { csi_out0: endpoint { remote-endpoint = <&vi_in0>; }; };
        };
    };
};
```

NVIDIA's downstream L4T tree adds `tegra-camera-platform` and per-mode properties
(`mode0 { ... }`) describing sensor modes, pixel rates, and timing for the proprietary
camera framework — a notable difference from mainline.

---

## 10. Boot-time bind/probe flow

```
 1. DT parsed → I2C core instantiates sensor device (compatible match)
 2. Sensor driver .probe(): get clocks/regulators/gpios, read endpoint,
    power on, read chip-ID, register subdev (v4l2_async_register_subdev_sensor)
 3. CSI/ISP bridge .probe(): register async notifier listing remote endpoints
 4. async core matches remote-endpoint phandles → .bound() callbacks
 5. when all expected subdevs bound → .complete() → create media links,
    register /dev/mediaN, /dev/v4l-subdevN, /dev/videoN
 6. -EPROBE_DEFER until clocks/regulators/remote subdevs are available
```

`-EPROBE_DEFER` loops are normal during boot (dependency ordering). A *permanent*
defer means a phandle is wrong (clock/regulator/remote-endpoint name mismatch).

---

## 11. Debugging camera DT

```bash
# See the live device tree the kernel is using
ls /proc/device-tree/                       # or:
dtc -I fs -O dts /proc/device-tree | less

# Why didn't the sensor probe?
dmesg | grep -iE "imx219|csi|camss|EPROBE_DEFER|failed to get"

# Which devices are stuck deferring?
cat /sys/kernel/debug/devices_deferred

# Confirm the graph completed
media-ctl -p -d /dev/media0
```

Common DT bugs:
- Wrong `reg` (I2C address) → sensor never ACKs.
- Mismatched `data-lanes`/`link-frequencies` between endpoints → CSI CRC errors.
- Wrong GPIO polarity → sensor held in reset (no chip-ID).
- Wrong clock-name/regulator-supply name → `devm_*_get` fails → permanent
  `-EPROBE_DEFER` or probe error.
- Missing/incorrect `remote-endpoint` phandle → async graph never completes, no video
  node appears.

---

## 12. Interview Q&A

**Q1. Explain the OF graph binding for a camera link.**
A point-to-point link is described with `port`/`endpoint` nodes; each endpoint's
`remote-endpoint` phandle points at the other side's endpoint. The kernel matches these
phandles to build the media graph. `data-lanes`, `clock-lanes`, `link-frequencies`,
and `bus-type` describe the MIPI configuration.

**Q2. The sensor probes but you get CSI CRC errors. What DT properties do you check?**
The `data-lanes` and `link-frequencies` (and `bus-type`) on both the sensor and CSI
endpoints — they must match each other and the sensor's actual PLL/lane configuration.
A lane-count or link-frequency mismatch misconfigures CSIPHY settle timing → CRC.

**Q3. What causes a permanent `-EPROBE_DEFER` for a camera sensor?**
A dependency that never resolves: a wrong clock-name/regulator-supply name so
`devm_*_get` keeps failing, or a bad `remote-endpoint` phandle so the async notifier
never completes. Check `/sys/kernel/debug/devices_deferred` and dmesg.

**Q4. How does the kernel know which sensor connects to which CSI port?**
Through the `remote-endpoint` phandles in the OF graph: each CSI input port's endpoint
points to a sensor endpoint and vice-versa. The V4L2 async/fwnode core matches them to
create the media links.

**Q5. Where are GPIO polarity bugs hidden and how do they manifest?**
In `reset-gpios = <&gpio X GPIO_ACTIVE_LOW|HIGH>`. If the polarity is wrong, the driver
holds the sensor in reset (or releases it at the wrong time), so the chip-ID read fails
and the sensor never probes.

**Q6. What's special about Qualcomm CAMSS DT vs a simple sensor+CSI?**
CAMSS is one large node with many reg regions (csid/vfe/csiphy), per-IFE
`power-domains` (GDSCs), a long camera clock list, `iommus` stream IDs for SMMU DMA, and
multiple CSIPHY input ports — it describes the whole on-SoC subsystem, not just one
receiver.

**Q7. How do clocks/regulators in DT relate to the sensor power sequence?**
DT declares them (`clocks`, `*-supply`, `reset-gpios`); the driver acquires them with
`clk_get`/`regulator_bulk_get`/`gpiod_get` and enables them in the datasheet-specified
order and timing during power-on/runtime-PM resume. DT provides the resources; the
driver enforces the sequence.

**Q8. How is a SerDes (automotive) represented in DT?**
The deserializer is a node with multiple ports: one endpoint per remote camera reached
over the GMSL/FPD-Link, plus a CSI output endpoint to the SoC. I2C address remapping and
per-link channels are described in its subnodes; each camera still has its own sensor
node bound through the SerDes.

---

### Key takeaways
- DT describes board-specific wiring: sensor I2C addr, CSI lanes/link-freq, clocks,
  regulators, reset GPIOs, and the media-graph links via OF graph endpoints.
- `remote-endpoint` phandles are what the V4L2 async/fwnode core matches to build the
  graph; `data-lanes`/`link-frequencies` must agree on both ends.
- DT provides resources; the sensor driver enforces the power sequence and validates
  link frequency.
- `-EPROBE_DEFER`, missing video nodes, and CSI CRC errors are usually DT mismatches —
  check names, phandles, lane counts, and GPIO polarity.
