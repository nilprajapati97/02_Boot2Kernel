# NVIDIA — System Design Interview: Linux Kernel Memory Management

## Target Role: Senior Linux Kernel / GPU Driver Engineer (9+ years)

These questions focus on **NVIDIA GPU driver** memory management challenges — unified memory, GPU page faults, BAR mapping, IOMMU interactions, pinned memory, GEM/TTM buffer management, and multi-GPU coherency.

---

## Q1: NVIDIA Unified Memory Architecture — How Does GPU Page Faulting Work?

### Interview Question
**"Explain NVIDIA's Unified Virtual Memory (UVM) architecture. How do GPU page faults work at the kernel driver level? How does the kernel migrate pages between CPU and GPU memory? What role does the Linux kernel's HMM (Heterogeneous Memory Management) play?"**

### Deep Answer

NVIDIA Unified Memory allows CPU and GPU to share a single virtual address pointer (`cudaMallocManaged`). Under the hood, pages migrate between CPU RAM and GPU VRAM on demand.

```mermaid
flowchart TB
    subgraph Application["🟦 CUDA Application"]
        A1["cudaMallocManaged(&ptr, size)"]
        A2["CPU accesses *ptr"]
        A3["kernel<<<...>>>(*ptr)"]
    end

    subgraph UVM_Driver["🟧 NVIDIA UVM Kernel Driver"]
        U1["UVM VA Space Manager"]
        U2["Fault Handler (CPU side)"]
        U3["Fault Handler (GPU side)"]
        U4["Migration Engine"]
        U5["Page Table Manager\n(CPU PT + GPU PT)"]
    end

    subgraph Linux_MM["🟩 Linux Kernel MM"]
        L1["mm_struct / VMA"]
        L2["HMM (hmm_range_fault)"]
        L3["migrate_pages()"]
        L4["MMU Notifiers"]
    end

    subgraph Hardware["🟥 Hardware"]
        H1["CPU MMU + TLB"]
        H2["GPU MMU + TLB"]
        H3["GPU Copy Engine (CE)"]
        H4["PCIe / NVLink"]
    end

    A1 --> U1
    A2 -->|CPU page fault| U2
    A3 -->|GPU page fault| U3
    U2 --> U4
    U3 --> U4
    U4 -->|CPU→GPU| H3
    U4 -->|GPU→CPU| H3
    H3 <-->|DMA Transfer| H4
    U5 --> H1
    U5 --> H2
    U1 --> L1
    U2 --> L2
    L4 -->|Invalidation| U5

    style Application fill:#e3f2fd,stroke:#1565c0,color:#000
    style UVM_Driver fill:#fff3e0,stroke:#e65100,color:#000
    style Linux_MM fill:#e8f5e9,stroke:#2e7d32,color:#000
    style Hardware fill:#ffebee,stroke:#c62828,color:#000
```

### GPU Page Fault Sequence

```mermaid
sequenceDiagram
    participant App as 🟦 CUDA Kernel
    participant GPU as 🟥 GPU MMU
    participant GMMU as 🟧 GPU Fault Buffer
    participant UVM as 🟧 UVM Driver
    participant KMM as 🟩 Linux MM
    participant CE as 🟥 Copy Engine
    participant CPU_MMU as 🟥 CPU MMU

    App->>GPU: GPU thread accesses virtual addr 0xABCD000
    GPU->>GPU: Walk GPU page tables → PTE invalid
    GPU->>GMMU: Enqueue fault (addr, access_type, instance)
    GPU-->>App: Stall GPU warp/SM

    Note over GMMU: Fault buffer batches faults<br/>Interrupts host CPU

    GMMU->>UVM: Interrupt → uvm_gpu_fault_handler()
    UVM->>UVM: Look up VA range → page on CPU RAM

    alt Page on CPU, needs migration to GPU
        UVM->>KMM: Unmap from CPU page tables
        KMM->>CPU_MMU: TLB shootdown (flush_tlb_range)
        UVM->>CE: DMA copy: CPU RAM → GPU VRAM
        CE->>CE: PCIe/NVLink transfer
        CE->>UVM: Transfer complete
        UVM->>GPU: Update GPU page table → valid PTE
        UVM->>GPU: Replay faulted access
        GPU-->>App: Warp resumes execution
    end

    alt Page already on GPU (permission fault)
        UVM->>GPU: Update PTE permissions (R→RW)
        UVM->>GPU: Replay faulted access
        GPU-->>App: Warp resumes
    end
```

### Key Kernel Interactions

```c
/* NVIDIA UVM driver registers MMU notifier with Linux kernel */
struct mmu_notifier_ops uvm_mmu_notifier_ops = {
    .invalidate_range_start = uvm_mmu_notifier_invalidate_range_start,
    .invalidate_range_end   = uvm_mmu_notifier_invalidate_range_end,
};

/* When CPU page tables change (munmap, COW, migration),
   Linux notifies UVM → UVM invalidates GPU page tables */

/* HMM mirror for tracking CPU page table state */
struct hmm_mirror {
    struct mmu_notifier notifier;
    /* UVM uses this to get CPU→physical mappings */
};
```

---

## Q2: GPU BAR Memory Mapping and PCIe Address Space

### Interview Question
**"Explain how the GPU's BAR (Base Address Register) regions are mapped into the kernel. How does a driver map GPU VRAM into user-space? What are BAR1, BAR2 in NVIDIA GPUs and how does remap_pfn_range work for GPU memory?"**

### Deep Answer

```mermaid
flowchart LR
    subgraph PCIe_Config["🟦 PCIe Configuration Space"]
        BAR0["BAR0: Registers\n(MMIO control regs)\n256MB"]
        BAR1["BAR1: VRAM Window\n(CPU-accessible VRAM)\n256MB-32GB"]
        BAR2["BAR2/3: NVLink / Other\n(varies by GPU)"]
    end

    subgraph Kernel_VA["🟩 Kernel Virtual Address Space"]
        K_BAR0["ioremap(BAR0)\n→ Read/Write GPU regs"]
        K_BAR1["ioremap_wc(BAR1)\n→ Access VRAM"]
    end

    subgraph User_VA["🟧 User Virtual Address Space"]
        U_VRAM["mmap() → GPU framebuffer\nvia remap_pfn_range()"]
        U_DOORBELL["mmap() → GPU doorbells\nvia io_remap_pfn_range()"]
    end

    BAR0 --> K_BAR0
    BAR1 --> K_BAR1
    K_BAR1 --> U_VRAM

    style PCIe_Config fill:#e3f2fd,stroke:#1565c0,color:#000
    style Kernel_VA fill:#e8f5e9,stroke:#2e7d32,color:#000
    style User_VA fill:#fff3e0,stroke:#e65100,color:#000
```

### BAR Mapping Sequence

```mermaid
sequenceDiagram
    participant PCI as 🟦 PCIe Subsystem
    participant DRV as 🟩 GPU Kernel Driver
    participant MM as 🟩 Linux MM
    participant USER as 🟧 User-space [CUDA]

    Note over PCI: BIOS/Firmware assigns<br/>BAR physical addresses

    PCI->>DRV: probe() → pci_resource_start(pdev, 0)
    DRV->>DRV: bar0_phys = pci_resource_start(pdev, 0)
    DRV->>DRV: bar0_len = pci_resource_len(pdev, 0)
    DRV->>MM: bar0_va = ioremap(bar0_phys, bar0_len)
    DRV->>DRV: GPU register access via readl/writel(bar0_va + offset)

    DRV->>MM: bar1_va = ioremap_wc(bar1_phys, bar1_len)
    Note over DRV: BAR1 = CPU window into VRAM<br/>Write-combining for throughput

    USER->>DRV: open(/dev/nvidia0)
    USER->>DRV: mmap(fd, offset=VRAM_region, size)
    DRV->>DRV: .mmap handler → validate offset, permissions
    DRV->>MM: remap_pfn_range(vma, vma->vm_start,<br/>bar1_phys >> PAGE_SHIFT, size, pgprot_writecombine)
    MM->>USER: User-space VA → GPU VRAM (via BAR1)

    USER->>USER: Direct load/store to GPU VRAM!
    Note over USER: Slow for random access (PCIe latency)<br/>Fast for streaming writes (WC coalescing)
```

### Resizable BAR (ReBAR)

```mermaid
flowchart TB
    subgraph Before["🟥 Without ReBAR"]
        B1["BAR1 = 256MB window"]
        B2["GPU VRAM = 24GB"]
        B3["CPU can only see 256MB\nat a time through BAR1"]
        B4["Driver must remap BAR1\nto access different VRAM regions"]
        B1 --> B3
        B2 --> B3
        B3 --> B4
    end

    subgraph After["🟩 With ReBAR"]
        A1["BAR1 = 24GB window"]
        A2["GPU VRAM = 24GB"]
        A3["CPU can access ALL\nVRAM directly"]
        A4["No remapping needed\nFull direct access"]
        A1 --> A3
        A2 --> A3
        A3 --> A4
    end

    style Before fill:#ffebee,stroke:#c62828,color:#000
    style After fill:#e8f5e9,stroke:#2e7d32,color:#000
```

---

## Q3: DRM/GEM and TTM Buffer Object Management

### Interview Question
**"Explain how NVIDIA's kernel driver manages GPU buffer objects. How does TTM (Translation Table Manager) or GEM work? How are GPU buffers allocated, pinned, mapped, and migrated? Explain the relationship between GEM handles, GEM objects, and the underlying physical memory."**

### Deep Answer

```mermaid
flowchart TB
    subgraph UserSpace["🟦 User-Space (mesa/CUDA)"]
        U1["Create Buffer Object\n(DRM_IOCTL_*_GEM_CREATE)"]
        U2["GEM Handle (integer)"]
        U3["mmap buffer\n(DRM_IOCTL_*_MAP)"]
    end

    subgraph GEM_Layer["🟩 DRM GEM Layer"]
        G1["drm_gem_object"]
        G2["Reference counting\n(kref)"]
        G3["Handle→Object\n(IDR lookup)"]
    end

    subgraph TTM_Layer["🟧 TTM (Translation Table Manager)"]
        T1["ttm_buffer_object"]
        T2["Placement Rules\n(VRAM, GTT/GART, SYSTEM)"]
        T3["ttm_resource\n(actual location)"]
        T4["ttm_bo_validate()\n(migrate between domains)"]
    end

    subgraph Memory_Domains["🟥 Physical Memory"]
        M1["GPU VRAM\n(fast, on-card)"]
        M2["GTT/GART\n(system RAM, GPU-accessible\nvia IOMMU/GART mapping)"]
        M3["System RAM\n(CPU only, swap-capable)"]
    end

    U1 --> G1
    G1 --> G2
    U2 --> G3
    G3 --> G1
    G1 --> T1
    T1 --> T2
    T2 --> T3
    T4 --> M1
    T4 --> M2
    T4 --> M3
    U3 --> G1

    style UserSpace fill:#e3f2fd,stroke:#1565c0,color:#000
    style GEM_Layer fill:#e8f5e9,stroke:#2e7d32,color:#000
    style TTM_Layer fill:#fff3e0,stroke:#e65100,color:#000
    style Memory_Domains fill:#ffebee,stroke:#c62828,color:#000
```

### Buffer Object Lifecycle

```mermaid
sequenceDiagram
    participant App as 🟦 User-Space
    participant DRM as 🟩 DRM Core
    participant TTM as 🟧 TTM
    participant VRAM as 🟥 GPU VRAM
    participant GTT as 🟥 System RAM [GTT]

    App->>DRM: ioctl(GEM_CREATE, size=64MB)
    DRM->>TTM: ttm_bo_init(placement=VRAM|GTT)
    TTM->>TTM: Try VRAM first

    alt VRAM has space
        TTM->>VRAM: Allocate from VRAM manager
        TTM->>DRM: BO created in VRAM
    else VRAM full — eviction needed
        TTM->>TTM: Find eviction candidate (LRU)
        TTM->>GTT: Migrate victim BO: VRAM→GTT
        Note over TTM: DMA copy via Copy Engine
        TTM->>VRAM: Allocate freed VRAM
        TTM->>DRM: BO created in VRAM
    end

    DRM->>App: Return GEM handle=7

    Note over App: Submit GPU command referencing handle=7

    App->>DRM: ioctl(SUBMIT, handle=7)
    DRM->>TTM: ttm_bo_validate(bo, VRAM_ONLY)
    TTM->>TTM: BO already in VRAM? Yes → pin it
    TTM->>DRM: BO pinned at VRAM offset
    DRM->>DRM: Patch GPU command buffer with VRAM address
    DRM->>App: Submitted

    Note over App: GPU command completes
    DRM->>TTM: Unpin BO → back on LRU
```

---

## Q4: Multi-GPU Memory and NVLink Topology

### Interview Question
**"In a multi-GPU system (DGX with 8 GPUs connected via NVLink), how does memory management work? How does peer-to-peer (P2P) GPU memory access work at the kernel level? How does NUMA awareness affect GPU memory allocation?"**

### Deep Answer

```mermaid
flowchart TB
    subgraph DGX_System["🟦 DGX-A100 / H100 System"]
        subgraph Node0["NUMA Node 0"]
            CPU0["CPU 0"]
            RAM0["System RAM\n256GB"]
            GPU0["GPU 0\n80GB HBM"]
            GPU1["GPU 1\n80GB HBM"]
            GPU2["GPU 2\n80GB HBM"]
            GPU3["GPU 3\n80GB HBM"]
        end

        subgraph Node1["NUMA Node 1"]
            CPU1["CPU 1"]
            RAM1["System RAM\n256GB"]
            GPU4["GPU 4\n80GB HBM"]
            GPU5["GPU 5\n80GB HBM"]
            GPU6["GPU 6\n80GB HBM"]
            GPU7["GPU 7\n80GB HBM"]
        end

        GPU0 <-->|"NVLink 600GB/s"| GPU1
        GPU0 <-->|"NVLink"| GPU2
        GPU0 <-->|"NVLink"| GPU3
        GPU1 <-->|"NVLink"| GPU2
        GPU1 <-->|"NVLink"| GPU3
        GPU2 <-->|"NVLink"| GPU3

        GPU4 <-->|"NVLink"| GPU5
        GPU4 <-->|"NVLink"| GPU6
        GPU4 <-->|"NVLink"| GPU7
        GPU5 <-->|"NVLink"| GPU6
        GPU5 <-->|"NVLink"| GPU7
        GPU6 <-->|"NVLink"| GPU7

        GPU0 <-->|"NVLink cross-node"| GPU4
        GPU1 <-->|"NVLink cross-node"| GPU5

        CPU0 <-->|"PCIe Gen5"| GPU0
        CPU0 <-->|"PCIe Gen5"| GPU1
        CPU1 <-->|"PCIe Gen5"| GPU4
        CPU1 <-->|"PCIe Gen5"| GPU5
    end

    style Node0 fill:#e3f2fd,stroke:#1565c0,color:#000
    style Node1 fill:#fff3e0,stroke:#e65100,color:#000
```

### P2P Memory Access Sequence

```mermaid
sequenceDiagram
    participant APP as 🟦 Application
    participant DRV as 🟩 NVIDIA Driver
    participant GPU0 as 🟥 GPU 0
    participant GPU1 as 🟥 GPU 1
    participant NVLINK as 🟧 NVLink

    APP->>DRV: cudaDeviceEnablePeerAccess(GPU0→GPU1)
    DRV->>DRV: Check NVLink topology: GPU0↔GPU1 connected?

    alt NVLink available
        DRV->>GPU0: Map GPU1's VRAM into GPU0's page table
        DRV->>GPU1: Map GPU0's VRAM into GPU1's page table
        Note over DRV: Each GPU gets PTEs pointing<br/>to peer's VRAM via NVLink
        DRV->>APP: P2P enabled (direct access)

        APP->>GPU0: Kernel reads ptr_on_gpu1
        GPU0->>GPU0: GPU MMU → PTE → peer VRAM address
        GPU0->>NVLINK: NVLink read request
        NVLINK->>GPU1: Forward to GPU1 memory controller
        GPU1->>NVLINK: Return data
        NVLINK->>GPU0: Data received (~300ns latency)
    else Only PCIe (no NVLink)
        DRV->>DRV: Use PCIe P2P or stage through CPU RAM
        Note over DRV: PCIe P2P: GPU0 → PCIe → GPU1<br/>(if ACS disabled and same root complex)<br/>Fallback: GPU0 → PCIe → CPU RAM → PCIe → GPU1
    end
```

---

## Q5: GPU Memory Pinning and RDMA (GPUDirect)

### Interview Question
**"Explain GPUDirect RDMA at the kernel level. How does a network adapter (InfiniBand HCA) directly DMA into GPU VRAM? What are the memory pinning requirements? How does nvidia_p2p_get_pages work?"**

### Deep Answer

```mermaid
flowchart TB
    subgraph Traditional["🟥 Traditional Path (without GPUDirect)"]
        T1["GPU VRAM"] -->|"1. DMA to CPU"| T2["CPU RAM\n(staging buffer)"]
        T2 -->|"2. DMA to NIC"| T3["Network (RDMA)"]
        T4["2 copies, 2x latency"]
    end

    subgraph GPUDirect["🟩 GPUDirect RDMA Path"]
        G1["GPU VRAM"] -->|"1. Direct DMA"| G2["Network (RDMA)"]
        G3["Zero copies through CPU\nLowest latency"]
    end

    style Traditional fill:#ffebee,stroke:#c62828,color:#000
    style GPUDirect fill:#e8f5e9,stroke:#2e7d32,color:#000
```

### GPUDirect RDMA Kernel Flow

```mermaid
sequenceDiagram
    participant APP as 🟦 User Application
    participant NIC_DRV as 🟩 Network Driver [mlx5]
    participant NV_DRV as 🟧 NVIDIA Driver
    participant GPU as 🟥 GPU VRAM
    participant NIC as 🟥 RDMA NIC [HCA]
    participant IOMMU as 🟩 IOMMU

    APP->>NIC_DRV: ibv_reg_mr(gpu_va, size, GPU)
    NIC_DRV->>NV_DRV: nvidia_p2p_get_pages(gpu_va, size, &pages)

    NV_DRV->>NV_DRV: Look up GPU VA → VRAM physical addresses
    NV_DRV->>NV_DRV: Pin pages in VRAM (prevent migration/eviction)
    NV_DRV->>NIC_DRV: Return page table (physical VRAM addresses)

    Note over NV_DRV: Register free_callback for<br/>notification if pages must move

    NIC_DRV->>IOMMU: If IOMMU enabled:<br/>Map GPU BAR phys addrs → IOVA
    NIC_DRV->>NIC: Program HCA with DMA addresses<br/>(VRAM physical or IOVA)
    NIC_DRV->>APP: MR registered, rkey returned

    Note over APP: RDMA Send/Recv/Read/Write<br/>NIC DMAs directly to/from GPU VRAM

    APP->>NIC: RDMA READ from remote
    NIC->>GPU: DMA Write → GPU VRAM (via PCIe BAR1)
    Note over NIC,GPU: Data goes directly:<br/>Network → PCIe → GPU VRAM<br/>CPU never involved!

    Note over APP: Cleanup
    APP->>NIC_DRV: ibv_dereg_mr()
    NIC_DRV->>NV_DRV: nvidia_p2p_put_pages()
    NV_DRV->>NV_DRV: Unpin VRAM pages
```

---

## Q6: NVIDIA GPU IOMMU Integration (SMMU on ARM, VT-d on x86)

### Interview Question
**"How does the GPU interact with the system IOMMU? What challenges arise with GPU DMA through an IOMMU? How do NVIDIA drivers handle IOMMU-backed DMA for compute and display workloads?"**

### Deep Answer

```mermaid
flowchart TB
    subgraph WithoutIOMMU["🟥 Without IOMMU"]
        W1["GPU programs DMA\nwith physical addresses"]
        W2["GPU has full access\nto all system RAM"]
        W3["Security risk:\nrogue GPU can read\nany memory"]
        W1 --> W2 --> W3
    end

    subgraph WithIOMMU["🟩 With IOMMU (VT-d / ARM SMMU)"]
        I1["GPU programs DMA\nwith IOVA (virtual)"]
        I2["IOMMU translates\nIOVA → Physical"]
        I3["Restricted access:\nGPU can only reach\nmapped pages"]
        I4["Supports >4GB DMA\nfor 32-bit devices"]
        I1 --> I2 --> I3
        I2 --> I4
    end

    style WithoutIOMMU fill:#ffebee,stroke:#c62828,color:#000
    style WithIOMMU fill:#e8f5e9,stroke:#2e7d32,color:#000
```

### IOMMU DMA Mapping Sequence

```mermaid
sequenceDiagram
    participant DRV as 🟩 NVIDIA GPU Driver
    participant DMA as 🟩 DMA API
    participant IOMMU as 🟧 IOMMU Driver
    participant HW_IOMMU as 🟥 IOMMU Hardware
    participant GPU as 🟥 GPU Engine

    DRV->>DRV: Allocate pages for GPU command buffer
    DRV->>DMA: dma_map_single(dev, cpu_va, size, DMA_TO_DEVICE)
    DMA->>IOMMU: iommu_map(domain, iova, phys, size, prot)
    IOMMU->>HW_IOMMU: Program IOMMU page table entry
    IOMMU->>IOMMU: IOTLB invalidate (flush stale entries)
    DMA->>DRV: Return IOVA (DMA address for GPU)

    DRV->>GPU: Write IOVA into GPU command buffer<br/>"Read data from DMA addr = IOVA"
    GPU->>HW_IOMMU: PCIe memory read → IOVA
    HW_IOMMU->>HW_IOMMU: Translate IOVA → Physical
    HW_IOMMU->>GPU: Return data from physical address

    Note over DRV: When done:
    DRV->>DMA: dma_unmap_single(dev, dma_addr, size, dir)
    DMA->>IOMMU: iommu_unmap(domain, iova, size)
    IOMMU->>HW_IOMMU: Remove mapping, flush IOTLB
```

---

## Q7: Kernel Memory Allocation Patterns in GPU Drivers

### Interview Question
**"Walk through the memory allocation patterns used in a GPU kernel driver. When do you use kmalloc vs vmalloc vs CMA vs DMA allocators? How do you handle allocation failures in the GPU probe path vs runtime?"**

### Deep Answer

```mermaid
flowchart TB
    subgraph Decision["🟦 GPU Driver Allocation Decision Tree"]
        START["Need to allocate memory"] --> Q1{"Size?"}

        Q1 -->|"< 1 page\n(small structs)"| KMALLOC["kmalloc()\nGFP_KERNEL"]

        Q1 -->|"1-few pages\nDMA needed"| DMA_Q{"GPU needs\nDMA access?"}

        Q1 -->|"Large buffer\nno DMA"| VMALLOC["vmalloc()\n(page tables, large arrays)"]

        DMA_Q -->|"Coherent\n(always in sync)"| DMA_COH["dma_alloc_coherent()\n(command buffers, ring buffers)"]

        DMA_Q -->|"Streaming\n(directional)"| DMA_MAP["dma_map_sg()\n(data buffers, textures)"]

        Q1 -->|"Very large\ncontiguous"| CMA_Q{"Physically\ncontiguous?"}

        CMA_Q -->|"Yes"| CMA["CMA or\ndma_alloc_coherent\nwith CMA backend"]

        CMA_Q -->|"No"| SG["Scatter-Gather\n(sg_alloc_table_from_pages)"]

        Q1 -->|"User-pinned\nmemory"| PIN["pin_user_pages_fast()\n(CUDA registered memory)"]
    end

    KMALLOC --> NOTE1["channel structs, work items\nioctl argument copies"]
    DMA_COH --> NOTE2["GPU command ring\ndoorbell pages"]
    DMA_MAP --> NOTE3["frame buffers\ntexture uploads"]
    VMALLOC --> NOTE4["GPU page tables\nlarge lookup arrays"]
    PIN --> NOTE5["Zero-copy from\nuser buffers"]

    style Decision fill:#e3f2fd,stroke:#1565c0,color:#000
    style KMALLOC fill:#e8f5e9,stroke:#2e7d32,color:#000
    style DMA_COH fill:#fff3e0,stroke:#e65100,color:#000
    style DMA_MAP fill:#fff3e0,stroke:#e65100,color:#000
    style VMALLOC fill:#e8f5e9,stroke:#2e7d32,color:#000
    style CMA fill:#f3e5f5,stroke:#6a1b9a,color:#000
    style SG fill:#f3e5f5,stroke:#6a1b9a,color:#000
    style PIN fill:#ffebee,stroke:#c62828,color:#000
```

### GPU Driver Probe Memory Allocation Sequence

```mermaid
sequenceDiagram
    participant PCI as 🟦 PCI Subsystem
    participant DRV as 🟩 GPU Driver
    participant MM as 🟩 Kernel MM
    participant DMA as 🟧 DMA Subsystem
    participant GPU as 🟥 GPU Hardware

    PCI->>DRV: probe(pdev, id)

    Note over DRV: Phase 1: Small structures
    DRV->>MM: priv = kzalloc(sizeof(*priv), GFP_KERNEL)
    DRV->>MM: channels = kcalloc(32, sizeof(*ch), GFP_KERNEL)

    Note over DRV: Phase 2: BAR mapping
    DRV->>MM: regs = ioremap(bar0_phys, bar0_len)
    DRV->>MM: vram = ioremap_wc(bar1_phys, bar1_len)

    Note over DRV: Phase 3: DMA-able buffers
    DRV->>DMA: cmd_ring = dma_alloc_coherent(dev, 4096,<br/>&cmd_ring_dma, GFP_KERNEL)
    DRV->>DMA: Set DMA mask: dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64))

    Note over DRV: Phase 4: Large internal tables
    DRV->>MM: gpu_pt = vmalloc(16 * 1024 * 1024)<br/>(GPU page table shadow — 16MB)

    Note over DRV: Phase 5: Initialize hardware
    DRV->>GPU: Write cmd_ring_dma to GPU register
    DRV->>GPU: Enable interrupts, start engines

    DRV->>PCI: return 0 (success)

    Note over DRV: On any alloc failure:<br/>goto err_cleanup (reverse order free)
```

---

## Q8: GPU Memory Overcommit and Eviction

### Interview Question
**"When GPU VRAM is full, how does the NVIDIA driver handle new allocations? Explain the GPU memory eviction policy — LRU, priority levels, and the interaction between compute and display buffers."**

### Deep Answer

```mermaid
flowchart TB
    subgraph VRAM_State["🟥 GPU VRAM State"]
        V1["Total: 80GB HBM3"]
        V2["Display (pinned): 32MB\n(scanout buffer, cursor)"]
        V3["Compute (active): 76GB\n(AI model weights, activations)"]
        V4["Free: ~4GB"]
    end

    subgraph Eviction["🟧 Eviction Process"]
        E1["New 8GB allocation request"]
        E2["Free (4GB) < Requested (8GB)"]
        E3{"Find eviction\ncandidates"}
        E4["Scan LRU list\n(least recently used BOs)"]
        E5{"BO pinned?"}
        E6["Skip\n(display, active GPU work)"]
        E7{"BO busy\n(GPU using it)?"}
        E8["Wait for GPU fence\nor skip"]
        E9["Migrate BO:\nVRAM → System RAM (GTT)"]
        E10["Free VRAM\nspace reclaimed"]
        E11["Allocate 8GB\nfrom freed space"]

        E1 --> E2 --> E3 --> E4 --> E5
        E5 -->|"Yes"| E6
        E5 -->|"No"| E7
        E7 -->|"Yes"| E8
        E7 -->|"No"| E9
        E8 --> E9
        E9 --> E10 --> E11
    end

    style VRAM_State fill:#ffebee,stroke:#c62828,color:#000
    style Eviction fill:#fff3e0,stroke:#e65100,color:#000
```

### Eviction Sequence Detail

```mermaid
sequenceDiagram
    participant CMD as 🟦 Command Submission
    participant TTM as 🟧 TTM BO Manager
    participant LRU as 🟧 LRU Lists
    participant CE as 🟥 Copy Engine
    participant VRAM as 🟥 VRAM
    participant GTT as 🟩 System RAM

    CMD->>TTM: ttm_bo_validate(new_bo, VRAM_ONLY)
    TTM->>VRAM: Try allocate from VRAM manager
    VRAM-->>TTM: ENOSPC (not enough free VRAM)

    TTM->>LRU: Scan VRAM LRU tail (oldest first)
    loop Find enough pages to evict
        LRU->>TTM: Candidate BO (64MB, last used 500ms ago)
        TTM->>TTM: Check: pinned? → No
        TTM->>TTM: Check: GPU fence signaled? → Yes
        TTM->>TTM: Lock BO for eviction

        TTM->>CE: Schedule DMA: VRAM → GTT (system RAM)
        CE->>VRAM: Read 64MB from VRAM
        CE->>GTT: Write 64MB to system RAM
        CE-->>TTM: Copy complete

        TTM->>TTM: Update BO resource: location = GTT
        TTM->>TTM: Update GPU page tables (if BO was mapped)
        TTM->>TTM: Free VRAM space → add to free pool
    end

    TTM->>VRAM: Retry allocation → success
    TTM->>CMD: new_bo placed in VRAM

    Note over CMD: If evicted BO is needed later,<br/>it will be migrated back: GTT → VRAM<br/>This may trigger another eviction cycle
```

---

## Q9: GPU Page Table and Virtual Address Space Management

### Interview Question
**"NVIDIA GPUs have their own MMU and page tables separate from the CPU. How are GPU page tables managed in the kernel driver? How does GPU virtual address space allocation work? How are GPU page table updates synchronized with command execution?"**

### Deep Answer

```mermaid
flowchart TB
    subgraph GPU_VA["🟦 GPU Virtual Address Space (per-context)"]
        VA1["0x00000000_00000000\n(NULL guard page)"]
        VA2["User managed region\n(shader programs, buffers)"]
        VA3["UVM managed region\n(unified memory)"]
        VA4["Kernel reserved region\n(driver internal)"]
    end

    subgraph GPU_PT["🟩 GPU Page Table (multi-level)"]
        PDE3["PDE3 (Page Dir Entry L3)"]
        PDE2["PDE2 (Page Dir Entry L2)"]
        PDE1["PDE1 (Page Dir Entry L1)"]
        PDE0["PDE0 (Page Dir Entry L0)"]
        PTE["PTE (Page Table Entry)\n→ Physical VRAM or System RAM address"]

        PDE3 --> PDE2 --> PDE1 --> PDE0 --> PTE
    end

    subgraph GPU_PTE_Fields["🟧 GPU PTE Format"]
        F1["Valid bit"]
        F2["Physical address (36-52 bits)"]
        F3["Aperture: VRAM / SYS_MEM / PEER"]
        F4["Read/Write/Atomic permissions"]
        F5["Volatile / Caching mode"]
        F6["Kind (compression format)"]
    end

    VA2 --> PDE3
    PTE --> F1
    PTE --> F2
    PTE --> F3

    style GPU_VA fill:#e3f2fd,stroke:#1565c0,color:#000
    style GPU_PT fill:#e8f5e9,stroke:#2e7d32,color:#000
    style GPU_PTE_Fields fill:#fff3e0,stroke:#e65100,color:#000
```

### GPU Page Table Update Sequence

```mermaid
sequenceDiagram
    participant DRV as 🟩 GPU Kernel Driver
    participant PT_MEM as 🟩 Page Table Memory<br/>[in VRAM or System RAM]
    participant CE as 🟥 Copy Engine
    participant GPU_MMU as 🟥 GPU MMU
    participant SM as 🟥 GPU Shader Cores

    Note over DRV: Map new buffer at GPU VA 0x1000_0000

    DRV->>DRV: Walk GPU page table structure<br/>Allocate missing PDE levels
    DRV->>PT_MEM: Write PTE: VA=0x1000_0000 →<br/>Phys=0x2_0000_0000 (VRAM),<br/>RW, cached, kind=pitch

    Note over DRV: Must ensure GPU sees new PTEs<br/>before accessing the VA

    alt Method 1: TLB Invalidate (fast, small updates)
        DRV->>GPU_MMU: Invalidate TLB entries for VA range<br/>(write to MMU invalidate register)
        GPU_MMU->>GPU_MMU: Flush TLB → next access walks page table
    end

    alt Method 2: Fence-based ordering (large updates)
        DRV->>CE: Submit PT update commands to Copy Engine
        CE->>PT_MEM: CE writes PTEs (DMA)
        DRV->>CE: Insert fence after PT updates
        DRV->>SM: Submit shader work with fence dependency
        Note over SM: Shader waits for fence<br/>→ guaranteed to see new PTEs
    end

    SM->>GPU_MMU: Access VA 0x1000_0000
    GPU_MMU->>PT_MEM: Walk page table → find PTE
    GPU_MMU->>SM: Translate to physical → access VRAM
```

---

## Q10: CUDA cudaHostAlloc (Pinned Memory) Kernel Internals

### Interview Question
**"What happens in the kernel when a CUDA application calls `cudaHostAlloc()` with `cudaHostAllocMapped`? How is pinned memory implemented? What are the performance implications and dangers of excessive pinning?"**

### Deep Answer

```mermaid
flowchart TB
    subgraph Pageable["🟥 Regular (Pageable) Memory"]
        P1["malloc() buffer"]
        P2["Pages can be swapped out"]
        P3["GPU DMA requires:\n1. Copy to staging buffer\n2. DMA from staging"]
        P4["Double-buffering overhead"]
        P1 --> P2 --> P3 --> P4
    end

    subgraph Pinned["🟩 Pinned Memory"]
        Q1["cudaHostAlloc() buffer"]
        Q2["Pages locked in RAM\n(cannot be swapped)"]
        Q3["GPU DMA directly\nfrom/to these pages"]
        Q4["Zero-copy, highest throughput"]
        Q1 --> Q2 --> Q3 --> Q4
    end

    style Pageable fill:#ffebee,stroke:#c62828,color:#000
    style Pinned fill:#e8f5e9,stroke:#2e7d32,color:#000
```

### cudaHostAlloc Kernel Sequence

```mermaid
sequenceDiagram
    participant APP as 🟦 CUDA Application
    participant CUDA_RT as 🟧 CUDA Runtime
    participant NV_UMD as 🟧 NVIDIA User-Mode Driver
    participant KERNEL as 🟩 Linux Kernel
    participant NV_KMD as 🟩 NVIDIA Kernel Driver
    participant GPU as 🟥 GPU

    APP->>CUDA_RT: cudaHostAlloc(&ptr, 256MB,<br/>cudaHostAllocMapped)

    CUDA_RT->>NV_UMD: Allocate pinned host memory

    NV_UMD->>KERNEL: mmap(NULL, 256MB, PROT_RW,<br/>MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    KERNEL->>NV_UMD: User VA = 0x7f0000000000

    NV_UMD->>NV_KMD: ioctl(nvidia_fd, PIN_MEMORY,<br/>{va=0x7f..., size=256MB})

    NV_KMD->>KERNEL: pin_user_pages_fast(va, npages,<br/>FOLL_WRITE, pages[])
    Note over KERNEL: Fault in all pages<br/>Mark pages as pinned<br/>(elevated refcount)

    KERNEL->>NV_KMD: 65536 pages pinned

    NV_KMD->>NV_KMD: Build DMA mapping for GPU:<br/>dma_map_sg(dev, sgt, nents, BIDIR)

    NV_KMD->>GPU: Map into GPU page table<br/>(PTE → system RAM via PCIe)

    NV_KMD->>NV_UMD: GPU VA = 0x200000000

    NV_UMD->>APP: ptr = 0x7f0000000000<br/>(CPU VA = GPU VA transparent via UVM)

    Note over APP: CPU writes to *ptr<br/>GPU kernel reads same data<br/>Zero-copy!

    Note over APP: Dangers of excessive pinning:
    Note over KERNEL: • 256MB locked = cannot be swapped<br/>• Reduces available RAM for page cache<br/>• Too much pinning → OOM!<br/>• ulimit -l controls per-user limit<br/>• vm.max_map_count may be hit
```

---

## Common NVIDIA Interview Follow-ups

### Memory-Related

| Question | Key Points |
|----------|------------|
| **ECC and GPU memory** | HBM has ECC; driver handles correctable/uncorrectable errors via interrupt; uncorrectable → page retirement |
| **GPU memory compression** | Hardware delta-color/lossless compression; managed by MMU kind bits in PTEs; 2-4x bandwidth savings |
| **NVSwitch vs NVLink** | NVLink = point-to-point; NVSwitch = all-to-all fabric; enables GPUDirect across 8+ GPUs |
| **CUDA Managed Memory prefetch** | `cudaMemPrefetchAsync()` → triggers proactive migration; avoids page fault latency |
| **GPU memory fragmentation** | VRAM has same buddy fragmentation issues; TTM handles compaction by migrating BOs |

---

## Key Source Files

| Component | Source |
|-----------|--------|
| DRM/TTM framework | `drivers/gpu/drm/ttm/` |
| DRM GEM helpers | `drivers/gpu/drm/drm_gem.c` |
| HMM (Heterogeneous Memory) | `mm/hmm.c`, `include/linux/hmm.h` |
| MMU notifiers | `mm/mmu_notifier.c` |
| IOMMU core | `drivers/iommu/iommu.c` |
| DMA mapping core | `kernel/dma/mapping.c` |
| pin_user_pages | `mm/gup.c` |
| Nouveau (open NVIDIA) | `drivers/gpu/drm/nouveau/` |
| NVIDIA open-gpu-kernel | `github.com/NVIDIA/open-gpu-kernel-modules` |
