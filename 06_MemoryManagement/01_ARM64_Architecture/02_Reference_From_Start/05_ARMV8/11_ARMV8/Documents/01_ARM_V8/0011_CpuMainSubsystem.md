# CPUSS Cluster Usage – Mermaid Diagram

```mermaid
flowchart LR

%% ================= LOW LOAD =================
subgraph LOW["Low Load"]

    subgraph LL_A57["Cortex-A57 Cluster"]
        LL_A57_1["A57 (Inactive)"]
        LL_A57_2["A57 (Inactive)"]
        LL_A57_3["A57 (Inactive)"]
        LL_A57_4["A57 (Inactive)"]
    end

    subgraph LL_A53["Cortex-A53 Cluster"]
        LL_A53_1["A53 (Active)"]
        LL_A53_2["A53 (Active)"]
        LL_A53_3["A53 (Active)"]
        LL_A53_4["A53 (Active)"]
    end

    LL_CCI["Cache Coherent Interconnect (CCI)"]

    LL_A57 --> LL_CCI
    LL_A53 --> LL_CCI
end


%% ================= HIGH LOAD =================
subgraph HIGH["High Load"]

    subgraph HL_A57["Cortex-A57 Cluster"]
        HL_A57_1["A57 (Active)"]
        HL_A57_2["A57 (Active)"]
        HL_A57_3["A57 (Active)"]
        HL_A57_4["A57 (Active)"]
    end

    subgraph HL_A53["Cortex-A53 Cluster"]
        HL_A53_1["A53 (Inactive)"]
        HL_A53_2["A53 (Inactive)"]
        HL_A53_3["A53 (Inactive)"]
        HL_A53_4["A53 (Inactive)"]
    end

    HL_CCI["Cache Coherent Interconnect (CCI)"]

    HL_A57 --> HL_CCI
    HL_A53 --> HL_CCI
end
