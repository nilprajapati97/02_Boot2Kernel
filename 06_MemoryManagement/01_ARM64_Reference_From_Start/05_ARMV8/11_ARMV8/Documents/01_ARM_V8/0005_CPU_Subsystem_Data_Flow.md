ARMv8 CPU Subsystem – Detailed Explanation with Diagrams
======================================================

------------------------------------------------------------
1. OVERVIEW
------------------------------------------------------------
This CPU subsystem follows big.LITTLE architecture:
- A7 → low power cores
- A15 → high performance cores
- L1 (private), L2 (shared), CCI (global interconnect)

------------------------------------------------------------
2. HIGH-LEVEL DIAGRAM (ASCII)
------------------------------------------------------------

        +------------------- CPU SUBSYSTEM -------------------+

        A7 CLUSTER                         A15 CLUSTER
   +-----------------+              +-----------------+
   | A7  A7  A7  A7  |              | A15   A15       |
   +-----------------+              +-----------------+
           |                                |
        +--------+                     +--------+
        |  L2    |                     |  L2    |
        +--------+                     +--------+
              \                          /
               \                        /
        +--------------------------------------+
        |   Coherent Interconnect (CCI)        |
        +--------------------------------------+
                         |
                    Main Memory

------------------------------------------------------------
3. CORE INTERNAL DIAGRAM
------------------------------------------------------------

        +---------------------------+
        |        CPU CORE           |
        +---------------------------+
        |  Fetch Unit              |
        |  Decode Unit             |
        |  Execution Units         |
        |  Registers               |
        +---------------------------+
        |  L1 Instruction Cache    |
        |  L1 Data Cache           |
        +---------------------------+

------------------------------------------------------------
4. CACHE HIERARCHY DIAGRAM
------------------------------------------------------------

        Core
         |
     +--------+
     |  L1    |   (Private, fastest)
     +--------+
         |
     +--------+
     |  L2    |   (Shared in cluster)
     +--------+
         |
     +--------+
     |  CCI   |   (Interconnect)
     +--------+
         |
     Memory

------------------------------------------------------------
5. DATA FLOW DIAGRAM (READ)
------------------------------------------------------------

Step-by-step:

 Core
  |
  v
[L1 Cache] ---- HIT ---> Done
  |
 MISS
  v
[L2 Cache] ---- HIT ---> Return to L1
  |
 MISS
  v
[CCI Interconnect]
  |
  +--> Check other cluster (snoop)
  |        |
  |        +--> Found → Return
  |
  +--> Not found → Memory
                 |
                 v
             Return Data

------------------------------------------------------------
6. INTER-CLUSTER COMMUNICATION
------------------------------------------------------------

   A7 Core                  A15 Core
      |                         |
     L1                        L1
      |                         |
     L2                        L2
      \                        /
       \                      /
        +--------------------+
        |        CCI         |
        +--------------------+
                |
        Data transferred directly

------------------------------------------------------------
7. MERMAID DIAGRAM (FOR GITHUB)
------------------------------------------------------------

```mermaid
flowchart TB

Core["CPU Core"]
L1["L1 Cache (Private)"]
L2["L2 Cache (Shared)"]
CCI["Coherent Interconnect"]
MEM["Main Memory"]

Core --> L1
L1 --> L2
L2 --> CCI
CCI --> MEM
