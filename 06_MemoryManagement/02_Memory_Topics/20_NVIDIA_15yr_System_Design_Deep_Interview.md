# 20 — NVIDIA: 15-Year Experience System Design Deep Interview — Memory Management

> **Target**: Principal/Staff/Distinguished Engineer interviews at NVIDIA (GPU Systems, CUDA, Drive, Tegra, DGX)
> **Level**: 15+ years — You are expected to design subsystems, debug silicon-level issues, and articulate tradeoffs for production GPU systems.

---

## 📌 Interview Focus Areas

| Domain | What NVIDIA Expects at 15yr Level |
|--------|----------------------------------|
| **UVM (Unified Virtual Memory)** | Design the page fault handler, migration policy, prefetch heuristics |
| **Multi-GPU Memory Coherence** | NVLink/NVSwitch memory model, XBAR coherency, ATS/PRI |
| **GPU IOMMU/SMMU Integration** | End-to-end DMA path, ATS (Address Translation Services), PASID |
| **DGX Memory Topology** | NUMA-aware GPU allocation, HBM bandwidth, NVSwitch routing decisions |
| **CUDA Memory Hierarchy** | Shared memory bank conflicts, L2 persistence, memory coalescing |
| **TLB Hierarchy & Shootdown** | GPU TLB design, GMMU invalidation cost, TLB coverage for HBM |
| **Memory Overcommit & ECC** | GPU memory oversubscription, ECC error handling, page retirement |
| **PCIe BAR & Resizable BAR** | BAR sizing strategy, SAR (System Address Remapping), ACS |

---

## 🎨 System Design 1: Design UVM Page Migration Policy for Multi-GPU DGX

### Context
A DGX H100 has 8×H100 GPUs, each with 80GB HBM3, connected via NVSwitch. A single CUDA application uses Unified Memory (`cudaMallocManaged`) spanning all GPUs. You must design the page migration policy engine.

### Design Diagram

```mermaid
flowchart TD
    subgraph App["📱 CUDA Application"]
        UP["Unified Pointer: 0x7f000000<br/>cudaMallocManaged(512GB)"]
    end
    
    subgraph UVM["🔵 UVM Driver (kernel module)"]
        FH["Page Fault Handler<br/>uvm_gpu_fault_entry()"]
        MP["Migration Policy Engine"]
        PF["Prefetch Engine<br/>Predictive migration"]
        AC["Access Counter Module<br/>Track per-GPU access frequency"]
    end
    
    subgraph GPUs["🟢 8× H100 GPUs"]
        G0["GPU 0: HBM 80GB<br/>Current resident: Pages A,B,C"]
        G1["GPU 1: HBM 80GB<br/>Current resident: Pages D,E,F"]
        G7["GPU 7: HBM 80GB"]
    end
    
    subgraph NVS["🟠 NVSwitch Fabric"]
        SW["NVSwitch 3.0<br/>900 GB/s per GPU<br/>All-to-all connectivity"]
    end
    
    subgraph CPU_MEM["🔴 CPU System Memory"]
        SYS["2TB DDR5<br/>Overflow / cold pages"]
    end
    
    UP --> FH
    FH --> MP
    MP -->|"Migrate page"| SW
    MP -->|"Remote map<br/>(no migration)"| SW
    MP -->|"Evict cold page"| CPU_MEM
    AC -->|"Access frequency"| MP
    PF -->|"Predictive"| MP
    SW --> G0
    SW --> G1
    SW --> G7

    style UP fill:#3498db,stroke:#2980b9,color:#fff
    style FH fill:#e74c3c,stroke:#c0392b,color:#fff
    style MP fill:#9b59b6,stroke:#8e44ad,color:#fff
    style AC fill:#f1c40f,stroke:#f39c12,color:#000
    style PF fill:#f39c12,stroke:#e67e22,color:#fff
    style SW fill:#e67e22,stroke:#d35400,color:#fff
    style SYS fill:#c0392b,stroke:#922b21,color:#fff
```

### Migration Decision Sequence

```mermaid
sequenceDiagram
    participant G2 as GPU 2<br/>[Faulting GPU]
    participant UVM as UVM Driver
    participant AC as Access Counters
    participant G0 as GPU 0<br/>[Page Owner]
    participant NVS as NVSwitch

    G2->>UVM: GPU fault: VA=0x7f001000<br/>Page owned by GPU 0
    
    UVM->>AC: Query: Access pattern for this page?
    AC->>UVM: GPU 0: 50 accesses/ms<br/>GPU 2: 200 accesses/ms<br/>GPU 5: 5 accesses/ms
    
    Note over UVM: 🧠 Migration Policy Decision
    
    alt Policy: MIGRATE (GPU 2 has highest access rate)
        UVM->>G0: Unmap page from GPU 0's GMMU
        UVM->>G0: Invalidate GPU 0 TLB for this VA
        UVM->>NVS: DMA: GPU 0 HBM → GPU 2 HBM (4KB)
        NVS->>G2: Page data arrives
        UVM->>G2: Map in GPU 2's GMMU (RW)
        UVM->>G0: Map remote read-only via NVLink
        G2->>G2: Replay faulting instruction ✅
    else Policy: REMOTE MAP (shared read pattern)
        UVM->>G2: Map VA → GPU 0's HBM via NVLink peer
        Note over G2: Access via NVLink (lower BW but no migration cost)
        G2->>G2: Replay instruction (accesses GPU 0 memory remotely) ✅
    else Policy: READ-DUPLICATE (read-heavy, multi-GPU)
        UVM->>NVS: Copy page GPU 0 → GPU 2
        UVM->>G2: Map as read-only local copy
        UVM->>G0: Keep original as read-only
        Note over UVM: Write to any copy → invalidate others → migrate
    end
```

### Deep Q&A

---

#### ❓ Q1: Design the access counter mechanism for UVM migration decisions. How would you avoid thrashing between GPUs?

**A:** The access counter system must solve: "Which GPU should own this page?"

**Hardware Component — GPU Access Counters:**
```
Each GPU has per-VA-range access counters in the GMMU:
- MOMC (MIMC) counters: Track accesses from local (remote) SM
- Counter granularity: configurable (64KB, 2MB, etc.)
- When counter exceeds threshold → generate interrupt to UVM driver
```

**Software Policy Engine:**
```c
struct uvm_page_migration_state {
    u64 va;
    u32 owner_gpu;                    /* Current page owner */
    u32 access_count[MAX_GPUS];       /* Per-GPU access frequency */
    ktime_t last_migration;           /* When was this page last moved */
    u32 migration_count;              /* Total migrations for this page */
    enum { STABLE, BOUNCING, NEW } state;
};

int uvm_should_migrate(struct uvm_page_migration_state *pms, 
                        int faulting_gpu)
{
    /* Anti-thrashing: exponential backoff */
    if (pms->state == BOUNCING) {
        ktime_t cooldown = ktime_set(0, 
            min(100 * NSEC_PER_MSEC * (1 << pms->migration_count), 
                10 * NSEC_PER_SEC));
        if (ktime_before(ktime_get(), 
                         ktime_add(pms->last_migration, cooldown)))
            return POLICY_REMOTE_MAP;  /* Don't migrate — too recent */
    }
    
    /* Majority access rule: migrate only if faulting GPU 
     * has 2x more accesses than current owner */
    if (pms->access_count[faulting_gpu] > 
        2 * pms->access_count[pms->owner_gpu]) {
        
        if (pms->migration_count > 5)
            pms->state = BOUNCING;
        
        pms->migration_count++;
        pms->last_migration = ktime_get();
        return POLICY_MIGRATE;
    }
    
    return POLICY_REMOTE_MAP;
}
```

**Anti-thrashing strategies:**
1. **Hysteresis**: Only migrate if access count exceeds 2× owner's count
2. **Cooldown timer**: After migration, prevent re-migration for exponentially increasing time
3. **Read-duplication**: For read-shared pages, replicate instead of migrating
4. **Pinning**: After N thrashes, pin the page to the "center of gravity" GPU (weighted by access counts)
5. **Batch migration**: Group nearby pages → amortize TLB invalidation cost

---

#### ❓ Q2: An H100 GPU has a 48-bit virtual address space with a 2-level GMMU. A workload with 500M page table entries causes severe GPU TLB pressure. How do you solve this?

**A:** This is a real production problem in LLM training with large KV caches.

**Problem Analysis:**
```
H100 GMMU TLB hierarchy:
- L1 TLB: ~512 entries per SM (132 SMs) — covers ~2MB per SM (4KB pages)
- L2 TLB: ~8192 entries shared — covers ~32MB
- Page Table Walker (PTW) cache: partial PDE caching

500M PTEs × 4KB pages = 2TB virtual mapping
L2 TLB coverage: 32MB → 0.0015% hit rate if random access → CATASTROPHIC
```

**Solution Architecture:**

```mermaid
flowchart TD
    A["🔴 Problem: 500M PTEs on 4KB pages<br/>TLB coverage = 32MB / 2TB = 0.0015%"] --> B{Solutions}
    
    B --> C["1️⃣ Large Pages (2MB)"]
    C --> C1["500M × 4KB = ~1M × 2MB pages<br/>L2 TLB coverage: 8192 × 2MB = 16GB<br/>Hit rate: 16GB/2TB = 0.8%<br/>Still not great"]
    
    B --> D["2️⃣ Huge Pages (512MB)"]
    D --> D1["2TB / 512MB = 4096 entries<br/>L2 TLB covers ALL!<br/>100% hit rate ✅"]
    
    B --> E["3️⃣ ATS (Address Translation Services)"]
    E --> E1["GPU uses CPU's page tables<br/>via PCIe ATS protocol<br/>CPU IOMMU does translation<br/>GPU TLB caches ATS results"]
    
    B --> F["4️⃣ Application-level redesign"]
    F --> F1["Tile data access patterns<br/>Sequential access within 2MB blocks<br/>Prefetch adjacent pages"]

    style A fill:#e74c3c,stroke:#c0392b,color:#fff
    style C1 fill:#f39c12,stroke:#e67e22,color:#fff
    style D1 fill:#2ecc71,stroke:#27ae60,color:#fff
    style E1 fill:#3498db,stroke:#2980b9,color:#fff
    style F1 fill:#9b59b6,stroke:#8e44ad,color:#fff
```

**Implementation — Huge Page Promotion in UVM:**
```c
/* UVM big page allocation — drivers/gpu/drm/nouveau/nvkm/subdev/mmu/ */
int uvm_promote_to_huge_page(struct uvm_va_range *range, u64 va)
{
    /* Check: are all 128 × 4KB pages in this 512KB region 
     * owned by the same GPU? */
    int owner = get_page_owner(va & ~(SZ_512K - 1));
    for (int i = 0; i < 128; i++) {
        if (get_page_owner(va + i * PAGE_SIZE) != owner)
            return -EAGAIN;  /* Mixed ownership — can't promote */
    }
    
    /* Promote: replace 128 PTEs with 1 big PTE */
    uvm_unmap_range(range, va & ~(SZ_512K - 1), SZ_512K);
    uvm_map_big_page(range, va & ~(SZ_512K - 1), SZ_512K, owner);
    
    /* Invalidate old TLB entries */
    uvm_tlb_batch_invalidate(owner, va & ~(SZ_512K - 1), SZ_512K);
    
    return 0;
}
```

**NVIDIA-specific considerations:**
- H100 GMMU supports 4KB, 64KB, and 2MB page sizes
- `cuMemAllocAsync` with pools → reduces page table churn
- For LLM: KV cache should use 2MB pages, model weights should use largest possible

---

#### ❓ Q3: Design the ECC error handling and page retirement flow for HBM on a DGX system running 24/7 AI training.

**A:** This is critical for DGX reliability. A single uncorrected ECC error can corrupt a multi-week training run.

```mermaid
flowchart TD
    subgraph HBM["🔧 HBM3 Memory"]
        BIT["Bit error in HBM cell"]
    end
    
    subgraph HW["⚡ Hardware Detection"]
        SECDED["ECC Engine (SECDED)<br/>Single Error Correct<br/>Double Error Detect"]
    end
    
    BIT --> SECDED
    SECDED --> DEC{Error Type?}
    
    DEC -->|"SBE<br/>(Single Bit Error)"| SBE_PATH
    DEC -->|"DBE<br/>(Double Bit Error)"| DBE_PATH
    
    subgraph SBE_PATH["🟡 Correctable (SBE)"]
        SBE1["HW auto-corrects in-flight"]
        SBE2["Increment SBE counter<br/>per-row, per-bank"]
        SBE3{SBE count ><br/>threshold?}
        SBE3 -->|Yes| SBE4["Schedule page retirement<br/>at next safe point"]
        SBE3 -->|No| SBE5["Log & continue<br/>(nvidia-smi shows SBE count)"]
    end
    
    subgraph DBE_PATH["🔴 Uncorrectable (DBE)"]
        DBE1["GPU halts context<br/>Xid 48: DBE on page 0x1234"]
        DBE2["Notify UVM driver"]
        DBE3["Poison affected pages"]
        DBE4["Retire page permanently<br/>(blacklist in InfoROM)"]
        DBE5{Training checkpoint<br/>available?}
        DBE5 -->|Yes| DBE6["Kill current training<br/>Restart from checkpoint"]
        DBE5 -->|No| DBE7["☠️ Training data loss<br/>Full restart required"]
    end
    
    subgraph RETIRE["🔵 Page Retirement"]
        R1["1. Migrate data from bad page"]
        R2["2. Unmap from all GPU contexts"]
        R3["3. Add to retirement list"]
        R4["4. Write to InfoROM<br/>(persists across reboots)"]
        R5["5. allocator blacklists page"]
    end
    
    SBE4 --> RETIRE
    DBE4 --> RETIRE

    style BIT fill:#e74c3c,stroke:#c0392b,color:#fff
    style SECDED fill:#f39c12,stroke:#e67e22,color:#fff
    style SBE1 fill:#f1c40f,stroke:#f39c12,color:#000
    style SBE4 fill:#e67e22,stroke:#d35400,color:#fff
    style DBE1 fill:#c0392b,stroke:#922b21,color:#fff
    style DBE6 fill:#e74c3c,stroke:#c0392b,color:#fff
    style R4 fill:#3498db,stroke:#2980b9,color:#fff
```

**Page Retirement Implementation:**

```c
/* Simplified GPU page retirement flow */
struct gpu_page_retire_info {
    u64 gpu_phys_addr;
    u32 bank, row, col;
    u32 sbe_count;
    u32 dbe_count;
    enum { PENDING, RETIRED, FAILED } state;
    bool persisted_to_inforom;
};

int gpu_retire_page(struct gpu_device *gpu, u64 phys_addr, 
                     enum retire_reason reason)
{
    struct gpu_page_retire_info *info;
    
    /* 1. Check if page is currently in use */
    if (gpu_page_in_use(gpu, phys_addr)) {
        /* Cannot retire immediately — schedule for later */
        list_add(&info->pending_list, &gpu->pending_retirements);
        /* Will be retired when current contexts release it */
        return -EBUSY;
    }
    
    /* 2. If page has live data, migrate first */
    if (gpu_page_has_data(gpu, phys_addr)) {
        u64 new_page = gpu_alloc_replacement_page(gpu);
        gpu_dma_copy(gpu, new_page, phys_addr, PAGE_SIZE);
        gpu_remap_all_contexts(gpu, phys_addr, new_page);
    }
    
    /* 3. Blacklist in allocator */
    gpu_allocator_blacklist(gpu, phys_addr, PAGE_SIZE);
    
    /* 4. Persist to InfoROM (survives reboot) */
    inforom_write_retired_page(gpu->inforom, phys_addr, reason);
    info->persisted_to_inforom = true;
    
    /* 5. Update nvidia-smi counters */
    gpu->retired_pages_sbe += (reason == RETIRE_SBE);
    gpu->retired_pages_dbe += (reason == RETIRE_DBE);
    
    pr_info("GPU %d: Retired page 0x%llx (reason=%s, total=%d)\n",
            gpu->id, phys_addr, 
            reason == RETIRE_SBE ? "SBE" : "DBE",
            gpu->retired_pages_sbe + gpu->retired_pages_dbe);
    
    return 0;
}
```

**DGX Fleet Management:**
```bash
# Monitor ECC on DGX
nvidia-smi --query-gpu=ecc.errors.corrected.volatile.total,\
ecc.errors.uncorrected.volatile.total,\
retired_pages.sbe,retired_pages.dbe \
--format=csv

# Alert thresholds for fleet:
# SBE > 10 in 24hr → schedule maintenance
# DBE > 0 → immediate page retirement
# Retired pages > 50 → GPU replacement recommended
# Row remapping exhausted → GPU EOL
```

---

#### ❓ Q4: Design the PCIe ATS (Address Translation Services) integration for GPU DMA on a Confidential Computing (CC) platform where the GPU must access encrypted host memory.

**A:** This is cutting-edge — NVIDIA's Confidential Computing for H100 (CC-on mode).

```mermaid
sequenceDiagram
    participant GPU as H100 GPU<br/>[Trusted]
    participant BAR as PCIe BAR<br/>[Config Space]
    participant ATS as ATS<br/>[PCIe TLP]
    participant IOMMU as IOMMU<br/>[VT-d / SMMU]
    participant CCA as CC Agent<br/>[on-die encryption]
    participant MEM as Host Memory<br/>[AES-XTS encrypted]

    Note over GPU,MEM: 🔒 Confidential Computing Mode (CC-On)

    GPU->>GPU: Need to access VA=0x7f001000<br/>GMMU TLB miss

    alt ATS Path (IOTLB miss)
        GPU->>ATS: PCIe ATS Translation Request<br/>VA=0x7f001000, PASID=42
        ATS->>IOMMU: Translate VA→PA using process page tables
        IOMMU->>IOMMU: Walk IOMMU page tables<br/>(2nd level) + CPU page tables (1st level)
        IOMMU->>ATS: Translation Completion:<br/>PA=0x100000, permissions=RW
        ATS->>GPU: Cache in GPU IOTLB
    end

    GPU->>GPU: Issue DMA Read to PA=0x100000
    GPU->>CCA: Encrypt DMA address<br/>Add authentication tag
    CCA->>MEM: Memory read with<br/>encrypted address + auth
    MEM->>CCA: Return encrypted data
    CCA->>GPU: Decrypt data<br/>Verify integrity
    GPU->>GPU: ✅ Data available in GPU memory

    Note over GPU,MEM: Without CC: GPU DMAs plaintext, MITM possible
    Note over GPU,MEM: With CC: All GPU↔Host traffic encrypted
```

**Key Design Decisions:**

1. **PASID (Process Address Space ID)**: Each CUDA context gets a unique PASID. The IOMMU uses PASID to select the correct page tables → per-process GPU isolation.

2. **ATS vs Static Mapping**: 
   - Without ATS: Driver must pin all GPU-accessible memory, program IOMMU IOTLB statically
   - With ATS: GPU can fault on-demand, IOMMU translates using CPU page tables → true shared virtual memory

3. **Bounce buffers eliminated**: GPU directly accesses user-space VA → no `copy_to_user / copy_from_user` for GPU data

4. **CC encryption overhead**: ~5-10% bandwidth reduction due to AES-XTS encryption on the memory link. Trade-off: security vs performance.

---

#### ❓ Q5: You're debugging a multi-GPU training job where GPU 3 shows 40% lower memory bandwidth than other GPUs. The application uses UVM. Walk through your debugging approach.

**A:**

```mermaid
flowchart TD
    A["🔴 GPU 3: 40% lower BW"] --> B{Step 1: HW check}
    
    B -->|nvidia-smi| C["Check: ECC errors?<br/>Throttling? Temp?<br/>Power? Clock?"]
    C -->|"All normal"| D{Step 2: Memory location}
    
    D -->|"nvidia-smi topo"| E["NVLink topology ok?<br/>All links active?"]
    E -->|"All links up"| F{Step 3: UVM analysis}
    
    F -->|"CUDA profiler"| G["cuda_memcpy events<br/>Page fault count<br/>Migration count"]
    
    G --> H{Diagnosis?}
    
    H -->|"High fault count<br/>on GPU 3"| I["🟠 Thrashing: Pages migrating<br/>in/out of GPU 3 constantly"]
    I --> I1["Fix: Pin hot data<br/>cudaMemAdvise(cudaMemAdviseSetPreferredLocation)"]
    
    H -->|"Remote access<br/>high percentage"| J["🟡 Data locality: GPU 3 accessing<br/>pages on other GPUs via NVLink"]
    J --> J1["Fix: Redistribute data<br/>Match data → GPU affinity"]
    
    H -->|"Normal faults<br/>but slow DMA"| K["🔵 NVLink bottleneck"]
    K --> K1["Check: nvidia-smi nvlink -g 0<br/>Is one link saturated?<br/>XBAR routing suboptimal?"]
    
    H -->|"Same fault count<br/>same locality"| L["🔴 HBM issue"]
    L --> L1["Run dcgm diag -r 3<br/>HBM stress test<br/>Check for row remapping<br/>Possible HBM stack failure"]

    style A fill:#e74c3c,stroke:#c0392b,color:#fff
    style I fill:#f39c12,stroke:#e67e22,color:#fff
    style J fill:#f1c40f,stroke:#f39c12,color:#000
    style K fill:#3498db,stroke:#2980b9,color:#fff
    style L fill:#c0392b,stroke:#922b21,color:#fff
```

**Step-by-step commands:**

```bash
# 1. Hardware health
nvidia-smi -q -d MEMORY,ECC,PERFORMANCE,TEMPERATURE -i 3

# 2. NVLink topology and bandwidth
nvidia-smi topo -m
nvidia-smi nvlink -s -i 3   # per-link stats
nvidia-smi nvlink -g 0 -i 3  # link utilization

# 3. UVM statistics
cat /proc/driver/nvidia/gpus/0000:41:00.0/information
# UVM: page_fault_count, migration_count, bytes_migrated

# 4. CUDA profiler
nsys profile --trace=cuda,nvtx,osrt ./training_app
# Look for: cudaMemcpyPeer, UVM faults, migration events

# 5. Memory bandwidth test (isolated)
./bandwidthTest --device=3 --mode=shmoo
# Compare against GPU 0 baseline

# 6. DCGM diagnostic
dcgm diag -r 3 -j  # Level 3: includes memory stress test
# Reports: HBM bandwidth per stack, ECC, row remapping status

# 7. If HBM row remapping:
nvidia-smi --query-remapped-rows=gpu_uuid,remapped_rows.correctable,\
remapped_rows.uncorrectable,remapped_rows.pending,\
remapped_rows.failure --format=csv
```

**Root cause hierarchy (in order of likelihood):**
1. UVM page thrashing (most common in multi-GPU training)
2. Suboptimal data partitioning → remote memory access overhead
3. NVLink degradation (rare but happens — link training failure)
4. HBM thermal throttling (GPU 3 in center of 8-GPU tray = hottest)
5. HBM stack failure/row remapping exhaustion

---

#### ❓ Q6: Design a GPU memory overcommit system that allows a 200GB model to run on an 80GB H100 GPU.

**A:** This is **GPU memory virtualization** — essential for large models.

```mermaid
flowchart TD
    subgraph Model["🧠 200GB LLM"]
        HOT["Hot pages (active layers)<br/>~30GB — in HBM"]
        WARM["Warm pages (next layers)<br/>~30GB — prefetching"]
        COLD["Cold pages (inactive layers)<br/>~140GB — in system RAM"]
    end
    
    subgraph GPU["🟢 H100 (80GB HBM)"]
        HBM["HBM3: 80GB<br/>3.2 TB/s bandwidth"]
        GMMU2["GMMU Page Tables<br/>MAP: hot → HBM<br/>MAP: warm → (prefetch target)<br/>UNMAP: cold → fault on access"]
    end
    
    subgraph HOST["🔴 Host System"]
        RAM["System RAM: 512GB<br/>Cold page storage"]
        SSD["NVMe SSD: 8TB<br/>Ultra-cold / checkpoint"]
    end
    
    subgraph Engine["⚙️ Overcommit Engine"]
        PREDICT["Layer Predictor<br/>Knows execution order<br/>of transformer layers"]
        PREFETCH["Prefetch Engine<br/>DMA cold→HBM 2 layers ahead"]
        EVICT["Eviction Engine<br/>LRU of completed layers"]
    end
    
    Model --> GMMU2
    GMMU2 --> HBM
    PREDICT --> PREFETCH
    PREFETCH -->|"PCIe 5.0<br/>64 GB/s"| RAM
    PREFETCH -->|"Async DMA<br/>copy engine"| HBM
    EVICT -->|"Completed layers"| RAM

    style HOT fill:#2ecc71,stroke:#27ae60,color:#fff
    style WARM fill:#f39c12,stroke:#e67e22,color:#fff
    style COLD fill:#3498db,stroke:#2980b9,color:#fff
    style HBM fill:#2ecc71,stroke:#27ae60,color:#fff
    style RAM fill:#e74c3c,stroke:#c0392b,color:#fff
    style PREDICT fill:#9b59b6,stroke:#8e44ad,color:#fff
```

**Execution Timeline:**

```mermaid
sequenceDiagram
    participant CE as Compute Engine<br/>[SM]
    participant PE as Prefetch Engine<br/>[Copy Engine]
    participant HBM as HBM3<br/>[80GB]
    participant PCIe as PCIe 5.0
    participant RAM as System RAM<br/>[512GB]

    Note over CE,RAM: Layer N executing, Layer N+2 prefetching

    par Layer N: Computing
        CE->>HBM: Read Layer N weights (30GB in HBM)
        CE->>CE: Forward pass computation
    and Layer N+2: Prefetching
        PE->>PCIe: DMA Request: Layer N+2 weights
        PCIe->>RAM: Read cold pages
        RAM->>PCIe: Data (64 GB/s)
        PCIe->>HBM: Write to HBM (replacing Layer N-2)
    end

    Note over CE: Layer N complete → start Layer N+1

    CE->>CE: Layer N+1 forward pass
    Note over CE: Layer N+1 weights already in HBM ✅<br/>(prefetched while N was computing)

    par Layer N+1: Computing
        CE->>HBM: Read Layer N+1 weights
    and Layer N-1: Evicting
        PE->>HBM: Evict Layer N-1 (no longer needed)
        PE->>PCIe: DMA Write to RAM
        PCIe->>RAM: Store cold pages
    and Layer N+3: Prefetching
        PE->>PCIe: Prefetch Layer N+3
        PCIe->>RAM: Read next batch
    end

    Note over CE,RAM: Pipeline: Compute(N) || Prefetch(N+2) || Evict(N-2)
```

**Key design considerations:**
1. **Double buffering**: Always have N and N+1 layers in HBM while prefetching N+2
2. **PCIe bandwidth budget**: 64 GB/s PCIe 5 × 16 → 30GB layer takes ~0.5s to transfer
3. **Compute must exceed transfer time**: If layer computes in < 0.5s, we're PCIe bandwidth bound → reduce model parallelism
4. **Activation checkpointing**: Don't store activations — recompute during backward pass → saves 50%+ HBM
5. **KV cache management**: KV cache grows per-token → may need separate eviction policy

---

#### ❓ Q7: NVLink vs PCIe for multi-GPU memory access — when does the kernel driver choose each path, and what are the latency/bandwidth tradeoffs?

**A:**

| Property | NVLink 4.0 (H100) | PCIe 5.0 x16 | Decision Factor |
|----------|-------------------|--------------|-----------------|
| **Bandwidth** | 900 GB/s (bidirectional) | 64 GB/s (bidirectional) | 14× faster on NVLink |
| **Latency** | ~1-2μs | ~3-5μs | 2-3× lower on NVLink |
| **Topology** | All-to-all via NVSwitch | Point-to-point or switch | NVLink: any GPU to any GPU |
| **Peer access** | Direct GMMU mapping | Via system memory (or P2P) | NVLink: true peer, no copy |
| **Coherence** | GMMU-based consistency | No HW coherence | NVLink can do atomic ops |
| **CPU involvement** | None (GPU-to-GPU direct) | CPU may mediate | NVLink: zero-copy, no CPU |

**Driver decision logic:**
```c
int uvm_select_transfer_path(int src_gpu, int dst_gpu, size_t size)
{
    struct gpu_topology *topo = get_topology();
    
    /* Check NVLink peer connectivity */
    if (topo->nvlink_connected[src_gpu][dst_gpu]) {
        int bw = topo->nvlink_bw[src_gpu][dst_gpu]; /* GB/s */
        
        /* For large transfers: always NVLink */
        if (size > 64 * 1024)  /* > 64KB */
            return PATH_NVLINK_DMA;
        
        /* For small transfers: NVLink still wins on latency */
        return PATH_NVLINK_DMA;
    }
    
    /* No NVLink: use PCIe peer-to-peer if supported */
    if (pcie_p2p_supported(src_gpu, dst_gpu) && 
        same_pcie_switch(src_gpu, dst_gpu))
        return PATH_PCIE_P2P;
    
    /* Worst case: stage through system memory */
    return PATH_SYSTEM_MEMORY_STAGING;
}
```

---

[← Previous: 19 — Qualcomm System Design Part 2](19_Qualcomm_System_Design_Part2.md) | [Next: 21 — Qualcomm 15yr Deep →](21_Qualcomm_15yr_System_Design_Deep_Interview.md)
