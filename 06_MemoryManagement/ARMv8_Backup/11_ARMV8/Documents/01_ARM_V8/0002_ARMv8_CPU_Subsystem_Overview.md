# SoC Architecture – Exact Layout Style

```mermaid
flowchart TB

%% ================= TOP ROW =================
subgraph TOP[" "]
direction LR

CPU["CPU Subsystem<br>(A7 + A15 + L2 + CCI)"]

GPU["GPU Subsystem<br>3D Graphics"]

DSP["DSP Subsystem<br>FlexWay + IP"]

APP["Application IP<br>FlexWay + IP"]

MULTI["Multimedia<br>AES / 2D / MPEG"]

end

%% ================= INTERCONNECT =================
NOC["FlexNoC Top Level Interconnect"]

%% ================= BOTTOM ROW =================
subgraph BOTTOM[" "]
direction LR

MEM["Memory Subsystem<br>Scheduler + DDR"]

WIRED["USB / PCIe / Ethernet"]

WIRELESS["WiFi / GSM / LTE"]

SEC["Security<br>Crypto + RSA"]

IO["I/O<br>HDMI / MIPI / JTAG"]

end

%% ================= CONNECTIONS =================

TOP --> NOC
NOC --> BOTTOM

%% individual connections for clarity
CPU --> NOC
GPU --> NOC
DSP --> NOC
APP --> NOC
MULTI --> NOC

NOC --> MEM
NOC --> WIRED
NOC --> WIRELESS
NOC --> SEC
NOC --> IO

%% ================= RIGHT SIDE =================
EXT["InterChip Links"]
NOC --> EXT
