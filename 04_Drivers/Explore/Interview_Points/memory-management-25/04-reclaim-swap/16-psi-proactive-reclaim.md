# Q16 — PSI (Pressure Stall Information) & Proactive Reclaim

> **Subsystem:** Reclaim · **Files:** `kernel/sched/psi.c`, `mm/vmscan.c` (`memory.reclaim`), `include/linux/psi.h`
> **Interviewer is really probing (Google favorite):** Do you understand how the kernel **quantifies
> memory pressure** (PSI), and how **proactive reclaim** + userspace OOM (systemd-oomd / lmkd) use it?

---

## TL;DR Cheat Sheet

- **PSI (Pressure Stall Information)** measures the **% of time tasks are stalled** waiting on a resource —
  for **memory**, **CPU**, **IO**, and **IRQ**. It answers "how much is this resource **hurting**
  progress?" — a far better signal than raw free-memory counts.
- Exposed at **`/proc/pressure/{memory,cpu,io}`** (system) and **`memory.pressure`/`io.pressure`/
  `cpu.pressure`** per **cgroup** (Q22). Two metrics:
  - **`some`** = time **at least one** task stalled on the resource (latency/contention signal).
  - **`full`** = time **all** non-idle tasks stalled (the resource is a hard bottleneck — productivity lost).
  Each gives **avg10 / avg60 / avg300** (running averages) and a **total** stall counter.
- **Why PSI:** free memory / load average are **misleading** (lots of "free" memory may be reclaimable
  cache; high load may be fine). PSI directly measures **lost work due to pressure** → the right trigger
  for action.
- **Proactive reclaim:** instead of waiting for the kernel to hit watermarks (Q-reclaim), userspace (or a
  cgroup knob) **reclaims memory ahead of demand**: write a byte count to **`memory.reclaim`** (cgroup v2)
  to reclaim from a cgroup; agents use **PSI** to decide how much, keeping pressure low and freeing cold
  memory before it causes stalls.
- **Userspace OOM:** **systemd-oomd** (servers/desktop) and **Android lmkd** watch **PSI** and kill /
  reclaim **before** the in-kernel OOM killer fires — graceful, policy-driven, responsive.
- **PSI triggers:** userspace can register a **threshold** (e.g. "tell me if memory `some` > 50 ms in any
  1 s window") via `poll()` on the PSI file → event-driven response.

---

## The Question

> What is PSI and why is it better than free memory or load average? How is it used for proactive reclaim
> and userspace OOM (systemd-oomd / Android lmkd)?

---

## Why PSI exists

Traditional health signals are **bad at predicting trouble**:

- **Free memory is misleading:** Linux deliberately uses "free" RAM for **page cache** (Q11), so
  `MemFree` is usually low and says little — most of it is **reclaimable**. A box with 100 MiB free might
  be perfectly healthy (reclaimable cache) or about to thrash (all anon) — `free` can't tell you.
- **Load average is misleading:** it counts runnable + uninterruptible tasks but doesn't distinguish "busy
  and productive" from "stalled waiting on memory/IO." High load can be fine; low load can hide stalls.
- **You learn too late:** by the time the kernel is in **direct reclaim** or invokes the **OOM killer**
  (Q-reclaim), latency has already spiked and a process is about to die. There's no early, **quantitative**
  "this is starting to hurt" signal.

**PSI fixes this by measuring the thing you actually care about: lost productivity due to resource
pressure.** It tracks how much **time tasks spend stalled** waiting for memory (e.g. in direct reclaim,
refault I/O, thrash), CPU, or IO. That's a **direct, normalized (%) measure of pain** that:

- predicts trouble **early** (pressure rises before OOM),
- is **comparable** across machines/workloads (it's a percentage of time),
- distinguishes **`some`** (some latency) from **`full`** (everything blocked — severe), and
- works **per-cgroup**, so you can attribute pressure to a specific container/app (Q22).

This enables a shift from **reactive** memory management (wait for watermarks/OOM) to **proactive**:
continuously reclaim cold memory and act on pressure **before** stalls and OOM. That's why Google/Meta
built PSI and why **systemd-oomd** and **Android lmkd** are built on it — it's the modern foundation of
responsive memory management.

---

## When PSI / proactive reclaim are used

| Use | Mechanism |
|-----|-----------|
| Detect memory pressure early | read `/proc/pressure/memory` or cgroup `memory.pressure` |
| Event-driven response | `poll()` a **PSI trigger** (threshold over a window) |
| Reclaim before stalls | write to cgroup **`memory.reclaim`** (proactive reclaim) |
| Userspace OOM (graceful) | **systemd-oomd** / **Android lmkd** act on PSI thresholds |
| Per-container attribution | cgroup v2 `*.pressure` files (Q22) |
| Autoscaling / scheduling | feed PSI into cluster schedulers (overcommit decisions) |

---

## Where in the kernel

```
kernel/sched/psi.c        <- PSI core: stall accounting, some/full, avg windows, triggers, poll
include/linux/psi*.h      <- psi_memstall_enter/leave, trigger API
mm/vmscan.c               <- reclaim accounts memstall; memory.reclaim handler (try_to_free_mem_cgroup_pages)
mm/memcontrol.c           <- cgroup memory.pressure, memory.reclaim (Q22)
/proc/pressure/{memory,cpu,io}      <- system-wide PSI
cgroup v2: memory.pressure, io.pressure, cpu.pressure, memory.reclaim
Userspace: systemd-oomd, Android lmkd
```

---

## How it works — mechanics

### 1. Stall accounting

The kernel marks regions where a task is **stalled on a resource**. For memory, `psi_memstall_enter()`/
`psi_memstall_leave()` wrap places where a task can't make progress due to memory: **direct reclaim**,
**refault** waits (faulting back a recently-evicted page, Q11/Q15), **swap-in** waits (Q14),
**thrashing**, **compaction** stalls. PSI aggregates, per CPU and per cgroup, the time **some** vs **all**
runnable tasks are in such stalls.

### 2. `some` vs `full`

```
some(memory) = fraction of time AT LEAST ONE task is stalled on memory  -> latency/contention is present
full(memory) = fraction of time ALL non-idle tasks are stalled on memory -> NOTHING productive runs (severe)
```
- **`some`** rising means memory pressure is causing **some** latency — an early warning.
- **`full`** rising means the system/cgroup is **bottlenecked** on memory — work is essentially stopped;
  this is the strong signal for aggressive action (kill/reclaim).
PSI reports **avg10/avg60/avg300** (decaying averages over 10/60/300 s) and a monotonically increasing
**total** microsecond counter.

```
/proc/pressure/memory:
some avg10=12.34 avg60=8.10 avg300=3.20 total=123456789
full avg10=4.50  avg60=2.00 avg300=0.80 total=45678901
```

### 3. PSI triggers (event-driven)

Polling files is crude; PSI supports **triggers**: userspace writes a spec like
`some 150000 1000000` ("notify me if `some` memory stall exceeds **150 ms** within any **1 s** window")
to the PSI file, then **`poll()`/`epoll`** on the fd. The kernel wakes the waiter the instant the
threshold is crossed → **low-latency, event-driven** pressure response without busy-polling. This is how
**lmkd**/`oomd` react within milliseconds.

### 4. Proactive reclaim via `memory.reclaim`

Classic reclaim is **reactive** — it runs when the kernel hits **watermarks** (Q-reclaim) or a memcg hits
its limit (Q22), i.e. **after** memory is already tight. **Proactive reclaim** flips this: a userspace
agent (or admin) **reclaims cold memory ahead of time** by writing a byte count to a cgroup's
**`memory.reclaim`** file:

```
echo 256M > /sys/fs/cgroup/<group>/memory.reclaim   # reclaim ~256 MiB of cold pages from this cgroup now
```
The kernel runs targeted reclaim (`try_to_free_mem_cgroup_pages`) on that cgroup — with **MGLRU** (Q15)
this accurately targets the **coldest generation**, so you free genuinely cold pages **without** evicting
hot ones. Agents use **PSI** as the controller: reclaim while pressure stays low (cold memory), back off
when pressure rises (you're hitting the working set). This keeps utilization high and pressure bounded —
the core of datacenter memory efficiency (pack more workloads, reclaim their cold memory proactively).

### 5. Userspace OOM: systemd-oomd & lmkd

The in-kernel OOM killer (Q-reclaim) is a **blunt last resort** — it fires late and picks by `oom_score`.
**Userspace OOM** does better, driven by PSI:

- **systemd-oomd** (servers/desktops): monitors **cgroup PSI** (and swap usage); when a cgroup's memory
  pressure exceeds a configured threshold for a duration, it **kills** the cgroup (or its worst unit)
  **gracefully and by policy** — before global OOM, with better selection.
- **Android lmkd** (Low Memory Killer Daemon): watches **PSI** thresholds and kills **background apps** by
  priority **before** the system thrashes or in-kernel OOM fires, keeping the **foreground** app
  responsive. It replaced the old in-kernel lowmemorykiller.

Both act **earlier, with policy**, using PSI as the trigger — far better UX/SLO than waiting for the kernel
OOM killer.

---

## Diagrams

### PSI as the controller

```mermaid
flowchart TD
    K["kernel stalls: direct reclaim, refault, swap-in, thrash, compaction"] --> PSI[PSI accounting: some/full, avg10/60/300]
    PSI --> P1["/proc/pressure/memory + cgroup memory.pressure"]
    P1 --> AG[userspace agent: systemd-oomd / lmkd / balancer]
    AG -->|pressure low| RECLAIM["memory.reclaim: proactively free COLD pages (MGLRU, Q15)"]
    AG -->|pressure high (full rising)| KILL[graceful kill: worst cgroup / background app]
    AG -->|trigger| POLL["poll() PSI trigger: event-driven, ms latency"]
```

### Reactive vs proactive

```
reactive:   memory tight -> watermark/limit hit -> direct reclaim (stall!) -> maybe OOM (kill, late)
proactive:  PSI low -> memory.reclaim cold pages continuously -> pressure stays low,
            PSI rising -> back off / graceful kill BEFORE stalls & kernel OOM
```

---

## Annotated C / interfaces

```c
/* Kernel marks memory stalls (kernel/sched/psi.c via callers in mm/). */
void psi_memstall_enter(unsigned long *flags);  /* entering a memory stall region */
void psi_memstall_leave(unsigned long *flags);  /* leaving it */
/* called around direct reclaim, refault wait, swap-in, etc. */

/* Proactive reclaim handler (cgroup v2, mm/memcontrol.c). */
/* echo <bytes> > memory.reclaim  -> try_to_free_mem_cgroup_pages(memcg, nr_pages, ...) */
```

```bash
# Read system pressure:
cat /proc/pressure/memory
# some avg10=.. avg60=.. avg300=.. total=..
# full avg10=.. avg60=.. avg300=.. total=..

# Per-cgroup (v2):
cat /sys/fs/cgroup/myservice/memory.pressure
echo 128M > /sys/fs/cgroup/myservice/memory.reclaim   # proactive reclaim

# Register a trigger (pseudo): write "some 150000 1000000" then poll() the fd
# (150 ms stall within any 1 s window) -> event on threshold crossing.

# Userspace OOM:
systemctl status systemd-oomd          # servers/desktop
# Android: lmkd reads PSI thresholds from init.rc props
```

> Senior nuance: PSI's power is that it measures **lost work**, not resource *levels* — `some` is your
> **early-warning latency** signal, `full` is your **"act now"** signal. Combined with **`memory.reclaim`
> + MGLRU**, it turns memory management from **reactive (watermarks/OOM)** into a **closed-loop
> controller**: reclaim cold memory while pressure is low, stop/kill when pressure spikes.

---

## Company Angle

- **Google/Meta (the headline):** PSI was created here; expect deep questions — `some` vs `full`, triggers,
  **proactive reclaim** (`memory.reclaim`) + MGLRU (Q15) for datacenter memory **overcommit/efficiency**,
  **oomd** policy, feeding PSI into cluster schedulers. This is core SRE/kernel territory.
- **Qualcomm/Android:** **lmkd** driven by PSI replaced the in-kernel lowmemorykiller; PSI thresholds for
  foreground responsiveness; PSI + zram (Q14) + MGLRU (Q15) is the Android memory stack.
- **AMD/Intel (scale):** PSI for NUMA/large-memory pressure attribution, autoscaling, IO/CPU pressure too.
- **NVIDIA:** PSI on shared GPU/CI hosts to detect memory pressure before OOM.

---

## War Story

*"A multi-tenant node was getting **OOM kills** that took down latency-sensitive services — the in-kernel
OOM killer fired **late** and sometimes picked the **wrong** victim by `oom_score`. Worse, **free memory**
looked fine in monitoring right up until the cliff, because most of it was reclaimable cache (Q11) — so our
dashboards gave **no early warning**. We switched the signal to **PSI**: `cat /proc/pressure/memory` and
per-cgroup `memory.pressure` showed `some` climbing well **before** the OOM, and `full` spiking at the
cliff. We deployed **systemd-oomd** with a policy on cgroup PSI (kill the worst **batch** cgroup when its
memory `full` pressure exceeded a threshold for N seconds), so noisy batch jobs were killed **gracefully
and correctly** before they could OOM the latency services. We also added **proactive reclaim** — an agent
wrote to `memory.reclaim` on idle cgroups while PSI was low (with **MGLRU**, Q15, this freed genuinely cold
pages) — raising utilization without pressure. OOM kills of the wrong services stopped. The interviewer's
follow-up — *'why not just alert on free memory?'* — let me explain free memory is mostly reclaimable cache
and a terrible predictor; **PSI measures actual stall time**, so it warns early and attributes pressure to
the right cgroup."*

---

## Interviewer Follow-ups

1. **What does PSI measure?** The **% of time tasks are stalled** waiting on memory/CPU/IO — lost
   productivity due to pressure — not resource levels.

2. **`some` vs `full`?** `some` = at least one task stalled (early latency signal); `full` = all non-idle
   tasks stalled (severe bottleneck → act now).

3. **Why is PSI better than free memory / load average?** Free memory is mostly reclaimable cache
   (misleading); load average mixes busy and stalled. PSI directly measures **pain** and predicts trouble
   early, per-cgroup.

4. **How do you get event-driven PSI?** Register a **trigger** (threshold over a window) on the PSI file
   and `poll()`/`epoll` it — wakes on threshold crossing (ms latency), no busy-polling.

5. **What is proactive reclaim?** Reclaiming cold memory **ahead** of demand (e.g. writing to cgroup
   `memory.reclaim`), driven by PSI, instead of waiting for watermarks/limits — keeps pressure low and
   utilization high.

6. **How does MGLRU help proactive reclaim?** Its accurate aging targets the **coldest** pages (Q15), so
   proactive reclaim frees cold memory without evicting the hot working set.

7. **systemd-oomd vs in-kernel OOM?** oomd acts in **userspace**, **earlier**, by **policy**, on **PSI**
   (and swap), killing the worst cgroup gracefully — vs the kernel OOM killer's late, `oom_score`-based
   last resort.

8. **What is Android lmkd?** A PSI-driven daemon that kills background apps by priority before thrash/OOM
   to keep the foreground responsive; replaced the in-kernel lowmemorykiller.

9. **Where does the kernel account memory stalls?** Around direct reclaim, refault waits, swap-in,
   thrash, and compaction (`psi_memstall_enter/leave`).

---

## 30-Minute Talk Track

| Min | Cover |
|-----|-------|
| 0–4 | Why free memory/load average mislead; need a direct "pain" signal; learn too late at OOM |
| 4–9 | PSI: stall accounting; some vs full; avg10/60/300; /proc/pressure + cgroup files |
| 9–13 | What gets accounted: direct reclaim, refault, swap-in, thrash, compaction |
| 13–16 | PSI triggers: threshold over window + poll() → event-driven ms response |
| 16–21 | Proactive reclaim: memory.reclaim + MGLRU targeting cold pages; reactive vs proactive |
| 21–26 | Userspace OOM: systemd-oomd (cgroup PSI policy) and Android lmkd; earlier, graceful, correct |
| 26–28 | Per-cgroup attribution (Q22); feeding PSI to schedulers/autoscaling |
| 28–30 | War story (PSI-driven oomd + proactive reclaim) + "measure pain, not levels" |
