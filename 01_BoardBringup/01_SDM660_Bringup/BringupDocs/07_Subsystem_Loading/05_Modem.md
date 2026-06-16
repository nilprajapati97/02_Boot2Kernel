# Modem — Cellular Subsystem

## Overview

The **Modem (MSS — Modem SubSystem)** is the largest subsystem on SDM660. It contains a Hexagon DSP that runs the cellular protocol stack (LTE, WCDMA, GSM, CDMA), handles RF control, GPS/GNSS, and IMS (Voice over LTE). The modem firmware is the largest PIL-loaded image (~126 MB).

---

## Modem Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Apps Processor                                              │
│  ├── RIL (Radio Interface Layer) ──┐                        │
│  ├── QMI client drivers            │ QMI/GLINK              │
│  └── IMS framework                 │                        │
│                                    │                        │
│  ┌─────────────────────────────────▼──────────────────────┐ │
│  │  Modem SubSystem (Hexagon QDSP6)                       │ │
│  │                                                        │ │
│  │  ├── LTE Layer 1/2/3 (PHY, MAC, RLC, PDCP, RRC)     │ │
│  │  ├── WCDMA / GSM protocol stacks                     │ │
│  │  ├── IMS (SIP, VoLTE, ViLTE)                         │ │
│  │  ├── GPS/GNSS baseband processing                     │ │
│  │  ├── RF front-end control (WTR/WCN)                  │ │
│  │  └── Data path (IP routing, QoS)                      │ │
│  └────────────────────────────────────────────────────────┘ │
│                        │                                     │
│                   RF Transceiver                             │
│                   (WTR2965 / SDR660)                         │
│                        │                                     │
│                   RF Front-End                               │
│                   (PA, LNA, Filters, Switches)               │
│                        │                                     │
│                     Antenna                                  │
└──────────────────────────────────────────────────────────────┘
```

---

## Modem Loading

```
PIL loads modem firmware:
1. request_firmware("modem.mdt") from /lib/firmware/
2. Load ~30+ segments to reserved DDR @ 0x8ac00000 (~126 MB)
3. TZ authenticates all segments (takes ~1-2 seconds)
4. TZ releases modem from reset
5. Modem boots → initializes RF → scans network
6. QMI services register (WDS, NAS, DMS, etc.)
7. RIL connects → Android telephony ready

Kernel log:
[    2.500] subsys-pil-tz 4080000.qcom,mss: firmware: requesting modem.mdt
[    4.200] subsys-pil-tz 4080000.qcom,mss: modem: Brought up successfully
[    5.000] qmi: NAS service connected
[    5.100] qmi: DMS service connected
```

---

## Modem QMI Services

| Service | ID | Purpose |
|---------|-----|---------|
| NAS (Network Access) | 0x03 | Network registration, signal strength |
| WDS (Wireless Data) | 0x01 | Data call setup/teardown |
| DMS (Device Management) | 0x02 | IMEI, firmware version |
| WMS (Wireless Messaging) | 0x05 | SMS send/receive |
| VOICE | 0x09 | Voice call management |
| UIM (SIM Card) | 0x0B | SIM card access |
| IMS | 0x4B | VoLTE services |

---

## Device Tree

```dts
modem_pil: qcom,mss@4080000 {
    compatible = "qcom,pil-q6v5-mss";
    reg = <0x4080000 0x100>;
    qcom,firmware-name = "modem";
    memory-region = <&modem_fw_mem>;
    
    clocks = <&gcc GCC_MSS_CFG_AHB_CLK>,
             <&gcc GCC_BIMC_MSS_Q6_AXI_CLK>;
    clock-names = "iface_clk", "bus_clk";
    
    vdd_mss-supply = <&pm660_s1>;   /* Modem power */
    
    qcom,ssctl-instance-id = <0x12>;
    qcom,sysmon-id = <0>;
};
```

---

## Debugging Modem

```bash
# Modem status
adb shell cat /sys/bus/msm_subsys/devices/subsys0/state
# ONLINE

# Signal strength
adb shell dumpsys telephony.registry | grep SignalStrength

# Modem crash logs
adb shell ls /data/vendor/ramdump/

# QMI service status
adb shell cat /sys/kernel/debug/qmi/services
```

---

## Related Documents

- [01_PIL_Framework.md](01_PIL_Framework.md) — Firmware loading and SSR
- [../08_IPC_Mechanisms/05_QMI.md](../08_IPC_Mechanisms/05_QMI.md) — QMI protocol for modem services
- [../08_IPC_Mechanisms/04_GLINK_SMD.md](../08_IPC_Mechanisms/04_GLINK_SMD.md) — Transport layer
