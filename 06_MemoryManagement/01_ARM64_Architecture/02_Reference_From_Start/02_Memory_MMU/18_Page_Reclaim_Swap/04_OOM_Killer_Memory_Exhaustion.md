# OOM Killer and Memory Exhaustion Deep Dive

Category: Page Reclaim and Swap  
Platform: ARM64 (AArch64), out-of-memory scenarios

---

## 1. Concept Foundation

Out-of-Memory (OOM) killer is the system's last resort when memory pressure cannot be resolved by reclaim.

Mechanism:
- kernel detects allocator repeatedly failing
- invokes OOM killer decision logic
- selects victim process
- force-kill victim to free memory

---

## 2. ARM64 Hardware Detail

### 2.1 Allocation failure indicators

GFP_FS | GFP_IO flags control what reclaim is allowed.
After exhausting all options, allocation declared impossible.

### 2.2 Signal delivery

OOM decision sends SIGKILL to victim.
Process is immediately terminated and pages freed.

---

## 3. Linux Kernel Implementation

### 3.1 OOM invocation

select_bad_process() analyzes all tasks.
Scoring based on:
- task_struct->oom_score (badness calculation)
- RSS (resident set size) estimation
- oom_adj tuneable override
- kernel threads excluded by default

### 3.2 Badness calculation

Simplified: task score = task_rss / oom_adj.
Larger RSS or lower oom_adj_score → higher target priority.

Tunable:
- /proc/[pid]/oom_adj: override badness
- /proc/[pid]/oom_score_adj: newer interface

### 3.3 Victim selection and killing

oom_kill_process():
1. send SIGKILL to selected task
2. also kill child processes in same mm_struct if they share memory
3. freed pages returned to allocator
4. retry allocation

### 3.4 cgroup OOM handling

Per-cgroup OOM killer:
- memory cgroup hits limit
- direct OOM killer invoked within cgroup
- only tasks in cgroup are candidates for killing

---

## 4. Hardware-Software Interaction

OOM scenario:
1. sustained memory pressure builds
2. kswapd unable to keep up
3. direct reclaim triggered, latency spikes
4. allocation still fails repeatedly
5. OOM killer activated
6. largest or lowest-oom_adj task selected
7. SIGKILL delivered
8. pages freed
9. waiting allocation retried and succeeds

System behavior:
- unpredictable which task dies
- performance cliff: sudden latency spike before crash
- data loss possible

---

## 5. Interview Q and A

Q1: How does OOM killer choose its victim?
Prioritizes large memory consumers with high badness score; tunable via oom_adj.

Q2: Can you prevent a critical task from being OOM killed?
Yes, set /proc/[pid]/oom_adj to -17 (OOM_DISABLE); should be used cautiously.

Q3: Why does OOM killer sometimes seem to pick wrong process?
RSS estimation may be inaccurate or shared memory may skew scoring.

Q4: What is oom_adj vs oom_score_adj?
oom_adj is older (-17 to 15 range); oom_score_adj is newer and preferred.

Q5: Can cgroups prevent OOM killer?
Memory cgroup can set memory.limit_in_bytes to trigger reclaim before OOM, but doesn't prevent it entirely.

Q6: How do you recover from OOM killer event?
Immediate response: restart killed service if managed. Long-term: increase memory, tune limits, or reduce workload.

---

## 6. Pitfalls and Gotchas

- Setting oom_adj globally without understanding per-process implications.
- Assuming OOM killer only targets memory-heavy processes (it also considers age and configuration).
- Ignoring memory.limit_in_bytes in cgroups; allows OOM to happen in container.
- Not monitoring /proc/pressure/memory for early warning signs.
- Tuning single process oom_adj without coordinating with system policy.

---

## 7. Quick Reference Table

| Config | Effect |
|---|---|
| oom_adj=-17 | disable OOM killer for this process |
| oom_adj=+10 | increase target priority (more likely to be killed) |
| memory.limit_in_bytes (cgroup) | OOM killer within cgroup when limit exceeded |

| Observable | Meaning |
|---|---|
| kernel message "Out of memory" | OOM killer activated |
| SIGKILL delivery | victim process being terminated |
| sudden RSS drop | pages freed by OOM killer |
