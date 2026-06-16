## 🔍 What’s happening here

### Scenario
- A big core wants to write to a cache line  
- That line is currently owned (Modified state) by a LITTLE core  

---

## 🧠 Step-by-step (Real Hardware Behavior)

### 1. ReadUnique from Big Core
- Signals intent to modify  
- Requires exclusive ownership  

### 2. Directory Lookup in CMN (Home Node)
- Home node checks directory  
- Finds the cache line is present in LITTLE cluster  

### 3. Snoop to LITTLE Cluster
- **SnpUnique** operation:
  - Forces invalidation  
  - Requests latest data if dirty  

### 4. LITTLE Cluster Response
- Sends:
  - **RSP** → indicates hit and dirty  
  - **DAT** → supplies updated cache line  

### 5. Data Forwarded to Big Core
- Big core receives latest data  
- Gains ownership of the cache line  

### 6. State Transitions
- LITTLE cluster → Invalid  
- Big core → Modified (exclusive owner)  

---

## 🔑 Key ARMv8 / CHI Concepts Demonstrated

- Cluster-to-cluster snooping  
- Directory-based coherency (HN-F)  
- Ownership transfer (Modified state migration)  

### Snoop Types
- **SnpUnique**  
  - Invalidates other copies  
  - Transfers ownership  

### Data Sourcing Optimization
- Data comes from cache (not DRAM) when dirty  
- Reduces latency and improves performance  

---

## 💡 Why this matters (Real-World Insight)

- This is a **critical path in big.LITTLE systems**

### Impacts
- **Performance**  
  - Latency of ownership transfer  

- **Power Efficiency**  
  - Avoids expensive DRAM accesses  

### Used in
- Mobile SoCs  
  - Qualcomm  
  - Exynos  
  - Apple Silicon  

- Server-class ARM  
  - Neoverse platforms with CMN  
