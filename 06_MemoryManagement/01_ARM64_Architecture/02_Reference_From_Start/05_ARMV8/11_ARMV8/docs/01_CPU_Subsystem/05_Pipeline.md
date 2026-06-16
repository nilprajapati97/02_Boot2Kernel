# Pipeline Architecture

## 1. What is a Pipeline?

A **pipeline** breaks instruction execution into stages that operate in parallel,
like an assembly line. While one instruction is being executed, the next is being
decoded, and the one after that is being fetched.

**Without pipeline (sequential):**

```mermaid
flowchart LR
    F["Fetch"] --> D["Decode"] --> E["Execute"] --> M["Memory"] --> W["Writeback"]
    style F fill:#4a90d9,color:#fff,stroke:#2c5f8a
    style D fill:#50b86c,color:#fff,stroke:#2e7d42
    style E fill:#e8a838,color:#fff,stroke:#b07d1e
    style M fill:#e06060,color:#fff,stroke:#a03030
    style W fill:#9b59b6,color:#fff,stroke:#6c3483
```

**With 5-stage pipeline:**

| Cycle | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|-------|---|---|---|---|---|---|---|---|
| Instr 1 | F | D | E | M | W | | | |
| Instr 2 | | F | D | E | M | W | | |
| Instr 3 | | | F | D | E | M | W | |
| Instr 4 | | | | F | D | E | M | W |

> After pipeline fills, one instruction completes **EVERY** cycle!

---

## 2. In-Order vs Out-of-Order Pipelines

ARMv8 microarchitectures use both approaches:

### In-Order Pipeline (e.g., Cortex-A53, Cortex-A55)

**Cortex-A53: 8-stage in-order dual-issue pipeline**

```mermaid
flowchart LR
    F1["F1<br/>Fetch 1"] --> F2["F2<br/>Fetch 2"] --> D1["D1<br/>Decode 1"] --> D2["D2<br/>Decode 2"] --> ISS["ISS<br/>Issue"] --> EX["EX<br/>Execute"] --> WB["WB<br/>Writeback"] --> RT["RT<br/>Retire"]
    style F1 fill:#4a90d9,color:#fff,stroke:#2c5f8a
    style F2 fill:#5ba3e8,color:#fff,stroke:#3a72b5
    style D1 fill:#50b86c,color:#fff,stroke:#2e7d42
    style D2 fill:#66cc80,color:#fff,stroke:#3d9956
    style ISS fill:#e8a838,color:#fff,stroke:#b07d1e
    style EX fill:#e06060,color:#fff,stroke:#a03030
    style WB fill:#9b59b6,color:#fff,stroke:#6c3483
    style RT fill:#1abc9c,color:#fff,stroke:#148f77
```

**Properties:**
- Instructions execute in program order
- Dual-issue: up to 2 instructions per cycle
- Simple hardware, low power
- If instruction stalls (cache miss), everything behind it stalls too
- Used in: efficiency/LITTLE cores

### Out-of-Order (OoO) Pipeline (e.g., Cortex-A72, A78, X1-X4)

**Cortex-A78: ~13 stage out-of-order, 4-wide decode/dispatch**

```mermaid
flowchart TD
    subgraph FE["Front End - Instruction Fetch"]
        direction LR
        F1["F1"] --> F2["F2"] --> F3["F3"]
    end
    subgraph DE["Decode + Register Rename"]
        direction LR
        D1["D1"] --> D2["D2"] --> RN["RN"]
    end
    subgraph DI["Dispatch / Issue"]
        ROB["Reorder Buffer<br/>(ROB, ~160 entries)<br/>Tracks all in-flight instructions<br/>Maintains program order for commit"]
    end
    subgraph EU["Execution Units"]
        ALU["ALU0 / ALU1<br/>Integer: add, sub, logical, shift"]
        MULDIV["MUL / DIV<br/>Multiply / Divide"]
        FP["FP0 / FP1<br/>Floating-Point / NEON / SVE"]
        AGU["AGU0 / AGU1<br/>Address Generation (Load/Store)"]
        BR["BR<br/>Branch resolution"]
    end
    subgraph RC["Retire / Commit"]
        RET["In-order writeback from ROB head<br/>Results committed in program order"]
    end
    FE --> DE --> DI
    DI --> ALU & MULDIV & FP & AGU & BR
    ALU & MULDIV & FP & AGU & BR --> RC
    style F1 fill:#4a90d9,color:#fff,stroke:#2c5f8a
    style F2 fill:#4a90d9,color:#fff,stroke:#2c5f8a
    style F3 fill:#4a90d9,color:#fff,stroke:#2c5f8a
    style D1 fill:#50b86c,color:#fff,stroke:#2e7d42
    style D2 fill:#50b86c,color:#fff,stroke:#2e7d42
    style RN fill:#50b86c,color:#fff,stroke:#2e7d42
    style ROB fill:#e8a838,color:#fff,stroke:#b07d1e
    style ALU fill:#e06060,color:#fff,stroke:#a03030
    style MULDIV fill:#e06060,color:#fff,stroke:#a03030
    style FP fill:#e06060,color:#fff,stroke:#a03030
    style AGU fill:#e06060,color:#fff,stroke:#a03030
    style BR fill:#e06060,color:#fff,stroke:#a03030
    style RET fill:#9b59b6,color:#fff,stroke:#6c3483
```

**Properties:**
- Instructions can execute out of program order
- Register renaming eliminates false dependencies (WAR, WAW)
- ROB ensures correct in-order retirement
- Higher IPC (Instructions Per Cycle) but more power/area
- Used in: performance/big cores

---

## 3. Pipeline Stages Explained

### Stage 1-3: Instruction Fetch

**Process:**
1. Branch predictor supplies predicted PC
2. I-Cache lookup using predicted PC
3. Fetch up to 4-8 instructions per cycle (fetch width)
4. If I-Cache miss → stall, fetch from L2/L3

```mermaid
flowchart LR
    BP["Branch<br/>Predictor"] -->|PC| IC["I-Cache<br/>(L1I)"] --> FB["Fetch<br/>Buffer"]
    style BP fill:#4a90d9,color:#fff,stroke:#2c5f8a
    style IC fill:#50b86c,color:#fff,stroke:#2e7d42
    style FB fill:#e8a838,color:#fff,stroke:#b07d1e
```

### Stage 4-5: Decode & Rename

```
Decode:
  • Parse 32-bit instruction encoding
  • Determine operation type, source/destination registers
  • Handle macro-fusion (combine CMP + B.cond into one μop)

Register Rename:
  • Map architectural registers (X0-X30) to physical registers
  • Eliminates false dependencies:
    
    Program:              After Rename:
    ADD X1, X2, X3       ADD P10, P2, P3      ← True dependency
    SUB X1, X4, X5       SUB P11, P4, P5      ← Now independent!
    MUL X6, X1, X7       MUL P12, P11, P7     ← Uses renamed result

    Without rename: SUB must wait for ADD (WAW on X1)
    With rename: SUB can issue immediately (P10 ≠ P11)
```

### Stage 6: Dispatch / Issue

- Instructions placed into Issue Queues (reservation stations)
- When all operands are ready → issue to execution unit
- Can issue multiple instructions per cycle (width = IPC potential)
- OoO: instructions issued based on operand readiness, not order

**Issue Queue:**

| μop | Src1 Rdy | Src2 Rdy | Age | Status |
|-----|----------|----------|-----|--------|
| ADD | ✓ | ✓ | 3 | → Issue! (both ready) |
| MUL | ✓ | ✗ | 2 | Wait for Src2 |
| LDR | ✓ | — | 1 | → Issue! |

### Stage 7-9: Execute

**Execution latencies (typical):**

| Operation | Latency (cycles) |
|-----------|------------------|
| Integer ADD/SUB | 1 |
| Integer MUL | 3 |
| Integer DIV | 4-12 |
| FP ADD | 2-3 |
| FP MUL | 3-5 |
| FP DIV | 7-14 |
| L1 Cache hit | 4 |
| L2 Cache hit | ~12 |
| L3 Cache hit | ~30-40 |
| DRAM access | ~100-200 |

### Stage 10+: Retire / Commit

- Reorder Buffer (ROB) tracks all in-flight instructions
- Instructions retire in ORDER, even if executed out of order
- On retire: update architectural register state
- On exception: flush all younger instructions from ROB

**ROB (conceptual):**

| Seq | μop | Status | Result | Note |
|-----|---------|----------|--------|------|
| 1 | ADD X0 | Complete | 42 | ← HEAD (retire next) |
| 2 | LDR X1 | Complete | 0xFF | |
| 3 | MUL X2 | Pending | — | ← Can't retire yet |
| 4 | SUB X3 | Complete | 10 | ← Must wait for #3 |

---

## 4. Pipeline Hazards

### Data Hazards

```
RAW (Read After Write) — True dependency:
  ADD X1, X2, X3     // Writes X1
  SUB X4, X1, X5     // Reads X1 — must wait for ADD result
  → Solved by: forwarding/bypassing

WAW (Write After Write) — Output dependency:
  ADD X1, X2, X3     // Writes X1
  MUL X1, X4, X5     // Also writes X1
  → Solved by: register renaming

WAR (Write After Read) — Anti-dependency:
  ADD X4, X1, X5     // Reads X1
  SUB X1, X2, X3     // Writes X1
  → Solved by: register renaming
```

### Control Hazards

```
Branch instructions create uncertainty about what to fetch next:

  CMP X0, #0
  B.EQ skip            // Branch taken or not? Don't know until EX stage
  ADD X1, X2, X3       // Fetch this? Or...
  skip:
  SUB X1, X4, X5       // ...this?

  → Solved by: Branch Prediction (see next document)
  → Penalty on misprediction: flush pipeline (10-20 cycles wasted)
```

### Structural Hazards

```
Multiple instructions competing for same execution unit:
  MUL X0, X1, X2       // Needs multiplier
  MUL X3, X4, X5       // Also needs multiplier
  → Solved by: multiple execution units, or stalling
```

---

## 5. Comparison: ARM Core Microarchitectures

| | Cortex-A53 | Cortex-A55 | Cortex-A72 | Cortex-A78 | Cortex-X4 |
|----------|-----------|-----------|-----------|-----------|----------|
| Type | In-order | In-order | OoO | OoO | OoO |
| Pipeline | 8 stage | 8 stage | 15 stage | 13 stage | 14 stage |
| Decode | 2-wide | 2-wide | 3-wide | 4-wide | 10-wide |
| Issue | — | — | 5-wide | 8-wide | 16-wide |
| ROB size | — | — | 128 | ~160 | ~320+ |
| L1I | 8-32 KB | 16-64 KB | 48 KB | 64 KB | 64 KB |
| L1D | 8-32 KB | 16-64 KB | 32 KB | 64 KB | 64 KB |
| L2 | 128K-2M | 64-256K | 0.5-4 MB | 256-512K | 2 MB |
| Profile | LITTLE | LITTLE | big | big | X (peak) |

---

Next: [Branch Prediction →](./06_Branch_Prediction.md)
