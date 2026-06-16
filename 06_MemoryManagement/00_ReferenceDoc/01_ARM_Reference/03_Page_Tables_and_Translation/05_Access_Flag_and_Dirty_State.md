# 03.05 — Access Flag and Dirty-Bit Management

> **ARM ARM Reference**: §D5.4.6 — *Hardware management of the Access flag and dirty state* (FEAT_HAFDBS)

---

## 1. The Access Flag (AF)

PTE bit[10]. Meaning:
- `AF=1`: page has been accessed since the bit was last cleared.
- `AF=0`: page has not been accessed → first access faults (Access Flag fault) unless HW updates AF.

Software uses AF to drive **page-replacement** policies (LRU approximation).

### Two modes:

| Mode | Set by | First access with `AF=0` |
|---|---|---|
| **SW-managed AF** | Kernel must set AF when faulting | Access Flag fault delivered |
| **HW-managed AF** (FEAT_HAFDBS, `TCR.HA=1`) | Hardware sets AF atomically | No fault — HW writes AF |

---

## 2. The Dirty-Bit Modifier (DBM)

There is **no architectural "dirty" bit**; instead ARMv8.1 introduces `DBM` (PTE bit[51]) and reuses `AP[2]` for dirty tracking.

The encoding:

| AP[2] (write-perm bit) | DBM | Meaning |
|---|---|---|
| 0 (writable) | 0 | clean — first write does **not** fault |
| 1 (read-only) | 1 | **clean writable candidate** — first write triggers HW update of AP[2]→0 (now dirty) |
| 0 | 1 | **dirty** (already had a write) |
| 1 | 0 | true read-only |

So a "writable but not yet dirty" mapping is marked `AP[2]=1, DBM=1`. On first write:
- **HW mode** (`TCR.HD=1`): HW atomically clears `AP[2]`. No fault.
- **SW mode**: a Permission fault occurs; kernel clears `AP[2]` and continues. The kernel knows the page is dirty.

---

## 3. Why This Encoding?

Pre-v8.1 there was no dirty-bit infrastructure. The DBM scheme cleverly:
- Repurposes the existing AP write-permission bit as the dirty indicator.
- Adds only one new bit (DBM) to PTE.
- Allows software-only emulation on older HW via fault-driven dirty tracking.

---

## 4. Enabling HW AF/DBM

| Field | Register | Meaning |
|---|---|---|
| `TCR_EL1.HA` | TCR | Enable HW Access flag update |
| `TCR_EL1.HD` | TCR | Enable HW Dirty bit update (requires HA) |
| `ID_AA64MMFR1_EL1.HAFDBS` | feature id | Indicates support level |

---

## 5. Diagram — first-write to a clean writable page

```mermaid
sequenceDiagram
    participant CPU
    participant TLB
    participant PTE
    Note over PTE: AP[2]=1 (RO) DBM=1 → "clean writable"
    CPU->>TLB: STR to page
    TLB-->>CPU: hit, but write-perm denies
    alt HW dirty mgmt
        CPU->>PTE: atomic CAS: AP[2] 1→0
        CPU->>TLB: invalidate stale entry
        CPU->>CPU: retry write
    else SW dirty mgmt
        CPU->>CPU: Permission fault
        CPU->>OS: kernel clears AP[2]; marks page dirty
        CPU->>CPU: retry
    end
```

---

## 6. Software Implications (Linux)

- **`pte_mkdirty()`** flips `AP[2]→0` and clears `DBM` (now plain RW).
- **`pte_mkclean()`** sets `AP[2]=1, DBM=1` — restores "clean writable" state.
- **`pte_young()` / `pte_mkold()`** manipulate AF.
- For COW (copy-on-write): page is marked truly RO (`AP[2]=1, DBM=0`); first write traps as a regular permission fault, kernel performs the copy.

---

## 7. Pitfalls

1. **Mixing `AP[2]=1, DBM=1` with COW intent** — HW will silently make it writable; you wanted to fault.
2. **HW AF on but TLB stale** — HW updates PTE in memory but a stale TLB entry persists; require IS broadcast TLBI.
3. **Concurrent HW DBM update vs software PTE rewrite** — needs atomic compare-and-exchange or BBM.
4. **Forgetting that HW AF update requires the PTE in cacheable, inner-shareable memory** — non-cacheable tables defeat HW management.

---

## 8. Interview Q&A

**Q1. What is the Access Flag?**
PTE bit[10] indicating the page has been accessed. Used by the OS for page reclaim heuristics.

**Q2. Does ARMv8 have a true dirty bit?**
No standalone bit. ARMv8.1 introduces DBM (PTE bit[51]) and overloads AP[2] to track dirty state.

**Q3. Encoding for "clean writable" — what bits?**
`AP[2]=1, DBM=1` — write-permission denies but DBM=1 tells HW to upgrade on first write.

**Q4. Difference between HW-managed and SW-managed AF/dirty?**
HW (`TCR.HA/HD=1`) updates the PTE in memory atomically; SW takes a fault per first access/write.

**Q5. Why might you prefer SW management?**
For precise per-page tracking (e.g., live migration's dirty-log mode), or on hardware lacking FEAT_HAFDBS.

**Q6. What state does Linux use for COW?**
True read-only (`AP[2]=1, DBM=0`) so write traps unconditionally; the handler does the copy.

**Q7. Is the HW PTE update visible to other cores immediately?**
It's an atomic memory write; coherency makes it visible. But stale TLBs in other cores still need invalidation if they have the old entry.

---

## 9. Cross-refs

- [01 Descriptor formats](01_Translation_Table_Format_Descriptors.md)
- [06 Permissions](06_Permission_Checks_AP_UXN_PXN.md)
- [08.01 Fault types](../08_Faults_and_Aborts/01_Translation_Permission_Alignment_Faults.md)
