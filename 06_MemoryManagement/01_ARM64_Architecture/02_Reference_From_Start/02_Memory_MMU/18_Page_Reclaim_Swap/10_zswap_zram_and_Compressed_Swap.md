# zswap, zram, and Compressed Swap Deep Dive

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Compressed swap reduces disk I/O by compressing pages in RAM before writing to backing swap.

Two key mechanisms:
- zswap: compressed cache in front of regular swap
- zram: compressed RAM block device used as swap

Goal:
- trade CPU cycles for lower I/O latency

---

## 2. ARM64 Hardware Detail

### 2.1 Compression vs memory bandwidth

ARM64 servers with strong cores can often compress quickly enough to reduce end-to-end stall time.

### 2.2 Cache and NUMA considerations

Compression pools on remote nodes can add latency; placement matters on multi-socket systems.

---

## 3. Linux Kernel Implementation

### 3.1 zswap path

On swap-out:
1. page is compressed
2. stored in zswap pool
3. if pool full/inefficient, page is written to disk swap

On swap-in:
1. check zswap pool
2. decompress if found
3. avoid disk read

### 3.2 zram path

zram provides a RAM-backed compressed block device.
Pages never hit disk unless an additional swap layer exists.

### 3.3 Tuning controls

Common knobs:
- compressor type
- zswap max pool percent
- zram device size

---

## 4. Hardware-Software Interaction

Observed behavior:
1. memory pressure rises
2. anon pages selected for swap
3. compression absorbs many pages in-memory
4. disk I/O drops, latency improves
5. CPU usage for compression increases

---

## 5. Interview Q and A

Q1: zswap vs zram in one line?
zswap caches swap pages before disk; zram is compressed swap device in RAM.

Q2: When is zram best?
Systems with slow storage or no swap disk, especially memory-constrained devices.

Q3: Main downside of compressed swap?
Extra CPU overhead and potentially higher memory metadata cost.

Q4: Can zswap eliminate disk swap entirely?
Not guaranteed; under heavy pressure pool spills to disk.

Q5: Which metric proves benefit?
Lower pswpin/pswpout disk impact with stable application latency.

Q6: Does compressed swap help all workloads?
No. Poorly compressible pages gain little.

---

## 6. Pitfalls and Gotchas

- Setting zswap pool too large and starving normal memory.
- Ignoring compressor choice impact on CPU budget.
- Assuming zram is free memory expansion.
- Not validating with workload-specific compressibility.

---

## 7. Quick Reference Table

| Mechanism | Description |
|---|---|
| zswap | compressed cache before backing swap |
| zram | compressed RAM block device used as swap |
| Compressor | algorithm balancing ratio and speed |
| Pool limit | cap to avoid over-consuming RAM |
