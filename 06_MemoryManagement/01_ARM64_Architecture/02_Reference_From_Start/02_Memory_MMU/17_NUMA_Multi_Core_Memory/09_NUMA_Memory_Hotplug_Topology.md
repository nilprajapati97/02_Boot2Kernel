# NUMA Memory Hotplug and Topology Changes Deep Dive

Category: NUMA and Multi-Core Memory  
Platform: ARM64 (AArch64), dynamic memory and topology

---

## 1. Concept Foundation

NUMA memory hotplug (adding/removing NUMA nodes or memory dynamically) complicates already-complex topology management.

Scenarios:
- hypervisor adds NUMA node at runtime
- memory module removed due to failure or maintenance
- zone rebalancing required

---

## 2. ARM64 Hardware Detail

### 2.1 Memory hotplug mechanisms

Firmware-driven:
- ACPI SPMI (System Management Interrupt) notifications
- device-tree updates

Kernel detection:
- reprobe SRAT/SLIT tables
- reinitialize zone lists
- rebalance kswapd threads

### 2.2 CPU hotplug interaction

CPU hotplug and memory hotplug often coordinated:
- adding CPU may enable new NUMA node
- removing CPU may disable node

---

## 3. Linux Kernel Implementation

### 3.1 Memory add path

online_memory() or equivalent:
1. add pages to zone (online_pages)
2. update node's memory stats
3. rebuild zonelist if needed
4. may spawn new kswapd thread

### 3.2 Memory removal path

offline_memory() or equivalent:
1. migrate pages off target region (if possible)
2. remove pages from zone
3. update stats
4. may retire kswapd thread

### 3.3 Zonelist rebuild

Critical: zonelist must reflect actual node topology.
After hotplug, zonelist reordered to maintain latency-aware fallback.

---

## 4. Hardware-Software Interaction

Hotplug scenario:
1. VM at runtime adds NUMA node 3 to guest
2. ACPI notification triggers kernel handler
3. kernel reprobe firmware tables
4. reinitialize zones and zonelist
5. allocator now aware of new node 3
6. kswapd thread spawned for node 3
7. future allocations can use node 3

Operational complexity:
- ongoing allocations may fail during transition
- page migration overhead during rebuild
- latency blips likely during topology change

---

## 5. Interview Q and A

Q1: Why is memory hotplug complicated on NUMA?
Because zonelist, watermarks, and kswapd configuration all depend on topology; changes require careful synchronization.

Q2: Can you hotplug memory out while applications are running?
Theoretically yes, but migrating all pages away is difficult and expensive; usually only done on retiring hardware.

Q3: What happens if memory hotplug fails mid-operation?
Graceful rollback with partial pages migrated may cause inconsistent state; kernel tries to handle but can be messy.

Q4: How do you monitor memory hotplug events?
Monitor udev events or check /sys/devices/system/memory/*/state for each memory block.

Q5: Can NUMA balancing handle topology changes?
Auto-NUMA should adapt, but aggressive probing during topology change can cause performance blips.

Q6: What is the typical hotplug latency?
Depends on memory size and page migration overhead; seconds to minutes for large regions.

---

## 6. Pitfalls and Gotchas

- Not considering migration cost when planning hotplug operations.
- Assuming zonelist automatically updates (it does, but timing matters).
- Forgetting to coordinate memory and CPU hotplug timings.
- Running memory-sensitive workloads during topology transitions.
- Not testing hotplug procedures in realistic failure scenarios.

---

## 7. Quick Reference Table

| Event | Typical outcome |
|---|---|
| memory added | pages online, zonelist rebuilt, new kswapd spawned |
| memory removed | pages migrated off, zone shrinks, kswapd may retire |
| zone_reclaim disabled | all-nodes fallback during topology rebuild |

| Observable | Meaning |
|---|---|
| /sys/devices/system/memory/memoryX/state | online or offline status of memory block |
| kswapd process list | new threads for new nodes |
| kernel messages | hotplug event logging |
