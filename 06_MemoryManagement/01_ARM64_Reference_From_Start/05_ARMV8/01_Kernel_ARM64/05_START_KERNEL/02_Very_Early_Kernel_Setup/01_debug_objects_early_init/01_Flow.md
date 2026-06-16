
**1) Whiteboard Call Flow**
Use this as your board narrative from boot to object-check path.

1. Boot enters start_kernel in main.c.
2. start_kernel calls debug_objects_early_init at main.c.
3. debug_objects_early_init in debugobjects.c does:
4. Initialize per-bucket raw spinlocks for obj_hash.
5. Seed pool_boot with all entries from obj_static_pool.
6. Now debug object tracking is usable in early boot without slab dependency.
7. Later, subsystem code calls debug_object_init / activate / deactivate / free from debugobjects.h.
8. Runtime path inside tracker:
9. Hash object address to bucket via get_bucket in debugobjects.c.
10. Take bucket lock, lookup existing metadata, allocate if missing.
11. In early phase allocation comes from pool_boot through alloc_object in debugobjects.c.
12. State machine validation runs and may warn/fixup.
13. Later memory phase calls debug_objects_mem_init from mm_init.c.
14. debug_objects_mem_init in debugobjects.c migrates static metadata to slab/per-CPU scalable model.

Mental model sentence:
This is phased bootstrap architecture: static correctness first, dynamic scalability later.

**2) Bug Scenario Walkthrough**
Interviewers love this because it proves you understand why it exists.

Scenario: Timer/work object is freed while still ACTIVE.

1. Driver initializes object and activates it.
2. Due to bug, cleanup path frees memory without proper deactivate.
3. Free path (or free-memory debug hook) reaches tracker in debugobjects.c and debugobjects.c.
4. Tracker finds object metadata in hash bucket.
5. State is ACTIVE, which is illegal for free.
6. Tracker emits warning via debug_print_object in debugobjects.c.
7. If descriptor provides fixup_free callback, tracker attempts recovery via debug_object_fixup in debugobjects.c.
8. If not fixable, issue remains visible, preventing silent corruption.
9. Without this subsystem, this may become nondeterministic later crash or memory corruption.
10. With it, bug is detected close to misuse site with object type context.

System-design angle:
This is a low-level safety net that converts latent distributed failures into local diagnosable failures.

**3) 10 Interview Q&A (ready answers)**
Use these almost verbatim.

1. What does debug_objects_early_init do exactly?
It initializes hash bucket locks and seeds the static early object pool, making lifecycle tracking operational before full allocator readiness.

2. Why call it so early in start_kernel?
Because lifecycle bugs can happen during early init too, and early boot should avoid allocator dependency loops.

3. What is being tracked?
Metadata per object address: state, active-substate, and descriptor callbacks for type-specific checks/fixups.

4. Why hash buckets?
To get near O(1) lookup and limit lock contention to one bucket instead of global lock.

5. Why raw spinlocks?
Tracker may be used in low-level contexts; raw spinlocks keep strict low-level locking semantics.

6. What if CONFIG_DEBUG_OBJECTS is disabled?
All APIs become stubs; overhead is near zero. See stubs in debugobjects.h.

7. How does early pool differ from later runtime pool?
Early pool is static init memory for deterministic bootstrap. Later pool is slab/per-CPU for scalability.

8. When does migration happen?
In debug_objects_mem_init called from mm init path at mm_init.c.

9. What class of bugs does this catch best?
Use-before-init, double-activate, destroy/free while active, bad stack/static object lifecycle annotations.

10. What is the architecture takeaway for ARM64/SMP systems?
Boot path prioritizes deterministic correctness first, then transitions to scalable concurrent data structures as CPUs and memory subsystems come fully online.

