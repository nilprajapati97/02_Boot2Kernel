Here’s a **step-by-step timing + status register update diagram** showing how a Linux I²C driver detects completion/errors for both **polling** and **interrupt** modes during a read transaction.

---

## **I²C Read Flow – Timing vs Status Register Changes**

```
SDA:   ┌───────┐     ┌───────┐     ┌───────┐
       │  SLA+R│     │ DATA1 │     │ DATA2 │ ...
       └───────┘     └───────┘     └───────┘
SCL:  _|¯|_|¯|_|¯|_|¯|_|¯|_|¯|_|¯|_|¯|_|¯|_|¯|_

Time →
┌──────────────────────────────────────────────────────────┐
│                POLLING FLOW (CPU busy waiting)           │
└──────────────────────────────────────────────────────────┘
t0: Write START → Status Reg: SB=1                         (START condition sent)
t1: Write SLA+R → Status Reg: ADDR=1 if ACK, AF=1 if NACK  (Address phase complete)
t2: Wait RXNE=1  → Status Reg: RXNE=1                      (Data byte ready)
t3: Read DR      → Status Reg: RXNE=0, BTF=1 if more data  (Byte transfer finished)
t4: Send NACK+STOP → Status Reg: STOPF=1                   (STOP condition detected)

┌──────────────────────────────────────────────────────────┐
│               INTERRUPT FLOW (CPU sleeps, wakes)         │
└──────────────────────────────────────────────────────────┘
t0: Driver enables IRQ → Write START
t1: IRQ fires when ADDR=1 → ISR reads SR, clears flag
t2: IRQ fires when RXNE=1 → ISR reads DR, stores byte
t3: IRQ fires for BTF=1   → ISR preps next read or sends STOP
t4: IRQ fires for STOPF=1 → ISR signals completion
t5: Main thread wakes via wait_event_interruptible()

---

### **Status Register Bit Changes Over Time**
(Time axis applies to both Polling & Interrupt modes)

Time   | Event                   | SR Bits Set                     | Driver Action
-------|------------------------|-----------------------------------|---------------------------------
t0     | START sent              | `SB=1`                           | Poll: check SB; Int: IRQ fires
t1     | Addr ACK                | `ADDR=1`                         | Check if AF=1 (NACK)
t2     | First byte ready        | `RXNE=1`                         | Read DR; if OVR=1 → error
t3     | More bytes ready        | `BTF=1`                          | Prepare next read
t4     | STOP sent/detected      | `STOPF=1`                        | End transaction
Any    | Bus error               | `BERR=1`                         | Abort transfer
Any    | Arbitration lost        | `ARLO=1`                         | Retry if multi-master
Any    | ACK fail (data phase)   | `AF=1`                           | Report -EREMOTEIO

---

### **Diagram Summary**
- **Polling**: Loops until the expected SR bit is set → read/clear → next step.
- **Interrupt**: Hardware sets SR bits → IRQ triggers → ISR handles → main thread sleeps until completion.
- **Error detection**: Always based on SR flags (`AF`, `BERR`, `ARLO`, `OVR`).

---

If you want, I can make this into a **color-coded bus+register timeline** where SDA/SCL, hardware status bits, and driver function calls are aligned — it’s a killer visual for interviews because it links **physical I²C events** to **software actions** in Linux.
```
