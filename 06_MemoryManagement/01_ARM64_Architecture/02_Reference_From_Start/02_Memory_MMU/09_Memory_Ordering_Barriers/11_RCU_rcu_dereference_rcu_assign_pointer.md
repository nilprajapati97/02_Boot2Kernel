# RCU Memory Ordering: rcu_dereference and rcu_assign_pointer

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. RCU Concept Overview

```
RCU (Read-Copy-Update): a synchronization mechanism for read-heavy workloads.

RCU philosophy:
  - READERS: can access shared data WITHOUT acquiring any lock
    (zero overhead on the read side — just an RCU read-lock "marker")
  - WRITERS: must CREATE a new copy, update it, then ATOMICALLY publish the pointer
    (writes are serialized, expensive)
  - Readers CAN run concurrently with writers
  - Guarantee: readers never see partially-updated objects

Why RCU works:
  Readers either see the OLD object or the NEW object — never a mix
  Because: the writer atomically updates a POINTER (single word = atomic on ARM64)
  Old object: not freed until ALL pre-existing readers have finished
  (This is the "grace period" — kernel collects callbacks after all CPUs quiesce)

Three primitives:
  1. rcu_read_lock()     / rcu_read_unlock()    — marks RCU read critical section
  2. rcu_dereference(p) — reads RCU-protected pointer
  3. rcu_assign_pointer(p, v) — writes RCU-protected pointer (after setup)
```

---

## 2. rcu_dereference() Deep Dive

```
rcu_dereference(p):
  Returns the value of pointer p, with appropriate memory ordering guarantees
  
  Implementation (include/linux/rcupdate.h):
  
  #define rcu_dereference(p) rcu_dereference_check(p, 0)
  
  #define rcu_dereference_check(p, c) \
      __rcu_dereference_check((p), __UNIQUE_ID(rcu), \
                              (c) || rcu_read_lock_held(), __rcu)
  
  #define __rcu_dereference_check(p, local, c, space)  \
  ({                                                    \
      typeof(*p) *local = (typeof(*p) __force *)READ_ONCE(p); \
      RCU_LOCKDEP_WARN(!(c), "suspicious rcu_dereference_check()"); \
      ((typeof(*p) __force __kernel *)(local)); \
  })
  
  Core: READ_ONCE(p)  +  (implied data dependency ordering on ARM64)

On ARM64 — data dependency magic:
  ptr = READ_ONCE(rcu_ptr);     // load pointer from memory
  val = ptr->field;             // load field USING THE POINTER
  
  The second load uses the VALUE of ptr as its ADDRESS
  ARM64 hardware guarantee: DATA DEPENDENCY preserves ordering!
  If load A provides the address for load B, B is ordered after A.
  
  This is the "dependency ordering" or "address dependency" guarantee.
  Result: NO extra barrier needed on ARM64 for rcu_dereference!
  (Unlike Alpha: Alpha was the only architecture to break data dependencies)

Without data dependency (safe on ARM64, but not all architectures):
  // Example where dependency chain is broken:
  idx = READ_ONCE(rcu_ptr);      // load an integer INDEX
  val = array[idx];              // use as index (not address of loaded pointer)
  // This is NOT a pointer deref — requires smp_rmb() explicitly
  
  // Example where it works:
  obj = READ_ONCE(rcu_obj_ptr);  // load a POINTER
  val = obj->field;              // deref the pointer
  // ARM64: data dependency: val load ordered after obj load ← SAFE without barrier
```

---

## 3. rcu_assign_pointer() Deep Dive

```
rcu_assign_pointer(p, v):
  Atomically assigns pointer v to p with proper publish ordering
  
  Implementation:
  #define rcu_assign_pointer(p, v)    \
  do {                                \
      uintptr_t _r_a_p__v = (uintptr_t)(v);  \
      rcu_check_sparse(p, __rcu);             \
      if (__builtin_constant_p(v) && (_r_a_p__v == 0)) \
          WRITE_ONCE((p), (typeof(p))(_r_a_p__v));      \
      else                                              \
          smp_store_release(&p, RCU_INITIALIZER(v));    \
  } while (0)
  
  Key: smp_store_release() → STLR (Store-Release) on ARM64!
  
  Why STLR?
    The new object was INITIALIZED before rcu_assign_pointer() is called:
      obj->field1 = val1;         // initialize new object
      obj->field2 = val2;
      rcu_assign_pointer(ptr, obj);  // smp_store_release → STLR
    
    STLR ensures: ALL stores before STLR (obj->field1, obj->field2) are
    globally visible BEFORE the pointer store (ptr = obj) becomes visible
    
    Without STLR: a reader on another CPU might load ptr = obj BEFORE
    obj->field1 and obj->field2 are visible → reader sees uninitialized data!

Complete RCU update pattern:
  // Writer:
  struct foo *new_obj = kmalloc(sizeof(*new_obj), GFP_KERNEL);
  new_obj->x = compute_x();       // initialize new object
  new_obj->y = compute_y();
  
  spin_lock(&my_lock);             // serialize writers
  struct foo *old_obj = rcu_dereference_protected(my_ptr, lockdep_is_held(&my_lock));
  rcu_assign_pointer(my_ptr, new_obj);  // STLR: all fields before pointer
  spin_unlock(&my_lock);
  
  synchronize_rcu();               // wait for all pre-existing readers to finish
  kfree(old_obj);                  // safe to free — no more readers

  // Reader:
  rcu_read_lock();                 // mark start of read-side critical section
  struct foo *obj = rcu_dereference(my_ptr);  // READ_ONCE (data dependency on ARM64)
  if (obj) {
      use(obj->x);                 // ordered after ptr load (data dependency!)
      use(obj->y);
  }
  rcu_read_unlock();               // mark end
```

---

## 4. rcu_read_lock/unlock Memory Ordering

```
rcu_read_lock():
  Implementation (non-preemptible RCU):
    preempt_disable() → barrier()
    
  Note: on non-preemptible RCU (CONFIG_PREEMPT_NONE or CONFIG_PREEMPT_VOLUNTARY):
    rcu_read_lock() = preempt_disable() — NO hardware barrier!
    
  Why no barrier at rcu_read_lock()?
    The reader uses data dependency ordering (rcu_dereference → READ_ONCE + dependency)
    No up-front barrier needed — the data dependency chain provides ordering
    
  On preemptible RCU (CONFIG_PREEMPT_RT):
    rcu_read_lock() involves a spinlock → acquire barrier (LDAXR/STXR)

rcu_read_unlock():
  Non-preemptible: preempt_enable() → barrier()
  Again: no explicit memory barrier
  
  Reader's obligations (from RCU contract):
    After rcu_read_unlock(): the reader MUST NOT access any RCU-protected pointer
    It's the reader's responsibility to have copied the pointer to local storage
    before rcu_read_unlock() if needed beyond the critical section
    
synchronize_rcu():
  Called by writer after updating the pointer
  Waits for a QUIESCENT STATE on all CPUs
  (Each CPU must execute a context switch, go to user space, or block/sleep)
  After synchronize_rcu() returns: ALL pre-existing readers have completed
  
  On ARM64: synchronize_rcu() includes full memory barriers:
    - Ensures the writer's pointer update was visible to all readers
    - Ensures any reader that started before the pointer update has finished
    - Returns only when it's safe to free old_obj
```

---

## 5. Practical Examples

```
Linux networking: sk_buff list (net/core/sock.c, net/core/dev.c)

  struct net_device: RCU-protected in routing tables
  
  // Route lookup (reader path, lockless):
  rcu_read_lock();
  dev = dev_get_by_index_rcu(net, ifindex);  // calls rcu_dereference internally
  if (dev) {
      use(dev->ifindex);   // data dependency: safe, no barrier
  }
  rcu_read_unlock();
  
  // Network device registration (writer path):
  new_dev = alloc_netdev(...);
  setup_netdev(new_dev);    // initialize all fields
  rcu_assign_pointer(net->dev_index_head[ifindex], new_dev);  // STLR
  // Readers now see new_dev

Linux task_struct (kernel/fork.c):
  // Process list: rcu_dereference'd in for_each_process
  // for_each_process macro:
  #define for_each_process(p) \
      for (p = &init_task; (p = next_task(p)) != &init_task; )
  
  next_task():
    return list_entry_rcu(p->tasks.next, struct task_struct, tasks);
    // list_entry_rcu: calls rcu_dereference internally

sysctl/proc tables:
  sysctl_table pointers: RCU-protected
  Readers: rcu_dereference to get table pointer
  Writers: rcu_assign_pointer after building new table
```

---

## 6. Common Pitfalls

```
Pitfall 1: Accessing RCU pointer outside rcu_read_lock()
  // WRONG:
  ptr = rcu_dereference(my_rcu_ptr);  // outside RCU read section!
  rcu_read_lock();
  use(ptr);                           // ptr may be freed by now!
  rcu_read_unlock();
  
  // CORRECT:
  rcu_read_lock();
  ptr = rcu_dereference(my_rcu_ptr);  // inside RCU read section
  use(ptr);                           // ptr protected for duration of critical section
  rcu_read_unlock();

Pitfall 2: Storing RCU pointer to local variable and using after unlock
  // WRONG:
  rcu_read_lock();
  ptr = rcu_dereference(my_rcu_ptr);
  rcu_read_unlock();
  use(ptr);    // ← WRONG: ptr may have been freed by synchronize_rcu!
  
  // CORRECT: kref or other mechanism to extend lifetime, or use inside lock

Pitfall 3: Not using smp_store_release in rcu_assign_pointer
  // WRONG:
  WRITE_ONCE(my_rcu_ptr, new_obj);  // NO release barrier!
  // Other CPUs may see ptr before obj fields are initialized!
  
  // CORRECT:
  rcu_assign_pointer(my_rcu_ptr, new_obj);  // STLR (smp_store_release)

Pitfall 4: Using plain dereference instead of rcu_dereference
  // WRONG (may fail on Alpha, bad practice everywhere):
  ptr = *my_rcu_ptr;   // no READ_ONCE → compiler may optimize away
  use(ptr->field);      // no data dependency tracking → may be reordered on Alpha
  
  // CORRECT:
  ptr = rcu_dereference(my_rcu_ptr);
  use(ptr->field);
```

---

## 7. Interview Questions & Answers

**Q1: Why doesn't rcu_dereference() need an explicit memory barrier on ARM64, even in an SMP system with 16 cores?**

`rcu_dereference()` relies on **data dependency ordering**, which ARM64 hardware guarantees. The implementation is essentially `READ_ONCE(p)` — a single load instruction. When the reader subsequently accesses `ptr->field`, it uses the VALUE loaded by `READ_ONCE()` as the base ADDRESS for the memory access. This creates a data dependency: the second load's address depends on the first load's result.

ARM64's memory model specifies that if Load B uses the result of Load A as its address (or part of its address computation), then Load A is guaranteed to be observed before Load B — with no extra barrier instructions. This is part of the ARM Architecture Reference Manual specification.

The only architecture that does NOT provide this guarantee is DEC Alpha (and to some extent early POWER implementations). Linux's `rcu_dereference()` was designed to add an `smp_read_barrier_depends()` (now called `smp_rmb()`) for Alpha compatibility, but on ARM64 this is a no-op.

On the WRITER side, `rcu_assign_pointer()` DOES use `smp_store_release()` (STLR), which is necessary: the STLR ensures all of the writer's initialization stores (obj->field1, obj->field2, etc.) are globally visible before the pointer store (ptr = obj) becomes visible. Without this, a reader might load the new pointer value but then read uninitialized fields.

---

## 8. Quick Reference

| Function | ARM64 Instruction | Purpose |
|---|---|---|
| rcu_dereference() | READ_ONCE (LDR) | Load pointer with data-dep ordering |
| rcu_assign_pointer() | smp_store_release (STLR) | Publish pointer after init |
| rcu_read_lock() | preempt_disable + barrier() | Mark read-side CS entry |
| rcu_read_unlock() | preempt_enable + barrier() | Mark read-side CS exit |
| synchronize_rcu() | smp_mb() + scheduling | Wait for all readers to complete |
| call_rcu() | (deferred callback) | Async free after grace period |

| Architecture | Data Dependency Ordered? | rcu_dereference barrier |
|---|---|---|
| ARM64 | Yes (guaranteed) | READ_ONCE only |
| x86 | Yes (TSO implies it) | READ_ONCE only |
| POWER | Partial (depends on version) | smp_read_barrier_depends |
| Alpha | NO! | Explicit smp_rmb() |
