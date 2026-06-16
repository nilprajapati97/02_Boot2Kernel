# Swap Subsystem and Page Swapping Deep Dive

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), swap device and page swapping

---

## 1. Concept Foundation

Swap extends effective memory beyond physical RAM by using block device storage.

Trade-off:
- gain capacity
- lose latency (disk/NVMe is ~1000× slower than RAM)

Linux swap typically used for:
- cold memory when system is fully committed
- emergency overflow
- workloads with clear hot/cold separation

---

## 2. ARM64 Hardware Detail

### 2.1 Block device integration

Swap typically on fast storage: NVMe, SSD.
On slow devices: swap hurts more than helps.

### 2.2 Page I/O mechanics

Swapping out: page copy to swap device, PTE updated with swap entry  
Swapping in: I/O from device, page installed, PTE updated with physical address

---

## 3. Linux Kernel Implementation

### 3.1 Swap device registration

Kernel maintains swap_info[] array.
Each entry tracks device, total slots, used slots.

### 3.2 Reclaim path to swap

shrink_page_list() on anon pages evaluates:
- file-backed: drop or writeback
- anon: must swap (no file to flush to)

### 3.3 Swap PTE encoding

PTE can encode:
- physical address (mapped in RAM)
- swap entry (offset in swap device)

On PTE fault: if swap entry, trigger swapin.

### 3.4 Swap slot allocation

Bitmap-based allocator for swap device slots.
Tracks used and free slots.

---

## 4. Hardware-Software Interaction

Swap-in scenario:
1. page faulted, PTE contains swap entry
2. allocate new page frame
3. invoke I/O to fetch page content from swap device
4. page installed in memory
5. PTE updated with physical address
6. task resumes with memory now local

Latency consequence:
- page fault takes milliseconds instead of nanoseconds
- application sees pause

---

## 5. Interview Q and A

Q1: Why swap instead of just letting system OOM?
Swap provides graceful degradation; OOM kills tasks; prefer swap if latency can be absorbed.

Q2: How fast must swap device be to be worth using?
NVMe or fast SSD (1-10GB/s); slower (HDD at 100MB/s) often worse than no swap.

Q3: Can you have multiple swap devices?
Yes; kernel balances across them to spread I/O load.

Q4: What is swappiness and how does it control swap usage?
swappiness (0-100) tunable: higher means more aggressive swap; lower means prefer reclaim.

Q5: How does swap interact with NUMA?
Single swap device is typically remote from some sockets; latency penalty multiplied on NUMA.

Q6: What happens if swap device fills up?
Allocations fail; system hits OOM condition; cannot swap further.

---

## 6. Pitfalls and Gotchas

- Assuming swap is "free" memory (it's not; latency trade-off is real).
- Setting swappiness too high and thrashing on swap I/O.
- Not monitoring swap usage; can mask underlying memory problems.
- Using slow devices for swap (better to just OOM or increase RAM).
- Misconfiguring swap size; too small and it fills, too large and wastes space.

---

## 7. Quick Reference Table

| Config | Effect |
|---|---|
| swappiness=0 | never swap unless system running out of memory |
| swappiness=60 | default balance between file cache drop and anon swap |
| swappiness=100 | aggressive swap, minimal file cache eviction |

| State | Meaning |
|---|---|
| pages swapped out (pswpout) | pages moving to swap device |
| pages swapped in (pswpin) | pages moving from swap device |
| swap free (MemAvailable less MemFree) | indicates swap usage |
