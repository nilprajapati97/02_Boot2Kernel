# smp_mb, smp_rmb, smp_wmb: Linux SMP Barrier Macros

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
Linux provides portable memory barrier macros that abstract the
underlying CPU instruction differences across architectures.

On ARM64, the key SMP macros map to:
  smp_mb()   → DMB ISH   (full bidirectional barrier, Inner Shareable)
  smp_rmb()  → DMB ISHLD (read-only barrier, Inner Shareable)
  smp_wmb()  → DMB ISHST (write-only barrier, Inner Shareable)

For non-SMP (device/MMIO) memory:
  mb()       → DSB SY   (system barrier)
  rmb()      → DSB LD   (system read barrier)
  wmb()      → DSB ST   (system write barrier)

Additionally:
  barrier()  → (compiler barrier only, no CPU instruction)
             Prevents compiler from reordering across this point
             No hardware effect

Location: include/linux/compiler.h, arch/arm64/include/asm/barrier.h

Semantics rule: use the WEAKEST barrier that satisfies the requirement
  Stronger barrier = more overhead = less throughput
  smp_wmb() is cheaper than smp_mb()
  smp_mb() is cheaper than mb()
  
  On x86: smp_mb() = MFENCE (expensive), smp_rmb() = no-op, smp_wmb() = no-op
  On ARM64: all three are real instructions (ARM64 is weaker than x86)
```

---

## 2. smp_mb() — Full Barrier

```
smp_mb():
  ARM64: DMB ISH
  
  Guarantees: ALL loads/stores before smp_mb() are ordered before
              ALL loads/stores after smp_mb()
              (relative to other CPUs in the Inner Shareable domain)
              
  This is a BIDIRECTIONAL barrier:
    - All four reordering types (LL, SS, LS, SL) prevented across smp_mb()
    
When to use smp_mb():
  1. Store-Load ordering needed (Dekker's algorithm, seqlocks):
       seq_write++;           // increment version (writer started)
       smp_wmb();             // write barrier: data after is ordered after seq_write
       write_data();          // write the data
       smp_wmb();             // write barrier
       seq_write++;           // increment version (writer done)
       
  2. Completion of one CPU's work before another CPU starts:
       data_ready = 1;
       smp_mb();              // full barrier
       // signal handler or interrupt handler reads data
       
  3. ring buffer producer:
       buf[head] = item;
       smp_mb();              // both stores above AND the head update below ordered
       head = (head + 1) % SIZE;
       
  4. atomic_read() + smp_mb() pattern:
       // Check flag, do work, update flag
       smp_mb();
       if (READ_ONCE(stop_flag)) ...

Linux seqlock implementation uses smp_wmb():
  write_seqlock():
    raw_spin_lock(&lock->lock);
    lock->sequence++;
    smp_wmb();                 // writer started: version odd, data writes below ordered
    
  write_sequnlock():
    smp_wmb();                 // writer done: data writes above ordered before version
    lock->sequence++;
    raw_spin_unlock(&lock->lock);
    
  read_seqbegin():
    seq = READ_ONCE(lock->sequence);
    smp_rmb();                 // read barrier: version load ordered before data reads
    
  read_seqretry():
    smp_rmb();                 // data reads ordered before re-reading version
    return READ_ONCE(lock->sequence) != seq;
```

---

## 3. smp_rmb() and smp_wmb()

```
smp_rmb() — Read (Load) Barrier:
  ARM64: DMB ISHLD
  
  Guarantees: all LOADS before smp_rmb() complete before any LOAD after smp_rmb()
  Does NOT order stores
  
  Use cases:
    1. Consumer in producer-consumer (after reading flag, before reading data):
         while (!READ_ONCE(ready));  // load flag
         smp_rmb();                  // load barrier: flag load ordered before data loads
         val = READ_ONCE(data);      // load data — guaranteed fresh
         
    2. Data dependency chains where you need explicit ordering:
         // When ARM64's data dependency guarantee isn't applicable (non-dependent addrs)
         idx = READ_ONCE(index);
         smp_rmb();
         val = array[idx];           // explicit ordering (idx and array are unrelated VAs)

smp_wmb() — Write (Store) Barrier:
  ARM64: DMB ISHST
  
  Guarantees: all STORES before smp_wmb() are visible before any STORE after smp_wmb()
  Does NOT order loads
  
  Use cases:
    1. Producer in producer-consumer (before writing flag):
         WRITE_ONCE(data, value);    // write data
         smp_wmb();                   // store barrier: data write ordered before flag
         WRITE_ONCE(ready, 1);        // write flag — guaranteed after data
         
    2. Initializing a structure before publishing its pointer:
         obj = kmalloc(sizeof(*obj));
         obj->field1 = val1;         // initialize fields
         obj->field2 = val2;
         smp_wmb();                   // all field stores ordered before pointer publish
         WRITE_ONCE(global_ptr, obj); // publish pointer — readers see initialized obj

Asymmetric barriers (preferred in production code):
  Instead of smp_wmb() + smp_rmb():
    Producer: smp_store_release(&ready, 1)  // STLR: cheaper + correct
    Consumer: while (!smp_load_acquire(&ready));  // LDAR: built-in ordering
    No separate barriers needed! STLR+LDAR pair guarantees the ordering.
    
  Use smp_wmb()/smp_rmb() when:
    - The flag is not a single-word variable (multi-word updates)
    - You cannot use STLR/LDAR (e.g., complex interrupt handler patterns)
    - Compatibility with older kernel APIs
```

---

## 4. barrier() — Compiler Barrier

```
barrier():
  GCC/LLVM: asm volatile("" ::: "memory")
  No ARM64 instruction generated
  No hardware effect
  
  Effect: tells the COMPILER that all memory values may change at this point
  Prevents the compiler from:
    1. Caching a memory value in a register across barrier()
    2. Reordering loads/stores relative to each other
    3. Eliminating "redundant" loads
    4. Speculating loads before barrier()
    
When barrier() is sufficient:
  Single-CPU code where only compiler reordering matters:
    jiffies volatile accesses
    Interrupt handler coordination (same CPU)
    
  When flag variable accessed from interrupt handler on SAME CPU:
    // Main thread:
    WRITE_ONCE(flag, 0);    // set flag to 0
    barrier();               // prevent compiler from moving check before write
    do_work();               
    // Interrupt may have set flag=1 during do_work
    if (READ_ONCE(flag))    // re-read from memory (not compiler-cached register)
    
When barrier() is NOT sufficient:
  SMP systems: compiler barrier does nothing for other CPUs' view of memory
  DMB ISH needed for cross-CPU visibility
  
  volatile keyword:
    C's volatile: prevents compiler from optimizing the ACCESS AWAY
    Does NOT provide memory ordering between multiple volatile accesses
    READ_ONCE/WRITE_ONCE better than volatile (same compiler effect, but clearer intent)
    
  READ_ONCE(x) = *(volatile typeof(x) *)&x
    - Single load (not cached in register)
    - No tearing (single access for up to pointer-sized values)
    - No compiler reordering around it (implied compiler barrier)
```

---

## 5. Usage in Real Linux Code

```
kfifo (circular buffer) in Linux (lib/kfifo.c):

Producer:
  kfifo_put():
    // Copy data to buffer
    memcpy(fifo->buf + (fifo->in & mask), buf, len);
    
    // Store barrier: data writes before in pointer update
    smp_wmb();
    
    // Update producer pointer
    WRITE_ONCE(fifo->in, fifo->in + len);

Consumer:
  kfifo_get():
    len = READ_ONCE(fifo->in) - READ_ONCE(fifo->out);
    if (!len) return 0;
    
    // Load barrier: in pointer load before data reads
    smp_rmb();
    
    // Read data
    memcpy(buf, fifo->buf + (fifo->out & mask), len);
    
    // Full barrier: data reads before out pointer update
    smp_mb();
    
    // Update consumer pointer
    WRITE_ONCE(fifo->out, fifo->out + len);

waitqueue (include/linux/wait.h):
  wake_up():
    // Set condition
    condition = 1;
    smp_mb();          // ensures condition visible before wake signal
    __wake_up(wq, ...);// wake sleeping process
    
  wait_event():
    for(;;) {
        // Load condition
        if (condition) break;
        smp_mb();          // ensure condition re-read after going to sleep
        schedule();
    }
    smp_mb__after_atomic();  // post-atomic barrier if needed

Mutexes:
  mutex_lock():   → down() → LDXR/STXR with ACQUIRE (LDAXR)
  mutex_unlock(): → up()   → STLR (release)
  
  Full lock/unlock path includes acquire/release barriers
  No separate smp_mb() needed for operations INSIDE the mutex-protected region
```

---

## 6. smp_mb__before/after_atomic

```
Special macros for atomic operations:

smp_mb__before_atomic():
  ARM64: DMB ISH
  Use: when you need a full barrier BEFORE an atomic op
  
  Example:
    data = prepare_value();
    smp_mb__before_atomic();   // data write ordered before atomic_inc
    atomic_inc(&counter);
    
smp_mb__after_atomic():
  ARM64: DMB ISH
  Use: when you need a full barrier AFTER an atomic op
  
  Example:
    atomic_set(&flag, 1);
    smp_mb__after_atomic();    // atomic_set ordered before the load
    val = READ_ONCE(other);
    
Why these exist:
  atomic_inc() / atomic_set() etc. are NOT barriers themselves
  (They use LDADD/STLR which provide acquire/release, but that's not always enough)
  
  For Store-Load ordering: need smp_mb() or smp_mb__after_atomic()
  
  Example pitfall:
    // WRONG: This may not work due to Store-Load reordering:
    atomic_set(&my_flag, 1);    // STLR (release)
    val = READ_ONCE(their_flag); // LDR (no acquire) — may be reordered before STLR!
    
    // CORRECT:
    atomic_set(&my_flag, 1);    // STLR
    smp_mb__after_atomic();      // DMB ISH: prevents Store-Load reordering
    val = READ_ONCE(their_flag); // LDR — guaranteed after the atomic_set
```

---

## 7. Interview Questions & Answers

**Q1: In a producer-consumer pattern on ARM64, can you use smp_wmb()/smp_rmb() instead of smp_store_release()/smp_load_acquire()? What are the trade-offs?**

Both approaches provide correct ordering for producer-consumer, but `smp_store_release()`/`smp_load_acquire()` is preferred for the following reasons:

**Correctness equivalence**: `smp_wmb()` + `WRITE_ONCE()` ≈ `smp_store_release()` for the producer side. `smp_rmb()` after `READ_ONCE()` ≈ `smp_load_acquire()` on the consumer side. Both prevent the critical reordering (data writes reaching consumer before the flag).

**Performance**: `smp_store_release()` compiles to a single `STLR` instruction. `WRITE_ONCE()` + `smp_wmb()` compiles to `DMB ISHST` + `STR` — two instructions. The `STLR` may be slightly cheaper than `STR + DMB` on some ARM64 microarchitectures.

**Semantics clarity**: Acquire/release semantics are a recognized programming pattern from C11. The code is more self-documenting.

**The key difference**: `smp_wmb()` + separate `WRITE_ONCE()` leaves the barrier and the store as two separate operations. A compiler theoretically could reorder them (though "memory" clobber in `smp_wmb()` prevents this in practice). `STLR` is a single atomic "store with release" — indivisible.

Use `smp_wmb()`/`smp_rmb()` when you're updating multiple words atomically (the "flag" write must come after multiple store barriers within a data block) and `smp_store_release()`/`smp_load_acquire()` for single-word atomic flag patterns.

---

## 8. Quick Reference

| Macro | ARM64 | Scope | Prevents |
|---|---|---|---|
| barrier() | (none) | Compiler only | Compiler reordering |
| smp_wmb() | DMB ISHST | CPUs in IS | Store-Store |
| smp_rmb() | DMB ISHLD | CPUs in IS | Load-Load |
| smp_mb() | DMB ISH | CPUs in IS | All reorderings |
| wmb() | DSB ST | Full system | Store-Store incl. devices |
| rmb() | DSB LD | Full system | Load-Load incl. devices |
| mb() | DSB SY | Full system | All incl. devices |

| Alternative | Maps To | Notes |
|---|---|---|
| smp_store_release() | STLR | One-way release fence |
| smp_load_acquire() | LDAR | One-way acquire fence |
| smp_mb__after_atomic() | DMB ISH | After atomic operations |
| READ_ONCE() | LDR | Compiler barrier + single access |
| WRITE_ONCE() | STR | Compiler barrier + single access |
