# SoC Architecture – Mermaid Diagram

```mermaid
flowchart TB

%% ================= CPU =================
subgraph CPU["CPU Subsystem (big.LITTLE)"]
    subgraph A7["A7 Cluster"]
        A7C1["A7 Core"]
        A7C2["A7 Core"]
        A7C3["A7 Core"]
        A7C4["A7 Core"]
        A7L2["L2 Cache"]
        A7C1 --> A7L2
        A7C2 --> A7L2
        A7C3 --> A7L2
        A7C4 --> A7L2
    end

    subgraph A15["A15 Cluster"]
        A15C1["A15 Core"]
        A15C2["A15 Core"]
        A15L2["L2 Cache"]
        A15C1 --> A15L2
        A15C2 --> A15L2
    end

    CCI["Coherent Interconnect"]
    A7L2 --> CCI
    A15L2 --> CCI
end

%% ================= TOP INTERCONNECT =================
NOC["FlexNoC Top Level Interconnect"]

CCI --> NOC

%% ================= GPU =================
GPU["GPU Subsystem (3D Graphics)"]
GPU --> NOC

%% ================= DSP =================
subgraph DSP["DSP Subsystem (A/V)"]
    DSPIP1["IP"]
    DSPIP2["IP"]
    DSPIP3["IP"]
    DSPINT["FlexWay Interconnect"]
    DSPIP1 --> DSPINT
    DSPIP2 --> DSPINT
    DSPIP3 --> DSPINT
end
DSPINT --> NOC

%% ================= APPLICATION IP =================
subgraph APP["Application IP Subsystem"]
    APPIP1["IP"]
    APPIP2["IP"]
    APPIP3["IP"]
    APPINT["FlexWay Interconnect"]
    APPIP1 --> APPINT
    APPIP2 --> APPINT
    APPIP3 --> APPINT
end
APPINT --> NOC

%% ================= MEMORY =================
subgraph MEM["Memory Subsystem"]
    MS["Memory Scheduler"]
    MC["Memory Controller"]
    DDR["LPDDR / DDR"]
    MS --> MC
    MC --> DDR
end
MEM --> NOC

%% ================= WIRED PERIPHERALS =================
subgraph WIRED["High-Speed Wired Peripherals"]
    USB["USB 2.0 / 3.0"]
    PCIE["PCIe"]
    ETH["Ethernet"]
end
WIRED --> NOC

%% ================= WIRELESS =================
subgraph WIRELESS["Wireless Subsystem"]
    WIFI["WiFi"]
    GSM["GSM"]
    LTE["LTE"]
end
WIRELESS --> NOC

%% ================= SECURITY =================
subgraph SEC["Security Subsystem"]
    CRYPTO["Crypto Firewall"]
    RSA["RSA / Cert Engine"]
end
SEC --> NOC

%% ================= IO =================
subgraph IO["I/O Peripherals"]
    HDMI["HDMI"]
    MIPI["MIPI"]
    DISP["Display"]
    PMU["PMU"]
    JTAG["JTAG"]
end
IO --> NOC

%% ================= EXTERNAL =================
EXT["InterChip Links"]
NOC --> EXT
