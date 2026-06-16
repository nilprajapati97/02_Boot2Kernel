# READ_ONCE, WRITE_ONCE, and Compiler Barriers

**Category**: Memory Ordering & Barriers  
**Platform**: ARM64 (AArch64)

---

## 1. Concept Foundation

```
READ_ONCE() and WRITE_ONCE() solve COMPILER-LEVEL problems with memory access.

Problem 1: Compiler caching (register caching)
  C compilers assume: if no write occurs between two reads, they're equal
  Compiler may replace:
    while (*flag == 0);    // load *flag every iteration
  with:
    if (*flag == 0) while (1);   // load ONCE, infinite loop!
  
  In kernel: another CPU or interrupt WILL modify flag
  The compiler doesn't know this → wrong optimization

Problem 2: Compiler speculation (load hoisting)
  Compiler may move:
    if (condition) { use(*ptr); }
  to:
    val = *ptr;   // ← HOISTED (even if condition is false!)
    if (condition) { use(val); }
  
  In kernel: *ptr may not be valid if condition is false
  Hoisting causes a NULL pointer dereference!

Problem 3: Store elimination
  Compiler may eliminate "redundant" stores:
    x = 1; x = 2; x = 3;  → x = 3 (only last store kept!)
  
  In kernel: MMIO register write sequences MUST NOT be collapsed
  Each write triggers a device action!

Problem 4: Load/store tearing
  Large value (u64) written to non-aligned address:
  Compiler may split into TWO 32-bit stores → other CPU sees half-updated value!
  
  READ_ONCE/WRITE_ONCE guarantee: SINGLE atomic access (no tearing for pointer-sized)

Solution: READ_ONCE() and WRITE_ONCE()
  These are "volatile-cast" helpers that prevent ALL of the above:
  - Single load/store (no caching, no elimination)
  - No tearing (atomic access)
  - No hoisting (compiler treats each access as side-effectful)
  - No loop optimization (forces loop re-read every iteration)
```

---

## 2. Implementation Deep Dive

```
READ_ONCE() implementation (include/linux/compiler.h):

#define READ_ONCE(x)                                    \
({                                                      \
    compiletime_assert_rwonce_type(x);                 \
    __READ_ONCE(x);                                     \
})

#define __READ_ONCE(x)  (*(volatile typeof(x) *)&(x))

What this does:
  1. Takes the address of x: &(x)
  2. Casts to volatile pointer: (volatile typeof(x) *)
  3. Dereferences: (*(volatile...))
  
  The volatile qualifier tells the compiler:
  - This access has SIDE EFFECTS (do not cache the result)
  - Do NOT eliminate, reorder, or combine this access
  - Generate a REAL memory access instruction (LDR on ARM64)

WRITE_ONCE() implementation:
#define WRITE_ONCE(x, val)                                \
({                                                        \
    compiletime_assert_rwonce_type(x);                   \
    __WRITE_ONCE(x, val);                                 \
})

#define __WRITE_ONCE(x, val)                             \
do {                                                     \
    *(volatile typeof(x) *)&(x) = (val);                 \
} while (0)

Same principle: volatile cast ensures single, non-eliminated store.

compiletime_assert_rwonce_type():
  Compile-time check: x must be a scalar type (no struct/array)
  Ensures no tearing: single load/store instruction for pointer-sized values
  ARM64: LDR/STR for 8-byte (u64/pointer), LDRW/STRW for 4-byte (u32)

Generated ARM64 assembly:
  // Without READ_ONCE:
  // int x; while (x == 0);
  // Compiler might generate:
  LDR  w0, [x_addr]    // load ONCE
  CBZ  w0, loop        // check
  loop: B loop          // infinite if x was 0
  
  // With READ_ONCE:
  // while (READ_ONCE(x) == 0);
  loop:
    LDR  w0, [x_addr]  // load EVERY iteration (volatile prevents elimination)
    CBZ  w0, loop
```

---

## 3. When READ_ONCE/WRITE_ONCE Are Sufficient

```
READ_ONCE/WRITE_ONCE are COMPILER BARRIERS only:
  They prevent COMPILER reordering and optimization
  They do NOT prevent CPU (hardware) reordering
  They do NOT add memory barriers (no DMB/DSB)

When they're sufficient:
  1. Single-CPU context with IRQs disabled:
       local_irq_disable();
       WRITE_ONCE(shared_data, new_val);   // no other CPU can interfere
       local_irq_enable();
       
  2. IRQ handler communication (same CPU):
       // Main thread:
       WRITE_ONCE(irq_expected, 1);
       ...
       // IRQ handler:
       if (READ_ONCE(irq_expected)) { ... }
       WRITE_ONCE(irq_expected, 0);
       (Same CPU: no CPU reordering between interrupt and non-interrupt code)
       
  3. Value marked "won't be optimized away" for debugging/polling:
       while (READ_ONCE(device_status) == BUSY);  // poll status register
       (Minimal usage — prefer proper interrupt-driven code)
       
  4. Pointer updates in RCU read-side critical sections:
       ptr = READ_ONCE(rcu_protected_pointer);
       // ARM64: data dependency ordering means no extra barrier needed!
       // The load of ptr's target is ordered after ptr itself (by hardware)
       use(ptr->field);
       
When they're NOT sufficient (need hardware barriers too):
  1. Two CPUs communicating:
       CPU0: WRITE_ONCE(data, val); WRITE_ONCE(flag, 1);
       CPU1: while (!READ_ONCE(flag)); val = READ_ONCE(data);
       → CPU0 needs smp_wmb(); CPU1 needs smp_rmb() (or use STLR/LDAR)
       
  2. Shared memory with device/DMA:
       WRITE_ONCE(dma_buffer, data); 
       → Needs DSB ISH (DC CIVAC + DSB) for DMA coherency
```

---

## 4. Compound Usage Patterns

```
Pattern 1: Latch (one-time initialization)
  // Writer (initializer):
  if (!READ_ONCE(initialized)) {
      fill_data_structure(&obj);   // write all fields
      smp_wmb();                   // store barrier: fields before flag
      WRITE_ONCE(initialized, 1); // publish
  }
  
  // Reader:
  if (READ_ONCE(initialized)) {   // load flag
      smp_rmb();                  // load barrier: flag before fields
      use(obj.field);             // read fields (guaranteed after flag)
  }

Pattern 2: Lock-free algorithm (pointer publish)
  // RCU-like pointer update:
  new_node = kmalloc(...);
  new_node->data = value;          // write data
  smp_wmb();                       // data before pointer
  WRITE_ONCE(*list_head, new_node);// publish pointer
  
  // Reader:
  node = READ_ONCE(*list_head);    // load pointer
  if (node) {
      // ARM64: DATA DEPENDENCY ordering applies!
      // node->data load is ordered after node load (same CPU chain)
      use(node->data);             // NO barrier needed on ARM64 (unlike alpha)
  }
  
  Why ARM64 data dependency works:
    The LOAD of node->data uses the ADDRESS loaded by READ_ONCE(*list_head)
    ARM64 hardware respects data dependencies (control dependency is NOT guaranteed)
    This is why READ_ONCE works in RCU read-side on ARM64 without extra barriers

Pattern 3: Status flag with retry
  // Writer:
  WRITE_ONCE(result, compute());
  smp_store_release(&done, 1);  // STLR: result before done flag
  
  // Waiter:
  while (!smp_load_acquire(&done));  // LDAR: done flag before result
  val = READ_ONCE(result);           // guaranteed to see updated result

Pattern 4: Per-CPU variable "anti-optimization"
  // In NMI handler — cannot acquire spinlock, cannot sleep
  // But need to update a counter:
  WRITE_ONCE(per_cpu(nmi_count, cpu), 
             READ_ONCE(per_cpu(nmi_count, cpu)) + 1);
  // READ_ONCE: prevents compiler from "optimizing" the counter
  // WRITE_ONCE: ensures single store (no tearing)
```

---

## 5. Common Bugs and How to Avoid Them

```
Bug 1: Forgot READ_ONCE in polling loop
  // BUG: compiler may cache done_flag in register
  while (done_flag == 0) cpu_relax();
  
  // FIX:
  while (READ_ONCE(done_flag) == 0) cpu_relax();

Bug 2: Using READ_ONCE without memory barrier in SMP
  // BUG: CPU may reorder (on ARM64!):
  done = READ_ONCE(flag);
  if (done) {
      val = READ_ONCE(data);  // ← may read stale data despite flag=1
  }
  
  // FIX (Option A: LDAR):
  done = smp_load_acquire(&flag);  // LDAR: flag load + barrier
  if (done) {
      val = READ_ONCE(data);       // ordered after LDAR
  }
  
  // FIX (Option B: separate barrier):
  done = READ_ONCE(flag);
  if (done) {
      smp_rmb();              // load barrier between flag and data
      val = READ_ONCE(data);
  }

Bug 3: Struct access without ensuring pointer validity
  // BUG: compiler may hoist ptr->field access:
  if (ptr) use(ptr->field);
  // Compiler: "val = ptr->field; if (ptr) use(val);"
  // If ptr is NULL: NULL dereference!
  
  // FIX:
  if (READ_ONCE(ptr)) {     // volatile load prevents hoisting
      use(ptr->field);      // compiler won't hoist this before the check
  }

Bug 4: WRITE_ONCE on struct (tearing)
  // BUG: struct is larger than pointer size!
  struct large { int a; int b; int c; };
  WRITE_ONCE(large_var, new_val);  // COMPILE ERROR (WRITE_ONCE enforces scalar!)
  
  // FIX: use spinlock for struct-sized updates, or update fields individually:
  spin_lock(&lock);
  large_var.a = new_a;
  large_var.b = new_b;
  large_var.c = new_c;
  spin_unlock(&lock);
  // Or use RCU for pointer-based update
```

---

## 6. Interview Questions & Answers

**Q1: What is the difference between READ_ONCE() and a normal read? Why can't you just use volatile?**

**READ_ONCE vs normal read**: A normal read (`x = *ptr`) allows the compiler to freely optimize: it may cache the result in a register (eliminating subsequent reads), hoist the load above a conditional check, or tear a 64-bit access into two 32-bit accesses on non-aligned memory. `READ_ONCE()` prevents all of this by using a volatile cast that forces: (1) a single, atomic load instruction, (2) no caching of the result in a register, (3) no reordering or elimination by the compiler.

**vs volatile**: The `volatile` keyword in C is specified too loosely — it prevents optimization on individual accesses but does NOT prevent reordering between multiple volatile accesses. The C standard does not define ordering between volatile accesses from the perspective of concurrency. Additionally, `volatile` doesn't provide the type-size checks that `compiletime_assert_rwonce_type()` does. `READ_ONCE()` is more explicit about intent (this is a shared variable access) and is better integrated with the Linux memory model documentation (`Documentation/memory-barriers.txt`). The Linux kernel prefers `READ_ONCE/WRITE_ONCE` to `volatile` for clarity and correctness.

---

## 7. Quick Reference

| Construct | Prevents Compiler Opts | Prevents CPU Reorder | Adds HW Barrier |
|---|---|---|---|
| Normal load/store | No | No | No |
| volatile | Partially | No | No |
| READ_ONCE() | Yes | No | No |
| WRITE_ONCE() | Yes | No | No |
| smp_rmb() + READ_ONCE() | Yes | Load-Load | DMB ISHLD |
| smp_load_acquire() | Yes | Load → later loads/stores | LDAR |
| smp_store_release() | Yes | Earlier stores → store | STLR |

| Pattern | Read | Write | Barrier |
|---|---|---|---|
| Single-CPU flag | READ_ONCE | WRITE_ONCE | None |
| SMP flag (producer) | — | WRITE_ONCE + smp_wmb() | DMB ISHST |
| SMP flag (consumer) | READ_ONCE + smp_rmb() | — | DMB ISHLD |
| SMP release | — | smp_store_release() | STLR |
| SMP acquire | smp_load_acquire() | — | LDAR |
