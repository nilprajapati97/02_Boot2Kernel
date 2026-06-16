# 05.02 — Points of Unification & Coherency; Inner vs Outer

> **ARM ARM Reference**: §D5.11

---

## 1. Four Conceptual Points

| Concept | Definition |
|---|---|
| **LoU** — Level of Unification | The level where I-cache and D-cache of a single PE see the same copy |
| **LoUIS** — LoU for the Inner Shareable domain | The LoU shared across the inner-shareable group |
| **LoC** — Level of Coherency | The level at which **all observers** see the same copy of memory |
| **LoUU** — LoU for the local PE (unique) | The LoU only for this CPU |

`CLIDR_EL1` reports these as level numbers.

---

## 2. PoU vs PoC

- **PoU (Point of Unification)** — the level at which the I-cache and D-cache of a given PE see the same copy. Below this point, writes via the D-cache become visible to instruction fetches via the I-cache (after appropriate maintenance).
- **PoC (Point of Coherency)** — the level at which **all observers in the system** (CPUs, GPUs, DMA, coherent IO) see the same copy. Below the PoC, you have memory or a coherent fabric.

Cache maintenance ops target one of these:

| Op | Target | Use |
|---|---|---|
| `DC CVAU` | PoU | Make data visible to instr fetch (JIT, SMC) |
| `DC CVAC` | PoC | Make data visible to non-coherent DMA |
| `DC CIVAC` | PoC | + invalidate (read-side DMA) |
| `DC IVAC` | PoC | Invalidate only — prep buffer for incoming DMA |
| `IC IVAU` | I-cache | Invalidate instruction by VA (to PoU) |
| `IC IALLU` | I-cache | Invalidate all I-cache (this PE) |
| `IC IALLUIS` | I-cache | Invalidate all I-cache (IS domain) |

---

## 3. Inner vs Outer

Independent of points, ARM defines two cacheability **domains**:

| Domain | Typical mapping |
|---|---|
| **Inner** | L1 (and L2, often) |
| **Outer** | L3 / SLC / system cache |

The domain boundary is **IMPLEMENTATION DEFINED**. Software treats them as opaque tiers:
- A memory region can be Inner-Cacheable but Outer-Non-cacheable (rare).
- Common: Inner+Outer WB for RAM; Inner+Outer NC for some DMA buffers.

Caches in the Inner domain participate in **Inner-Shareable coherency** at minimum; Outer-domain caches participate in Outer-Shareable.

---

## 4. Diagram — Points and Domains

```
┌────────────────────────────────────────┐
│  CPU pipeline                          │
├────────────────────────────────────────┤
│  L1-I    L1-D     ← Inner              │
├────────────────────────────────────────┤
│  L2 (private)    ← Inner (often)       │
├──────────── PoU here (typical) ────────┤   ← I and D agree at this point
│  L3 / SLC        ← Outer               │
├──────────── PoC here (typical) ────────┤   ← all observers agree
│  DRAM                                  │
└────────────────────────────────────────┘
```

---

## 5. Why It Matters

### Self-modifying code (JIT)
Writes go through D-cache. Without action, I-cache fetches still hit stale lines. Sequence:

```asm
    ; W → data, then make executable
    DC   CVAU, x_dst      ; clean D to PoU
    DSB  ISH
    IC   IVAU, x_dst      ; invalidate I-cache line in IS domain
    DSB  ISH
    ISB
```

### Non-coherent DMA
Outbound (CPU → device): writes might still be in L1/L2. Clean to PoC:

```asm
    DC   CVAC, x_buf      ; clean each line
    DSB  OSH
    ; now poke doorbell
```

Inbound (device → CPU): device wrote DRAM directly; CPU caches may have stale lines. Invalidate:

```asm
    DC   IVAC, x_buf      ; invalidate before reading
    DSB  OSH
```

On **IO-coherent** systems, the SLC sits at the PoC and snoops CPU caches — these maintenance ops are unnecessary.

---

## 6. Pitfalls

1. **Confusing PoU and PoC** — using `DC CVAU` for DMA leaves data in the SLC, not visible to non-coherent device.
2. **`IC IALLUIS` is broadcast** but `IC IALLU` is local — don't mix up on SMP.
3. **Set/way ops** (`DC ISW/CSW/CISW`) are **only** for PE-local cache management at power-down. Never use for coherency on running system.
4. **Forgetting `DSB ISH` / `DSB OSH`** between maintenance and consumer.
5. **`DC ZVA` block size** — read `DCZID_EL0.BS` (in words). Hard-coding 64 may be wrong on future parts.

---

## 7. Interview Q&A

**Q1. PoU vs PoC?**
PoU — I and D agree on one PE. PoC — all observers in the system agree.

**Q2. Why does JIT need `DC CVAU` not `DC CVAC`?**
We only need consistency with the I-cache of this PE; cleaning to PoU is cheaper than to PoC.

**Q3. When is `DC CVAC` needed in modern arm64?**
For non-IO-coherent SoCs talking to DMA; modern server/SoC silicon is usually IO-coherent and skips this.

**Q4. What does `IC IVAU` do?**
Invalidates an instruction-cache line by VA, broadcast to the Inner Shareable domain (the "U" means "Unification point").

**Q5. What's the difference between `DC IVAC` and `DC CIVAC`?**
`IVAC` invalidates only (discards). `CIVAC` cleans then invalidates (preserves dirty data).

**Q6. When are set/way ops appropriate?**
Powering down a CPU cluster — flush local caches. Never for coherency on a running system.

**Q7. What does `DCZID_EL0.BS` indicate?**
The block size used by `DC ZVA` instruction (in log2 words). Read at runtime.

---

## 8. Cross-refs

- [01 Cache hierarchy](01_Cache_Hierarchy_L1_L2_L3.md)
- [03 Maintenance ops](03_Cache_Maintenance_Ops_DC_IC.md)
- [04 Coherency](04_Cache_Coherency_MESI_MOESI.md)
