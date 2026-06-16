# reserve_crashkernel() — System Design Approach and Q&A

## 1. Why Early Reservation?

The crash kernel reservation must happen in `setup_arch()` because:
1. `memblock_phys_alloc_range()` must run before `mem_init()` (which transitions from memblock to buddy allocator)
2. The buddy allocator would fragment the reserved region if initialized first
3. `insert_resource(&iomem_resource, &crashk_res)` marks it in the resource tree — drivers can't claim this address range

If reservation were delayed to `late_initcall`:
- Buddy allocator would have already split this region into pages
- No guarantee of finding contiguous free memory at the desired address
- Even if found, removing pages from buddy (marking as reserved) is complex

---

## 2. Memory Lifecycle

```
reserve_crashkernel() at boot:
  memblock reserves [crash_base .. crash_base+crash_size]
  crashk_res inserted into iomem tree
        │
[mem_init()] — memblock transitions to buddy
  crash region NOT given to buddy allocator
  Crash memory: physically exists but invisible to buddy
        │
[User runs: kexec -p /boot/vmlinuz-capture ...]
  kexec syscall → sys_kexec_load()
  Copies capture kernel + initramfs into crash region
  Crash region now contains a valid bootable kernel
        │
[kernel panic]
  machine_kexec() → jumps to crash region start
  Capture kernel boots from crash region
  /dev/crash maps all production kernel memory
  Crash dump saved → system reboots
```

---

## 3. Dependency Graph

```
[parse_early_param()] — parses crashkernel= from cmdline early
  └── crash_size / crash_base parameters available
        │
[arm_memblock_init()] — memblock populated with all RAM
        │
[paging_init()] — page tables ready
        │
[request_standard_resources()] — iomem tree built
        │
[reserve_crashkernel()]
  ├── parse_crashkernel() → crash_size, crash_base
  ├── memblock_phys_alloc_range() → allocate in memblock
  ├── crashk_res.start = crash_base
  ├── crashk_res.end   = crash_base + crash_size - 1
  └── insert_resource(&iomem_resource, &crashk_res)
        │
[mem_init()] — crash region excluded from buddy allocator
        │
[kexec -p at runtime] — fills crash region with capture kernel
```

---

## 4. System Design Q&A

**Q: What is the relationship between kexec and kdump? Are they the same?**
> `kexec` is the general mechanism for loading and jumping to a new kernel without a full hardware reboot — essentially software-controlled rebooting. You can `kexec` into a new kernel voluntarily (for live kernel updates, rolling upgrades). `kdump` is a specific application of kexec: it uses kexec to boot a **capture kernel** specifically in response to a kernel crash/panic. The capture kernel is loaded in advance (at boot + user-space `kexec -p`) into the crash-reserved memory. When panic fires, `machine_kexec()` (the architecture-level kexec execution) jumps to the pre-loaded capture kernel. So: kexec is the mechanism, kdump is the application.

**Q: What is vmcoreinfo and how does it help crash analysis?**
> `vmcoreinfo` is a special kernel section (registered via `vmcoreinfo_append_str()`) that contains a text blob with key kernel symbols, struct offsets, and constants. For example:
> ```
> SYMBOL(init_uts_ns)=c0a12345
> OFFSET(task_struct.pid)=548
> SIZE(task_struct)=4608
> ```
> The physical address of this section is passed to the capture kernel via the kexec control block. `makedumpfile` and the `crash` tool read `vmcoreinfo` first, then use the symbol addresses to locate kernel data structures (`task_struct` list for all processes, `vm_area_struct` for memory mappings, dmesg ring buffer) in the raw memory dump. Without `vmcoreinfo`, you'd need the `System.map` file and correct offsets for the exact crashed kernel version — `vmcoreinfo` embeds them directly in the dump.

**Q: Why is crashkernel memory marked IORESOURCE_BUSY? Couldn't a driver accidentally use it?**
> Without `IORESOURCE_BUSY`, a device driver calling `request_mem_region(crash_base, crash_size, "mydriver")` would succeed silently — driver would be mapped over the reserved crash kernel region, overwriting it with MMIO mappings. This would make kdump fail (the crash region no longer contains the capture kernel). `IORESOURCE_BUSY` causes `request_resource()` to return `-EBUSY` for any overlapping range, so drivers trying to claim that physical address range get an error and fail their probe. This is the same protection that prevents drivers from claiming "Kernel code" or "Kernel data" memory regions.
