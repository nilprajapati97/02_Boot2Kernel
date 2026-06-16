```mermaid
flowchart TD
    A[CPU Core 0 Issues Read Write] --> B[L1 Cache Check]
    
    B -->|Hit| C[Serve from L1 Cache]
    B -->|Miss| D[Send Request to Interconnect]
    
    D --> E[Interconnect or Coherency Manager]
    
    E --> F{Other Caches Hold Line}
    
    F -->|No| G[Forward to L2 or Memory Controller]
    G --> H[Fetch from DRAM]
    H --> I[Fill Cache Line in L1]
    
    F -->|Yes| J[Send Snooping Requests]
    
    J --> K[Other CPU L1 or L2 Caches]
    
    K --> L{Cache Line State}
    
    L -->|Modified| M[Writeback Data to Interconnect]
    L -->|Shared| N[Provide Shared Copy]
    L -->|Invalid| O[Ignore]
    
    M --> P[Update Requesting Core]
    N --> P
    
    P --> Q[Update Cache State MESI MOESI]
    Q --> R[Complete Transaction]
    
    I --> Q
