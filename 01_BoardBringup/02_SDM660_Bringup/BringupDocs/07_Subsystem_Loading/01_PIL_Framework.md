# PIL — Peripheral Image Loader Framework

## Overview

**PIL (Peripheral Image Loader)** is Qualcomm's kernel framework for loading firmware into co-processors (modem, ADSP, CDSP, SLPI). Each subsystem has its own processor that runs dedicated firmware. PIL handles loading, authentication (via TrustZone), and lifecycle management.

---

## PIL Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    PIL Framework                              │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  PIL Core (drivers/soc/qcom/peripheral-loader.c)       │  │
│  │                                                        │  │
│  │  1. Load firmware segments from /lib/firmware/          │  │
│  │  2. Copy to subsystem's DDR region                     │  │
│  │  3. Call TZ (SCM) to authenticate firmware             │  │
│  │  4. Release subsystem from reset                       │  │
│  │  5. Monitor subsystem (SSR — crash detection)          │  │
│  └────────────┬───────────────────────────────────────────┘  │
│               │                                              │
│  ┌────────────▼───────────────────────────────────────────┐  │
│  │  PIL Drivers (per subsystem)                            │  │
│  │                                                        │  │
│  │  ├── Modem PIL (q6v5_pil.c)    → Modem DSP firmware   │  │
│  │  ├── ADSP PIL (q6v5_pil.c)     → Audio DSP firmware   │  │
│  │  ├── CDSP PIL (q6v5_pil.c)     → Compute DSP firmware │  │
│  │  └── SLPI PIL (q6v5_pil.c)     → Sensor DSP firmware  │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  Firmware Location: /lib/firmware/ (or /vendor/firmware/)    │
│  ├── modem.mdt + modem.b00-bNN   (modem firmware)          │
│  ├── adsp.mdt + adsp.b00-bNN     (ADSP firmware)           │
│  ├── cdsp.mdt + cdsp.b00-bNN     (CDSP firmware)           │
│  └── slpi.mdt + slpi.b00-bNN     (SLPI firmware)           │
└──────────────────────────────────────────────────────────────┘
```

---

## MDT/Split Image Format

Subsystem firmware is stored in **MDT (Metadata)** + split segment files:

```
Firmware files:
───────────────
modem.mdt     → ELF header + hash segment (metadata)
modem.b00     → Segment 0 (code/data)
modem.b01     → Segment 1
modem.b02     → Segment 2
...
modem.bNN     → Last segment

MDT structure:
┌────────────────────┐
│  ELF Header        │  Standard ELF32/ELF64 header
├────────────────────┤
│  Program Headers   │  Describes each segment's load address
│  ├── Segment 0     │  type, vaddr, paddr, filesz, memsz
│  ├── Segment 1     │
│  └── ...           │
├────────────────────┤
│  Hash Segment      │  SHA-256 hashes of each segment
│  ├── Hash of seg 0 │  Used by TZ for authentication
│  ├── Hash of seg 1 │
│  └── ...           │
├────────────────────┤
│  Signature         │  RSA signature of hash segment
│  └── Certificate   │  OEM certificate chain
│      Chain         │
└────────────────────┘
```

---

## PIL Load Flow

```
Subsystem boot trigger (e.g., modem):
─────────────────────────────────────
1. PIL driver: request_firmware("modem.mdt")
   └── Kernel reads from /lib/firmware/modem.mdt

2. Parse ELF header → determine segment count and load addresses

3. For each segment:
   ├── request_firmware("modem.bNN")
   └── Copy to reserved DDR region (from DT reserved-memory)

4. SCM call: qcom_scm_pas_init_image(MODEM_PAS_ID, mdt_buf)
   └── TZ reads metadata, prepares authentication

5. SCM call: qcom_scm_pas_mem_setup(MODEM_PAS_ID, addr, size)
   └── TZ configures memory protection (xPU) for modem region

6. SCM call: qcom_scm_pas_auth_and_reset(MODEM_PAS_ID)
   └── TZ: verify segment hashes → verify signature
   └── TZ: if PASS → release modem from reset
   └── Modem DSP starts executing firmware

7. Handshake: wait for subsystem to report ready via SMSM/SMEM

8. PIL reports: "modem: Brought up successfully"
```

---

## Reserved Memory for Subsystems

```dts
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;

    modem_fw_mem: modem_fw_region@8ac00000 {
        reg = <0x0 0x8ac00000 0x0 0x7e00000>;  /* ~126 MB */
        no-map;
    };
    
    adsp_fw_mem: adsp_fw_region@92a00000 {
        reg = <0x0 0x92a00000 0x0 0x1e00000>;  /* 30 MB */
        no-map;
    };
    
    cdsp_fw_mem: cdsp_fw_region@94800000 {
        reg = <0x0 0x94800000 0x0 0x600000>;   /* 6 MB */
        no-map;
    };
    
    slpi_fw_mem: slpi_fw_region@94e00000 {
        reg = <0x0 0x94e00000 0x0 0x1400000>;  /* 20 MB */
        no-map;
    };
};
```

---

## SSR (SubSystem Restart)

If a subsystem crashes, PIL's SSR mechanism handles recovery:

```
Crash Detection:
1. Subsystem watchdog expires (no heartbeat)
2. OR subsystem sends error fatal indication
3. PIL error handler triggered

Recovery:
1. Notify dependent services (via notifier chain)
2. Stop subsystem (assert reset)
3. Collect ramdump (if enabled)
4. Reload firmware (repeat PIL load flow)
5. Notify services: subsystem is back
```

```bash
# SSR policy
adb shell cat /sys/bus/msm_subsys/devices/subsys0/restart_level
# related  (restart subsystem + dependencies)
# system   (full system restart)

# Trigger subsystem crash (debug)
adb shell echo 1 > /sys/kernel/debug/subsys_dbg/modem/crashme

# SSR log
adb shell dmesg | grep -i "subsys\|ssr\|pil"
```

---

## Related Documents

- [02_ADSP.md](02_ADSP.md) — Audio DSP loading
- [03_CDSP.md](03_CDSP.md) — Compute DSP loading
- [04_SLPI.md](04_SLPI.md) — Sensor DSP loading
- [05_Modem.md](05_Modem.md) — Modem DSP loading
- [../02_XBL_Secondary_Bootloader/05_TrustZone_QSEE_Init.md](../02_XBL_Secondary_Bootloader/05_TrustZone_QSEE_Init.md) — TZ authenticates firmware
