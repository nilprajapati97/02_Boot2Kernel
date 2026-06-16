# Cache in Hypervisor and Virtualization Context

**Category**: Cache Architecture  
**Platform**: ARM64 (AArch64)

---

## 1. Cache Architecture in Virtualized Environment

```
In an ARM64 virtualized system (KVM, Xen):

Physical cache hierarchy (hardware, unchanged):
  L1 I/D-cache: per-core, shared between hypervisor and all VMs
  L2 unified: per-core, shared between hypervisor and all VMs
  L3/LLC: shared across all cores, ALL VMs and hypervisor

Cache coherency is transparent to virtualization:
  Hardware MESI/MOESI/ACE handles coherency between cores
  VM1 core and VM2 core: see coherent memory (MESI protocol)
  No special cache management for CPU-to-CPU sharing between VMs

What changes with virtualization:
  1. Stage 2 translation: VA→IPA→PA adds a translation stage
     (TLB entries combined, but cache lines still indexed by PA)
  2. Memory attributes: Stage 1 (guest) AND Stage 2 (hypervisor) must agree
  3. Cache maintenance instructions: need additional privilege checks
  4. MAIR_EL1 vs MAIR_EL2: different attribute configurations

Stage 1 + Stage 2 memory attributes interaction:
  A cache line is cached according to the MOST RESTRICTIVE combination:
  
  Stage 1 (guest OS) maps as: Normal WB-WA
  Stage 2 (KVM) maps as: Normal NC
  Combined result: Normal NC (non-cacheable wins!)
  
  ARM64 spec: more restrictive of Stage 1/Stage 2 applies
  Order: Device > NC > WT > WB (Device is most restrictive, WB is least)
  
  Reason: hypervisor may need to control guest memory mapping
  Example: KVM lends memory to guest as NC for coherent DMA regions
```

---

## 2. Cache Maintenance in Guest VM

```
Guest OS performs cache maintenance (DC CIVAC, IC IVAU, etc.):
  These operate on GUEST VIRTUAL addresses
  Hardware translates: GVA → GPA (Stage 1) → PA (Stage 2) before cache maintenance
  
  Trap behavior:
    Most cache maintenance instructions: NOT trapped by default in ARM64
    (No HCR_EL2 trap bit for DC CIVAC, IC IVAU in normal operation)
    Guest can perform cache maintenance freely → no hypervisor intervention needed
    
  EXCEPTION: set/way cache operations (DC CISW):
    HCR_EL2.TPC (Trap Pointer Coalescing) bit is NOT about set/way
    ARMv8.8: FEAT_HAFT may affect trapping
    
    More importantly: DC CISW operates on the PHYSICAL cache level
    Guest should NOT use DC CISW for set/way flushes (hypervisor concern)
    Linux guest: never uses DC CISW anyway (uses DC CIVAC instead)

IC IALLU in guest:
  IC IALLU: Invalidate ALL I-caches on this CPU
  HCR_EL2.TID4: trap ID register access
  IC IALLU: not typically trapped (guest can flush its own I-cache)
  
  KVM handles this correctly because:
    I-cache flush is per-context (per-ASID/VMID)
    Guest IC IALLU flushes THIS CPU's I-cache
    Other CPUs' I-caches not affected (guest would need IC IALLUIS)
    IC IALLUIS: Inner Shareable (broadcast) — may be trapped or limited in some implementations

Cache coloring for VM isolation (hypervisor feature):
  Some SoCs: partition L3 cache ways between VMs using CAT (Cache Allocation Technology)
  ARM doesn't have standard CAT (unlike x86 Intel CAT/MBA)
  Qualcomm/ARM server: proprietary cache partitioning at LLC level
  Used for: real-time VM isolation (VM A's cache thrashing doesn't affect VM B)
```

---

## 3. KVM Cache Management

```
KVM/ARM64 cache operations (arch/arm64/kvm/):

Guest page table setup:
  KVM sets up Stage 2 page tables (IPA→PA mappings)
  Memory attributes in Stage 2 PTEs determine guest cache behavior
  
  kvm_set_spte_hva() → set_memory_region_vma_attrs():
    Sets MAIR-equivalent attributes in Stage 2 PTE
    For normal RAM: S2AP RW, MemAttr=Normal WB
    For device passthrough: S2AP RW, MemAttr=Device nGnRE

VM memory mapping (Stage 2 PTE):
  S2 descriptor memory attributes (different from Stage 1!):
    MemAttr[3:0] in Stage 2 descriptor:
      0b0000 = Device nGnRnE
      0b0001 = Device nGnRE
      0b0010 = Device nGRE
      0b0011 = Device GRE
      0b0100 = Normal, Outer NC, Inner NC
      0b0101 = Normal, Outer NC, Inner WB
      0b0110 = Normal, Outer NC, Inner WT
      0b1001 = Normal, Outer WB, Inner NC
      0b1010 = Normal, Outer WB, Inner WT
      0b1111 = Normal, Outer WB, Inner WB (fully cached)
  
  Note: Stage 2 does NOT use MAIR_EL1 — attributes encoded directly in PTE!

KVM cache flush on VM entry:
  kvm_flush_remote_tlbs(): TLB flush
  No explicit cache flush on VM entry/exit (hardware maintains coherency)
  
  Exception: VGIC (Virtual GIC) memory:
    GIC memory-mapped registers may be NC
    KVM maps VGIC as Device memory to prevent speculative CPU accesses
    
Guest page migration:
  Memory ballooning: KVM reclaims guest memory
  Before reclaim: must DC CIVAC guest page to clean to PoC
  arch/arm64/kvm/mmu.c: kvm_unmap_hva_range() calls flush_tlb_kernel_range()
  TLB flush handles TLB; cache flush handled by Stage 2 attribute removal
```

---

## 4. Memory Encryption and Cache

```
ARM Confidential Compute Architecture (CCA) / Realm VMs:
  New in ARMv9: Granule Protection Tables (GPT)
  Each 4KB (or larger) granule tagged as: Root, Realm, Secure, or NS
  CPU enforces: EL1 cannot access Realm memory; Realm VM cannot access NS memory
  
  Cache implications:
    L1/L2 cache: shared physically between CPU cores
    GPT enforcement: hardware blocks cross-security-domain cache access
    Implementation: cache lines tagged with PAS (Physical Address Space)
    
  Memory encryption:
    Some SoCs: transparent memory encryption (TME, Total Memory Encryption)
    Each VM gets unique encryption key
    Cache: stores UNENCRYPTED data (decrypted on load, re-encrypted on writeback)
    DMA: device sees ENCRYPTED data at DRAM → device needs to decrypt
    
    Impact: no change to cache maintenance procedures
    The encryption/decryption is transparent between cache and DRAM

TrustZone and cache:
  Secure world (S=1) and Normal world (S=0) share the SAME physical cache
  NS bit[63] in the physical address distinguishes worlds
  
  Cache lines are tagged internally with NS bit
  Secure world writes: cache line tagged as Secure
  Normal world cannot hit a Secure-tagged cache line (hardware enforced)
  
  Cache flush when switching worlds:
    ATF (Arm Trusted Firmware) flushes caches on exception level transitions
    SMC from NS → Secure: no automatic cache flush
    ATF must ensure Secure world data not in Normal world's L1/L2 cache
    Practice: ATF invalidates L1/L2 of the current CPU on entry to Secure world
```

---

## 5. Interview Questions & Answers

**Q1: In a KVM virtual machine, the guest OS does DC CIVAC on a memory address. Does this flush reach the physical DRAM (PoC), or only the guest's "virtual cache"?**

In ARM64 virtualization with KVM, there is only ONE physical cache hierarchy — caches are NOT virtualized or abstracted per VM. When the guest OS executes `DC CIVAC <GVA>`, the hardware performs the following:

1. **Stage 1 translation**: translates GVA (guest virtual address) → GPA (guest physical address = IPA) using the guest's TTBR0/TTBR1
2. **Stage 2 translation**: translates GPA → HPA (host physical address = real PA) using KVM's Stage 2 page tables in VTTBR_EL2
3. **Cache maintenance**: performs the actual cache clean+invalidate on the **physical cache line** corresponding to HPA

So yes — the `DC CIVAC` reaches the **physical hardware PoC (DRAM)**. The cache maintenance reaches the real physical cache lines, not some virtualized abstraction. This is why guest DMA operations work correctly: the guest can perform its own DMA cache flushing with `DC CIVAC`, and the hardware correctly translates through Stage 1+2 to find the right physical cache lines.

The only complication is that the combined Stage 1+Stage 2 memory attributes apply: if KVM's Stage 2 maps the page as Device or NC, the cache line may not be cached at all — making the `DC CIVAC` a no-op (nothing to flush for an uncached line). KVM must configure Stage 2 attributes consistently with the guest's Stage 1 attributes to avoid surprises.

---

## 6. Quick Reference

| Scenario | Cache Action Needed | Who Does It |
|---|---|---|
| Guest VM DMA transfer | DC CIVAC (guest does it) | Guest OS via dma_map_single |
| VM migration | Flush all guest pages to DRAM | KVM: DC CIVAC all guest pages |
| Stage 1=WB, Stage 2=NC | Combined: NC | Hardware resolves at PTW |
| Guest JIT compilation | DC CVAU + IC IVAU (in guest) | Guest OS (executed in guest EL1) |
| Hypervisor patches guest code | DC CIVAC + IC IVAU from EL2 | KVM/hypervisor |

| KVM/Stage 2 MemAttr[3:0] | Policy |
|---|---|
| 0b0000 | Device nGnRnE |
| 0b0001 | Device nGnRE |
| 0b0100 | NC Normal |
| 0b1111 | WB Normal |
