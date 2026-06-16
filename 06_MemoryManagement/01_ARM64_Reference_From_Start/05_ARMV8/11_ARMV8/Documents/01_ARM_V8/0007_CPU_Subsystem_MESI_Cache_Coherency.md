------------------------------------------------------------
10. MESI CACHE COHERENCY PROTOCOL
------------------------------------------------------------

MESI stands for:

M → Modified  
E → Exclusive  
S → Shared  
I → Invalid  

This protocol ensures that all CPU cores see **consistent data**.

------------------------------------------------------------
11. MESI STATES EXPLAINED
------------------------------------------------------------

1. MODIFIED (M)
- Cache line is modified (different from memory)
- Exists ONLY in one cache
- Must be written back to memory before sharing

2. EXCLUSIVE (E)
- Present in only one cache
- Same as memory (not modified yet)
- Can be changed to Modified without notifying others

3. SHARED (S)
- Present in multiple caches
- Same as memory
- Read-only

4. INVALID (I)
- Cache line is invalid
- Data must be fetched again

------------------------------------------------------------
12. MESI STATE DIAGRAM (ASCII)
------------------------------------------------------------

             +-----------+
             |  INVALID  |
             +-----------+
              |   ^
   Read Miss  |   | Invalidate
              v   |
         +-----------+
         |  SHARED   |
         +-----------+
          |   ^   |
 Write    |   |   | Read by others
          v   |   v
     +-----------+------+
     |  MODIFIED        |
     +-----------+------+
          ^ 
          |
     +-----------+
     | EXCLUSIVE |
     +-----------+

------------------------------------------------------------
13. MESI STATE TRANSITIONS (IMPORTANT)
------------------------------------------------------------

From INVALID:
- Read → go to EXCLUSIVE (if no other cache has it)
- Read → go to SHARED (if others have it)
- Write → go to MODIFIED

From EXCLUSIVE:
- Read → stay EXCLUSIVE
- Write → go to MODIFIED
- Other core reads → go to SHARED

From SHARED:
- Read → stay SHARED
- Write → go to MODIFIED (invalidate others)

From MODIFIED:
- Read → stay MODIFIED
- Other core reads → go to SHARED (write-back happens)
- Write → stay MODIFIED

------------------------------------------------------------
14. MESI FLOW EXAMPLE (VERY IMPORTANT)
------------------------------------------------------------

Example:

Step 1:
Core A reads X
→ X loaded into cache → EXCLUSIVE

Step 2:
Core B reads X
→ Now both have X → SHARED

Step 3:
Core A writes X
→ Core A → MODIFIED
→ Core B → INVALID

Step 4:
Core B reads X
→ CCI fetches updated data from Core A
→ Both move to SHARED

------------------------------------------------------------
15. MESI DIAGRAM (MERMAID)
------------------------------------------------------------

```mermaid
stateDiagram-v2

[*] --> Invalid

Invalid --> Exclusive : Read (no sharers)
Invalid --> Shared : Read (others have copy)
Invalid --> Modified : Write

Exclusive --> Modified : Write
Exclusive --> Shared : Read by other core

Shared --> Modified : Write (invalidate others)

Modified --> Shared : Read by other core
Modified --> Invalid : Eviction
