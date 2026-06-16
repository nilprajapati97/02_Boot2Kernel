# ARMv8 CPU Subsystem (big.LITTLE) – Complete Explanation

---

## 📊 Diagram (Mermaid)

```mermaid
flowchart TB

subgraph CPU["CPU Subsystem (big.LITTLE)"]

    subgraph A7["A7 Cluster (Efficiency)"]
        A7C1["A7 Core"]
        A7C2["A7 Core"]
        A7C3["A7 Core"]
        A7C4["A7 Core"]
        A7L2["Shared L2 Cache"]
        A7C1 --> A7L2
        A7C2 --> A7L2
        A7C3 --> A7L2
        A7C4 --> A7L2
    end

    subgraph A15["A15 Cluster (Performance)"]
        A15C1["A15 Core"]
        A15C2["A15 Core"]
        A15L2["Shared L2 Cache"]
        A15C1 --> A15L2
        A15C2 --> A15L2
    end

    CCI["Gladiator Coherent Interconnect"]

    A7L2 --> CCI
    A15L2 --> CCI

end
