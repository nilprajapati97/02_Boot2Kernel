
## Deep Dive: The Full `setup_per_cpu_areas()` Flow

---

### Stage 0: Why — The Problem Being Solved

The compiler places every `DEFINE_PER_CPU(type, name)` variable into the **`.data..percpu`** ELF section:

```c
// include/linux/percpu-defs.h:49
#define __PCPU_ATTRS(sec) \
    __percpu __attribute__((section(PER_CPU_BASE_SECTION sec)))
```

At link time there is **exactly one copy** in the kernel image — the "master template". It lives between the linker symbols `__per_cpu_start` / `__per_cpu_load` and `__per_cpu_end`. The goal of `setup_per_cpu_areas()` is to replicate this template once per possible CPU and wire up each CPU to its own copy, **without any locks at runtime**.

---

### Stage 1: `pcpu_build_alloc_info()` — CPU Grouping

```c
// mm/percpu.c:2864
static struct pcpu_alloc_info * __init pcpu_build_alloc_info(
    size_t reserved_size, size_t dyn_size, size_t atom_size,
    pcpu_fc_cpu_distance_fn_t cpu_distance_fn)
```

**What it does:**

1. Reads `__per_cpu_end - __per_cpu_start` to get `static_size` — the exact byte count of all static percpu variables in the kernel image.

2. Computes `size_sum`:
   ```
   size_sum = ALIGN(static_size + reserved_size + max(dyn_size, PERCPU_DYNAMIC_EARLY_SIZE))
   ```
   - `reserved_size` = `PERCPU_MODULE_RESERVE` — space for kernel module percpu variables
   - `dyn_size` = `PERCPU_DYNAMIC_RESERVE` — space for `alloc_percpu()` at runtime

3. **Groups CPUs by NUMA distance**: iterates `cpu_possible_mask`, calls `cpu_distance_fn(cpu_a, cpu_b)`. CPUs with `LOCAL_DISTANCE` bidirectionally are put in the same group. Non-NUMA ARM systems get one group.

4. Computes `unit_size` = the per-CPU allocation size (must be page-aligned and hold `size_sum`), and `upa` (units-per-allocation) chosen to keep ≥75% memory utilization (prevents waste when `atom_size` >> `unit_size`).

5. Returns a `pcpu_alloc_info` struct describing `nr_groups`, `unit_size`, `alloc_size`, and the `cpu_map[]` (which unit slot each CPU owns).

**Key data structure:**
```
pcpu_alloc_info {
    static_size,   reserved_size,   dyn_size,
    unit_size,     atom_size,       alloc_size,
    nr_groups,
    groups[] → { nr_units, base_offset, cpu_map[] }
}
```

---

### Stage 2: `memblock_alloc()` per group — Physical Memory Allocation

```c
// mm/percpu.c:3113 (inside pcpu_embed_first_chunk)
ptr = pcpu_fc_alloc(cpu, gi->nr_units * ai->unit_size, atom_size, cpu_to_nd_fn);
```

**`pcpu_fc_alloc`** ultimately calls `memblock_alloc_try_nid()` — the bootmem allocator, because slab/vmalloc don't exist yet. Each group gets one physically contiguous block of `nr_units × unit_size` bytes.

- On a 4-CPU system with `unit_size = 128KB`, group 0 gets `4 × 128KB = 512KB` from bootmem.
- On NUMA: group 0 allocates from node 0's memory, group 1 from node 1's, etc. — critical for performance because each CPU's data lands on its local NUMA node.

The lowest base address across all groups becomes `pcpu_base_addr`.

**Why "embed"?** The memory is allocated inside the kernel's linear physical mapping (lowmem). This means the MMU's large page TLB entries covering the linear map also cover percpu data — no extra TLB pressure per access.

---

### Stage 3: `memcpy(__per_cpu_load, ...)` — Template Copy

```c
// mm/percpu.c:3142
memcpy(ptr, __per_cpu_load, ai->static_size);
```

For each CPU's unit area, the `static_size` bytes from the linker template (`__per_cpu_load` = the original `.data..percpu` section) are copied in. This gives each CPU its own initialized copy of all static percpu variables.

After the copy, unused tail space (`unit_size - size_sum`) is freed back to memblock.

---

### Stage 4: `pcpu_setup_first_chunk()` — The Allocator Infrastructure

```c
// mm/percpu.c:2608
void __init pcpu_setup_first_chunk(const struct pcpu_alloc_info *ai, void *base_addr)
```

This is the allocator bookkeeping setup. Key things it establishes:

1. **`pcpu_unit_offsets[cpu]`** — for each CPU, the byte offset of its unit from `base_addr`:
   ```c
   unit_off[cpu] = gi->base_offset + i * ai->unit_size;
   ```

2. **Chunk structures**: The first chunk is split into:
   - *Static region* (not managed — can never be freed)
   - *Reserved chunk* (`pcpu_reserved_chunk`) — serves module `DEFINE_PER_CPU` allocations
   - *Dynamic chunk* (`pcpu_first_chunk`) — serves `alloc_percpu()` at runtime

3. **Slot lists** (`pcpu_chunk_lists`) — the free-list infrastructure for the dynamic allocator, organized by free space.

4. **Sets `pcpu_base_addr`** — the global base pointer used to compute offsets.

---

### Stage 5: `__per_cpu_offset[]` Computation

Back in `setup_per_cpu_areas()`:

```c
// mm/percpu.c:3399
delta = (unsigned long)pcpu_base_addr - (unsigned long)__per_cpu_start;
for_each_possible_cpu(cpu)
    __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu];
```

`__per_cpu_start` is the linker symbol for the template section. `pcpu_base_addr` is the start of the first allocated block. The **delta** accounts for the distance between the template address and the runtime percpu base.

So `__per_cpu_offset[cpu]` is the **single number** you add to any percpu symbol address (which is template-relative) to get that CPU's private instance.

**Example** (simplified):
```
__per_cpu_start  = 0xC0A00000  (template in kernel image)
pcpu_base_addr   = 0xC1000000  (allocated by memblock)
delta            = 0x00600000

CPU0 unit offset = 0x00000000  →  __per_cpu_offset[0] = 0x00600000
CPU1 unit offset = 0x00020000  →  __per_cpu_offset[1] = 0x00620000
CPU2 unit offset = 0x00040000  →  __per_cpu_offset[2] = 0x00640000
CPU3 unit offset = 0x00060000  →  __per_cpu_offset[3] = 0x00660000

DEFINE_PER_CPU(int, my_var) located at  0xC0A00100 (in template)
CPU2's my_var                located at  0xC0A00100 + 0x00640000 = 0xC0E40100
```

---

### Stage 6: CPU Bring-Up — Writing the Hardware Register

#### ARM32 (`smp_prepare_boot_cpu` / `secondary_start_kernel`)

```c
// arch/arm/kernel/smp.c:500
void __init smp_prepare_boot_cpu(void)
{
    set_my_cpu_offset(per_cpu_offset(smp_processor_id()));
}
```

For secondary CPUs, this same call happens at the end of `secondary_start_kernel()`. The implementation:

```c
// arch/arm/include/asm/percpu.h:18
static inline void set_my_cpu_offset(unsigned long off)
{
    asm volatile("mcr p15, 0, %0, c13, c0, 4" : : "r" (off) : "memory");
}
```

**`mcr p15, 0, Rn, c13, c0, 4`** — "Move to Coprocessor Register". It writes `Rn` into CP15 register `c13` (Thread and Process ID Register) with CRm=`c0`, opcode2=`4`. This is the **`TPIDRPRW`** — Thread ID Register, Privileged Read/Write.

The key hardware property: `TPIDRPRW` is **not shared** — each CPU core has its own physical register. So each core independently stores its own percpu offset with no contention.

Reading it back:

```c
// arch/arm/include/asm/percpu.h:28
static __always_inline unsigned long __my_cpu_offset(void)
{
    unsigned long off;
    asm("mrc p15, 0, %0, c13, c0, 4" : "=r" (off) : ...);
    return off;
}
#define __my_cpu_offset __my_cpu_offset()
```

**`mrc p15, 0, Rd, c13, c0, 4`** — "Move from Coprocessor Register" — reads `TPIDRPRW` into `Rd`. This is a **single instruction** with no memory access.

**ARMv6 special case**: On pure ARMv6 cores, `TPIDRPRW` may not exist. The code uses `.alt.smp.init` patching — on SMP boot, the instruction is patched to use the register; on UP it falls through to load `__per_cpu_offset[0]` directly. This patching happens at kernel init time by scanning the `.alt.smp.init` section and overwriting the instruction bytes.

#### ARM64 (`smp_prepare_boot_cpu` / `secondary_start_kernel`)

```c
// arch/arm64/kernel/smp.c:456
set_my_cpu_offset(per_cpu_offset(smp_processor_id()));
```

```c
// arch/arm64/include/asm/percpu.h:15
static inline void set_my_cpu_offset(unsigned long off)
{
    asm volatile(ALTERNATIVE("msr tpidr_el1, %0",
                             "msr tpidr_el2, %0",
                             ARM64_HAS_VIRT_HOST_EXTN)
                :: "r" (off) : "memory");
}
```

- **`msr tpidr_el1, Xn`** — writes the 64-bit offset into the Thread Pointer ID Register for EL1. Like ARM32's TPIDRPRW, this register is banked per-CPU.
- **`ALTERNATIVE()`** — at kernel boot, `apply_alternatives()` scans the kernel text for `ALTERNATIVE()` sites. If the CPU supports VHE (Virtualization Host Extensions, where the kernel runs at EL2), the `msr tpidr_el1` instruction is patched in-place to `msr tpidr_el2`. This is a **runtime binary patch** — the `.altinstructions` section contains the location, condition, and replacement bytes. This way the same kernel binary works correctly in bare-metal EL1 and KVM-host EL2 without branches in the hot path.

Reading:
```c
static inline unsigned long __kern_my_cpu_offset(void)
{
    unsigned long off;
    asm(ALTERNATIVE("mrs %0, tpidr_el1",
                    "mrs %0, tpidr_el2",
                    ARM64_HAS_VIRT_HOST_EXTN)
        : "=r" (off) : "Q" (*(const unsigned long *)current_stack_pointer));
    return off;
}
```

The `"Q" (*(const unsigned long *)current_stack_pointer)` is a **compiler hazard trick** — it pretends to read from the stack pointer, forcing GCC to treat this as a memory dependency. This prevents the compiler from caching the `tpidr_el1` value across `barrier()` calls (e.g., after a `preempt_enable()`) while still allowing it to be cached within a preempt-disabled region.

---

### Stage 7: Runtime Access — `this_cpu_read(var)`

```c
// include/linux/percpu-defs.h:239
#define per_cpu(var, cpu)   (*per_cpu_ptr(&(var), cpu))

#define per_cpu_ptr(ptr, cpu) \
    SHIFT_PERCPU_PTR((ptr), per_cpu_offset((cpu)))
    // = (typeof(*ptr) *)((unsigned long)(ptr) + __per_cpu_offset[cpu])

// For the current CPU — hot path:
#define this_cpu_ptr(ptr)   raw_cpu_ptr(ptr)
#define raw_cpu_ptr(ptr)    arch_raw_cpu_ptr(ptr)
// arch_raw_cpu_ptr = SHIFT_PERCPU_PTR(ptr, __my_cpu_offset)
// __my_cpu_offset  = read TPIDRPRW (ARM32) or tpidr_el1 (ARM64)
```

**The full access sequence for `this_cpu_read(my_var)` at the machine level:**

```asm
; ARM32
mrc p15, 0, r0, c13, c0, 4   ; r0 = TPIDRPRW = __per_cpu_offset[current_cpu]
add r0, r0, #<offset_of_my_var_in_percpu_section>
ldr r1, [r0]                  ; r1 = my_var for this CPU

; ARM64
mrs x0, tpidr_el1             ; x0 = __per_cpu_offset[current_cpu]
add x0, x0, #<offset_of_my_var_in_percpu_section>
ldr w1, [x0]                  ; w1 = my_var for this CPU
```

**3 instructions. No locks. No atomic operations. No TLB miss (data is in linear map).** The offset of `my_var` within the percpu section is a **link-time constant** — the compiler encodes it directly as an immediate.

---

### Why No Locks Are Needed

The design is lock-free because of this invariant: **a CPU only ever accesses its own percpu variable via `this_cpu_*`**. There is no sharing — CPU0's data and CPU1's data are at completely different physical addresses. The hardware register simply acts as a fast, per-core base-pointer redirect.

`get_cpu_var(var)` does call `preempt_disable()` before returning `this_cpu_ptr()` — this prevents the task from migrating to a different CPU between getting the pointer and using it. But this is a **software scheduling guard**, not a hardware lock.

---

### Full Annotated Flow Summary

```
DEFINE_PER_CPU(int, my_counter)
   └─ Placed in .data..percpu section by linker
   └─ Symbol &my_counter = address within template (e.g. 0xC0A00100)

start_kernel()
   └─ setup_per_cpu_areas()
         └─ pcpu_build_alloc_info()
               ├─ static_size = __per_cpu_end - __per_cpu_start
               ├─ size_sum = static + reserved + dynamic (page-aligned)
               ├─ Groups CPUs by NUMA LOCAL_DISTANCE
               └─ Returns pcpu_alloc_info with unit_size, cpu_map[]

         └─ pcpu_embed_first_chunk()
               ├─ for each group:
               │     ptr = memblock_alloc(nr_units × unit_size)   ← bootmem
               │     for each cpu in group:
               │         memcpy(ptr + unit_offset, __per_cpu_load, static_size)
               │                                    ↑ copies template
               ├─ pcpu_base_addr = lowest ptr across all groups
               └─ pcpu_setup_first_chunk()
                     ├─ pcpu_unit_offsets[cpu] = group_base + i * unit_size
                     ├─ pcpu_reserved_chunk   (for modules)
                     ├─ pcpu_first_chunk      (for alloc_percpu)
                     └─ pcpu_base_addr = base_addr  ← finalized

         └─ delta = pcpu_base_addr - __per_cpu_start
         └─ for_each_possible_cpu(cpu):
               __per_cpu_offset[cpu] = delta + pcpu_unit_offsets[cpu]

   └─ smp_prepare_boot_cpu()
         └─ set_my_cpu_offset(__per_cpu_offset[0])
               ARM32: mcr p15, 0, r0, c13, c0, 4  → writes TPIDRPRW
               ARM64: msr tpidr_el1, x0            → writes tpidr_el1

Secondary CPU N boots:
   └─ secondary_start_kernel()
         └─ set_my_cpu_offset(__per_cpu_offset[N])
               → CPU N's private hw register now holds its unique offset

Runtime — any code on CPU N:
   └─ this_cpu_read(my_counter)
         └─ reads tpidr_el1 / TPIDRPRW  → offset_N
         └─ &my_counter + offset_N      → CPU N's private address
         └─ load from that address
         └─ Result: no locking, no bus traffic, cache-line owned by CPU N
```

Similar code found with 1 license type