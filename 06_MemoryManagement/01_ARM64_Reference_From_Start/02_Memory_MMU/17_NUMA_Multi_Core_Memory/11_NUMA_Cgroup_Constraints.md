# NUMA Cgroup Constraints and Memory Binding Deep Dive

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), multi-socket systems with cgroups

---

## 1. Concept Foundation

cgroups can constrain processes to specific NUMA nodes, restricting memory allocation and task placement.

Use cases:
- isolate workloads to prevent cross-node interference
- enforce hard memory boundaries for multi-tenant environments
- improve performance isolation in container deployments

Key mechanisms:
- cpuset cgroup for CPU and memory node constraints
- memory cgroup for memory limits and node preferences

---

## 2. ARM64 Hardware Detail

### 2.1 Memory node enforcement

When cgroup restricts to nodes {0, 1}:
- memory allocations only use those nodes
- page migration restricted to within set
- allocation failure if no space on allowed nodes

### 2.2 CPU and memory asymmetry

CPUs can be on node 0, but memory can be allocated from node 1 or vice versa.
Constraints handle both dimensions independently.

### 2.3 NUMA topology visibility in cgroups

Cgroup hierarchy can be assigned node sets.
Child cgroups inherit or further restrict parent's node set.

---

## 3. Linux Kernel Implementation

### 3.1 cpuset cgroup interface

User controls:
- cpuset.cpus: allowed CPUs
- cpuset.mems: allowed memory nodes
- cpuset.sched_load_balance: enable/disable inter-node load balance

### 3.2 Enforcement at allocation time

When memory allocator is invoked for cgroup-constrained task:
1. check allowed node set
2. attempt allocation within set
3. if fails, may trigger reclaim within set
4. if still fails, OOM kill or return error

### 3.3 Page migration boundary

Migrate_pages called within cgroup context:
- can only migrate between allowed nodes
- prevents escape to disallowed nodes

### 3.4 Interaction with NUMA balancing

Auto-NUMA still runs but respects cgroup constraints.
May cause redundant probing if cpuset mems is already optimal.

---

## 4. Hardware-Software Interaction

Isolation scenario:
1. two containers on shared host.
2. container A: restricted to nodes {0, 1}.
3. container B: restricted to nodes {2, 3}.
4. each container NUMA-balances within its allowed set.
5. no cross-container interference on interconnect.

Benefit:
- predictable latency within container
- reduced contention on shared interconnect

---

## 5. Interview Q and A

Q1: Why separate cpuset.cpus and cpuset.mems controls?
Because CPU placement and memory allocation are independent concerns with different performance implications.

Q2: Can a container's CPUs be on node 0 while memory is restricted to node 1?
Yes. This is inefficient but possible, and the scheduler may prefer local CPUs if available.

Q3: What happens if cgroup memory limit plus node restriction cause OOM?
Task is OOM killed unless memory reclaim is sufficient within the constrained set.

Q4: How does Auto-NUMA behave under cpuset mems constraint?
It still samples access patterns and migrates pages, but only within allowed nodes.

Q5: Can a child cgroup further restrict parent's node set?
Yes. Child is subset of parent; it cannot expand beyond parent's allowed nodes.

Q6: What workload benefits most from node binding?
Latency-sensitive HPC or financial workloads where isolation and locality are critical.

---

## 6. Pitfalls and Gotchas

- Setting cpuset constraints so tight that OOM becomes likely.
- Mismatching CPU and memory node sets inadvertently.
- Forgetting that CPU affinity and memory binding are separate concerns.
- Assuming all tasks benefit from node isolation (some lose fairness).
- Not monitoring cross-node traffic even with cgroups enabled.

---

## 7. Quick Reference Table

| Control | Effect |
|---|---|
| cpuset.cpus | allowed CPU list for tasks in cgroup |
| cpuset.mems | allowed memory nodes for allocations |
| cpuset.sched_load_balance | enable inter-node scheduler balancing |

| Scenario | Typical outcome |
|---|---|
| both cpus and mems matched to same node | optimal local access |
| cpus on node 0, mems on node 1 | cross-node latency penalty |
| tight memory limit on small node set | potential OOM pressure |
