# Per-CPU Memory Layout: Visual Diagrams and Address Relationships

## Source Reference
- `mm/percpu.c:3075` — `pcpu_embed_first_chunk()`
- `mm/percpu.c:2608` — `pcpu_setup_first_chunk()`
- `include/asm-generic/percpu.h:11` — `__per_cpu_offset[]`
- `include/linux/percpu-defs.h` — layout constants

---

## 1. Kernel Image Layout (At Boot, Before Setup)

```
Physical Memory (after decompression):
┌─────────────────────────────────────────────────────────┐
│  0x00000000                                             │
│  ...                                                    │
│  [Kernel Load Address]                                  │
├─────────────────────────────────────────────────────────┤
│  .text (code)                                           │
│  .rodata (read-only data)                               │
│  .init (init-time code, freed after boot)               │
├─────────────────────────────────────────────────────────┤
│  __per_cpu_load  ┐                                      │
│  .data..percpu   │  ← TEMPLATE SECTION                 │
│    cpu_number    │     (from ELF .data..percpu section) │
│    current_task  │     Size = __per_cpu_end -            │
│    runqueues     │           __per_cpu_start             │
│    net_device    │     Typically 64KB - 512KB            │
│    ...           │                                      │
│  __per_cpu_end   ┘                                      │
├─────────────────────────────────────────────────────────┤
│  .data (other kernel data)                              │
│  .bss                                                   │
│  ...                                                    │
└─────────────────────────────────────────────────────────┘
```

The template section (`.data..percpu`) is the **master copy** of all per-CPU
variables at their initial values. It is used as a `memcpy` source.

---

## 2. After `pcpu_embed_first_chunk()` — Allocated Units

```
Physical Memory (NUMA node 0, contiguous allocation):
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  pcpu_base_addr ──────────────────────────────────────┐    │
│                                                        ↓    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │             FIRST CHUNK (embedded)                  │    │
│  │                                                     │    │
│  │  CPU 0 unit (size = PCPU_MIN_UNIT_SIZE = 32KB):     │    │
│  │  ┌───────────────────────────────────────────────┐  │    │
│  │  │ [static data: copied from template]           │  │    │
│  │  │   cpu_number = 0                              │  │    │
│  │  │   current_task = &init_task                   │  │    │
│  │  │   runqueues[0]                                │  │    │
│  │  │   ...                                         │  │    │
│  │  │ [reserved: PERCPU_MODULE_RESERVE = 8KB]       │  │    │
│  │  │ [dynamic: PERCPU_DYNAMIC_RESERVE = 20KB]      │  │    │
│  │  └───────────────────────────────────────────────┘  │    │
│  │                                                     │    │
│  │  CPU 1 unit (PCPU_MIN_UNIT_SIZE = 32KB):            │    │
│  │  ┌───────────────────────────────────────────────┐  │    │
│  │  │ [static data: copied from template]           │  │    │
│  │  │   cpu_number = 1                              │  │    │
│  │  │   current_task = ...                          │  │    │
│  │  │   runqueues[0]                                │  │    │
│  │  │   ...                                         │  │    │
│  │  │ [reserved: 8KB]                               │  │    │
│  │  │ [dynamic: 20KB]                               │  │    │
│  │  └───────────────────────────────────────────────┘  │    │
│  │                                                     │    │
│  │  CPU 2 unit ...                                     │    │
│  │  CPU 3 unit ...                                     │    │
│  │                                                     │    │
│  └─────────────────────────────────────────────────────┘    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Single-Unit Layout (CPU N's Private Area)

```
Address:  pcpu_base_addr + pcpu_unit_offsets[N]
           ↓
┌──────────────────────────────────────────────────────────────────────────┐
│ Per-CPU Unit for CPU N (= PCPU_MIN_UNIT_SIZE minimum, typically 32KB)    │
│                                                                          │
│  Offset 0:                                                               │
│  ┌────────────────────────────────────────────────────────────────┐      │
│  │ STATIC SECTION: copied from .data..percpu template at boot     │      │
│  │                                                                │      │
│  │  +0x0000: cpu_number  = N           (from DEFINE_PER_CPU_FIRST)│      │
│  │  +0x0004: ...                                                  │      │
│  │  +0x0100: current_task              (read-mostly section)      │      │
│  │  +0x0200: runqueues                 (page-aligned section)     │      │
│  │  ...                                                           │      │
│  │  +<static_size>: end of static data                            │      │
│  └────────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  Offset <static_size>:                                                   │
│  ┌────────────────────────────────────────────────────────────────┐      │
│  │ MODULE RESERVED: 8KB                                           │      │
│  │   For per-CPU variables in loadable kernel modules             │      │
│  │   (PERCPU_MODULE_RESERVE = 8 * 1024)                           │      │
│  └────────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  Offset <static_size + 8KB>:                                             │
│  ┌────────────────────────────────────────────────────────────────┐      │
│  │ DYNAMIC RESERVED: 20KB                                         │      │
│  │   For runtime alloc_percpu() allocations                       │      │
│  │   (PERCPU_DYNAMIC_RESERVE = 20 * 1024)                         │      │
│  └────────────────────────────────────────────────────────────────┘      │
│                                                                          │
│  Total = MAX(static+module+dynamic, PCPU_MIN_UNIT_SIZE = 32KB)           │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Virtual Address Calculation Formula

```
Virtual address of var for CPU N:

  VA = (template_VA_of_var) + __per_cpu_offset[N]

Where:
  template_VA_of_var = &var  (the symbol's address in .data..percpu section)
  __per_cpu_offset[N] = pcpu_base_addr - __per_cpu_start + pcpu_unit_offsets[N]

Breaking down __per_cpu_offset[N]:
  
  pcpu_base_addr       = VA of first byte of the embedded chunk allocation
  __per_cpu_start      = VA of first byte of .data..percpu template section
  pcpu_unit_offsets[N] = byte offset of CPU N's unit from pcpu_base_addr

So:
  &var + (pcpu_base_addr - __per_cpu_start + pcpu_unit_offsets[N])
  = pcpu_base_addr + (&var - __per_cpu_start) + pcpu_unit_offsets[N]
  = [base of chunk] + [offset of var within template] + [CPU N's unit offset]
  = [exact address of var in CPU N's private unit]
```

---

## 5. NUMA Multi-Group Layout

```
NUMA Node 0 (CPUs 0-3):                NUMA Node 1 (CPUs 4-7):
┌────────────────────────────────┐      ┌────────────────────────────────┐
│ Allocation from node 0 memory  │      │ Allocation from node 1 memory  │
│                                │      │                                │
│  CPU 0 unit: 32KB              │      │  CPU 4 unit: 32KB              │
│  CPU 1 unit: 32KB              │      │  CPU 5 unit: 32KB              │
│  CPU 2 unit: 32KB              │      │  CPU 6 unit: 32KB              │
│  CPU 3 unit: 32KB              │      │  CPU 7 unit: 32KB              │
│                                │      │                                │
│  Group 0: 4 * 32KB = 128KB     │      │  Group 1: 4 * 32KB = 128KB     │
└────────────────────────────────┘      └────────────────────────────────┘

pcpu_unit_offsets:
  CPU 0: group0_base + 0 * 32KB
  CPU 1: group0_base + 1 * 32KB
  CPU 2: group0_base + 2 * 32KB
  CPU 3: group0_base + 3 * 32KB
  CPU 4: group1_base + 0 * 32KB
  CPU 5: group1_base + 1 * 32KB
  CPU 6: group1_base + 2 * 32KB
  CPU 7: group1_base + 3 * 32KB

pcpu_base_addr = lowest of (group0_base, group1_base)
(groups are laid out to minimize the spread, per pcpu_build_alloc_info() algorithm)
```

---

## 6. Key Symbols and Their Addresses

| Symbol | Type | Value | Meaning |
|---|---|---|---|
| `__per_cpu_start` | Linker symbol | Fixed at link time | Start of `.data..percpu` template section |
| `__per_cpu_end` | Linker symbol | Fixed at link time | End of `.data..percpu` template section |
| `__per_cpu_load` | Linker symbol | = `__per_cpu_start` (usually) | Physical load address of template |
| `pcpu_base_addr` | Runtime variable | Set by `pcpu_setup_first_chunk()` | VA of first byte of first chunk allocation |
| `__per_cpu_offset[N]` | Runtime array | Set by `pcpu_setup_first_chunk()` | Per-CPU offset for CPU N |
| `pcpu_unit_offsets[N]` | Runtime array | Set by `pcpu_embed_first_chunk()` | Byte offset of CPU N's unit from pcpu_base_addr |

---

## 7. Template vs Allocated Copy

```
.data..percpu (TEMPLATE):                CPU 0's unit (ALLOCATED COPY):
┌─────────────────────────┐             ┌─────────────────────────┐
│ cpu_number = 0          │  memcpy     │ cpu_number = 0          │
│ DEFINE_PER_CPU(int, x)  │  ────────→  │ x = 0                   │
│ = 0 (zero-init)         │             │                         │
│                         │             │ (CPU 0 modifies x here) │
│ current_task = ...      │             │ current_task = &init_task│
└─────────────────────────┘             └─────────────────────────┘

                                        CPU 1's unit (ALLOCATED COPY):
                                        ┌─────────────────────────┐
                                        │ cpu_number = 1          │
                                        │ x = 0 (fresh copy)      │
                                        │                         │
                                        │ (CPU 1 modifies x here) │
                                        │ current_task = ...      │
                                        └─────────────────────────┘
```

The template is **not used at runtime**. After `memcpy` during `pcpu_embed_first_chunk()`,
the template's data is only a historical artifact. All runtime accesses go to the
CPU-private allocated copies.

---

## 8. Memory Reservation Constants

```c
/* include/linux/percpu.h */
#define PERCPU_MODULE_RESERVE   (8 << 10)   /* 8KB for module per-CPU vars */
#define PERCPU_DYNAMIC_RESERVE  (20 << 10)  /* 20KB for runtime alloc_percpu */

/* mm/percpu.c */
#define PCPU_MIN_UNIT_SIZE      SZ_32K      /* 32KB minimum unit size */
#define PCPU_MIN_ALLOC_SIZE     8           /* 8 bytes minimum allocation */
#define PCPU_BITMAP_BLOCK_SIZE  PAGE_SIZE   /* allocation block size */
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| Where is the template section? | `.data..percpu` in the kernel image |
| What is `__per_cpu_start`? | Linker symbol marking start of .data..percpu template |
| What is `pcpu_base_addr`? | Runtime VA of first byte of first chunk allocation |
| What is `__per_cpu_offset[N]`? | `pcpu_base_addr - __per_cpu_start + pcpu_unit_offsets[N]` |
| Is template section used at runtime? | No — only for initial memcpy; runtime uses allocated copies |
| Where are NUMA CPUs allocated? | From their local NUMA node's memory for cache locality |
| What is PCPU_MIN_UNIT_SIZE? | 32KB — minimum size of each CPU's private area |
| What is PERCPU_MODULE_RESERVE? | 8KB reserved within each unit for module per-CPU variables |
| What is PERCPU_DYNAMIC_RESERVE? | 20KB reserved for `alloc_percpu()` runtime allocations |
