# unflatten_device_tree() — Detailed Design Bottom-To-Top Flow

## 1. What Is the Flat Device Tree?

When the bootloader (U-Boot, UEFI, etc.) starts the kernel, it passes a **Flattened Device Tree (FDT)** blob — a binary representation of all hardware:

```
FDT blob at physical address (passed in r2/x1):
┌─────────────────────────────────┐
│ struct fdt_header               │
│   magic: 0xD00DFEED             │
│   totalsize: N bytes            │
│   off_dt_struct: offset to tree │
│   off_dt_strings: offset        │
├─────────────────────────────────┤
│ Structure block (tree nodes)    │
│   FDT_BEGIN_NODE "/"            │
│     FDT_BEGIN_NODE "cpus"       │
│       FDT_BEGIN_NODE "cpu@0"    │
│         FDT_PROP compatible     │
│         FDT_PROP reg            │
│       FDT_END_NODE              │
│     FDT_END_NODE                │
│     FDT_BEGIN_NODE "memory@0"   │
│       FDT_PROP device_type      │
│       FDT_PROP reg              │
│     FDT_END_NODE                │
│   ...                           │
│   FDT_END_NODE                  │
│   FDT_END                       │
├─────────────────────────────────┤
│ Strings block                   │
│   "compatible\0"                │
│   "device_type\0"               │
│   "reg\0"                       │
│   ...                           │
└─────────────────────────────────┘
```

This FDT blob is:
- **Compact** — uses offsets, string table deduplication
- **Read-only format** — not a kernel-native tree
- **Cannot be searched efficiently** — iterating requires parsing each token

---

## 2. The Problem: FDT is Unusable for Runtime Kernel Code

Before `unflatten_device_tree()`:
- `of_find_node_by_name()` → must parse FDT blob linearly (O(N))
- `of_find_compatible_node()` → slow, unusable for driver probe
- No parent/child/sibling pointers in FDT
- Properties stored as raw bytes (big-endian), need `fdt32_to_cpu()` on each access

After `unflatten_device_tree()`:
- `of_find_node_by_name()` → traverses `device_node` tree (O(depth))
- `of_find_compatible_node()` → fast tree walk
- `property` structs with pre-converted data
- `of_root` global pointer to tree root
- `of_find_node_by_path("/cpus/cpu@0")` → usable immediately

---

## 3. unflatten_device_tree() Source

**File:** `drivers/of/fdt.c`

```c
void __init unflatten_device_tree(void)
{
    __unflatten_device_tree(initial_boot_params, NULL,
                            &of_root, early_init_dt_alloc_memory_arch, false);

    /* Setup pointer shortcuts to important nodes */
    of_alias_scan(early_init_dt_alloc_memory_arch);

    unitaddr_tail_drop = of_property_read_bool(of_root, "linux,#address-cells");
}
```

`initial_boot_params` was set to the physical FDT address (mapped to virtual) during `setup_machine_fdt()` earlier in `setup_arch()`.

---

## 4. __unflatten_device_tree() — Two Passes

```c
static void __init __unflatten_device_tree(
    const void *blob,           /* FDT blob */
    struct device_node *dad,    /* parent (NULL for root) */
    struct device_node **mynodes, /* output: root device_node */
    void * (*dt_alloc)(u64 size, u64 align),  /* allocator */
    bool detached)
{
    unsigned long size;
    int start;
    void *mem;

    /* PASS 1: Calculate total memory needed */
    start = unflatten_dt_nodes(blob, NULL, dad, NULL);
    size = (unsigned long)start;

    /* Allocate all memory at once */
    mem = dt_alloc(size + 4, __alignof__(struct device_node));
    memset(mem, 0, size);

    /* PASS 2: Actually fill in the device_node structures */
    unflatten_dt_nodes(blob, mem, dad, mynodes);
}
```

**Pass 1 (dry run):** Traverses FDT, counting bytes needed for all `device_node` and `property` structs. Returns total size.

**Pass 2 (real):** Traverses FDT again, allocating from the pre-allocated `mem` block, and fills in all structs. Sets parent/child/sibling/properties pointers.

This two-pass approach allows a single large `memblock_alloc()` instead of many small allocations, avoiding fragmentation.

---

## 5. struct device_node — The Runtime Tree

```c
struct device_node {
    const char  *name;      /* node name (e.g., "cpu", "memory") */
    phandle      phandle;   /* unique handle for cross-references */
    const char  *full_name; /* path: "/cpus/cpu@0" */
    struct fwnode_handle fwnode;

    struct property *properties;  /* linked list of this node's properties */
    struct property *deadprops;   /* properties removed via DT overlay */

    struct device_node *parent;   /* parent node */
    struct device_node *child;    /* first child */
    struct device_node *sibling;  /* next sibling */
};
```

```c
struct property {
    char *name;         /* property name: "compatible", "reg", etc. */
    int   length;       /* value length in bytes */
    void *value;        /* raw property bytes (big-endian) */
    struct property *next;  /* next property in this node */
};
```

---

## 6. Tree Structure After unflatten_device_tree()

```
of_root ("/")
  ├── compatible = "raspberrypi,2-model-b", "brcm,bcm2836"
  ├── #address-cells = <1>
  ├── child: "cpus"
  │     ├── #address-cells = <1>
  │     ├── child: "cpu@0"
  │     │     ├── compatible = "arm,cortex-a7"
  │     │     ├── device_type = "cpu"
  │     │     ├── reg = <0x0>
  │     │     └── enable-method = "psci"
  │     └── sibling: "cpu@1", "cpu@2", "cpu@3"
  ├── sibling: "memory@0"
  │     ├── device_type = "memory"
  │     └── reg = <0x0 0x3b400000>
  ├── sibling: "timer"
  │     ├── compatible = "arm,armv7-timer"
  │     └── interrupts = ...
  └── sibling: ... (many more nodes)
```

---

## 7. Interview Q&A

**Q1: Why does unflatten_device_tree() run after paging_init() but before driver init?**
> Two requirements: (1) `unflatten_device_tree()` allocates large amounts of memory for `device_node` structs via `memblock_alloc()` — this needs permanent page mappings from `paging_init()`. (2) `arm_dt_init_cpu_maps()`, `psci_dt_init()`, and `smp_init_cpus()` all need to traverse the device tree (`of_find_node_by_path("/cpus")`, etc.) — they come immediately after in `setup_arch()`. So it must run after paging but before any DT traversal. In the Linux boot sequence, this is the first time `of_find_node_by_*()` functions become usable.

**Q2: What is the memory size of a typical unflattened device tree?**
> A typical embedded ARM board DT blob is 30-80KB. After unflattening, the tree uses roughly 5-10x more memory due to: struct overhead (each `device_node` ~80 bytes + each `property` ~32 bytes + name/value copies). A device tree with 500 nodes × 5 properties each = 500×80 + 2500×32 = 120KB of device_node/property structs, plus string/value data. Total: typically 300-600KB for an embedded board, 1-3MB for a server with many PCIe devices and CPUs. This memory is **never freed** — the device tree must be available for driver probe throughout the kernel lifetime.

**Q3: How does of_find_compatible_node() work after unflattening?**
> After unflattening, `of_find_compatible_node(from, type, compat)` calls `of_get_next_node(from, NULL)` which walks the tree using child/sibling pointers (DFS). For each node, it calls `of_device_is_compatible(np, compat)` which calls `of_prop_cmp()` on the `compatible` property string list. The property value is a list of null-terminated strings: e.g., `"arm,cortex-a7\0arm,cortex-a5\0"`. Matching any one string returns the node. Before unflattening, this would require parsing the FDT structure block linearly and comparing raw bytes — much slower and more complex.
