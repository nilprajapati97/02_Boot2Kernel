# 05.03 — Cache Maintenance Operations (DC, IC)

> **ARM ARM Reference**: §D5.11.5–D5.11.7

---

## 1. Op Naming Convention

`<DC|IC> <action><scope>[, <reg>]`

| Action | Meaning |
|---|---|
| `I` | Invalidate |
| `C` | Clean |
| `CI` | Clean and Invalidate |
| `Z` | Zero (data cache) |

| Scope (for DC) | Meaning |
|---|---|
| `VAC` | by VA, to PoC |
| `VAU` | by VA, to PoU |
| `VAP` | by VA, to PoP (Persistence — FEAT_DPB) |
| `SW` | by Set/Way |
| `ZVA` | Zero, by VA |

| Scope (for IC) | Meaning |
|---|---|
| `IVAU` | Invalidate by VA, to PoU |
| `IALLU` | Invalidate all, this PE |
| `IALLUIS` | Invalidate all, Inner Shareable |

---

## 2. The Full DC Operation Table

| Instruction | Effect |
|---|---|
| `DC IVAC, Xt`  | Invalidate D-cache line at VA, to PoC |
| `DC ISW, Xt`   | Invalidate D-cache by set/way (local) |
| `DC CSW, Xt`   | Clean by set/way |
| `DC CISW, Xt`  | Clean+Invalidate by set/way |
| `DC CVAC, Xt`  | Clean by VA to PoC |
| `DC CVAU, Xt`  | Clean by VA to PoU |
| `DC CIVAC, Xt` | Clean+Invalidate by VA to PoC |
| `DC ZVA, Xt`   | Zero a block (size from DCZID_EL0.BS) |
| `DC CVAP, Xt`  | Clean by VA to Point of Persistence (FEAT_DPB) |
| `DC CVADP, Xt` | Clean by VA to Point of Deep Persistence (FEAT_DPB2) |

---

## 3. The Full IC Operation Table

| Instruction | Effect |
|---|---|
| `IC IALLU`         | Invalidate all I-cache (local PE) |
| `IC IALLUIS`       | Invalidate all I-cache (Inner Shareable) |
| `IC IVAU, Xt`      | Invalidate I-cache by VA, to PoU (broadcast IS) |

---

## 4. Operand Encoding

For VA-based ops, Xt is a virtual address (any address within the line; granularity is one cache line = `CTR_EL0.DminLine` or `IminLine`).

For Set/Way ops, Xt encodes:

```
 Xt[63:32] : 0
 Xt[31:4]  : Way[B-1:0] | reserved | Set[A-1:0] | Level[2:0]
```

Set/Way ops are **per-PE only** and used at cache power-down/up sequences.

---

## 5. Required Sequences

### 5.1 Self-modifying code

```asm
    ; assume code written via VA in x_dst, length in x_len
1:  DC   CVAU, x_dst         ; clean D to PoU
    add  x_dst, x_dst, x_lsz ; advance one D-line
    subs x_len, x_len, x_lsz
    b.gt 1b
    DSB  ISH                 ; ensure cleans done

    ; invalidate I-cache for the region
    ...IC IVAU loop, stride = IminLine ...
    DSB  ISH
    ISB                      ; this PE flush pipeline
```

### 5.2 DMA out (CPU → device, non-coherent)

```asm
    ; clean each line in [buf, buf+len) to PoC
    DC   CVAC, x_buf ... loop
    DSB  OSH
    ; now hand to device
```

### 5.3 DMA in (device → CPU, non-coherent)

```asm
    ; before DMA: invalidate range
    DC   IVAC, x_buf ... loop   ; (or CIVAC to be safe if buffer might be partially dirty)
    DSB  OSH
    ; start DMA, wait for completion
    ; after DMA:
    DSB  OSH
    ; safe to read
```

### 5.4 Fast zero with DC ZVA

```asm
    mrs   x1, DCZID_EL0
    and   x1, x1, #0xF       ; block size in log2 words (e.g. 4 → 64 B)
    mov   x2, #1
    lsl   x_blk, x2, x1
    lsl   x_blk, x_blk, #2   ; bytes
1:  DC    ZVA, x_dst
    add   x_dst, x_dst, x_blk
    subs  x_len, x_len, x_blk
    b.gt  1b
```

`DC ZVA` is much faster than store loops for large zero-fills; Linux uses it in `clear_page` and similar.

---

## 6. EL0 Access

By default EL0 cannot issue cache maintenance ops; `SCTLR_EL1.UCI=1` enables EL0 access to `DC CVAU`, `DC CVAC`, `DC CIVAC`, `DC CVAP`, `IC IVAU`. Used by JIT runtimes in user space.

---

## 7. Pitfalls

1. **Set/Way for coherency** — wrong; only architecturally for power management.
2. **Forgetting line granularity** — must loop over `CTR_EL0.DminLine`/`IminLine`, not assume 64 B.
3. **Using `DC CVAU` for DMA** — only cleans to PoU, may leave dirty data above PoC.
4. **Skipping DSB between op and consumer** — op is asynchronous w.r.t. pipeline.
5. **`DC ZVA` on Device memory** — `UNPREDICTABLE`.
6. **`IC IVAU` only on local PE** — wrong; it broadcasts.

---

## 8. Interview Q&A

**Q1. Difference between DC CVAU and DC CVAC?**
`CVAU` cleans to PoU (sufficient for I-cache coherency on the same PE). `CVAC` cleans to PoC (needed for non-coherent DMA / cross-observer visibility).

**Q2. Why are set/way ops not used for coherency?**
They're per-PE, not broadcast; running CPUs can re-allocate lines mid-sequence; race conditions. Architecturally meant for power-down only.

**Q3. How does Linux handle non-coherent DMA on arm64?**
`arch_sync_dma_for_device` issues `DC CVAC` (clean to PoC) for outbound; `arch_sync_dma_for_cpu` issues `DC IVAC` (invalidate) for inbound.

**Q4. What's DC ZVA and when use it?**
Zeroes a block whose size = `1 << (DCZID_EL0.BS + 2)` bytes. Faster than store loops; used in clear_page.

**Q5. Why is line size read from CTR_EL0 instead of hard-coded?**
ARM doesn't fix line size; varies across implementations (32, 64, 128 bytes). Future-proofing.

**Q6. Cache maintenance + barriers — what's the rule?**
Always `DSB <scope>` after maintenance; add `ISB` if instruction-cache or self-modification is involved.

---

## 9. Cross-refs

- [02 PoU/PoC](02_PoU_PoC_Inner_Outer.md)
- [04 Coherency](04_Cache_Coherency_MESI_MOESI.md)
- [06.01 DMB/DSB/ISB](../06_Memory_Barriers_Ordering/01_DMB_DSB_ISB.md)
