# Direct Reclaim Path Deep Dive

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), memory pressure and direct reclaim

---

## 1. Concept Foundation

Direct reclaim occurs when an allocator cannot satisfy a request from free pages and must reclaim pages directly (synchronously) as part of the allocation path.

Contrast to kswapd (background daemon):
- direct reclaim is synchronous and task-local
- kswapd is background and system-wide
- both use similar page scanning algorithms

---

## 2. ARM64 Hardware Detail

### 2.1 I-cache and workload cache dynamics

Direct reclaim involves page scanning, disk I/O, and page clearing.

On ARM64:
- page scanning is memory-access intensive
- I-cache pressure if scan loop is large
- memory barriers in page flags updates

### 2.2 GFP flag semantics

GFP_ATOMIC: no reclaim, fail if no free pages  
GFP_KERNEL: allow kswapd or direct reclaim  
GFP_USER: allow reclaim but not FS operations  
GFP_THISNODE: reclaim within single node only

---

## 3. Linux Kernel Implementation

### 3.1 Allocation path entry

get_page_from_freelist() fails → __alloc_pages_slowpath() → invoke_shrink_slab and shrink_zones recursively.

### 3.2 Direct reclaim invocation

shrink_zones() iterates zones in zonelist order.
For each zone: call shrink_lruvec() to scan LRU lists.

### 3.3 LRU page scanning

For each list (LRU_INACTIVE_FILE, LRU_ACTIVE_FILE, LRU_INACTIVE_ANON, LRU_ACTIVE_ANON):
1. isolate page
2. test reclaim candidate (referenced, pinned, etc.)
3. if cleanable: move to batch
4. if unmovable: skip
5. shrink_page_list() processes batch

### 3.4 Latency implications

Direct reclaim is synchronous → requestor task stalls.
Latency-sensitive applications see hiccups during reclaim.

---

## 4. Hardware-Software Interaction

Direct reclaim scenario:
1. task calls malloc() which eventually hits page allocator
2. freelist empty, triggers direct reclaim
3. kswapd is behind or insufficient
4. synchronously scan and shrink LRU lists
5. pages dropped or swapped
6. retry allocation
7. if successful, return; task resumes
8. latency spike observed

Prevention strategies:
- maintain sufficient free pages via kswapd watermarks
- pre-allocate or reserve memory for critical paths
- tune memory limit to prevent direct reclaim

---

## 5. Interview Q and A

Q1: Why is direct reclaim worse than kswapd?
Direct reclaim stalls the requesting task; kswapd is background and doesn't stall user work.

Q2: Can you disable direct reclaim?
Partially (via GFP_ATOMIC or cgroup constraints), but system may OOM instead of reclaiming.

Q3: What is the latency cost of direct reclaim?
Highly variable; from microseconds (cache clear) to milliseconds (disk I/O); sometimes tens of ms.

Q4: How does cgroup memory.limit interact with direct reclaim?
Exceeding limit triggers direct reclaim within cgroup constraint; can cause OOM if tight limit.

Q5: Why does swappiness affect direct reclaim?
High swappiness directs reclaim toward anon pages over file-backed; low means file-backed priority.

Q6: How do you avoid direct reclaim for latency-critical code?
Reserve memory, use memory-locked regions, or isolate on dedicated nodes with sufficient free pages.

---

## 6. Pitfalls and Gotchas

- Assuming kswapd keeps system out of direct reclaim (it doesn't guarantee it).
- Configuring watermarks too high and triggering kswapd constantly.
- Setting memory limits too tight and forcing direct reclaim.
- Ignoring reclaim latency in latency-sensitive code paths.
- Over-swapping and causing reclaim to be dominated by I/O.

---

## 7. Quick Reference Table

| Condition | Outcome |
|---|---|
| freelist not empty | immediate allocation, no reclaim |
| kswapd keeping up | occasional direct reclaim at peaks |
| kswapd behind | frequent direct reclaim, latency spikes |
| memory limit hit | direct reclaim triggered until limit satisfied |

| Metric | Meaning |
|---|---|
| pgsteal_* counters | pages evicted by direct reclaim |
| direct_scan_* | direct reclaim scanning activity |
| allocstall_* | allocation stalls due to direct reclaim |
