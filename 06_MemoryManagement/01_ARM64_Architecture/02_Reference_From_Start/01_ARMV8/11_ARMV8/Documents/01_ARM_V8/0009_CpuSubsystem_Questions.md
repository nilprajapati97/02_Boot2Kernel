============================================================
ARMv8 CPU SUBSYSTEM – 1 PAGE REVISION CHEAT SHEET
============================================================

🔹 ARCHITECTURE (big.LITTLE)
- A7 → Low power (efficiency cores)
- A15 → High performance cores
- Separate L2 caches per cluster
- Connected via Coherent Interconnect (CCI)

------------------------------------------------------------
🔹 CORE STRUCTURE
- Fetch → Decode → Execute → Writeback
- Registers (fastest storage)
- L1 Cache (private)

------------------------------------------------------------
🔹 CACHE HIERARCHY
Core → L1 → L2 → CCI → Memory

L1 Cache:
- Private per core
- Fastest (1–3 cycles)

L2 Cache:
- Shared per cluster
- Slower (10–20 cycles)

------------------------------------------------------------
🔹 CCI (COHERENT INTERCONNECT)
- Connects clusters + memory
- Maintains cache coherency
- Performs snooping
- Handles arbitration

------------------------------------------------------------
🔹 DATA FLOW (READ)
1. L1 hit → done
2. L1 miss → L2
3. L2 miss → CCI
4. CCI snoops other caches
5. If not found → Memory

------------------------------------------------------------
🔹 MESI PROTOCOL
M → Modified (dirty, single owner)
E → Exclusive (clean, single owner)
S → Shared (clean, multiple)
I → Invalid

Key:
- Only ONE modified copy exists
- Write → invalidates others

------------------------------------------------------------
🔹 MOESI PROTOCOL (ADVANCED)
+ O → Owned (shared but dirty)

Benefit:
- No need to write to memory before sharing
- Owner supplies data directly

------------------------------------------------------------
🔹 ACE PROTOCOL (ARM)
Channels:
- AR → Read request
- AW → Write request
- W  → Write data
- R  → Read data
- AC → Snoop request
- CR → Snoop response
- CD → Snoop data

------------------------------------------------------------
🔹 KEY FORMULAS (FLOW)
Core → L1 → L2 → CCI → Memory / Other Core

------------------------------------------------------------
🔹 IMPORTANT POINTS
✔ L1 = fastest, private
✔ L2 = shared per cluster
✔ CCI = global traffic + coherency
✔ MESI/MOESI = data correctness
✔ ACE = hardware communication

------------------------------------------------------------
🔹 INTERVIEW ONE-LINER
"ARM big.LITTLE uses separate clusters connected via a 
coherent interconnect implementing MOESI protocol using 
ACE signals to maintain cache coherency."

============================================================
VIVA QUESTIONS & ANSWERS
============================================================

Q1. What is big.LITTLE architecture?
A: A heterogeneous design combining low-power (A7) and high-performance (A15) cores.

------------------------------------------------------------

Q2. Why do we need cache memory?
A: To reduce memory access latency and improve performance.

------------------------------------------------------------

Q3. Difference between L1 and L2 cache?
A:
- L1 → private, fastest, small
- L2 → shared, larger, slower

------------------------------------------------------------

Q4. What is cache coherency?
A: Ensuring all cores see the same updated data.

------------------------------------------------------------

Q5. What is MESI protocol?
A: A cache coherency protocol with states:
Modified, Exclusive, Shared, Invalid.

------------------------------------------------------------

Q6. What is MOESI?
A: Extension of MESI adding "Owned" state for efficient sharing.

------------------------------------------------------------

Q7. What is the benefit of Owned state?
A: Allows sharing dirty data without writing to memory.

------------------------------------------------------------

Q8. What is CCI?
A: Coherent Interconnect connecting clusters and maintaining coherency.

------------------------------------------------------------

Q9. What is snooping?
A: Checking other caches for requested data.

------------------------------------------------------------

Q10. What is ACE protocol?
A: ARM protocol enabling cache coherency using AXI extensions.

------------------------------------------------------------

Q11. What happens on L1 cache miss?
A: Request goes to L2, then CCI if needed.

------------------------------------------------------------

Q12. What happens on write operation?
A: Other cache copies are invalidated.

------------------------------------------------------------

Q13. Why separate L2 for A7 and A15?
A: To avoid contention and optimize performance.

------------------------------------------------------------

Q14. What is write-back cache?
A: Data updated in cache first, written to memory later.

------------------------------------------------------------

Q15. What is write-through cache?
A: Data written to cache and memory simultaneously.

------------------------------------------------------------

Q16. What is interconnect role?
A: Data routing, arbitration, and coherency.

------------------------------------------------------------

Q17. What is a snoop request?
A: A signal to check if other caches have a data copy.

------------------------------------------------------------

Q18. Which protocol ARM uses?
A: MOESI (commonly) with ACE interface.

------------------------------------------------------------

Q19. Why is coherency important?
A: Prevents stale or incorrect data across cores.

------------------------------------------------------------

Q20. What is the fastest memory in system?
A: Registers → then L1 cache.

============================================================
END OF DOCUMENT
