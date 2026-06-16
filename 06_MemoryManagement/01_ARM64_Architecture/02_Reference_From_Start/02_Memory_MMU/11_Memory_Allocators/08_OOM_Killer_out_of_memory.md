# OOM Killer: Out-of-Memory Process Selection

**Category**: Linux Kernel Memory Allocators  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
OOM (Out Of Memory) killer: last resort when memory cannot be freed

Trigger chain:
  1. Process calls malloc() / mmap() → kernel page fault
  2. Page fault handler calls alloc_pages()
  3. alloc_pages() fails (all zones empty, watermarks violated)
  4. Direct reclaim: try to free pages (swap out, drop caches)
  5. Memory compaction: try to defragment
  6. Retry allocation
  7. Still fails → out_of_memory()
  8. Select a process to kill → kill it → free its memory → retry

When OOM is reached:
  System has either truly run out of memory
  OR: memory accounting is wrong (overcommit + swap exhausted)
  OR: process has mlocked huge amounts of memory
  OR: CMA/DMA allocations consumed too much physically contiguous memory

ARM64 context:
  Large ARM64 systems (256GB+ RAM): OOM less common for obvious reasons
  Mobile ARM64 (Android, 6–12GB): OOM common; Android has its own lmkd (Low Memory Killer Daemon)
  ARM64 embedded (1–4GB): OOM can happen with media/GPU workloads
  
  ARM64 OOM is the same kernel code as x86; no ARM64-specific OOM behavior
  EXCEPT: memory zones (DMA zone different sizes)

Key design principles:
  - Kill ONE process (not many): give it a chance to free enough memory
  - Kill the "best" victim: maximize freed memory, minimize collateral damage
  - Kill quickly: OOM killer must not get stuck
  - Avoid killing critical system processes
```

---

## 2. OOM Badness Score

```c
/* mm/oom_kill.c */

long oom_badness(struct task_struct *p, unsigned long totalpages)
{
    // 1. Get process's RSS (Resident Set Size) in pages:
    points = get_mm_rss(p->mm);          // anonymous + file + shmem pages
    points += get_mm_counter(p->mm, MM_SWAPENTS);  // + swap usage
    
    // 2. Add children's memory usage:
    //    (killing parent also kills children → credit parent)
    for each child c of p:
        points += get_mm_rss(c->mm) + get_mm_counter(c->mm, MM_SWAPENTS);
    
    // 3. Apply oom_score_adj:
    //    /proc/pid/oom_score_adj: range -1000 to +1000
    //      -1000: never kill this process (OOM_SCORE_ADJ_MIN)
    //      +1000: always kill this process first
    //      0: neutral (default)
    //    Most browsers set renderer processes to +300–+500
    //    System daemons typically set to -900 or -1000
    
    adj = (long)p->signal->oom_score_adj;
    if (adj == OOM_SCORE_ADJ_MIN) return LONG_MIN;  // protected!
    
    points += adj * totalpages / 1000;
    // adj > 0: increase score (more likely to kill)
    // adj < 0: decrease score (less likely to kill)
    
    return max(1, (int)points);  // minimum score 1 (always killable unless adj=-1000)
}

// /proc/pid/oom_score: what user-visible OOM score is (derived from oom_badness)
//   Range: 0–1000
//   0: never killed
//   1000: first to be killed
```

---

## 3. OOM Kill Process

```c
bool out_of_memory(struct oom_control *oc)
{
    // 0. Check if someone else is already killing:
    if (task_will_free_mem(current)) return true;  // let current task die
    
    // 1. Notify memory cgroup if applicable:
    if (oc->memcg) {
        mem_cgroup_out_of_memory(oc->memcg, ...);
        return true;
    }
    
    // 2. Handle sysctl_panic_on_oom:
    //    /proc/sys/vm/panic_on_oom
    //    0: default (kill process)
    //    1: panic if no killable process found
    //    2: always panic on OOM
    if (sysctl_panic_on_oom == 2) panic("OOM: out_of_memory...");
    
    // 3. Select victim:
    p = select_bad_process(oc):
        best_points = 0;
        for_each_process(tsk):
            // Skip kernel threads (mm == NULL)
            if (!tsk->mm) continue;
            
            // Check oom_score_adj == OOM_SCORE_ADJ_MIN: skip
            if (tsk->signal->oom_score_adj == OOM_SCORE_ADJ_MIN) continue;
            
            points = oom_badness(tsk, oc->totalpages);
            if (points > best_points):
                best_points = points;
                victim = tsk;
        return victim;
    
    // 4. Kill victim:
    oom_kill_process(oc, "Out of memory"):
        pr_warn("Out of memory: Kill process %d (%s)...\n", ...);
        
        // Kill victim's process group? (optional, oc->kill_all)
        
        // Send SIGKILL:
        do_send_sig_info(SIGKILL, SEND_SIG_PRIV, victim, PIDTYPE_TGID);
        
        // Mark as OOM victim:
        set_bit(MMF_OOM_VICTIM, &victim->mm->flags);
        
        // Wake OOM reaper:
        wake_oom_reaper(victim);
    
    return true;
}
```

---

## 4. OOM Reaper

```
OOM Reaper: fast memory reclaim after OOM kill
  
  Problem before OOM reaper:
    Process receives SIGKILL
    Process may be blocked on I/O, sleeping in kernel
    Kernel can't actually free memory until process runs and exits
    System may stay stuck (no free memory) waiting for victim to exit
  
  OOM Reaper solution (Linux 4.6+):
    Dedicated kernel thread: oom_reaper (kthread)
    Woken immediately after SIGKILL sent to victim
    
    oom_reap_task_mm(tsk, mm):
      // Don't wait for process to exit!
      // Directly walk victim's page tables and unmap all anonymous pages
      
      acquire mmap_read_lock(mm) (or trylock — if locked, wait briefly)
      
      for each VMA in victim's mm:
        if (VM_LOCKED): skip (mlock'd — don't touch)
        if (VM_HUGETLB): skip (complex)
        
        // Zap all PTEs in this VMA:
        unmap_page_range(&tlb, vma, vma->vm_start, vma->vm_end, ...):
          for each PTE in VMA:
            if pte_present: 
              page_remove_rmap(page) → _mapcount--
              put_page(page) → _refcount--
              if _refcount == 0: free to buddy!
              pte_clear(pte)
      
      tlb_finish_mmu(&tlb): TLBI flush all released pages
      // ARM64: TLBI ASIDE1IS(ASID) — flush all entries for this process
      
      set_bit(MMF_OOM_SKIP, &mm->flags):  // Done — skip this mm in future OOM scans
  
  Result: memory freed in milliseconds instead of waiting for process exit
  Process still needs to exit eventually (kernel threads, files, etc.)
  But: memory (anonymous pages = largest part) freed immediately by reaper
```

---

## 5. Memory Cgroup OOM

```
memcg (memory cgroup) OOM: per-cgroup memory limits

  docker/container_d/systemd: sets memcg limits per container
  
  When container hits its memory limit:
    mem_cgroup_try_charge() → ENOMEM
    mem_cgroup_out_of_memory():
      select_bad_process() within THIS CGROUP only
      Kill highest-score process within cgroup
      Do NOT kill processes in other cgroups
  
  This is why containers "crash" (process killed) when exceeding limit
  rather than taking down the whole system
  
  /proc/pid/oom_score_adj: application can request protection
    container entrypoint process: -500 to -1000 (protect)
    worker processes: +100 to +300 (prefer these as victims)
  
  ARM64 Android (LMK: Low Memory Killer):
    Android uses a custom driver: /drivers/staging/android/lowmemorykiller.c
    (being replaced by LMKD userspace daemon)
    LMK kills processes proactively based on:
      /proc/sys/vm/lowmem_notify_adj_values (thresholds)
      Kills before actual OOM to keep system responsive
    Android processes: foreground (adj=0), background (adj=906), cached (adj=906+)
```

---

## 6. Interview Questions & Answers

**Q1: A process sets oom_score_adj to -1000. What exactly does this guarantee?**

Setting `oom_score_adj = -1000` means `oom_badness()` returns `LONG_MIN` for this process. The OOM killer's `select_bad_process()` skips any process returning `LONG_MIN`. This guarantees the OOM killer will NEVER select this process as a victim.

However, there are important caveats:
1. The kernel may still kill the process for OTHER reasons (SIGKILL from another process, watchdog, etc.)
2. If ALL processes have `oom_score_adj = -1000` and no unkillable process can free memory: the kernel panics with "Out of memory and no killable processes..." if `panic_on_oom = 1`, otherwise it gets stuck trying without success.
3. This requires `CAP_SYS_RESOURCE` to set (or root). Unprivileged processes can only INCREASE their score (less negative values).
4. Memory cgroup OOM: even with adj=-1000 at global level, cgroup OOM operates within the cgroup scope — check if cgroup OOM also respects the adj.

Typical use: `systemd` sets its own oom_score_adj to -1000. Init must not be killed.

**Q2: After OOM kill, why might the system still appear to have no free memory for a few seconds?**

Several reasons:

1. **OOM Reaper not yet run**: the OOM reaper is a separate kernel thread that must be scheduled and run before anonymous pages are freed. If system is heavily loaded, this scheduling delay can take hundreds of milliseconds.

2. **Process still alive (blocking in kernel)**: if the victim is blocked on uninterruptible I/O (disk read, NFS), it may not be able to receive SIGKILL immediately. The OOM reaper bypasses this for anonymous memory, but kernel stack/file descriptors remain.

3. **File-backed pages**: the OOM reaper only reclaims anonymous memory (can be freed immediately). File-backed pages require writeback (if dirty) before reclaim. This happens through the normal reclaim path, not the OOM reaper.

4. **Slab memory**: kernel slab memory allocated by the killed process is NOT freed by the OOM reaper. It's freed when the process fully exits.

5. **DMA pinned pages**: pages pinned for DMA (FOLL_PIN or `get_user_pages`) cannot be freed until the device DMA transfer completes.

After the victim exits completely, all these are freed. The few-second delay is usually dominated by items 1–2.

---

## 7. Quick Reference

| oom_score_adj | Effect |
|---|---|
| -1000 | Never kill (OOM_SCORE_ADJ_MIN) |
| -900 | Almost never kill (critical daemons) |
| 0 | Neutral (default) |
| +500 | Prefer to kill (browsers set renderer) |
| +1000 | Kill first (OOM_SCORE_ADJ_MAX) |

| /proc/sys/vm/panic_on_oom | Effect |
|---|---|
| 0 | Default: kill process |
| 1 | Panic if cannot kill |
| 2 | Always panic on OOM |

| OOM Timeline | Event |
|---|---|
| T+0ms | alloc_pages fails after reclaim+compact |
| T+1ms | out_of_memory() called, victim selected |
| T+2ms | SIGKILL sent, OOM reaper woken |
| T+5ms | OOM reaper unmaps anonymous pages (~1GB/s) |
| T+?ms | Process exits, remaining memory freed |
