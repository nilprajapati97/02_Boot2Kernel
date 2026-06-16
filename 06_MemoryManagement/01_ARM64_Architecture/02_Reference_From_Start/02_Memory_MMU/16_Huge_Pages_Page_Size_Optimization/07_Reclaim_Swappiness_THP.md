# Page Reclaim and Swappiness in Huge Page Context Deep Dive

Category: Huge Pages and Page Size Optimization  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Huge page availability is also affected by page reclaim behavior and memory pressure policy.

Key tensions:
- huge pages provide efficiency but occupy larger ranges
- reclaim must free space for other needs without destroying huge pages if possible
- swappiness influences anonymous versus file-backed reclaim balance

---

## 2. ARM64 Hardware Detail

### 2.1 Reclaim impact on fragmentation

When reclaim targets memory region containing huge pages:
- huge page may be split if partial reclaim is needed
- alternative: whole huge page evicted if pressure is high
- result: fragmentation increases if split frequently

### 2.2 Swap and huge pages

Huge anonymous pages are candidates for swap under pressure.
Typical behavior:
- split to base pages before swapping (some systems)
- or swap in bulk with MADV_NOHUGEPAGE hinting

### 2.3 TLB and reclaim interaction

Reclaim that splits huge mappings:
- reduces TLB entry efficiency immediately
- fragments page-table structures
- may regain huge pages later via background collapse

---

## 3. Linux Kernel Implementation

### 3.1 Reclaim and compaction interplay

Under memory pressure:
1. reclaim runs to free pages
2. fragmentation may actually increase initially
3. compaction may run afterward to defragment
4. THP allocation may succeed if compaction is effective

### 3.2 Swappiness and huge page policy

swappiness sysctl influences:
- ratio of anonymous reclaim versus file-backed
- high swappiness favors swapping out huge anonymous pages
- low swappiness favors paging in file pages

### 3.3 Memory pressure signals

Kernel adjusts behavior based on pressure:
- low pressure: THP and other optimizations prioritized
- high pressure: aggressive reclaim and fragmentation accepted

### 3.4 Madvise hints and reclaim

Madvise can heuristically guide reclaim:
- MADV_WILLNEED: suggest keeping mapped
- MADV_DONTNEED: suggest early eviction
- MADV_NOHUGEPAGE: prevent future THP

---

## 4. Hardware-Software Interaction

Pressure scenario:
1. system runs heavy workload with huge pages
2. new allocation demand arrives
3. reclaim triggered to free space
4. reclaim may split or evict huge pages
5. pressure relieves or mounts further
6. fragmentation and THP efficiency may degrade

Long-term behavior:
- repeated pressure cycles degrade huge page sustainability
- compaction and background collapse help recovery
- stable workloads recover THP efficiency over time

---

## 5. Interview Q and A

Q1: Why does reclaim sometimes hurt huge page efficiency?
Because splitting huge pages to reclaim space fragments memory further.

Q2: What is the tradeoff between swappiness and huge pages?
High swappiness evicts huge anonymous pages readily; low swappiness preserves them but may fill up memory.

Q3: Can madvise NOHUGEPAGE harm reclaim?
It hints against future huge pages, which may be appropriate for sparse or write-heavy regions.

Q4: How does background collapse help after reclaim?
By reassembling base pages back into huge pages when memory allows.

Q5: What pressure level favors huge page maintenance?
Low to moderate pressure where reclaim is occasional and compaction can keep up.

Q6: Why is this interaction complex for interviewers to understand?
Because it involves competing goals: memory efficiency, reclaim urgency, and huge page preservation.

---

## 6. Pitfalls and Gotchas

- Assuming high swappiness and huge pages coexist well.
- Not monitoring reclaim-induced huge page split counters.
- Tuning only allocator policy while ignoring reclaim balance.
- Failing to measure pressure-phase behavior before optimizing globally.
- Confusing background collapse frequency with allocation success.

---

## 7. Quick Reference Table

| Pressure state | Typical huge page behavior |
|---|---|
| low | THP allocations succeed, minimal splitting |
| moderate | occasional splits, some compaction activity |
| high | frequent splits, fragmentation risk, hard to maintain |

| Control | Effect on huge pages |
|---|---|
| swappiness high | anonymous THP evicted earlier |
| compaction aggressive | faster reclaim of split fragments |
| MADV_NOHUGEPAGE | prevents future THP for region |
