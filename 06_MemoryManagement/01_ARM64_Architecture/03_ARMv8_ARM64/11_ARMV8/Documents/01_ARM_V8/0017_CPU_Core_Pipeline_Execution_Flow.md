# ARM CPUSS Architecture

## Overview
- Octa-core architecture  
- Big.LITTLE architecture  

## Core Types
- Cortex-A57 → High performance applications  
- Cortex-A53 → Power saving, low performance applications  

## Core Configuration
- Flexible mix of high-performance and low-performance cores  

### Example Configuration
- 1 × Cortex-A57  
- 7 × Cortex-A53  

---

## Core Roles
- One core acts as the **primary core**  
- Remaining 7 cores act as **secondary cores**  

### Boot Behavior
- Primary core boots first on reset  
- Primary core is responsible for booting the remaining cores  

---

## Reset and PC Initialization

### Program Counter (PC)
- Each CPU core has a **Program Counter (PC) register**  
- Default reset value:
  - `PC = 0x0000_0004` → points to **reset handler**  

### Execution Start
- CPU core begins executing instructions from address:
  - `0x0000_0004`  

### Reset Behavior
- When reset is applied to CPUSS:
  - Only one core (primary core) is activated  

### Secondary Core Boot
- After primary core is up:
  - It programs the registers of other cores  
  - Brings secondary cores out of reset  
  - Initiates their execution  

---

## Summary
- ARM CPUSS uses a **heterogeneous multi-core design (big.LITTLE)**  
- A single **primary core controls system boot**  
- Secondary cores are **software-activated after reset**  
- Execution begins from a **fixed reset vector (0x0000_0004)**  
