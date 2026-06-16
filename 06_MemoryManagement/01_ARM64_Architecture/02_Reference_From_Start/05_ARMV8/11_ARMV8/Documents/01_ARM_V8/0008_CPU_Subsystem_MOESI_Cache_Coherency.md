------------------------------------------------------------
17. MOESI CACHE COHERENCY PROTOCOL (ADVANCED)
------------------------------------------------------------

MOESI is an extension of MESI with one extra state:

M → Modified  
O → Owned   (NEW)  
E → Exclusive  
S → Shared  
I → Invalid  

------------------------------------------------------------
18. WHY MOESI IS BETTER THAN MESI
------------------------------------------------------------

Problem in MESI:
- Modified data must be written back to memory before sharing

Solution in MOESI:
- "Owned" state allows sharing WITHOUT writing to memory

👉 Improves performance and reduces memory traffic

------------------------------------------------------------
19. MOESI STATES EXPLAINED
------------------------------------------------------------

1. MODIFIED (M)
- Only one cache has the data
- Data is dirty (different from memory)
- Must supply data to others

2. OWNED (O)  ⭐ IMPORTANT
- Data is shared with other caches
- This cache is the "owner"
- Memory is NOT updated yet
- Owner supplies data to others

3. EXCLUSIVE (E)
- Only one cache has data
- Clean (same as memory)

4. SHARED (S)
- Multiple caches have same clean data

5. INVALID (I)
- Data not valid

------------------------------------------------------------
20. MOESI STATE DIAGRAM (ASCII)
------------------------------------------------------------

             +-----------+
             |  INVALID  |
             +-----------+
              |   ^
              v   |
         +-----------+
         | EXCLUSIVE |
         +-----------+
          |     |
          v     v
     +-----------+--------+
     | MODIFIED           |
     +-----------+--------+
          |      |
          v      v
     +-----------+
     |  OWNED    |
     +-----------+
          |
          v
     +-----------+
     |  SHARED   |
     +-----------+

------------------------------------------------------------
21. MOESI FLOW EXAMPLE
------------------------------------------------------------

Step 1:
Core A reads X
→ EXCLUSIVE

Step 2:
Core A writes X
→ MODIFIED

Step 3:
Core B reads X
→ Core A → OWNED
→ Core B → SHARED

(Note: No memory write needed!)

Step 4:
Core C reads X
→ A remains OWNER
→ B and C → SHARED

------------------------------------------------------------
22. KEY DIFFERENCE (MESI vs MOESI)
------------------------------------------------------------

MESI:
Modified → must write to memory before sharing

MOESI:
Modified → can go to OWNED → share directly

👉 Faster and more efficient

------------------------------------------------------------
23. MOESI MERMAID DIAGRAM
------------------------------------------------------------

```mermaid
stateDiagram-v2

[*] --> Invalid

Invalid --> Exclusive : Read
Invalid --> Shared : Read (others have)
Invalid --> Modified : Write

Exclusive --> Modified : Write
Exclusive --> Shared : Read by other core

Shared --> Modified : Write

Modified --> Owned : Read by other core
Owned --> Shared : More readers

Owned --> Modified : Write by owner
