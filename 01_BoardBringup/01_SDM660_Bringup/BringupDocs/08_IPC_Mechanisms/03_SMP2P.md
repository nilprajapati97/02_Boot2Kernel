# SMP2P — Shared Memory Point-to-Point

## Overview

**SMP2P (Shared Memory Point-to-Point)** provides a mechanism for two processors to exchange up to 32 named state bits. Unlike SMSM (which has a fixed set of bits per processor), SMP2P allows dynamic creation of "entries" between specific processor pairs.

---

## SMP2P Architecture

```
Per processor pair (e.g., Apps ↔ Modem):
────────────────────────────────────────
┌──────────────────────────────────────────┐
│  SMP2P Table in SMEM                      │
│                                          │
│  ┌────────────────────────────────────┐  │
│  │  Apps → Modem entries              │  │
│  │  ├── "sleepstate" : 32 bits       │  │
│  │  ├── "smp2pgpio"  : 32 bits       │  │
│  │  └── (up to 16 entries)           │  │
│  ├────────────────────────────────────┤  │
│  │  Modem → Apps entries              │  │
│  │  ├── "sleepstate" : 32 bits       │  │
│  │  ├── "err_fatal"  : 32 bits       │  │
│  │  └── (up to 16 entries)           │  │
│  └────────────────────────────────────┘  │
└──────────────────────────────────────────┘
```

---

## SMP2P as GPIO

SMP2P entries can be exposed as **virtual GPIOs** in the kernel, allowing standard GPIO/interrupt APIs to signal between processors:

```dts
/* Apps → SLPI direction */
smp2pgpio_sleepstate_SLPI: qcom,smp2pgpio-sleepstate-slpi {
    compatible = "qcom,smp2pgpio";
    qcom,entry-name = "sleepstate";
    qcom,remote-pid = <3>;       /* SLPI PID */
    gpio-controller;
    #gpio-cells = <2>;
};

/* SLPI → Apps direction (interrupt source) */
smp2pgpio_slpi_in: qcom,smp2pgpio-slpi-in {
    compatible = "qcom,smp2pgpio";
    qcom,entry-name = "err_fatal";
    qcom,remote-pid = <3>;
    qcom,is-inbound;
    interrupt-controller;
    #interrupt-cells = <2>;
};
```

### Usage: Signal SLPI to enter sleep

```c
/* Toggle bit 0 of "sleepstate" SMP2P entry → SLPI receives interrupt */
gpio_set_value(smp2p_sleepstate_gpio, 1);
```

---

## SMP2P Initialization

```
Boot-time handshake:
1. Each processor creates its outbound SMP2P table in SMEM
2. Sends IPC interrupt to remote processor
3. Remote processor discovers inbound table
4. Negotiates version
5. SMP2P entries can now be created and modified
```

---

## Related Documents

- [01_SMEM_Shared_Memory.md](01_SMEM_Shared_Memory.md) — SMEM storage
- [02_SMSM.md](02_SMSM.md) — Similar but simpler state notification
- [04_GLINK_SMD.md](04_GLINK_SMD.md) — Message-based communication
