# Hugetlb and Huge Page Reservation Deep Dive

Category: Huge Pages and Page Size Optimization  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

hugetlb provides explicit huge page allocation and reservation for deterministic, high-performance workloads.

Key differences from THP:
- explicit pre-allocation and reservation
- pool-based management
- no automatic fallback to base pages
- precise control over mapping granularity

Use cases:
- databases with predictable large-memory footprint
- HPC simulations requiring stable latency
- real-time applications needing guaranteed huge mappings

---

## 2. ARM64 Hardware Detail

### 2.1 Huge page sizes on ARM64

Common sizes with 4KB granule:
- 2MB (PMD level)
- 1GB (PUD level)
- potentially larger with CONFIG_ARM64_CONT_PMD or CONT_PTE hint

### 2.2 Contiguous hint and larger sizes

CONT hints allow firmware and hardware to treat multiple consecutive PTEs/PMDs as a unified larger block for TLB efficiency.
Example:
- 16 consecutive 2MB PMD entries with CONT hint = 32MB contiguous mapping

### 2.3 Memory layout and fragmentation

Huge page allocation success depends on:
- sufficient contiguous physical space
- memory fragmentation over time
- CMA or migratability constraints

---

## 3. Linux Kernel Implementation

### 3.1 Reservations and pools

hugetlb maintains pre-reserved pools:
- admin sets size at boot or dynamically
- pages held in reserve pool for allocation
- surplus pages allocated on demand if policy permits

### 3.2 Allocation path

Request for huge page:
1. check available pool pages
2. if pool exhausted, try dynamic allocation if enabled
3. return page or fail with clear error

### 3.3 Special mapping and permissions

hugetlb VMAs have distinct handling:
- no page-table reallocation during split
- stable mapping across fork and COW
- special hugetlb page descriptor management

### 3.4 Reclaim and pool management

Pool resizing involves:
- page migration to satisfy allocation
- eventual return to pool or general free state
- careful ordering to prevent fragmentation

---

## 4. Hardware-Software Interaction

Typical hugetlb workload path:
1. admin pre-allocates huge page pool via sysfs or boot parameter
2. application mmap with MAP_HUGETLB flag
3. kernel maps pre-reserved pages into VMA
4. application uses stable large-page mappings
5. on close or munmap, pages return to pool

Benefit:
- predictable allocation without fragmentation risk
- strong TLB efficiency for entire allocation

---

## 5. Interview Q and A

Q1: Why pre-reserve if THP provides automatic huge pages?
Because THP can fall back to base pages, while hugetlb fails cleanly if huge pages are unavailable.

Q2: What is allocation pool exhaustion?
When reserved huge pages are all allocated and surplus allocation is disabled or fails.

Q3: Why can hugetlb improve fork performance?
Huge mappings reduce page-table COW overhead during fork.

Q4: What is the CONT hint used for?
To hint TLB that multiple consecutive entries can be merged into larger TLB entries.

Q5: How do you size a hugetlb pool correctly?
Based on working set analysis and application memory requirements plus some headroom.

Q6: Can hugetlb be used for anonymous memory only?
Historically yes, though modern kernels extend support to tmpfs and other backing stores.

---

## 6. Pitfalls and Gotchas

- Under-allocating pool and hitting allocation failures at runtime.
- Forgetting to configure hugetlb support in kernel config.
- Assuming hugetlb pool size is automatically optimal.
- Mixing hugetlb and THP policies without understanding implications.
- Not accounting for fragmentation when calculating long-term pool sufficiency.

---

## 7. Quick Reference Table

| Feature | Purpose |
|---|---|
| hugetlb pool | pre-reserved huge pages for deterministic allocation |
| surplus pages | dynamically allocated huge pages beyond reservation |
| CONT hint | optimization for larger effective TLB entries |
| sysfs control | admin management of pool size and allocation policy |

| Size option | Typical benefit |
|---|---|
| 2MB pages | widely available, good for database footprints |
| 1GB pages | excellent for very large allocations, less fragmentation |
