Here are two **colored Mermaid diagrams** showing the path of `malloc()` from **userspace → kernel space → back to userspace**.

### Flow diagram

```mermaid
flowchart TD
    A["User code calls malloc(size)"] --> B["libc allocator<br/>glibc malloc / jemalloc / musl"]
    B --> C{"Free chunk available<br/>in cache/bin/arena?"}

    C -->|Yes| D["Mark chunk allocated"]
    D --> E["Return pointer to user code"]

    C -->|No| F{"Large request?"}
    F -->|No| G["Extend heap<br/>via brk/sbrk path"]
    F -->|Yes| H["Create mapping<br/>via mmap path"]

    G --> I["System call into kernel"]
    H --> I

    I --> J["Kernel virtual memory manager"]
    J --> K{"Can address space be expanded<br/>or mapped?"}

    K -->|No| L["Allocation fails"]
    L --> M["Return NULL to userspace"]

    K -->|Yes| N["Create / adjust VMA<br/>update process mappings"]
    N --> O["Return virtual address range<br/>to allocator"]
    O --> P["Allocator builds chunk metadata"]
    P --> Q["Return pointer to user code"]

    Q --> R{"User touches memory?"}
    R -->|No| S["Pages may remain lazily mapped"]
    R -->|Yes| T["Page fault on first access"]
    T --> U["Kernel allocates physical page"]
    U --> V["Zero-fill page, update page tables"]
    V --> W["Execution resumes in userspace"]

    style A fill:#dbeafe,stroke:#1d4ed8,stroke-width:2px,color:#111
    style B fill:#e0e7ff,stroke:#4338ca,stroke-width:2px,color:#111
    style C fill:#fef3c7,stroke:#d97706,stroke-width:2px,color:#111
    style D fill:#dcfce7,stroke:#15803d,stroke-width:2px,color:#111
    style E fill:#bbf7d0,stroke:#166534,stroke-width:2px,color:#111
    style F fill:#fde68a,stroke:#b45309,stroke-width:2px,color:#111
    style G fill:#fee2e2,stroke:#b91c1c,stroke-width:2px,color:#111
    style H fill:#fecaca,stroke:#991b1b,stroke-width:2px,color:#111
    style I fill:#f3e8ff,stroke:#7e22ce,stroke-width:2px,color:#111
    style J fill:#ede9fe,stroke:#6d28d9,stroke-width:2px,color:#111
    style K fill:#fef3c7,stroke:#d97706,stroke-width:2px,color:#111
    style L fill:#fecaca,stroke:#991b1b,stroke-width:2px,color:#111
    style M fill:#fee2e2,stroke:#b91c1c,stroke-width:2px,color:#111
    style N fill:#ddd6fe,stroke:#5b21b6,stroke-width:2px,color:#111
    style O fill:#c7d2fe,stroke:#3730a3,stroke-width:2px,color:#111
    style P fill:#e9d5ff,stroke:#7e22ce,stroke-width:2px,color:#111
    style Q fill:#bbf7d0,stroke:#166534,stroke-width:2px,color:#111
    style R fill:#fef3c7,stroke:#d97706,stroke-width:2px,color:#111
    style S fill:#cffafe,stroke:#0f766e,stroke-width:2px,color:#111
    style T fill:#fde68a,stroke:#b45309,stroke-width:2px,color:#111
    style U fill:#ccfbf1,stroke:#0f766e,stroke-width:2px,color:#111
    style V fill:#a7f3d0,stroke:#047857,stroke-width:2px,color:#111
    style W fill:#bbf7d0,stroke:#166534,stroke-width:2px,color:#111
```

---

### Sequence diagram

```mermaid
sequenceDiagram
    autonumber
    participant U as User Program
    participant A as Userspace Allocator
    participant K as Kernel VM Subsystem
    participant MM as Page Tables / Memory Manager

    U->>A: malloc(size)
    A->>A: Check tcache / bins / arena

    alt Free chunk already available
        A->>A: Mark chunk as in-use
        A-->>U: Return pointer
    else Need more memory
        alt Small/medium allocation
            A->>K: brk / sbrk syscall
            K->>K: Expand heap VMA
            K-->>A: New heap range
        else Large allocation
            A->>K: mmap syscall
            K->>K: Create anonymous VMA
            K-->>A: New mapped range
        end

        A->>A: Create allocator metadata
        A-->>U: Return pointer
    end

    Note over U: Pointer is virtual address in userspace

    U->>U: First read/write to returned memory
    U->>K: Page fault trap (if page not backed yet)
    K->>MM: Allocate physical page
    MM->>MM: Zero-fill page and update PTE
    MM-->>K: Mapping ready
    K-->>U: Resume instruction
    U->>U: Memory access succeeds
```

### What this captures

`malloc()` usually does **not** directly allocate physical RAM immediately. It often:

1. checks allocator-managed free memory first,
2. asks the kernel for more **virtual address space** with `brk` or `mmap` if needed,
3. returns a pointer to userspace,
4. and only when the program actually touches the page does the kernel often allocate the real physical page on demand.

If you want, I can also give you a **single polished architecture diagram** combining allocator internals, syscalls, page faults, and `free()` behavior.
