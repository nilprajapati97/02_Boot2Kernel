# Scenario 9: Memory Corruption / Bit Flip

## Symptom

### Random Oops at Varying Locations:
```
[ 12045.789012] Unable to handle kernel paging request at virtual address ffff0000abcd5678
[ 12045.789020] Internal error: Oops: 0000000096000007 [#1] PREEMPT SMP
[ 12045.789030] pc : ext4_iget+0x3c4/0xcf0
[ 12045.789035] ...
[ 12045.789040] Call trace:
[ 12045.789042]  ext4_iget+0x3c4/0xcf0
[ 12045.789046]  ext4_lookup+0xe4/0x1e0
[ 12045.789050]  __lookup_slow+0x13c/0x1e0
[ 12045.789054]  walk_component+0x174/0x220
```
Next time the crash is somewhere completely different:
```
[ 15890.123456] BUG: unable to handle kernel paging request at ffff0001deadbe00
[ 15890.123465] pc : __list_del_entry_valid+0x44/0x100
```

### EDAC (Hardware Error) Report:
```
[ 5678.901234] EDAC MC0: 1 CE memory read error on CPU_SrcID#0_Ha#0_Chan#1_DIMM#0
[ 5678.901240] EDAC MC0: page:0x12345 offset:0x678 grain:32 syndrome:0x0000
[ 5678.901245] EDAC MC0: label "DIMM_A1": 1 Corrected Errors
```

### ARM64 SError (Async External Abort):
```
[ 8901.234567] SError Interrupt on CPU2, code 0xbf000002 -- SError
[ 8901.234575] CPU: 2 PID: 1000 Comm: my_app Tainted: G           O      6.8.0 #1
[ 8901.234580] pstate: 60400009 (nZCv daif +PAN -UAO)
[ 8901.234585] Kernel panic - not syncing: Asynchronous SError Interrupt
```

### How to Recognize
- **Crashes at random, unrelated locations** — different functions each time
- **Corrupted data structures** — list pointers invalid, magic numbers wrong
- **EDAC error messages** — corrected or uncorrected memory errors
- **SError on ARM64** — asynchronous external abort (bus error, ECC uncorrectable)
- **Single-bit patterns** — a value differs from expected by exactly one bit
- **KFENCE / KASAN reports at unexpected locations** — corruption from elsewhere

---

## Background: Sources of Memory Corruption

### Hardware vs Software Corruption

```
┌─────────────────────────────────────────────────────────────┐
│                  HARDWARE CORRUPTION                         │
├─────────────────────────────────────────────────────────────┤
│ • DRAM bit flip (cosmic ray, aging, heat)                    │
│ • ECC failure (uncorrectable multi-bit error)                │
│ • DMA to wrong address (buggy DMA engine / IOMMU bypass)     │
│ • PCIe data corruption (link error, signal integrity)        │
│ • Firmware/TrustZone writing kernel memory                   │
│ • Voltage/frequency instability (overclocking)               │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                  SOFTWARE CORRUPTION                         │
├─────────────────────────────────────────────────────────────┤
│ • Buffer overflow (write past array bounds)                  │
│ • Wild pointer (uninitialized or corrupted pointer)          │
│ • DMA buffer overrun (device writes more than expected)      │
│ • Missing IOMMU protection (device DMA to kernel pages)      │
│ • Race condition (concurrent unprotected write)              │
│ • Stack overflow corrupting adjacent memory                  │
│ • Incorrect __iomem access (MMIO written as regular memory)  │
└─────────────────────────────────────────────────────────────┘
```

### Bit Flip Example
```
Correct value:  0x00000000_00400000  (bit 22 set)
Corrupted:      0x00000000_00400100  (bit 22 + bit 8 set)
                                ^^^
                                Single bit flipped!

This changes:
  - A valid kernel pointer → invalid pointer → page fault
  - A struct field value → wrong value → logic error
  - A page table entry → wrong mapping → silent data corruption
  - An instruction → different instruction → undefined behavior
```

### ARM64 Memory System
```
CPU → L1 Cache → L2 Cache → LLC → Interconnect → DDR Controller → DIMM
                                                         │
                                                    ECC Check
                                                    │
                                         ┌──────────┴──────────┐
                                         │ Correctable (CE)     │
                                         │ → EDAC reports,      │
                                         │   data corrected     │
                                         ├──────────────────────┤
                                         │ Uncorrectable (UE)   │
                                         │ → SError / Machine   │
                                         │   Check → PANIC      │
                                         └──────────────────────┘
```

---

## ARM64 SError (System Error) — Async External Abort

```c
// arch/arm64/kernel/traps.c

// SError is ARM64's asynchronous abort mechanism:
// - Caused by: ECC uncorrectable errors, bus errors, PCIe errors
// - Delivered asynchronously (not at the faulting instruction)
// - EC = 0x2F in ESR_EL1

void do_serror(struct pt_regs *regs, unsigned long esr)
{
    // Decode the ISS field:
    // AET (Asynchronous Error Type):
    //   0b00 = Recoverable (UC)
    //   0b01 = Uncontainable
    //   0b10 = Restartable
    //   0b11 = Recoverable

    nmi_enter();

    // On most implementations: panic
    arm64_serror_panic(regs, esr);

    nmi_exit();
}

static void arm64_serror_panic(struct pt_regs *regs, unsigned long esr)
{
    pr_crit("SError Interrupt on CPU%d, code 0x%016lx -- %s\n",
            smp_processor_id(), esr, "SError");
    __show_regs(regs);
    panic("Asynchronous SError Interrupt");
}
```

---

## EDAC (Error Detection and Correction) Subsystem

```c
// drivers/edac/edac_mc.c

// EDAC polls memory controllers for error counters:
// - Correctable Errors (CE): single-bit → ECC corrected
// - Uncorrectable Errors (UE): multi-bit → data is corrupted

void edac_mc_handle_error(const enum hw_event_mc_err_type type,
                          struct mem_ctl_info *mci,
                          const u16 error_count,
                          const unsigned long page_frame_number,
                          const unsigned long offset_in_page,
                          const unsigned long syndrome, ...)
{
    if (type == HW_EVENT_ERR_CORRECTED)
        // CE: data was corrected by ECC, but the DIMM may be failing
        edac_ce_error(mci, error_count, ...);
    else
        // UE: data is WRONG — potential corruption
        edac_ue_error(mci, error_count, ...);
}
```

---

## Common Causes

### 1. DMA Buffer Overrun
```c
/* Hardware writes more data than allocated buffer: */
struct my_device {
    void *dma_buf;     // 4096 bytes allocated
    dma_addr_t dma_addr;
};

int setup_dma(struct my_device *dev) {
    dev->dma_buf = dma_alloc_coherent(dev->dev, 4096,
                                       &dev->dma_addr, GFP_KERNEL);
    // Program device to transfer 8192 bytes → overflow!
    writel(8192, dev->regs + DMA_LENGTH);  // BUG!
    writel(dev->dma_addr, dev->regs + DMA_ADDR);
    // Device writes 8192 bytes starting at dma_addr
    // → 4096 bytes past buffer → corrupts adjacent kernel memory
}
```

### 2. Missing IOMMU Protection
```c
/* Without IOMMU, device DMA uses physical addresses directly.
   A buggy device can DMA to ANY physical address.

   With IOMMU: device sees virtual addresses → can only access
   mapped pages → contained. */

// Check IOMMU status:
// dmesg | grep -i iommu
// /sys/kernel/iommu_groups/
```

### 3. Hardware Memory Failure (Bit Flip)
```
DIMM aging, cosmic rays, heat, or manufacturing defects:
→ Single-bit error: ECC corrects it, logs CE
→ Multi-bit error: ECC can't correct → UE → SError → panic

Symptoms:
- edac_mc0: Corrected Error count increasing
- Random crashes in unrelated code
- Data files with unexpected bit changes
```

### 4. Buffer Overflow (Software)
```c
void process_packet(struct sk_buff *skb) {
    char header[64];
    // Bug: packet header is 128 bytes but buffer is 64
    memcpy(header, skb->data, skb->len);  // skb->len = 128
    // → Overwrites 64 bytes past header on the stack
    // → Corrupts return address, saved registers, adjacent variables
}
```

### 5. Wild Pointer / Uninitialized Variable
```c
void my_function(void) {
    struct page *page;  // NOT initialized

    if (some_condition)
        page = alloc_page(GFP_KERNEL);

    // If !some_condition, page is garbage
    page->flags |= PG_dirty;  // Writing to random kernel memory!
}
```

### 6. Incorrect Cache Management (ARM64 Specific)
```c
/* On ARM64, DMA requires cache maintenance:
   CPU cache and device may see different memory contents.
   Missing cache flush/invalidate → stale data → corruption */

// WRONG: missing sync
void *buf = kmalloc(4096, GFP_KERNEL);
dma_addr_t addr = dma_map_single(dev, buf, 4096, DMA_FROM_DEVICE);
start_dma_transfer(dev);
// ... DMA completes ...
// BUG: forgot dma_unmap_single() → CPU reads stale cache line
use_data(buf);  // May see old data!

// CORRECT:
dma_unmap_single(dev, addr, 4096, DMA_FROM_DEVICE);
use_data(buf);  // CPU cache invalidated, reads fresh DMA data
```

---

## Debugging Steps

### Step 1: Determine Hardware vs Software Corruption
```bash
# Check EDAC for hardware errors:
cat /sys/devices/system/edac/mc/mc0/ce_count
cat /sys/devices/system/edac/mc/mc0/ue_count

# Check for SErrors in dmesg:
dmesg | grep -i "SError\|serror\|EDAC\|Hardware Error\|MCE"

# Run memtest86+ from GRUB:
# If memtest finds errors → hardware problem → replace DIMM
```

### Step 2: Check for Bit Flip Pattern
```bash
# Compare corrupted value to expected:
# Expected: 0xffff000012345678
# Actual:   0xffff000012345e78
#                          ^
#                     0x678 → 0xe78
#                     bit 11 flipped (0x800)

# If exactly 1 or 2 bits differ → likely hardware
# If many bits differ → likely software (wild write)
```

### Step 3: Enable KASAN for Software Corruption
```bash
CONFIG_KASAN=y              # Address sanitizer
CONFIG_KASAN_GENERIC=y      # Full checking
# KASAN detects: buffer overflow, UAF, out-of-bounds
```

### Step 4: Enable KFENCE for Production
```bash
CONFIG_KFENCE=y
CONFIG_KFENCE_SAMPLE_INTERVAL=100
# Low-overhead, catches some buffer overflows via guard pages
```

### Step 5: Use `slub_debug` to Detect Slab Corruption
```bash
# Boot with:
slub_debug=FZPU

# F: sanity checks on free
# Z: red zones (detect overflow past object boundary)
# P: poison freed objects (detect UAF reads)
# U: track alloc/free callers

# Red zone check catches:
# Writing 1 byte past a kmalloc-64 object → red zone overwritten
# → "Redzone overwritten" BUG at next alloc/free
```

### Step 6: Debug DMA Corruption
```bash
# DMA debug:
CONFIG_DMA_API_DEBUG=y

# Boot with:
dma_debug=1

# This detects:
# - DMA to non-DMA-mapped memory
# - Missing dma_unmap calls
# - Wrong DMA direction
# - DMA to freed memory
```

### Step 7: Crash Tool Analysis
```bash
crash vmlinux vmcore

# Check if corrupted address is in a slab:
crash> kmem ffff0000abcd5678
# If it's in a slab → look for overflow from adjacent object

# Check page flags:
crash> page ffff0000abcd5000
# PG_poison, PG_hwpoison → hardware error

# Dump surrounding memory:
crash> rd ffff0000abcd5600 64
# Look for poison patterns (6b6b, 5a5a) or valid data mixed with garbage
```

### Step 8: Use `page_poison` for Page-Level Detection
```bash
CONFIG_PAGE_POISONING=y
# or boot: page_poison=1

# When page is freed, fill with 0xAA
# When allocated, check for 0xAA
# If value changed while page was free → external corruption (DMA, HW)
```

---

## Fixes

| Cause | Fix |
|-------|-----|
| DMA buffer overrun | Size DMA transfer correctly; use scatter-gather |
| Missing IOMMU | Enable IOMMU (`iommu=pt` → `iommu=strict`) |
| Hardware bit flip | Replace DIMM; enable ECC; use `hwpoison` |
| Buffer overflow | Bounds checking; use `ksize()` to verify |
| Wild pointer | Initialize all pointers; use KASAN |
| Cache coherency | Proper `dma_map/unmap_single()`; correct direction |
| Race condition | Locking; RCU for read-heavy paths |

### Fix Example: Correct DMA Mapping
```c
/* BEFORE: missing cache sync */
void receive_data(struct my_device *dev) {
    dma_addr_t addr = dma_map_single(dev->dev, dev->buf,
                                      4096, DMA_FROM_DEVICE);
    start_hw_dma(dev, addr);
    wait_for_dma_done(dev);
    // BUG: reading buf without unmapping → stale cache
    process(dev->buf);
    dma_unmap_single(dev->dev, addr, 4096, DMA_FROM_DEVICE);
}

/* AFTER: correct ordering */
void receive_data(struct my_device *dev) {
    dma_addr_t addr = dma_map_single(dev->dev, dev->buf,
                                      4096, DMA_FROM_DEVICE);
    if (dma_mapping_error(dev->dev, addr))
        return -EIO;

    start_hw_dma(dev, addr);
    wait_for_dma_done(dev);

    dma_unmap_single(dev->dev, addr, 4096, DMA_FROM_DEVICE);
    // Now CPU cache is invalidated → fresh data
    process(dev->buf);
}
```

### Fix Example: Handle Hardware Memory Errors
```bash
# 1. Enable memory error handling:
CONFIG_MEMORY_FAILURE=y
CONFIG_HWPOISON_INJECT=y    # for testing

# 2. Kernel marks bad pages as hwpoison:
# When ECC reports UE on a page:
# → memory_failure(pfn, flags)
# → Page marked PG_hwpoison
# → Never allocated again
# → Process using it gets SIGBUS

# 3. Monitor:
cat /sys/devices/system/edac/mc/mc0/ce_count
# Rising count → DIMM failing, schedule replacement
```

---

## Corruption Pattern Analysis

| Pattern | Likely Cause |
|---------|-------------|
| Single bit flipped | Hardware (cosmic ray, DIMM fail) |
| 1-8 bytes zeroed | DMA overwrite or memset bug |
| `0x6b6b6b6b` in live object | UAF: reading freed slab memory |
| `0x5a5a5a5a` pattern | SLUB padding overwritten |
| `0xdead...` pattern | Kernel debug poison (`POISON_POINTER_DELTA`) |
| Valid pointer ± small offset | Buffer overflow from adjacent object |
| Random multi-byte corruption | Wild pointer write or DMA |
| Corruption only after suspend/resume | Power state / cache corruption |
| Corruption only under load | Race condition or thermal |

---

## Quick Reference

| Item | Value |
|------|-------|
| Hardware detection | EDAC subsystem, SError (EC=0x2F) |
| Software detection | KASAN, KFENCE, SLUB debug, page_poison |
| EDAC sysfs | `/sys/devices/system/edac/mc/` |
| DMA debug | `CONFIG_DMA_API_DEBUG=y` |
| Page poison | `CONFIG_PAGE_POISONING=y` or `page_poison=1` |
| HW error handler | `memory_failure()` in `mm/memory-failure.c` |
| IOMMU enforcement | `iommu=strict` boot param |
| Memory test | memtest86+ (from GRUB/bootloader) |
| #1 HW indicator | EDAC CE count increasing |
| #1 SW indicator | Corruption at different locations each crash |
