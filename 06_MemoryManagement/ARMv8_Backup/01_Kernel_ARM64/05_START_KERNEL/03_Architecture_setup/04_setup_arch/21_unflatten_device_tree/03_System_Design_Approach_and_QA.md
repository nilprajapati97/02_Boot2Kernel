# unflatten_device_tree() — System Design Approach and Q&A

## 1. Why Two Formats? FDT Blob vs device_node Tree

The FDT blob format (chosen by the Open Firmware / IEEE 1275 standard) is optimized for **compact storage and transmission** from firmware to kernel:
- Compact binary encoding
- Shared string table (each property name stored once)
- Easy to generate by U-Boot, UEFI, or qemu
- Architecture-neutral (big-endian)

The `device_node` tree is optimized for **kernel runtime access**:
- Pointer-based traversal (parent/child/sibling)
- Fast `of_find_node_by_*()` with DFS
- Properties accessible as `struct property` (no re-parsing)
- Compatible with `struct fwnode_handle` (unified firmware node interface used by drivers)

The two-format approach separates the firmware communication protocol (FDT) from the kernel's internal data structure.

---

## 2. Memory Lifecycle of the Device Tree

```
Boot:
  [Bootloader] creates FDT blob → placed in RAM
        │
  [early boot] FDT blob mapped read-only (preserved by memblock_reserve)
        │
  [unflatten_device_tree()] allocates device_node tree via memblock_alloc
  FDT blob still kept (referenced by initial_boot_params)
        │
  [Driver model init] drivers probe using of_find_compatible_node()
        │
  [Boot complete] FDT blob could be freed (but isn't — kept for /proc/device-tree)
        │
  [Runtime] device_node tree persists forever (driver references held)
```

The unflattened tree memory is **never freed** — it's allocated via `memblock_alloc()` before the buddy allocator is initialized, so it can't be returned to the allocator. This is by design: the DT tree must be available for driver hotplug events (USB devices, PCIe hotplug) throughout kernel lifetime.

---

## 3. /proc/device-tree — Userspace Access

After `unflatten_device_tree()`, the `/proc/device-tree` filesystem (procfs overlay) is populated:

```
/proc/device-tree/
├── compatible (file: "raspberrypi,2-model-b\0brcm,bcm2836\0")
├── #address-cells (file: 4 bytes big-endian: 0x00000001)
├── cpus/
│   ├── cpu@0/
│   │   ├── compatible
│   │   ├── reg
│   │   └── enable-method
│   └── cpu@1/, cpu@2/, cpu@3/
├── memory@0/
│   ├── device_type
│   └── reg
└── ...
```

Tools like `dtc -I fs /proc/device-tree` can reconstruct the DTS source from the live kernel tree, useful for debugging.

---

## 4. Dependency Graph

```
[setup_machine_fdt()] — early in setup_arch()
  └── initial_boot_params = FDT virtual address (mapped)
  └── FDT reserved in memblock
        │
[paging_init()] → permanent page tables
  └── memblock_alloc() now returns properly-mapped memory
        │
[request_standard_resources()] → iomem tree built
        │
[unflatten_device_tree()]
  ├── Pass 1: calculate size → memblock_alloc(size)
  ├── Pass 2: fill device_node/property structs
  ├── of_root → points to "/" node
  └── of_alias_scan() → builds alias table (aliases like "serial0 = &uart0")
        │
[arm_dt_init_cpu_maps()] — uses of_find_node_by_path("/cpus")
[psci_dt_init()] — uses of_find_compatible_node(NULL, NULL, "arm,psci-0.2")
[smp_init_cpus()] — iterates cpu@N nodes
```

---

## 5. System Design Q&A

**Q: Why not just make the FDT format the native kernel format? Why unflatten at all?**
> The FDT format requires sequential scanning to find nodes (no random access by pointer), has big-endian property values requiring conversion on every access, lacks parent pointers (can't go "up" the tree), and properties are raw bytes without type information. Every `of_find_compatible_node()` call would need to re-scan the entire FDT blob linearly. The `device_node` tree with pointers enables O(log N) traversal, random access, and caching. The cost is a one-time O(N) traversal at boot and 5-10x memory expansion — a worthwhile trade for dramatically faster runtime performance when thousands of driver probes traverse the tree.

**Q: How does of_alias_scan() work and why is it called after unflatten?**
> `of_alias_scan()` reads the `/aliases` node in the DT, which maps short names to node paths: `serial0 = "/soc/serial@20201000"`. After unflattening, `of_alias_scan()` traverses all properties of the `/aliases` node, calls `of_find_node_by_path()` for each alias target, and builds a sorted list of `alias_prop` structs. This enables `of_alias_get_id("serial", dev)` — used by UART, SPI, I2C controllers to determine their bus number (e.g., `/dev/ttyS0` is "serial0"). Aliases must be scanned after unflattening because `of_find_node_by_path()` requires the `device_node` tree to exist.

**Q: What does the `detached` parameter in __unflatten_device_tree() do?**
> When `detached=false` (used for the main device tree), the unflattened root node is connected to the global `of_root` and all OF (Open Firmware) APIs work on it immediately. When `detached=true`, the unflattened tree is standalone — not connected to `of_root`. This is used for DT overlays and for the OF unittest infrastructure. A detached tree can be inspected and modified without affecting the live kernel DT. When an overlay is applied via `of_overlay_fdt_apply()`, the overlay's nodes are merged into the live tree atomically.
