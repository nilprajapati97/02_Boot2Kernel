# OOM Killer Tuning and Prevention

Category: Production Memory Incident Response  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

The OOM killer is a last-resort mechanism. Repeated OOM kills are a symptom of policy failure, not a memory management strategy.

Goals for production systems:
- prevent OOM before it triggers
- control kill targets when it does
- recover without cascading failure

---

## 2. ARM64 Hardware Detail

### 2.1 Physical exhaustion path

ARM64 systems with large core counts can exhaust memory faster under concurrent allocation bursts.

Nodes can diverge in pressure: one node OOMs while another remains free if policy does not redistribute.

### 2.2 Per-zone watermarks

OOM is triggered per-zone after all direct reclaim paths fail. NUMA topology means zone/node distribution is a direct factor.

---

## 3. Linux Kernel Implementation

Key components:
- `oom_score` and `oom_score_adj` per process
- `select_bad_process()` — victim selection algorithm
- cgroup v2 `memory.max` — invokes cgroup-local OOM
- `oom_kill_allocating_task` sysctl — kill allocating task vs highest scorer

### 3.1 Prevention policy

1. set meaningful `memory.max` per cgroup
2. reserve headroom with `memory.low` for critical services
3. use `memory.high` to throttle aggressors before OOM
4. deploy PSI alerts to catch pressure early

### 3.2 Tuning oom_score_adj

| Value range | Meaning |
|---|---|
| -1000 | never kill (protected) |
| -999 to 0 | increasingly willing to kill |
| 1 to 1000 | increasingly likely to kill |

Set critical services to -1000, batch/disposable services to 500-1000.

### 3.3 Postmortem signals

- kernel ring buffer: `Out of memory: Kill process`
- cgroup `memory.events`: `oom` and `oom_kill` counters
- vmstat: `oom_kill` counter
- `/proc/<pid>/oom_score` for ongoing risk monitoring

---

## 4. Hardware-Software Interaction

When OOM fires, process teardown triggers extensive unmap/TLB-shootdown sequences. On large ARM64 systems with many-core CPUs, this can create secondary latency spikes as invalidations broadcast across the topology.

---

## 5. Interview Q and A

Q1: Why is repeated OOM kill a policy failure?  
OOM means limits and headroom were not configured to prevent exhaustion.

Q2: What is the safest per-workload control?  
cgroup v2 `memory.max` combined with `memory.low` for critical services.

Q3: What happens after `oom_kill`?  
Process teardown unmaps all pages and shoots down TLBs — secondary stalls are possible.

Q4: How do you protect a critical daemon?  
Set `oom_score_adj` to -1000 and enforce via service manager.

Q5: What is the earliest OOM warning?  
Rising PSI memory full sustained trend, well before OOM triggers.

Q6: What confirms prevention tuning is working?  
Zero `oom_kill` events over representative load intervals with stable p99.

---

## 6. Pitfalls and Gotchas

- Protecting all services at -1000: then nothing useful is killed when needed.
- Ignoring per-node pressure imbalance leading to node-local OOM.
- Tuning only global knobs without cgroup boundaries.
- Treating OOM kill as acceptable steady-state behavior.

---

## 7. Quick Reference Table

| Control | Protects | Risk of Misuse |
|---|---|---|
| `oom_score_adj = -1000` | critical services | must not be applied blindly |
| cgroup `memory.max` | limits blast radius | too low causes frequent cgroup OOM |
| `memory.low` | reserves local headroom | set too high starves other workloads |
| PSI alerting | early warning | false positives from short bursts |
