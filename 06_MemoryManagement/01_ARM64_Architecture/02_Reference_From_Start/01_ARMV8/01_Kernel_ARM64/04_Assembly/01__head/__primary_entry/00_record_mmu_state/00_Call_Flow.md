## `record_mmu_state` — Explanation

### Purpose
Determine **whether the bootloader handed control to the kernel with the MMU and D-cache already ON**. The result is stored in `x19` and used throughout the entire boot path up to `start_kernel()`.

### What `x19` means on return
| `x19` value | Meaning |
|-------------|---------|
| `0` | MMU was OFF (or cache was OFF) — treat as cold entry |
| non-zero | MMU **and** D-cache were both ON at kernel entry |

---

### Two distinct paths inside the function

**Path A — Correct Endianness (normal case)**
The CPU endianness matches the kernel's expected endianness. The function simply checks whether the MMU and D-cache were both enabled, and encodes that answer into `x19`.

**Path B — Wrong Endianness (fix-up case)**
The CPU is running in the wrong byte order. Before anything else can work correctly, the function must flip the endianness bit in the SCTLR register and force the MMU off. Result is always `x19 = 0`.

---

### Call Sequence / Flow

```
record_mmu_state()
│
├── Read CurrentEL
│     ├── At EL2? → read SCTLR_EL2 into x19
│     └── At EL1? → read SCTLR_EL1 into x19
│
├── Check Endianness bit (EE) in x19
│   [LE kernel: EE should be 0]
│   [BE kernel: EE should be 1]
│
│   ┌─── Endianness CORRECT ──────────────────────────────────────┐
│   │                                                             │
│   │   Check D-Cache bit (C) in SCTLR                            │
│   │     └── Cache OFF?                                          │
│   │           YES → x19 = 0  (even if MMU was on,               │
│   │                            cache off = not safe to reuse)   │
│   │           NO  → isolate M (MMU enable) bit                  │
│   │                   M=1 → x19 = non-zero (MMU was ON)         │
│   │                   M=0 → x19 = 0        (MMU was OFF)        │
│   │                                                             │
│   │   ret  →  x19 = MMU+cache state                             │
│   └─────────────────────────────────────────────────────────────┘
│
│   ┌─── Endianness WRONG ────────────────────────────────────────┐
│   │                                                             │
│   │   Flip EE bit in SCTLR  (fix byte order)                    │
│   │   Clear M bit in SCTLR  (force MMU off)                     │
│   │                                                             │
│   │   Write corrected value back:                               │
│   │     At EL2? → write SCTLR_EL2                               │
│   │     At EL1? → write SCTLR_EL1                               │
│   │                                                             │
│   │   isb  (instruction sync barrier — changes take effect)     │
│   │   x19 = 0  (MMU is now definitively OFF)                    │
│   │                                                             │
│   │   ret  →  x19 = 0  (always)                                 │
│   └─────────────────────────────────────────────────────────────┘
```

---

### Why this matters downstream

```
record_mmu_state  →  x19 result used by:
│
├── preserve_boot_args
│     x19≠0 → skip dcache_inval_poc (cache coherent, no need)
│     x19=0 → run  dcache_inval_poc (flush stale lines)
│
├── primary_entry (cache maintenance branch)
│     x19=0  → invalidate page tables  (dcache_inval_poc)
│     x19≠0  → clean  idmap text       (dcache_clean_poc)
│
└── init_kernel_el
      x19 passed as x0 to indicate MMU state
      init_el2: if MMU was on, clean HYP code to PoC before disabling
```

### Key Design Insight
The function must handle **two separate concerns**:
1. **Endianness** — must be fixed *immediately*, before any memory access, because even reading `boot_args[]` with wrong byte order would corrupt data.
2. **MMU/cache state** — must be recorded so later code knows whether to *invalidate* (cold, MMU off) or *clean* (warm, MMU on) cache regions.You've used 65% of your weekly rate limit. Your weekly rate limit will reset on April 27 at 5:30 AM. [Learn More](https://aka.ms/github-copilot-rate-limit-error)
