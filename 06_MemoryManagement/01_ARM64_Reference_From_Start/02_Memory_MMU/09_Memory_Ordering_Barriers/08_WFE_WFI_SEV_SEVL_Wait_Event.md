# WFE, WFI, SEV, SEVL: Wait and Event Instructions

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
ARM64 provides CPU idle / wait instructions to reduce power consumption
and cache coherency traffic when spinning on a condition.

WFI (Wait For Interrupt):
  The CPU enters a LOW-POWER state (micro-sleep)
  Wakes up on: IRQ, FIQ, SError, reset, debug event
  Used by: OS idle loop (when no runnable tasks)
  Does NOT flush caches or TLBs; just halts instruction dispatch
  
WFE (Wait For Event):
  The CPU enters a LOW-POWER state similar to WFI
  Wakes up on: SEV/SEVL instruction, interrupt, STLR to monitored address, ESB event
  
  KEY DIFFERENCE from WFI:
  WFE is designed for SPINLOCK/SPIN-WAIT scenarios
  The CPU can wake on a STLR (Store-Release) by another CPU
  → Spinlock holder releases lock via STLR → ALL CPUs executing WFE wake up
  → Spinlocks with WFE use dramatically LESS power and bus traffic
  
SEV (Send Event):
  Broadcasts an event to ALL CPUs in the system
  Causes ALL sleeping WFE on all CPUs to WAKE UP
  Used for: general broadcast wakeup (rare in Linux spinlocks, more for boot)
  
SEVL (Send Event Local):
  Sends an event to THIS CPU only
  Used to "prime" the WFE loop: if condition already true, don't sleep
  Pattern: SEVL → WFE → check condition (if already set: first WFE is cheap)
```

---

## 2. WFI: CPU Idle Implementation

```
WFI usage in Linux (arch/arm64/kernel/idle.c):

cpu_do_idle():
  dsb(sy)       // DSB SY: ensure all memory ops complete before idle
  wfi()         // enter low-power state
  
Why DSB SY before WFI?
  Ensures all pending stores (DMA completions, MMIO writes) are visible
  to other observers BEFORE the CPU enters the idle state
  Without DSB: CPU may idle while store buffer still has pending writes!
  
Power states from WFI:
  Implementation-defined how deep the CPU idles:
  
  Level 1 (clock gating):
    CPU pipeline clock gated off
    L1 cache: powered, coherent
    Wake latency: 1–5 µs
    
  Level 2 (retention):
    CPU state retained (registers, L1 cache)
    Less power, longer wake latency: 5–50 µs
    
  Level 3 (power down):
    CPU powered off (state saved to DRAM by ARM Trusted Firmware)
    L1/L2 cache flushed (DC CISW before power-off)
    Wake latency: 100+ µs
    
  Deeper states: ARM CPU Power States Standard (PSCI)
    PSCI_CPUIDLE_ENTER: communicate desired state to firmware
    Firmware (ATF) handles actual power control

WFI in hypervisor (HCR_EL2.TWI):
  If HCR_EL2.TWI = 1: WFI in EL0/EL1 traps to EL2
  KVM uses this to: receive guest idle notifications
  KVM then: schedules other vCPUs or enters host idle
  Guest OS: executes WFI normally (from its perspective)
  KVM intercepts: switches to another VM or calls host WFI
```

---

## 3. WFE: Efficient Spinlock Waiting

```
Without WFE (polling spinlock — power/bus waste):
  while (!try_lock(&lock));
  // → CPU continuously:
  //   1. Issues LOAD to lock variable (LDAR or LDR)
  //   2. Result: lock is held (1)
  //   3. Immediately issues another LOAD
  //   → CONSTANT CACHE SNOOPING traffic on the bus!
  //   → CPU at ~100% power consumption while spinning
  
With WFE (event-based spinlock):
  SEVL             // ensure we don't sleep if already unlocked
  1: WFE           // sleep until: STLR to lock addr, interrupt, or event
     LDAXR W0,[X1] // load-acquire lock value
     CBNZ W0, 1b   // if locked: go back to WFE sleep
     // fall through: lock was 0 (or we try to acquire with STXR)

Hardware mechanism:
  When CPU does WFE: hardware records it's waiting
  When another CPU does STLR to the same address:
    Hardware detects: "a CPU is WFE-waiting for this address"
    Sends WAKE signal to the waiting CPU
    Waiting CPU exits WFE and re-executes LDAXR
    
  The wake granule: same as exclusive monitor granule (~cache line)
  Any STLR to the same cache line wakes ALL WFE-waiting CPUs
  → False wakeups possible (benign: just re-check condition)

ARM64 LDXR/WFE spinlock (arch/arm64/include/asm/spinlock.h):

arch_spin_lock():
  PRFM PSTL1STRM, [x0]     // prefetch lock for streaming store
  
  // Ticket allocation:
  MOV   x2, #(1 << TICKET_SHIFT)
  LDAXR w0, [x0]            // Load-Acquire Exclusive: get ticket+owner
  ADD   w1, w0, w2          // increment next ticket
  STXR  w2, w1, [x0]       // Store Exclusive: try to get my ticket
  CBNZ  w2, retry           // retry if exclusivity lost
  
  // Extract my ticket (next field from old value)
  LSR   w1, w0, #16
  AND   w0, w0, #0xFFFF     // owner field
  CMP   w1, w0              // if my_ticket == owner: lock acquired
  BEQ   acquired
  
  // Spin using WFE:
  SEVL               // "set event local" — primes the WFE
  WFE_loop:
    WFE              // sleep: wake on STLR to lock address
    LDAXRH w0, [x0]  // Load-Acquire Half: re-read owner ticket
    CMP    w1, w0
    BNE    WFE_loop   // owner not our ticket yet → sleep again
  
acquired:
  // We have the lock!
  
arch_spin_unlock():
  STLRH  WZR, [x0]   // Store-Release Half: increment owner ticket
  // This STLR wakes ALL WFE-sleeping CPUs waiting on this lock!
  // Each wakes, re-checks if its ticket matches, only one proceeds
```

---

## 4. SEV and SEVL Details

```
SEV (Send Event):
  Sends event to ALL CPUs (broadcast, Inner Shareable domain)
  Causes ALL sleeping WFE instructions on ALL CPUs to wake up
  
  When to use SEV:
    After changing a variable that multiple CPUs may be waiting on
    When you don't know WHICH CPU (or many CPUs) need to wake
    
  WARNING: SEV wakes ALL CPUs in the IS domain
    On 16-core system: 16 WFEs wake up, 15 go back to sleep
    Performance impact: use when truly broadcasting
    Spinlock unlock uses STLR (not SEV) — STLR is more targeted
    
SEVL (Send Event Local):
  Sends event ONLY to the current CPU
  Costs almost nothing (no interconnect traffic)
  
  Why useful in WFE loops?
  
  Problem without SEVL:
    CPU checks condition: condition is TRUE
    But WFE hasn't been executed yet
    CPU executes WFE: sleeps until NEXT event
    → CPU misses the condition! Stuck sleeping!
  
  Solution with SEVL:
    SEVL           // set event LOCAL: pre-prime the event register
    WFE_loop:
      WFE          // if event pending (from SEVL): returns immediately!
      check condition
      BNE WFE_loop
    
  If condition already set: first WFE returns immediately (from SEVL)
  → No unnecessary sleep
  
Event register (per-CPU):
  ARM64 CPUs have a 1-bit "event register"
  SEVL: sets this register to 1
  WFE: checks this register; if 1: clears it and returns immediately
       if 0: enters sleep until event arrives

Timekeeping while WFE:
  ARM Generic Timer: continues counting while CPU is in WFE state
  CNTP_CVAL_EL0 (physical timer compare): generates virtual interrupt → wakes WFE
  Linux scheduler: sets next timer interrupt to pre-determine WFE wakeup
```

---

## 5. ARM64 CPU Idle in Linux

```
ARM64 cpuidle framework (arch/arm64/kernel/):

struct cpuidle_state:
  .enter = arm64_enter_idle_state
  
arm64_enter_idle_state(dev, drv, index):
  1. arm_cpuidle_suspend(index)      // call PSCI or platform-specific
  2. If deep idle: save CPU state to stack, call PSCI_CPUIDLE_ENTER
  3. PSCI calls ATF (EL3): ATF controls power gating
  4. On wakeup: ATF restores state, returns to Linux
  
Shallow idle (WFI only):
  cpu_do_idle():
    dsb(sy)         // all memory ops complete
    wfi()           // brief low-power state (interrupts wake)
    // returns immediately on any interrupt
    
Deep idle (CPUidle with PSCI):
  psci_cpu_suspend():
    saves EL1 context (CPACR, TCR, MAIR, VBAR, TTBR, etc.) to DRAM
    DC CVAC (flush saved context to DRAM)
    DSB ISH
    calls PSCI SMC → ATF powers down CPU
    on wakeup: ATF calls back, restores context
    ISB (force context reload)

Power numbers (example: Cortex-A78):
  Active:      ~2W (per core, full load)
  WFI:         ~100mW (clock gated, L1 warm)
  Deep idle:   ~10mW (powered down, L1 lost)
  Off:          0W
```

---

## 6. Interview Questions & Answers

**Q1: Why do ARM64 spinlocks use WFE instead of a simple polling loop, and what is the role of SEVL in this?**

A simple polling loop (`while (READ_ONCE(lock) != 0);`) wastes CPU power and generates constant coherency traffic on the interconnect — the spinning CPU continuously reads the lock's cache line, which causes repeated snoop requests across all cores and keeps the CPU's L1 cache in "shared" state. On a 16-core system with 4 contending threads, all 4 threads spinning simultaneously means 4× the bus traffic.

WFE-based spinlocks work differently: after checking the lock and finding it held, the CPU executes `WFE` which puts the CPU in a low-power sleep state. No cache line reads occur during sleep. When the lock holder executes `STLR` to release the lock, the hardware detects that CPUs are WFE-waiting for that cache line address and sends wake events — only then do the waiting CPUs exit WFE and re-attempt the lock.

`SEVL` (Send Event Local) is the "priming" instruction used at the START of the wait loop. Without it, there's a race: the lock holder might release the lock (STLR) between the moment the waiter reads the lock value (sees it's taken) and the moment the waiter executes WFE. In that window, the STLR that would have woken the waiter has already occurred. The waiter then sleeps indefinitely. SEVL solves this by setting the CPU's local event register to 1 before the first WFE. The first WFE returns immediately (consuming the pre-set event), allowing the waiter to re-check the lock condition BEFORE sleeping.

---

## 7. Quick Reference

| Instruction | Wakes On | Used For |
|---|---|---|
| WFI | IRQ, FIQ, SError, reset | CPU idle (OS scheduler) |
| WFE | Event, STLR, SEV, interrupt | Spinlock waiting |
| SEV | — | Broadcast wakeup (all CPUs) |
| SEVL | — | Local event priming (WFE loop setup) |

| Pattern | Code | Purpose |
|---|---|---|
| CPU idle | DSB SY; WFI | OS scheduler idle |
| Spinlock wait | SEVL; WFE; LDAXR; cmp; BNE back | Efficient spin |
| Lock release | STLR to lock var | Wakes WFE spinners |
| Busy-wait with event | SEVL; WFE_loop: WFE; check | Primed polling loop |

| HCR_EL2 bit | Effect |
|---|---|
| TWI | Trap WFI in EL0/EL1 → EL2 (KVM guest idle) |
| TWE | Trap WFE in EL0/EL1 → EL2 |
