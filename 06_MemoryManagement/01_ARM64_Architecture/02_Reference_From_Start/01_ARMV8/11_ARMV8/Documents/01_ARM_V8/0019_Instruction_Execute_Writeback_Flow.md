# ARMv8 SoC Execution Flow (Detailed)

## Example Scenario
Opening a file (e.g., PPT) from an SSD connected via PCIe.

---

## 1. Device Connection & Detection

- SSD is connected to the SoC via interfaces like:
  - PCIe
  - SATA
- The **SSD controller (peripheral)** detects the device presence.

---

## 2. Interrupt Generation

- After detection or when data is ready:
  - SSD controller raises an **interrupt**
- Interrupt is sent to:
  - **GIC (Generic Interrupt Controller)** in ARMv8 systems

---

## 3. Interrupt Handling by CPU

- GIC routes interrupt to a CPU core
- CPU performs:
  1. Saves current execution context
  2. Jumps to **Interrupt Service Routine (ISR)** using vector table
  3. ISR identifies the source (SSD controller)

---

## 4. Driver Execution

- Corresponding **device driver** is invoked
- Driver is already loaded in:
  - **DDR memory (main memory)**

### Driver Responsibilities:
- Communicate with SSD controller
- Setup data transfer (read/write requests)
- Configure DMA (if used)

---

## 5. Program Control Flow Change

- CPU **Program Counter (PC)** jumps to driver code
- Execution flow:
  - Fetch instructions from memory
  - Decode and execute

---

## 6. Data Transfer Mechanism

### Case A: CPU-driven (Programmed I/O)
- CPU directly reads/writes data via registers

### Case B: DMA-driven (Typical in ARM SoCs)
- Driver configures **DMA engine**
- Data transfer happens:
  - SSD → DDR memory
- CPU is free during transfer

---

## 7. Interconnect Activity

- Data moves through SoC interconnect:
  - Example: **FlexNoC / CMN / CCI**
- Transactions generated:
  - **AXI / AHB protocol transactions**

### Types of transactions:
- Read transactions (load)
- Write transactions (store)

---

## 8. Memory Subsystem

- Data reaches:
  - **Memory Controller**
  - Stored in **DDR (LPDDR / DDR3 / DDR4)**

- Memory scheduler optimizes:
  - Latency
  - Bandwidth

---

## 9. CPU Data Consumption

- CPU accesses data from memory:
  - Via L1 / L2 caches
- Cache coherency maintained using:
  - **ACE / CHI protocols**

---

## 10. Completion Flow

- DMA (or controller) raises **completion interrupt**
- CPU:
  - Handles interrupt
  - Marks I/O operation complete
  - Returns to user application

---

## 🔁 End-to-End Flow Summary

1. SSD detected  
2. Interrupt generated → GIC  
3. CPU executes ISR  
4. Driver configures transfer  
5. Data moves via interconnect (AXI)  
6. Stored in DDR  
7. CPU processes data  

---

## 🔑 Key ARMv8 Components Involved

### CPU Subsystem
- Cortex-A cores (A53, A57, etc.)
- L1/L2 caches

### Interconnect
- CCI / CMN / FlexNoC

### Interrupt Controller
- GIC (v2/v3/v4)

### Memory
- DDR via Memory Controller

### Peripheral Subsystem
- PCIe controller
- SSD controller

---

## 💡 Important Concepts

### Interrupt-driven execution
- CPU reacts to events instead of polling

### Memory-mapped I/O
- Devices accessed like memory addresses

### DMA (Direct Memory Access)
- Offloads CPU for bulk data transfer

### Cache Coherency
- Ensures all cores see consistent data

### AXI Protocol
- High-performance bus for SoC communication

---

## 🚀 Real-World Insight

- This flow is fundamental for:
  - File I/O
  - Networking
  - Storage systems
- Performance depends heavily on:
  - Interconnect efficiency
  - Cache coherency
  - DMA usage
