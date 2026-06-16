# Lock Contention and Memory Subsystem Impact

Category: Performance Engineering and Benchmarking  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

Lock contention is not just a scheduler problem — it is a memory subsystem problem. Atomic operations, memory barriers, and cache-line ownership transfers all generate coherence traffic that scales with contention.

---

## 2. ARM64 Hardware Detail

### 2.1 Atomic operation cost

ARM64 atomics evolved across ARMv8 versions:
- **ARMv8.0**: LL/SC pairs (`LDXR`/`STXR`) — retry loops under contention
- **ARMv8.1 LSE**: single-instruction atomics (`CAS`, `LDADD`, `SWP`) — better contention behavior
- **ARMv8.2+**: `CASA`/`CASL`/`CASAL` — acquire/release variants

Use LSE atomics on modern hardware: `gcc -march=armv8.2-a+lse` or `-moutline-atomics` for compatibility.

### 2.2 Memory barrier cost

| Barrier | Scope | Approximate Cost |
|---|---|---|
| `DMB ISHLD` | inner-shareable, load | low |
| `DMB ISH` | inner-shareable, all | moderate |
| `DSB ISH` | data sync inner-shareable | higher |
| `DSB SY` | full system | highest |
| `ISB` | instruction stream | high (pipeline flush) |

Acquire/release atomic instructions encode barriers efficiently — prefer them over explicit barriers.

### 2.3 Contention amplification

Under heavy contention, a single lock causes:
- cache line ping-pong between cores
- interconnect bandwidth consumption
- TLB pressure if lock is in different page from data

On large ARM64 SMP systems, contention costs grow super-linearly with core count.

---

## 3. Linux Kernel Implementation

### 3.1 Lock types and memory subsystem behavior

| Lock | Mechanism | Memory Behavior |
|---|---|---|
| spinlock | qspinlock (MCS variant) | per-CPU node, less bouncing |
| mutex | adaptive spin + sleep | shared lock word + wait list |
| rwlock | reader-writer | reader counter bouncing under load |
| seqlock | sequence counter + retry | readers do not bounce write line |
| RCU | reference + grace period | no write-side coherence on read |

### 3.2 Diagnosing lock contention

```bash
# Kernel lock contention
perf lock record -- workload
perf lock report

# Wait time per lock
echo 1 > /proc/sys/kernel/lock_stat
cat /proc/lock_stat

# BPF approach
bpftrace -e 'kprobe:_raw_spin_lock { @[ksym(arg0)] = count(); }'
```

### 3.3 Reducing memory subsystem impact

- prefer per-CPU data with periodic aggregation
- use RCU for read-mostly structures
- shard locks (lock striping) to reduce contention
- use lockless algorithms where correctness permits
- align lock structures to cache lines

### 3.4 Example: bad vs good counter

```c
// Bad: shared atomic counter — all cores bounce one line
atomic_t total_count;
atomic_inc(&total_count);

// Good: per-CPU counter with periodic merge
DEFINE_PER_CPU(long, local_count);
this_cpu_inc(local_count);

// Read: sum across CPUs occasionally
long sum = 0;
for_each_possible_cpu(cpu)
    sum += per_cpu(local_count, cpu);
```

---

## 4. Hardware-Software Interaction

Locks are coherence pathways. Every lock operation must:
1. acquire ownership of the lock cache line (coherence traffic)
2. execute atomic operation (potential retry on LL/SC)
3. execute memory barrier (pipeline and ordering effects)
4. release ownership (more coherence traffic on next acquirer)

The kernel's choice of lock primitive affects how much of this cost is hidden via per-CPU queueing (qspinlock) versus exposed (naive test-and-set).

---

## 5. Interview Q and A

Q1: Why are LSE atomics better than LL/SC under contention?  
LL/SC retries on every interference; LSE atomics complete in hardware in a single instruction.

Q2: What is qspinlock and why does it scale better?  
It's an MCS-style queued spinlock where each waiter spins on its own per-CPU cache line, eliminating contention on the lock variable itself.

Q3: When is RCU the right choice?  
For read-mostly data structures where readers must not block and writers are infrequent.

Q4: Why is `seqlock` cheap for readers?  
Readers do not modify the sequence counter; they only check it, so no coherence write traffic from readers.

Q5: What does `-moutline-atomics` do?  
Compiles atomics that dispatch to LL/SC or LSE at runtime based on CPU capability, allowing single binary for mixed deployments.

Q6: When does lock striping make things worse?  
When the access pattern hashes to one stripe predominantly, you still have hot contention plus higher memory footprint.

---

## 6. Pitfalls and Gotchas

- Using LL/SC atomics on ARMv8.1+ hardware — leaves significant performance on the table.
- Treating lock contention as a pure CPU stall — coherence traffic affects DRAM bandwidth too.
- Adding RCU without understanding grace period costs.
- Per-CPU counters without proper read-side aggregation logic.

---

## 7. Quick Reference Table

| Scenario | Best Primitive | Why |
|---|---|---|
| frequent reads, rare writes | RCU | no read-side cache bounces |
| read-mostly with consistency check | seqlock | cheap reader, no writes |
| short critical section | spinlock (qspinlock) | per-CPU queueing |
| long critical section | mutex | sleeps instead of spinning |
| high-frequency counter | per-CPU + aggregation | no contention on update |
| concurrent hash table | lock striping | reduces single-lock contention |
