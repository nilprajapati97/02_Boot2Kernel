ARMv8 CPU Subsystem – Detailed Explanation
==========================================

1. OVERVIEW
-----------
This CPU subsystem follows a big.LITTLE architecture:
- A7 cores → low power (efficiency)
- A15 cores → high performance
- Each cluster has its own L2 cache
- All clusters connect via a Coherent Interconnect (CCI)

------------------------------------------------------------

2. CORE ARCHITECTURE (A7 / A15)
-------------------------------
Each CPU core contains:

- Fetch Unit → fetches instructions
- Decode Unit → decodes instructions
- Execution Units → ALU, FPU, SIMD
- Registers → fastest storage
- L1 Cache → private to each core

A7 Core:
- Low power
- Simpler design
- Used for light tasks

A15 Core:
- High performance
- Complex pipeline
- Used for heavy tasks

------------------------------------------------------------

3. L1 CACHE (PER CORE)
----------------------
Each core has two L1 caches:

- L1 Instruction Cache (I-Cache)
- L1 Data Cache (D-Cache)

Properties:
- Private to each core
- Very fast (1–3 cycles)
- Small size (32KB–64KB)

Working:
- Core first checks L1 cache
- If data is present → immediate use
- If miss → go to L2 cache

------------------------------------------------------------

4. L2 CACHE (PER CLUSTER)
--------------------------
Each cluster (A7 and A15) has its own shared L2 cache.

Properties:
- Shared among cores in same cluster
- Larger than L1 (256KB–2MB)
- Slower than L1 (10–20 cycles)

Connection:
Core → L1 → Cluster Bus → L2

Working:
- On L1 miss → request goes to L2
- If found → sent back to L1
- If not found → forwarded to interconnect

------------------------------------------------------------

5. CACHE COHERENCY (WITHIN CLUSTER)
-----------------------------------
Multiple cores may access same data.

Example:
- Core 1 updates variable X
- Core 2 reads X

L2 ensures:
- Core 2 gets updated value
- No stale data

------------------------------------------------------------

6. COHERENT INTERCONNECT (CCI)
------------------------------
This is the central communication unit.

Main functions:

1. Cache Coherency
   - Ensures all cores see the latest data
   - Uses protocols like MESI/MOESI

2. Snoop Mechanism
   - Checks if other cores have requested data
   - Fetches data from another cache if available

3. Data Routing
   - Routes requests between:
     - A7 cluster
     - A15 cluster
     - Memory

4. Arbitration
   - Decides which request is served first
   - Handles multiple simultaneous requests

------------------------------------------------------------

7. DATA FLOW (READ OPERATION)
-----------------------------
Step-by-step:

1. Core requests data
2. Check L1 cache
   - Hit → return data
   - Miss → go to L2

3. Check L2 cache
   - Hit → return data
   - Miss → go to CCI

4. CCI checks:
   - Other cluster caches (snoop)
   - If found → fetch from there
   - Else → go to memory

5. Data returns:
   Memory → CCI → L2 → L1 → Core

------------------------------------------------------------

8. DATA FLOW (WRITE OPERATION)
------------------------------
Two methods:

1. Write-Through
   - Data written to L1 and memory immediately

2. Write-Back (common)
   - Data written only to L1
   - Marked as "dirty"
   - Written to memory later

CCI tracks these updates to maintain consistency.

------------------------------------------------------------

9. INTER-CLUSTER COMMUNICATION
------------------------------
Example:

- A7 core updates data
- A15 core needs same data

Flow:
A15 → L1 miss → L2 miss → CCI
CCI snoops A7 cluster
Data found → sent directly to A15

(No need to access memory → faster)

------------------------------------------------------------

10. COMPLETE DATA PATH
----------------------
Core
 ↓
L1 Cache (private)
 ↓
L2 Cache (shared)
 ↓
Coherent Interconnect (CCI)
 ↓
Memory or Other Cluster

------------------------------------------------------------

11. KEY DESIGN BENEFITS
-----------------------

- L1 private → very fast access
- L2 shared → efficient within cluster
- Separate clusters → optimized performance + power
- CCI → maintains global data consistency

------------------------------------------------------------

12. SIMPLE ANALOGY
-------------------

- A7 cores → fuel-efficient cars
- A15 cores → high-speed race cars
- L1 cache → personal desk
- L2 cache → shared office storage
- CCI → highway system
- Memory → warehouse

------------------------------------------------------------

END OF DOCUMENT
