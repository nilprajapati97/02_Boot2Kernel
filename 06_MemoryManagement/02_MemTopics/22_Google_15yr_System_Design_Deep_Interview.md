# 22 — Google: 15-Year Experience System Design Deep Interview — Memory Management

> **Target**: Staff/Principal/Distinguished Engineer interviews at Google (Linux Kernel, Infrastructure, ChromeOS, Android Platform, Cloud/GKE)
> **Level**: 15+ years — You are expected to design fleet-wide memory policies, optimize memory for warehouse-scale computing, and make Linux kernel mm subsystem changes that scale to millions of machines.

---

## 📌 Interview Focus Areas

| Domain | What Google Expects at 15yr Level |
|--------|----------------------------------|
| **cgroup v2 Memory Controller** | Memory limit, OOM handling, memory.high back-pressure, PSI (Pressure Stall Information) |
| **Huge Pages at Fleet Scale** | THP promotion/demotion, khugepaged, fragmentation vs performance, 1G pages |
| **NUMA Optimization** | Memory tiering (CXL), NUMA balancing, page migration, node reclaim |
| **Memory Overcommit & OOM** | Design OOM killer policy for heterogeneous workloads, oom_score_adj |
| **Page Reclaim Under Pressure** | kswapd, direct reclaim, writeback, LRU generations, MGLRU |
| **Memory Accounting at Scale** | RSS vs PSS vs USS, memory.stat, working set estimation |
| **Kernel Memory Allocator Tuning** | SLAB vs SLUB, per-CPU caches, slab merging, memory fragmentation |
| **Memory Safety** | KASAN, KMSAN, KFENCE in production, shadow memory overhead |

---

## 🎨 System Design 1: Design a Fleet-Wide Memory QoS System for Borg/Kubernetes

### Context
Google runs millions of containers on shared bare-metal machines. Each machine has 256GB–2TB RAM. Containers have diverse memory profiles: latency-sensitive (search), throughput (MapReduce), ML training (large working set). You must design the memory QoS system that ensures:
1. Latency-sensitive jobs never OOM or experience reclaim stalls
2. Batch jobs use leftover memory efficiently
3. Total memory utilization > 85% (efficiency target)

### Architecture Diagram

```mermaid
flowchart TD
    subgraph Machine["🖥️ Bare-Metal Machine (512GB RAM)"]
        subgraph Prod["🔴 Production Tier (Guaranteed)"]
            S1["Search Frontend<br/>memory.min = 32GB<br/>memory.high = 40GB<br/>oom_score_adj = -800"]
            S2["Ad Serving<br/>memory.min = 16GB<br/>memory.high = 20GB<br/>oom_score_adj = -700"]
        end
        
        subgraph Mid["🟠 Mid Tier (Burstable)"]
            M1["ML Inference<br/>memory.min = 8GB<br/>memory.max = 64GB<br/>oom_score_adj = 0"]
            M2["Bigtable Server<br/>memory.min = 16GB<br/>memory.max = 128GB<br/>oom_score_adj = -200"]
        end
        
        subgraph Batch["🟢 Batch Tier (Best-Effort)"]
            B1["MapReduce Worker<br/>memory.min = 0<br/>memory.high = 48GB<br/>oom_score_adj = 500"]
            B2["Build Job<br/>memory.min = 0<br/>memory.max = 32GB<br/>oom_score_adj = 800"]
        end
        
        subgraph Kernel["⚙️ Kernel Memory Manager"]
            MGLRU["Multi-Gen LRU<br/>Age-based reclaim"]
            PSI["PSI Monitor<br/>Pressure Stall Info"]
            OOM["OOM Killer<br/>Smart victim selection"]
            CGROUP["cgroup v2<br/>Memory controller"]
            KSWAPD["kswapd<br/>Background reclaim"]
        end
    end
    
    S1 --> CGROUP
    M1 --> CGROUP
    B1 --> CGROUP
    CGROUP --> MGLRU
    PSI --> KSWAPD
    MGLRU --> OOM

    style S1 fill:#e74c3c,stroke:#c0392b,color:#fff
    style S2 fill:#e74c3c,stroke:#c0392b,color:#fff
    style M1 fill:#f39c12,stroke:#e67e22,color:#fff
    style M2 fill:#f39c12,stroke:#e67e22,color:#fff
    style B1 fill:#2ecc71,stroke:#27ae60,color:#fff
    style B2 fill:#2ecc71,stroke:#27ae60,color:#fff
    style MGLRU fill:#3498db,stroke:#2980b9,color:#fff
    style PSI fill:#9b59b6,stroke:#8e44ad,color:#fff
    style OOM fill:#c0392b,stroke:#922b21,color:#fff
```

### Memory Pressure Response Sequence

```mermaid
sequenceDiagram
    participant APP as Batch Job<br/>[MapReduce]
    participant CG as cgroup Controller
    participant MGLRU as Multi-Gen LRU
    participant KSWAPD as kswapd
    participant PSI as PSI Monitor
    participant OOM as OOM Killer
    participant SCHED as Borg Scheduler

    Note over APP: Machine memory usage: 75%
    APP->>APP: Allocate 20GB more<br/>(ML model load)
    
    Note over CG: memory.current > memory.high
    CG->>APP: Throttle: memory.high exceeded<br/>→ slow down allocation (reclaim)
    
    APP->>MGLRU: Direct reclaim: scan LRU
    MGLRU->>MGLRU: Generation 0 (oldest pages):<br/>Evict cold file-backed pages
    
    Note over PSI: PSI some = 5% (moderate pressure)
    PSI->>SCHED: Alert: Memory pressure detected
    SCHED->>SCHED: Don't schedule new batch jobs<br/>to this machine
    
    Note over APP: Still allocating...
    
    alt memory.current < memory.max
        CG->>APP: Allow allocation<br/>(within limit, but throttled)
        KSWAPD->>KSWAPD: Background reclaim<br/>Scan batch tier cgroups first
    else memory.current >= memory.max
        CG->>OOM: Trigger cgroup OOM
        OOM->>OOM: Select victim within cgroup<br/>(highest oom_score)
        OOM->>APP: Kill: MapReduce worker
        Note over APP: Borg restarts on<br/>another machine
    end
    
    Note over PSI: PSI some = 0% → pressure relieved
    PSI->>SCHED: Clear: ready for new work
```

### Deep Q&A

---

#### ❓ Q1: Design the `memory.high` back-pressure mechanism. How does it throttle allocations without killing the process?

**A:** `memory.high` is the most important cgroup memory knob for QoS — it's a soft limit that applies **allocation throttling** instead of OOM killing.

```mermaid
flowchart TD
    A["Process: malloc(4KB)"] --> B["__alloc_pages()"]
    B --> C["charge memcg"]
    C --> D{memory.current ><br/>memory.high?}
    
    D -->|No| E["✅ Charge succeeds<br/>Return page immediately"]
    
    D -->|Yes| F["mem_cgroup_handle_over_high()"]
    F --> G["Calculate penalty:<br/>penalty_ms = excess / high * 200ms"]
    G --> H["Direct reclaim attempt<br/>try_to_free_mem_cgroup_pages()"]
    
    H --> I{Reclaimed<br/>enough?}
    I -->|Yes| J["✅ Allocation proceeds<br/>No sleep needed"]
    I -->|No| K["Sleep for penalty_ms<br/>(throttle the process)"]
    K --> L["Wake up"]
    L --> M{Still over high?}
    M -->|Yes| N["Return to allocator<br/>(may sleep again)"]
    M -->|No| E

    style A fill:#3498db,stroke:#2980b9,color:#fff
    style D fill:#f39c12,stroke:#e67e22,color:#fff
    style E fill:#2ecc71,stroke:#27ae60,color:#fff
    style F fill:#e74c3c,stroke:#c0392b,color:#fff
    style K fill:#c0392b,stroke:#922b21,color:#fff
    style J fill:#2ecc71,stroke:#27ae60,color:#fff
```

**Implementation details:**

```c
/* mm/memcontrol.c — memory.high throttling */

void mem_cgroup_handle_over_high(gfp_t gfp_mask)
{
    unsigned long usage = page_counter_read(&memcg->memory);
    unsigned long high = READ_ONCE(memcg->memory.high);
    unsigned long penalty_jiffies;
    
    if (usage <= high)
        return;  /* Below soft limit — no throttling */
    
    /* Calculate overage ratio */
    unsigned long overage = usage - high;
    unsigned long penalty_pct = overage * 100 / high;
    
    /* Grow penalty exponentially with overage */
    /* 10% over → 20ms sleep, 50% over → 100ms, 100% over → 200ms */
    penalty_jiffies = msecs_to_jiffies(min(penalty_pct * 2, 200UL));
    
    /* Try reclaiming first (cheaper than sleeping) */
    unsigned long nr_reclaimed = try_to_free_mem_cgroup_pages(
        memcg,
        max(overage >> PAGE_SHIFT, 1UL),  /* nr_pages to reclaim */
        gfp_mask,
        MEMCG_RECLAIM_MAY_SWAP
    );
    
    if (nr_reclaimed >= (overage >> PAGE_SHIFT))
        return;  /* Reclaimed enough — no sleep */
    
    /* Throttle: sleep to slow down allocation rate */
    set_current_state(TASK_KILLABLE);
    schedule_timeout(penalty_jiffies);
    
    /* Track for PSI accounting */
    psi_memstall_enter(&pflags);
    psi_memstall_leave(&pflags);
}
```

**Why `memory.high` matters at Google scale:**
- `memory.max` is a **hard wall** → OOM kill → job restart → wasted work
- `memory.high` is a **rubber wall** → gradual slowdown → job self-regulates
- For batch jobs: set `memory.high` at 80% of `memory.max` → gives 20% buffer before OOM
- For latency-sensitive: set `memory.high = memory.max` → never throttle, just OOM fast if needed

---

#### ❓ Q2: Design the Multi-Generation LRU (MGLRU) and explain why Google contributed it to the kernel upstream. What problem did it solve at fleet scale?

**A:** 

**The problem with traditional LRU:**
```
Classic 2-list LRU (active/inactive):
- One access → page goes to active list
- Clock algorithm to demote active → inactive
- Problem: All pages in active list are "equally hot"
- No age information: a page accessed 1 second ago and 
  one accessed 1 hour ago are in the same list
- Under memory pressure: kswapd scans HUGE lists blindly
- At Google scale: kswapd CPU overhead = 5-15% on some machines
```

**MGLRU Solution:**

```mermaid
flowchart LR
    subgraph Classic["❌ Classic LRU"]
        direction TB
        Active["Active List<br/>All 'hot' pages<br/>(no age distinction)"]
        Inactive["Inactive List<br/>Candidates for reclaim"]
        Active -->|"Clock algorithm<br/>(expensive scan)"| Inactive
    end
    
    subgraph MGLRU["✅ Multi-Gen LRU"]
        direction TB
        G3["Generation 3 (Youngest)<br/>Recently accessed pages<br/>🟢 Keep"]
        G2["Generation 2<br/>Accessed a while ago<br/>🟡 Monitor"]
        G1["Generation 1<br/>Not accessed recently<br/>🟠 Demote"]
        G0["Generation 0 (Oldest)<br/>Cold pages<br/>🔴 Reclaim"]
        G3 -->|"Age"| G2
        G2 -->|"Age"| G1
        G1 -->|"Age"| G0
    end

    style Active fill:#f39c12,stroke:#e67e22,color:#fff
    style Inactive fill:#e74c3c,stroke:#c0392b,color:#fff
    style G3 fill:#2ecc71,stroke:#27ae60,color:#fff
    style G2 fill:#f1c40f,stroke:#f39c12,color:#000
    style G1 fill:#f39c12,stroke:#e67e22,color:#fff
    style G0 fill:#e74c3c,stroke:#c0392b,color:#fff
```

**MGLRU Page Aging Mechanism:**

```mermaid
sequenceDiagram
    participant PT as Page Tables<br/>[Access Bits]
    participant AGING as lru_gen_aging<br/>[walks page tables]
    participant GENS as Generation Lists<br/>[0 to max_seq]
    participant EVICT as lru_gen_eviction<br/>[reclaims coldest]

    Note over PT: Kernel periodically walks<br/>page tables checking Access (A) bit

    AGING->>PT: Walk mm_struct page tables
    PT->>AGING: Page X: A=1 (accessed since last walk)
    AGING->>GENS: Page X: promote to youngest generation<br/>(max_seq)
    
    AGING->>PT: Page Y: A=0 (NOT accessed)
    AGING->>GENS: Page Y: stays in current generation<br/>(ages naturally as new gen created)
    
    Note over GENS: Time passes...<br/>New generation created<br/>All pages age by 1

    EVICT->>GENS: Need to reclaim memory!<br/>Start from Generation 0 (oldest)
    GENS->>EVICT: Gen 0 pages: Y, Z, W<br/>(cold — not accessed in 4 generations)
    EVICT->>EVICT: Evict file-backed pages<br/>Swap out anonymous pages
    
    Note over EVICT: 🔑 Only scans Gen 0<br/>NOT the entire list!<br/>→ O(cold_pages) not O(all_pages)
```

**Google's fleet-scale impact:**

```c
/* Performance comparison at Google fleet scale */

/*
 * Classic LRU (before MGLRU):
 * - kswapd scans active+inactive lists sequentially
 * - Cost: O(total_pages) per reclaim cycle
 * - 256GB machine with 64M pages → scans millions of pages
 * - kswapd CPU: 5-15% during memory pressure
 * - Latency: p99 allocation latency spikes to 100ms+
 * 
 * Multi-Gen LRU (MGLRU):
 * - Only scans coldest generation (Gen 0)
 * - Cost: O(cold_pages) — typically 1-5% of total
 * - Page table walk uses hardware Access bits — efficient
 * - kswapd CPU: < 1% during same pressure
 * - Latency: p99 allocation latency < 5ms
 * 
 * Fleet-wide results (Google internal measurements):
 * - 40% reduction in kswapd CPU overhead
 * - 26% improvement in memory efficiency (can pack more containers)
 * - Major p99 latency improvement for search frontend
 */

/* Enable MGLRU (Linux 6.1+) */
echo y > /sys/kernel/mm/lru_gen/enabled
echo 1000 > /sys/kernel/mm/lru_gen/min_ttl_ms
/* min_ttl_ms: minimum age before a generation can be evicted
 * Prevents premature eviction of recently-touched pages */
```

**Why Google upstreamed it:**
1. **Fleet-scale efficiency**: Even 1% improvement across millions of machines = enormous savings
2. **Reduced tail latency**: kswapd scanning spikes caused p99 search delays
3. **Better container isolation**: Classic LRU had global scanning → noisy neighbor effects between containers
4. **Upstream maintenance**: Maintaining out-of-tree patches across kernel versions was expensive

---

#### ❓ Q3: Design a CXL memory tiering system for a Google cloud server with 256GB DDR5 + 1TB CXL-attached memory. How does the kernel decide which pages go where?

**A:**

```mermaid
flowchart TD
    subgraph CPU["🔵 CPU (Sapphire Rapids)"]
        CORE["CPU Cores"]
        IMC["Integrated Memory Controller"]
        CXLC["CXL Controller<br/>(PCIe 5.0 x16)"]
    end
    
    subgraph Tier0["🟢 Tier 0: DDR5 (Fast)"]
        DDR["256GB DDR5<br/>Bandwidth: 300 GB/s<br/>Latency: ~80ns<br/>NUMA Node 0"]
    end
    
    subgraph Tier1["🟠 Tier 1: CXL Memory (Slow)"]
        CXL["1TB CXL 2.0 Memory<br/>Bandwidth: 64 GB/s<br/>Latency: ~170ns<br/>NUMA Node 2"]
    end
    
    subgraph Policy["⚙️ Tiering Policy Engine"]
        NUMA_BAL["NUMA Balancing<br/>(kernel autoNUMA)"]
        DAMON["DAMON<br/>(Data Access Monitor)"]
        PROMOTE["Hot Page Promoter<br/>CXL → DDR"]
        DEMOTE["Cold Page Demoter<br/>DDR → CXL"]
    end
    
    CORE --> IMC --> DDR
    CORE --> CXLC --> CXL
    
    DAMON -->|"Track access<br/>frequency"| PROMOTE
    DAMON -->|"Track access<br/>frequency"| DEMOTE
    
    PROMOTE -->|"Hot pages<br/>(> 10 accesses/sec)"| DDR
    DEMOTE -->|"Cold pages<br/>(< 1 access/10sec)"| CXL

    style DDR fill:#2ecc71,stroke:#27ae60,color:#fff
    style CXL fill:#f39c12,stroke:#e67e22,color:#fff
    style DAMON fill:#3498db,stroke:#2980b9,color:#fff
    style PROMOTE fill:#2ecc71,stroke:#27ae60,color:#fff
    style DEMOTE fill:#f39c12,stroke:#e67e22,color:#fff
```

**Tiering Decision Sequence:**

```mermaid
sequenceDiagram
    participant APP as Application
    participant DDR as DDR5 Node [256GB]
    participant CXL as CXL Node [1TB]
    participant DAMON as DAMON Monitor
    participant KSWAPD as kswapd/kcompactd
    participant MIGRATE as migrate_pages[]

    Note over APP: App allocates 800GB total<br/>DDR: 256GB (full), CXL: 544GB

    DAMON->>DAMON: Sample page access bits<br/>every 100ms for 10 seconds
    
    DAMON->>DAMON: Classify pages:<br/>HOT: accessed > 10x/sec<br/>WARM: accessed 1-10x/sec<br/>COLD: accessed < 1x/10sec

    Note over DAMON: DDR has 60GB of COLD pages!

    alt Demotion: Cold DDR → CXL
        DAMON->>KSWAPD: memory.reclaim hint:<br/>demote cold pages
        KSWAPD->>MIGRATE: migrate_pages(cold_list,<br/>dest=CXL_NODE)
        MIGRATE->>DDR: Unmap cold pages from DDR
        MIGRATE->>CXL: Copy to CXL memory
        MIGRATE->>DDR: Free DDR space (60GB freed)
        Note over DDR: Now has room for hot pages
    end

    alt Promotion: Hot CXL → DDR
        DAMON->>DAMON: Detect: Page X on CXL<br/>accessed 50x/sec (HOT!)
        DAMON->>MIGRATE: promote_page(X, DDR_NODE)
        MIGRATE->>CXL: Read hot page from CXL
        MIGRATE->>DDR: Write to DDR
        MIGRATE->>DDR: Remap PTE → DDR PA
        Note over APP: Page X now in DDR<br/>Access latency: 170ns → 80ns ✅
    end

    Note over APP,CXL: 🔑 Working set in DDR<br/>Cold data in CXL<br/>Application sees 1.25TB total<br/>Performance close to all-DDR
```

**DAMON-based tiering configuration:**

```bash
# Linux 6.x CXL memory tiering setup

# 1. Verify CXL memory detected as NUMA node
numactl -H
# Node 0: 256GB (DDR5)
# Node 2: 1024GB (CXL, interleaved)

# 2. Set up memory tiering
echo 2 > /sys/devices/system/node/node0/memtier
echo 1 > /sys/devices/system/node/node2/memtier
# Higher tier = faster. Demotion: tier 2 → tier 1

# 3. Enable NUMA balancing (promotion)
echo 2 > /proc/sys/kernel/numa_balancing
# 0=off, 1=classic, 2=tiered (promote + demote)

# 4. Configure DAMON for demotion policy
cd /sys/kernel/mm/damon/admin
echo "demotion" > kdamonds/0/contexts/0/schemes/0/action
echo "1000000" > kdamonds/0/contexts/0/schemes/0/access_pattern/sz/min
echo "0" > kdamonds/0/contexts/0/schemes/0/access_pattern/nr_accesses/max
echo "10000000" > kdamonds/0/contexts/0/schemes/0/access_pattern/age/min
# Demote pages > 1MB, 0 accesses, older than 10 seconds

# 5. Start monitoring
echo "on" > kdamonds/0/state
```

**Kernel implementation:**
```c
/* mm/memory-tiers.c — Memory tier management */

struct memory_tier {
    int tier_id;              /* Higher = faster */
    struct list_head nodes;   /* NUMA nodes in this tier */
    int demote_tier_id;       /* Where to demote cold pages */
};

/* Demotion path: called by kswapd when DDR node is under pressure */
int demote_folio(struct folio *folio, struct list_head *demote_folios)
{
    int target_nid = next_demotion_node(folio_nid(folio));
    
    if (target_nid == NUMA_NO_NODE)
        return -ENOSPC;  /* No lower tier available */
    
    /* Migrate folio to CXL node */
    return migrate_folio_to_node(folio, target_nid, 
                                  MIGRATE_ASYNC, MR_DEMOTION);
}

/* Promotion path: called by NUMA balancing when hot CXL page detected */
void numa_promote_preferred(struct task_struct *p, struct folio *folio)
{
    int src_nid = folio_nid(folio);
    int dst_nid = p->numa_preferred_nid;
    
    /* Only promote if destination is a higher tier */
    if (node_tier(dst_nid) <= node_tier(src_nid))
        return;
    
    /* Rate limit: don't overwhelm DDR migration bandwidth */
    if (p->numa_scan_period < MIN_PROMOTION_INTERVAL)
        return;
    
    migrate_misplaced_folio(folio, dst_nid);
}
```

**Design tradeoffs:**
| Decision | Option A | Option B | Google's Choice |
|----------|----------|----------|-----------------|
| Promotion trigger | Hardware PMU sampling | NUMA fault-based | **NUMA faults** (lower overhead) |
| Demotion trigger | Watermark (kswapd) | DAMON cold detection | **Both** (watermark for urgency, DAMON for proactive) |
| Migration granularity | 4KB pages | 2MB THP | **2MB** (amortize migration cost) |
| Hot threshold | Fixed (N accesses) | Adaptive (percentile) | **Adaptive** (varies by workload) |

---

#### ❓ Q4: An SRE reports that a Bigtable server process has RSS of 180GB but the working set is only 40GB. The remaining 140GB are stale cached data. How do you reclaim it without impacting latency?

**A:**

```mermaid
flowchart TD
    A["🔍 Problem: 180GB RSS<br/>40GB active, 140GB stale"] --> B{Investigation}
    
    B -->|"/proc/PID/smaps"| C["Analyze per-VMA:<br/>Rss, Referenced, Anonymous"]
    B -->|"DAMON"| D["Monitor access pattern:<br/>Which 40GB is hot?"]
    B -->|"memory.stat"| E["Check: file vs anon<br/>Active vs inactive"]
    
    C --> F["Found: 140GB file-backed<br/>pages (mmap'd SSTable files)<br/>Referenced bit = 0"]
    
    F --> G{Reclaim Strategy}
    
    G -->|"1. MADV_COLD"| H["madvise(addr, 140GB, MADV_COLD)<br/>Move pages to inactive LRU<br/>Reclaimed by kswapd naturally"]
    
    G -->|"2. MADV_PAGEOUT"| I["madvise(addr, 140GB, MADV_PAGEOUT)<br/>Immediately reclaim pages<br/>Fastest — but reclaim storm risk"]
    
    G -->|"3. memory.reclaim"| J["echo '140G' > memory.reclaim<br/>cgroup-scoped reclaim<br/>Kernel selects victim pages"]
    
    G -->|"4. DAMON DAMOS"| K["DAMON-based Operation Scheme:<br/>Automatically pageout pages<br/>cold for > 60 seconds"]
    
    H --> L["✅ Gradual: kswapd reclaims<br/>over next 30 seconds"]
    I --> M["⚠️ Fast: but may cause<br/>latency spike during reclaim"]
    J --> N["✅ Controlled: kernel paces reclaim"]
    K --> O["✅ Proactive: prevents buildup"]

    style A fill:#e74c3c,stroke:#c0392b,color:#fff
    style F fill:#f39c12,stroke:#e67e22,color:#fff
    style H fill:#2ecc71,stroke:#27ae60,color:#fff
    style J fill:#3498db,stroke:#2980b9,color:#fff
    style K fill:#9b59b6,stroke:#8e44ad,color:#fff
```

**Production solution — DAMON-based proactive reclaim:**

```c
/* Google's approach: Use DAMON to continuously identify and 
 * reclaim cold memory at a controlled rate */

/* DAMOS (DAMON-based Operation Schemes) configuration */
struct damos cold_reclaim_scheme = {
    /* Target: pages accessed 0 times in 60 seconds */
    .pattern = {
        .min_sz_region = PAGE_SIZE,
        .max_sz_region = ULONG_MAX,
        .min_nr_accesses = 0,       /* Zero accesses */
        .max_nr_accesses = 0,
        .min_age_region = 60,       /* Seconds cold */
        .max_age_region = UINT_MAX,
    },
    /* Action: page out to disk/swap */
    .action = DAMOS_PAGEOUT,
    
    /* Rate limiting: max 1GB/sec reclaim to avoid latency impact */
    .quota = {
        .sz = SZ_1G,
        .reset_interval = MSEC_PER_SEC,
    },
    
    /* Watermarks: only reclaim when memory pressure exists */
    .wmarks = {
        .metric = DAMOS_WMARK_FREE_MEM_RATE,
        .high = 20,    /* Start reclaiming when free < 20% */
        .mid = 15,     /* Target free = 15% */
        .low = 10,     /* Urgent reclaim below 10% */
    },
};
```

**The `memory.reclaim` interface (Linux 6.0+, Google contribution):**

```bash
# Proactive reclaim for a specific cgroup
echo "3G" > /sys/fs/cgroup/bigtable/memory.reclaim
# Kernel reclaims ~3GB from this cgroup
# Prefers file-backed pages, avoids hot anonymous pages
# Returns success when target is met, or partial reclaim amount

# Google's agent runs this periodically:
while true; do
    current=$(cat /sys/fs/cgroup/bigtable/memory.current)
    target=40G  # Known working set
    excess=$((current - target))
    if [ $excess -gt 0 ]; then
        echo "${excess}" > /sys/fs/cgroup/bigtable/memory.reclaim
    fi
    sleep 30
done
```

---

#### ❓ Q5: Design the OOM killer policy for a machine running search-serving (critical) and batch MapReduce (expendable). The default OOM killer is not suitable — why, and what do you replace it with?

**A:**

**Why default OOM killer fails:**

```mermaid
flowchart TD
    A["🔴 Machine OOM!<br/>512GB exhausted"] --> B["Default OOM Killer"]
    B --> C["Score = RSS size ×<br/>oom_score_adj factor"]
    
    C --> D["Bigtable: RSS=200GB<br/>score = 200 × 1 = 200"]
    C --> E["Search: RSS=100GB<br/>Score = 100 × 1 = 100"]
    C --> F["MapReduce: RSS=180GB<br/>Score = 180 × 1 = 180"]
    
    B --> G["Kill highest score:<br/>Bigtable! (RSS=200GB)"]
    
    G --> H["🔴 WRONG!<br/>Bigtable is critical infrastructure!<br/>Should have killed MapReduce!"]

    style A fill:#e74c3c,stroke:#c0392b,color:#fff
    style G fill:#c0392b,stroke:#922b21,color:#fff
    style H fill:#c0392b,stroke:#922b21,color:#fff
```

**Google's approach — User-space OOM killer with PSI:**

```mermaid
sequenceDiagram
    participant PSI as PSI Monitor<br/>[kernel]
    participant AGENT as OOM Agent<br/>[user-space]
    participant BORG as Borg Scheduler
    participant BATCH as Batch Container
    participant PROD as Prod Container

    Note over PSI: memory PSI some > 25%<br/>Sustained for 5 seconds
    
    PSI->>AGENT: PSI notification via epoll<br/>(memory pressure HIGH)
    
    AGENT->>AGENT: Evaluate kill candidates<br/>using business priority:<br/>1. oom_score_adj<br/>2. Job priority (Borg)<br/>3. Restartability<br/>4. Data loss risk
    
    AGENT->>AGENT: Candidate ranking:<br/>1. Build job (priority=450, restartable) ✅<br/>2. MapReduce (priority=300, restartable) ✅<br/>3. Search (priority=100, CRITICAL) ❌<br/>4. Bigtable (priority=50, CRITICAL) ❌
    
    AGENT->>BATCH: SIGKILL Build job<br/>(lowest priority, easily restartable)
    
    BATCH->>BATCH: Process killed
    
    Note over PSI: Check: pressure relieved?
    
    alt Pressure continues
        AGENT->>BATCH: SIGKILL MapReduce worker
        AGENT->>BORG: Notify: killed jobs X, Y<br/>Reason: memory pressure
        BORG->>BORG: Reschedule killed jobs<br/>on machines with free memory
    else Pressure relieved
        AGENT->>AGENT: Stand down
        Note over PROD: Search & Bigtable survived ✅
    end
```

**Implementation — PSI-based OOM agent:**

```c
/* User-space OOM killer agent (simplified) */

#define PSI_THRESHOLD "some 250000 1000000"
/* Trigger when >25% of time in memory stall over 1-second window */

int main(void) 
{
    int fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK);
    
    /* Register PSI trigger */
    write(fd, PSI_THRESHOLD, strlen(PSI_THRESHOLD));
    
    struct pollfd fds = { .fd = fd, .events = POLLPRI };
    
    while (1) {
        int ret = poll(&fds, 1, -1);
        if (ret > 0 && (fds.revents & POLLPRI)) {
            handle_memory_pressure();
        }
    }
}

void handle_memory_pressure(void)
{
    struct kill_candidate *candidates;
    int n = 0;
    
    /* Enumerate all cgroups */
    DIR *d = opendir("/sys/fs/cgroup");
    /* ... iterate cgroups ... */
    
    /* Score each container */
    for (each cgroup) {
        candidates[n].cgroup = cgroup;
        candidates[n].priority = read_borg_priority(cgroup);
        candidates[n].memory = read_file_u64(cgroup, "memory.current");
        candidates[n].restartable = is_restartable(cgroup);
        
        /* Composite score: lower = kill first */
        candidates[n].kill_score = 
            candidates[n].priority * 1000 +  /* Business priority dominates */
            (candidates[n].restartable ? 0 : 500) +
            (1000 - candidates[n].memory / SZ_1G);  /* Larger = more memory freed */
        n++;
    }
    
    /* Sort: lowest score = best kill candidate */
    qsort(candidates, n, sizeof(*candidates), compare_score);
    
    /* Kill from lowest score until pressure relieved */
    for (int i = 0; i < n; i++) {
        if (!check_psi_still_high())
            break;
        
        /* Kill entire cgroup */
        write_file(candidates[i].cgroup, "cgroup.kill", "1");
        
        syslog(LOG_WARNING, "OOM: killed cgroup %s (priority=%d, memory=%luGB)",
               candidates[i].cgroup, candidates[i].priority,
               candidates[i].memory / SZ_1G);
        
        /* Wait 500ms for memory to be freed */
        usleep(500000);
    }
}
```

---

#### ❓ Q6: How would you measure the true working set size (WSS) of a process at Google scale, and why is RSS not sufficient?

**A:**

```mermaid
flowchart TD
    subgraph Metrics["📊 Memory Metrics"]
        RSS["RSS (Resident Set Size)<br/>All pages in physical RAM<br/>Includes: hot + cold + shared"]
        PSS["PSS (Proportional Set Size)<br/>RSS / num_sharing_processes<br/>Better for shared libs"]
        WSS["WSS (Working Set Size)<br/>Pages actually accessed<br/>in time window T"]
        USS["USS (Unique Set Size)<br/>Private pages only<br/>Memory freed on kill"]
    end
    
    subgraph Problem["❓ Why RSS ≠ WSS"]
        P1["RSS = 180GB"]
        P2["But only 40GB accessed<br/>in last 60 seconds"]
        P3["140GB = old mmap'd files,<br/>allocated but untouched buffers,<br/>fragmented SLAB objects"]
    end
    
    subgraph Methods["🔍 WSS Measurement Methods"]
        M1["1️⃣ /proc/PID/clear_refs + smaps<br/>Clear Referenced bits → wait → read Referenced<br/>Accuracy: ✅ Perfect<br/>Overhead: ⚠️ High (TLB flush!)"]
        
        M2["2️⃣ DAMON Sampling<br/>Statistical sampling of access patterns<br/>Accuracy: 📊 Statistical<br/>Overhead: ✅ Very low (~1%)"]
        
        M3["3️⃣ Idle Page Tracking<br/>/sys/kernel/mm/page_idle/<br/>Bitmap of idle pages<br/>Accuracy: ✅ Per-page<br/>Overhead: 🟡 Moderate"]
        
        M4["4️⃣ PG_young + kstaled<br/>Google kernel patch: PG_young flag reset<br/>Accuracy: ✅ Good<br/>Overhead: ✅ Low"]
    end

    style RSS fill:#e74c3c,stroke:#c0392b,color:#fff
    style WSS fill:#2ecc71,stroke:#27ae60,color:#fff
    style M1 fill:#f39c12,stroke:#e67e22,color:#fff
    style M2 fill:#2ecc71,stroke:#27ae60,color:#fff
    style M4 fill:#3498db,stroke:#2980b9,color:#fff
```

**Google's practical WSS measurement with idle page tracking:**

```c
/* Idle page tracking — /sys/kernel/mm/page_idle/ */

/*
 * Algorithm:
 * 1. Mark all pages as "idle"
 * 2. Wait T seconds (e.g., 60s)
 * 3. Scan pages: if still "idle" → not accessed → cold
 *    if NOT idle → accessed → part of WSS
 */

int measure_wss(pid_t pid, int window_seconds)
{
    size_t wss = 0;
    
    /* Step 1: Get page frame numbers from /proc/PID/pagemap */
    int pagemap_fd = open_fmt("/proc/%d/pagemap", pid);
    int idle_fd = open("/sys/kernel/mm/page_idle/bitmap", O_RDWR);
    
    /* Step 2: Mark all pages as idle */
    for (each vma in /proc/PID/maps) {
        for (each page in vma) {
            uint64_t pfn = read_pagemap(pagemap_fd, va);
            if (pfn == 0) continue;  /* Not resident */
            
            /* Set idle flag */
            uint64_t idle_offset = (pfn / 64) * 8;
            uint64_t idle_bit = 1ULL << (pfn % 64);
            
            uint64_t bits;
            pread(idle_fd, &bits, 8, idle_offset);
            bits |= idle_bit;
            pwrite(idle_fd, &bits, 8, idle_offset);
        }
    }
    
    /* Step 3: Wait for measurement window */
    sleep(window_seconds);
    
    /* Step 4: Read back — non-idle pages = working set */
    for (each page) {
        uint64_t pfn = read_pagemap(pagemap_fd, va);
        
        uint64_t bits;
        pread(idle_fd, &bits, 8, (pfn / 64) * 8);
        
        if (!(bits & (1ULL << (pfn % 64)))) {
            /* Page was accessed! (idle flag cleared by HW) */
            wss += PAGE_SIZE;
        }
    }
    
    return wss;
}
```

**At Google scale (practical):**
```bash
# Google's fleet WSS measurement tool (simplified)
# Runs on every machine, reports to Monarch monitoring

# DAMON-based (preferred — low overhead)
cd /sys/kernel/mm/damon/admin
# Configure DAMON for the target process
echo $PID > kdamonds/0/contexts/0/targets/0/pid_target
echo "on" > kdamonds/0/state

# Read working set estimate from DAMON
cat /sys/kernel/mm/damon/admin/kdamonds/0/contexts/0/schemes/tried_regions
# Output: regions classified by access frequency
# Sum of "hot" regions = WSS estimate

# Fleet-wide insight:
# Average machine: RSS utilization 85%, but WSS only 55%
# → 30% of "used" memory is actually reclaimable
# → This drives memory.high tuning decisions
```

---

#### ❓ Q7: Design a kernel memory fragmentation mitigation system for long-running servers. After 30 days uptime, huge page allocation fails 50% of the time.

**A:**

```mermaid
flowchart TD
    subgraph Problem["🔴 Fragmentation After 30 Days"]
        FRESH["Day 0: Memory fresh<br/>Large contiguous blocks<br/>THP success: 99%"]
        FRAG["Day 30: Memory fragmented<br/>Scattered 4KB allocations<br/>THP success: 50%"]
        CAUSE["Causes:<br/>1. SLAB allocator fragments zones<br/>2. Long-lived kernel objects pin pages<br/>3. Unmovable pages block compaction"]
    end
    
    subgraph Solutions["✅ Mitigation System"]
        S1["1️⃣ Proactive Compaction<br/>(kcompactd)"]
        S2["2️⃣ Page Grouping<br/>(Movability-based allocation)"]
        S3["3️⃣ Memory Hot-Remove Trick<br/>(offline + online memory block)"]
        S4["4️⃣ SLAB Defragmentation<br/>(kmem cache shrink)"]
        S5["5️⃣ CMA Reservation<br/>(Contiguous area for THP)"]
    end
    
    FRESH -->|"30 days of<br/>alloc/free churn"| FRAG
    FRAG --> Solutions

    style FRESH fill:#2ecc71,stroke:#27ae60,color:#fff
    style FRAG fill:#e74c3c,stroke:#c0392b,color:#fff
    style S1 fill:#3498db,stroke:#2980b9,color:#fff
    style S2 fill:#9b59b6,stroke:#8e44ad,color:#fff
    style S3 fill:#f39c12,stroke:#e67e22,color:#fff
```

**Proactive Compaction Configuration:**

```bash
# Linux 5.9+: Proactive compaction (Google contribution)

# Enable proactive compaction
echo 20 > /proc/sys/vm/compaction_proactiveness
# Range: 0 (off) to 100 (aggressive)
# 20 = compact when fragmentation score > 20%

# Monitor fragmentation
cat /proc/buddyinfo
# Node 0, zone   Normal 1234  567  234   89   12    5    2    1    0    0    0
#                 4KB  8KB  16KB  32KB  ...                            4MB
# Want: right side (large blocks) should have entries

# Per-zone fragmentation index
cat /sys/kernel/debug/extfrag/extfrag_index
# Shows fragmentation severity per zone per order

# Compact all zones NOW (manual trigger)
echo 1 > /proc/sys/vm/compact_memory

# Monitor compaction effectiveness
cat /proc/vmstat | grep compact
# compact_stall         12345  (direct compaction attempts)
# compact_success       11000  (successful!)
# compact_fail          1345   (failed — unmovable pages)
# compact_migrate_scanned 5000000 (pages scanned for migration)
```

**Custom defragmentation daemon for Google servers:**

```c
/* Proactive defrag agent — runs during low-load periods */

void defrag_agent_main(void)
{
    while (1) {
        /* Only defrag during low-load periods */
        double load = getloadavg_1min();
        if (load > 0.5)
            goto sleep;
        
        /* Check fragmentation score */
        int frag_score = read_fragmentation_index(ZONE_NORMAL, 
                                                    PMD_ORDER);
        if (frag_score < 30)
            goto sleep;  /* Fragmentation acceptable */
        
        /* Strategy 1: Trigger compaction */
        write_file("/proc/sys/vm/compact_memory", "1");
        
        /* Strategy 2: Shrink SLAB caches */
        write_file("/proc/sys/vm/drop_caches", "2"); /* dentries+inodes */
        
        /* Strategy 3: SLAB-specific shrink */
        for (each slab in /sys/kernel/slab/) {
            int slabs = read_file_int(slab, "slabs");
            int partial = read_file_int(slab, "partial");
            
            /* Shrink highly fragmented caches */
            if (partial * 100 / slabs > 50) {
                write_file(slab, "shrink", "1");
            }
        }
        
        /* Strategy 4: If still fragmented → memory hot-remove trick */
        if (read_fragmentation_index(ZONE_NORMAL, PMD_ORDER) > 60) {
            /* Offline a memory block → forces migration of all pages */
            write_file("/sys/devices/system/memory/memoryN/state", 
                       "offline");
            usleep(100000);
            /* Online it again → fresh contiguous block */
            write_file("/sys/devices/system/memory/memoryN/state", 
                       "online");
        }
        
sleep:
        sleep(300);  /* Check every 5 minutes */
    }
}
```

**Key insights for Google's fleet:**
1. **THP promotion rate** is tracked fleet-wide via `/proc/vmstat` `thp_fault_alloc` vs `thp_fault_fallback`
2. **Compaction success rate** below 90% triggers automated investigation
3. **Memory hot-remove** is used as a last resort on machines with persistent fragmentation
4. **CMA reservation** of 512MB-2GB dedicated to THP allocation avoids fragmentation entirely for critical workloads
5. **SLAB merging** (`slub_merge=y`) reduces the number of distinct caches → reduces fragmentation sources

---

[← Previous: 21 — Qualcomm 15yr Deep](21_Qualcomm_15yr_System_Design_Deep_Interview.md) | [Back to Index →](../ReadMe.Md)
