# Branch Prediction

## 1. Why Branch Prediction?

In a pipelined CPU, the next instruction must be fetched **before** the current branch
is resolved. Without prediction, the pipeline stalls at every branch (10-20% of all
instructions are branches).

```mermaid
flowchart TD
    subgraph NP["Without Prediction"]
        direction LR
        A1["B.EQ: F → D → E<br/>(branch resolved)"] --> A2["⏸ 3-cycle stall<br/>(pipeline flush)"] --> A3["Next instr: F → D → E"]
    end
    subgraph WP["With Prediction — correct"]
        direction LR
        B1["B.EQ: F → D → E<br/>(branch resolved, correct!)"] --> B2["Target: F → D → E → M → W<br/>(no cycles wasted)"]
    end
    style NP fill:#ffe0e0,stroke:#cc0000,color:#333
    style WP fill:#e0ffe0,stroke:#00aa00,color:#333
    style A1 fill:#ffcccc,stroke:#cc0000,color:#333
    style A2 fill:#ff6666,stroke:#cc0000,color:#fff
    style A3 fill:#ffcccc,stroke:#cc0000,color:#333
    style B1 fill:#ccffcc,stroke:#00aa00,color:#333
    style B2 fill:#66cc66,stroke:#00aa00,color:#fff
```

**Misprediction penalty**: 10-20 cycles on modern OoO cores. All speculatively
executed instructions after the mispredicted branch must be flushed.

---

## 2. Types of Branch Prediction

### 2.1 Direction Prediction — "Taken or Not Taken?"

Predicts whether a conditional branch will be taken or fall through.

#### Bimodal Predictor (1-level)

Branch History Table (BHT): Index using lower bits of PC. Each entry is a 2-bit saturating counter.

**States:** `00` = Strongly Not Taken | `01` = Weakly Not Taken | `10` = Weakly Taken | `11` = Strongly Taken

```mermaid
flowchart LR
    SNT["Strongly<br/>Not Taken<br/>(00)"] -- "Taken" --> WNT["Weakly<br/>Not Taken<br/>(01)"]
    WNT -- "Not Taken" --> SNT
    WNT -- "Taken" --> WT["Weakly<br/>Taken<br/>(10)"]
    WT -- "Not Taken" --> WNT
    WT -- "Taken" --> ST["Strongly<br/>Taken<br/>(11)"]
    ST -- "Not Taken" --> WT
    style SNT fill:#ff6666,stroke:#cc0000,color:#fff
    style WNT fill:#ffaa66,stroke:#cc6600,color:#fff
    style WT fill:#66aaff,stroke:#0055cc,color:#fff
    style ST fill:#66cc66,stroke:#008800,color:#fff
```

> **Example:** Loop with 100 iterations — First: predict Not Taken (wrong on iteration 1), then predict Taken (correct for 99 iterations), last: predict Taken (wrong at loop exit). **Accuracy: 98/100 = 98%**

#### Correlating / Two-Level Predictor

Uses **global branch history** to predict based on patterns:

Global History Register (GHR): records last N branch outcomes (e.g., GHR = `10110` → T, NT, T, T, NT)

Pattern History Table (PHT): indexed by GHR (GHR `10110` → 2-bit counter for this specific pattern)

```mermaid
flowchart TD
    PC["🔹 PC<br/>(Branch Address)"] --> HASH["XOR / Hash"]
    GHR["🔹 GHR<br/>(Global History Register)"] --> HASH
    HASH --> IDX["PHT Index"]
    IDX --> PHT["📋 PHT<br/>(Pattern History Table)<br/>2-bit counters"]
    PHT --> PRED["Prediction:<br/>Taken / Not Taken"]
    style PC fill:#4a90d9,stroke:#2c5f8a,color:#fff
    style GHR fill:#4a90d9,stroke:#2c5f8a,color:#fff
    style HASH fill:#f5a623,stroke:#c47d0e,color:#fff
    style IDX fill:#9b59b6,stroke:#7d3c98,color:#fff
    style PHT fill:#2ecc71,stroke:#1a9c4e,color:#fff
    style PRED fill:#e74c3c,stroke:#c0392b,color:#fff
```

#### TAGE Predictor (Tagged Geometric History Length)

State-of-the-art predictor used in modern ARM cores:

```mermaid
flowchart TD
    PC["PC + Branch History"] --> T1 & T2 & T3 & T4
    subgraph TAGE["TAGE Predictor — Multiple History Lengths"]
        T1["📗 Table T1<br/>History: 1-2 branches<br/>Short context"]
        T2["📘 Table T2<br/>History: 4-8 branches<br/>Medium context"]
        T3["📙 Table T3<br/>History: 16-32 branches<br/>Long context"]
        T4["📕 Table T4<br/>History: 64-128+ branches<br/>Very long context"]
    end
    T1 & T2 & T3 & T4 --> SEL["🏆 Longest Matching<br/>History Wins"]
    SEL --> PRED["Prediction<br/>>95% accuracy"]
    style TAGE fill:#f0f0ff,stroke:#6666cc,color:#333
    style T1 fill:#66cc66,stroke:#339933,color:#fff
    style T2 fill:#4a90d9,stroke:#2c5f8a,color:#fff
    style T3 fill:#f5a623,stroke:#c47d0e,color:#fff
    style T4 fill:#e74c3c,stroke:#c0392b,color:#fff
    style SEL fill:#9b59b6,stroke:#7d3c98,color:#fff
    style PRED fill:#2ecc71,stroke:#1a9c4e,color:#fff
    style PC fill:#34495e,stroke:#2c3e50,color:#fff
```

> Each entry has a **TAG** to verify the match. Longest matching history wins — more context = better prediction. Achieves **>95% accuracy** on most workloads.

---

### 2.2 Target Prediction — "Where does it go?"

For indirect branches (BR X0, BLR X0), the target address is in a register
and must be predicted.

#### Branch Target Buffer (BTB)

**BTB:** Cache mapping branch PC → target address (Typical sizes: 2K - 8K entries)

| Branch PC | Target Address | Type |
|-----------|----------------|------|
| 0x400100  | 0x400500       | B    |
| 0x400200  | 0x401000       | BL   |
| 0x400300  | varies         | BR   |

```mermaid
flowchart LR
    F["1️⃣ Fetch Stage<br/>Look up PC in BTB"] --> HIT{"BTB Hit?"}
    HIT -- "Yes" --> REDIR["2️⃣ Redirect fetch<br/>to predicted target"]
    HIT -- "No" --> SEQ["3️⃣ Assume sequential<br/>(fall-through)"]
    REDIR --> RES["4️⃣ Branch resolves<br/>Update BTB"]
    SEQ --> RES
    style F fill:#4a90d9,stroke:#2c5f8a,color:#fff
    style HIT fill:#f5a623,stroke:#c47d0e,color:#fff
    style REDIR fill:#2ecc71,stroke:#1a9c4e,color:#fff
    style SEQ fill:#e74c3c,stroke:#c0392b,color:#fff
    style RES fill:#9b59b6,stroke:#7d3c98,color:#fff
```

#### Indirect Branch Predictor

For polymorphic dispatch (virtual function calls), targets change:

```mermaid
flowchart LR
    BLR["BLR X0<br/>(vtable dispatch)<br/>X0 varies each time"] --> HASH["hash(PC, GHR)"]
    HASH --> IDX["Index"]
    IDX --> ITT["📋 Indirect Target Table<br/>index → target address"]
    ITT --> TGT["Predicted Target"]
    style BLR fill:#e74c3c,stroke:#c0392b,color:#fff
    style HASH fill:#f5a623,stroke:#c47d0e,color:#fff
    style IDX fill:#9b59b6,stroke:#7d3c98,color:#fff
    style ITT fill:#4a90d9,stroke:#2c5f8a,color:#fff
    style TGT fill:#2ecc71,stroke:#1a9c4e,color:#fff
```

---

### 2.3 Return Address Stack (RAS)

Function returns are highly predictable — they go back to where BL was called from.

```mermaid
flowchart TD
    subgraph OPS["BL/RET Operations"]
        direction LR
        BL1["BL func_A<br/>→ Push PC+4 onto RAS"] --> BL2["BL func_B<br/>→ Push return addr"]
        BL2 --> RET1["RET<br/>→ Pop → return to func_A"]
        RET1 --> RET2["RET<br/>→ Pop → return to caller"]
    end
    subgraph RAS["📚 Return Address Stack"]
        direction TB
        TOS["⬆ TOS: 0x400108 (func_A)"]
        E2["0x400204 (caller)"]
        E3["0x400300 (main)"]
        E4["..."]
        TOS --- E2 --- E3 --- E4
    end
    OPS --> RAS
    RAS --> INFO["Depth: 16-32 entries | Accuracy: ~99%+"]
    style OPS fill:#f0f0ff,stroke:#6666cc,color:#333
    style RAS fill:#e8f5e9,stroke:#4caf50,color:#333
    style TOS fill:#4a90d9,stroke:#2c5f8a,color:#fff
    style E2 fill:#66aaff,stroke:#3377cc,color:#fff
    style E3 fill:#88bbff,stroke:#5599dd,color:#fff
    style E4 fill:#aaccff,stroke:#77aaee,color:#333
    style BL1 fill:#2ecc71,stroke:#1a9c4e,color:#fff
    style BL2 fill:#2ecc71,stroke:#1a9c4e,color:#fff
    style RET1 fill:#e74c3c,stroke:#c0392b,color:#fff
    style RET2 fill:#e74c3c,stroke:#c0392b,color:#fff
    style INFO fill:#fff3cd,stroke:#ffc107,color:#333
```

---

## 3. Speculative Execution

When a branch is predicted, the CPU **speculatively executes** instructions along
the predicted path before knowing if the prediction is correct.

```mermaid
flowchart TD
    P["1️⃣ Predict branch<br/>direction + target"] --> F["2️⃣ Fetch & execute<br/>speculatively"]
    F --> R{"3️⃣ Branch<br/>Resolves"}
    R -- "✅ Correct" --> C["Commit results<br/>No penalty"]
    R -- "❌ Wrong" --> FL["Flush pipeline + ROB<br/>Restart from correct path<br/>~15-20 cycle penalty"]
    subgraph IS["Instruction Stream Example"]
        I1["CMP X0, #0"]
        I2["B.EQ skip<br/>(Predicted TAKEN)"]
        I3["ADD X1, X2, X3<br/>❌ NOT fetched"]
        I4["skip: SUB X4, X5, X6<br/>⚡ Speculative"]
        I5["MUL X7, X8, X9<br/>⚡ Speculative"]
        I1 --> I2
        I2 -. "predicted taken" .-> I4
        I4 --> I5
        I2 -. "skipped" .-> I3
    end
    style P fill:#4a90d9,stroke:#2c5f8a,color:#fff
    style F fill:#f5a623,stroke:#c47d0e,color:#fff
    style R fill:#9b59b6,stroke:#7d3c98,color:#fff
    style C fill:#2ecc71,stroke:#1a9c4e,color:#fff
    style FL fill:#e74c3c,stroke:#c0392b,color:#fff
    style IS fill:#f9f9f9,stroke:#999,color:#333
    style I1 fill:#ddd,stroke:#999,color:#333
    style I2 fill:#ffe0b2,stroke:#f5a623,color:#333
    style I3 fill:#ffcdd2,stroke:#e74c3c,color:#333
    style I4 fill:#b3e5fc,stroke:#4a90d9,color:#333
    style I5 fill:#b3e5fc,stroke:#4a90d9,color:#333
```

### Security Implications: Spectre

Speculative execution can leak data through side channels:

```
  Spectre attack concept:
  1. Mistrain branch predictor
  2. Cause speculative access to secret data
  3. Secret data affects cache state
  4. Attacker observes cache timing to extract secret

  ARM mitigations:
  • SSBS (Speculative Store Bypass Safe) — PSTATE.SSBS
  • CSV2 (Cache Speculation Variant 2) — hardware fixes
  • BTI (Branch Target Identification) — restrict branch targets
  • Firmware patches (SMCCC workarounds)
```

---

## 4. Loop Prediction

Specialized prediction for loops:

```
  Loop Predictor:
  • Detects backward branches that form loops
  • Counts iterations to predict loop exit
  • Much more accurate than general predictor for loops

  Example:
    MOV X0, #100
  loop:
    SUBS X0, X0, #1
    B.NE loop            ← Loop predictor tracks this
    
    After a few iterations, predictor learns:
    → Predict TAKEN for ~99 iterations
    → Predict NOT TAKEN on the 100th
```

---

## 5. Branch Prediction in ARM Cores

```
┌──────────┬─────────────────────────────────────────────────┐
│  Core    │  Branch Prediction Details                       │
├──────────┼─────────────────────────────────────────────────┤
│ A53      │ Conditional: 2-level + loop detector            │
│          │ BTB: ~256 entries                                │
│          │ RAS: 8 entries                                   │
│          │ Misprediction penalty: ~8 cycles                 │
├──────────┼─────────────────────────────────────────────────┤
│ A55      │ Improved bimodal + loop predictor               │
│          │ BTB: 256-512 entries                             │
│          │ RAS: 16 entries                                  │
│          │ Misprediction penalty: ~8 cycles                 │
├──────────┼─────────────────────────────────────────────────┤
│ A72      │ Multi-level TAGE-like predictor                 │
│          │ BTB: 2K-4K entries                               │
│          │ RAS: 16 entries                                  │
│          │ Misprediction penalty: ~15 cycles                │
├──────────┼─────────────────────────────────────────────────┤
│ A78      │ Advanced TAGE + indirect predictor              │
│          │ BTB: 4K-8K entries                               │
│          │ RAS: 32 entries                                  │
│          │ Misprediction penalty: ~11 cycles                │
├──────────┼─────────────────────────────────────────────────┤
│ X4       │ TAGE + multi-component indirect predictor       │
│          │ BTB: 12K+ entries, multi-level                   │
│          │ RAS: 32+ entries                                 │
│          │ Misprediction penalty: ~13 cycles                │
└──────────┴─────────────────────────────────────────────────┘
```

---

## 6. BTI — Branch Target Identification (ARMv8.5)

A security feature that restricts indirect branch targets:

```
  Without BTI:
    BLR X0    → Can jump to ANY instruction
    BR  X0    → Can jump to ANY instruction
    → Attacker can redirect control flow (JOP/ROP attacks)

  With BTI enabled (SCTLR_EL1.BT = 1):
    BLR X0    → Target MUST start with BTI C or BTI JC
    BR  X0    → Target MUST start with BTI J or BTI JC
    B   label → No restriction (direct branch)
    
    If target doesn't have correct BTI → Branch Target Exception

  BTI instruction variants:
    BTI C    — Valid target for BLR (call)
    BTI J    — Valid target for BR (jump)
    BTI JC   — Valid target for both
    BTI      — Valid target for neither (compatibility NOP on non-BTI)
```

---

Next: Back to [CPU Subsystem Overview](./README.md) | Continue to [Memory Subsystem →](../02_Memory_Subsystem/)
