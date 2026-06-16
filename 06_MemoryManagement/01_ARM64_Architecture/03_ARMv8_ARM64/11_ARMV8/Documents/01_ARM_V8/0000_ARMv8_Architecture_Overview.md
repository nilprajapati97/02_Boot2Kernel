# ARMv8 Architecture - Complete Reference Guide

A comprehensive guide to ARMv8 AArch64 architecture covering execution states registers exception levels memory management SIMD NEON virtualization security extensions and more.

---

## Table of Contents

- [1 Overview](#1-overview)
- [2 Execution States](#2-execution-states)
- [3 Register File](#3-register-file)
- [4 Exception Levels](#4-exception-levels)
- [5 TrustZone Security](#5-trustzone-security)
- [6 Instruction Set Architecture A64](#6-instruction-set-architecture-a64)
- [7 NEON SIMD and Floating Point](#7-neon-simd-and-floating-point)
- [8 Memory Management](#8-memory-management)
- [9 Virtualization Support](#9-virtualization-support)
- [10 Cryptographic Extensions](#10-cryptographic-extensions)
- [11 Exception and Interrupt Handling](#11-exception-and-interrupt-handling)
- [12 GIC Generic Interrupt Controller](#12-gic-generic-interrupt-controller)
- [13 ARMv8 Sub Versions and Extensions](#13-armv8-sub-versions-and-extensions)
- [14 LSE Atomics ARMv8 1](#14-lse-atomics-armv8-1)
- [15 Pointer Authentication ARMv8 3](#15-pointer-authentication-armv8-3)
- [16 Memory Tagging Extension ARMv8 5](#16-memory-tagging-extension-armv8-5)
- [17 SVE Scalable Vector Extension](#17-sve-scalable-vector-extension)
- [18 Debug Architecture](#18-debug-architecture)
- [19 ARMv8 vs ARMv7 Comparison](#19-armv8-vs-armv7-comparison)
- [20 Notable Processor Implementations](#20-notable-processor-implementations)
- [21 Boot Sequence](#21-boot-sequence)
- [22 System Control Registers](#22-system-control-registers)
- [23 Cache Architecture](#23-cache-architecture)
- [24 Power Management](#24-power-management)
- [25 Programming Examples](#25-programming-examples)
- [26 Quick Reference Summary](#26-quick-reference-summary)
- [27 Useful Resources](#27-useful-resources)

---

## 1 Overview

| Parameter | Details |
|-----------|---------|
| Released | October 2011 |
| Developer | ARM Holdings now Arm Ltd |
| Key Change | First ARM architecture with 64 bit support |
| Instruction Set | AArch64 64 bit plus AArch32 32 bit backward compatible |
| Used In | Smartphones Servers Embedded Systems HPC IoT |
| Notable Processors | Apple M1 M2 Qualcomm Snapdragon AWS Graviton Cortex A53 A57 A72 A76 A78 X1 |

### Architecture Profiles

| Profile | Name | Target |
|---------|------|--------|
| ARMv8-A | Application | Smartphones Servers PCs |
| ARMv8-R | Real-time | Automotive Industrial Storage |
| ARMv8-M | Microcontroller | IoT Embedded Sensors |

---

## 2 Execution States

ARMv8 introduces two execution states

| Feature | AArch64 | AArch32 |
|---------|---------|---------|
| Register Width | 64 bit | 32 bit |
| Instruction Set | A64 | A32 ARM plus T32 Thumb2 |
| GP Registers | 31 X0 to X30 | 15 R0 to R14 |
| PC Access | Not directly accessible | Directly accessible R15 |
| Address Space | 64 bit virtual | 32 bit virtual |
| Backward Compatible | New | ARMv7 compatible |
| Condition Codes | Conditional branch and select only | IT blocks and predicated instructions |
| Instruction Width | Fixed 32 bit | 32 bit A32 and 16 bit T32 |

### State Switching Rules

- Lower EL can be AArch32 if higher EL is AArch64
- Lower EL CANNOT be AArch64 if higher EL is AArch32
- State change only happens at exception level change
- AArch64 to AArch32 on exception return to lower EL
- AArch32 to AArch64 on exception entry to higher EL

---

## 3 Register File

### 3.1 General Purpose Registers

| Register Type | Count | Size | Purpose |
|---------------|-------|------|---------|
| General Purpose | 31 X0 to X30 | 64 bit | Data operations |
| Zero Register | 1 XZR WZR | 64 or 32 bit | Hardwired zero |
| Stack Pointer | 1 SP | 64 bit | Stack management |
| Program Counter | 1 PC | 64 bit | Instruction address |
| SIMD and FP | 32 V0 to V31 | 128 bit | Floating point and NEON |
| System Registers | Multiple | Various | Control and status |

### 3.2 AAPCS64 Calling Convention

| Register | Role | Saved By |
|----------|------|----------|
| X0-X7 | Arguments and Return values | Caller |
| X8 | Indirect result location | Caller |
| X9-X15 | Temporary scratch | Caller |
| X16-X17 | Intra procedure call linker | Caller |
| X18 | Platform register | Platform specific |
| X19-X28 | Callee saved | Callee |
| X29 | Frame pointer | Callee |
| X30 | Link register | Callee |
| SP | Stack pointer | N/A |

### 3.3 Special Registers

| Register | Purpose |
|----------|---------|
| SP_EL0 | Stack Pointer for EL0 |
| SP_EL1 | Stack Pointer for EL1 |
| SP_EL2 | Stack Pointer for EL2 |
| SP_EL3 | Stack Pointer for EL3 |
| ELR_EL1 | Exception Link Register for EL1 |
| ELR_EL2 | Exception Link Register for EL2 |
| ELR_EL3 | Exception Link Register for EL3 |
| SPSR_EL1 | Saved Program Status Register for EL1 |
| SPSR_EL2 | Saved Program Status Register for EL2 |
| SPSR_EL3 | Saved Program Status Register for EL3 |
| VBAR_EL1 | Vector Base Address Register for EL1 |
| VBAR_EL2 | Vector Base Address Register for EL2 |
| VBAR_EL3 | Vector Base Address Register for EL3 |

### 3.4 Process State PSTATE

| Flag | Name | Description |
|------|------|-------------|
| N | Negative | Result is negative |
| Z | Zero | Result is zero |
| C | Carry | Carry or borrow occurred |
| V | Overflow | Signed overflow occurred |
| D | Debug | Debug exception mask |
| A | SError | SError interrupt mask |
| I | IRQ | IRQ interrupt mask |
| F | FIQ | FIQ interrupt mask |
| nRW | Execution State | 0 equals AArch64 and 1 equals AArch32 |
| EL | Exception Level | Current exception level 0 to 3 |
| SP | Stack Pointer | 0 equals SP_EL0 and 1 equals SP_ELn |

---

## 4 Exception Levels

| Exception Level | Purpose | Example | Privilege |
|-----------------|---------|---------|-----------|
| EL0 | User Application | Apps Programs | Lowest Unprivileged |
| EL1 | OS Kernel | Linux Windows RTOS | Privileged |
| EL2 | Hypervisor | KVM Xen VMware | Higher Privilege |
| EL3 | Secure Monitor | ARM Trusted Firmware TrustZone | Highest Privilege |

### Exception Level Rules

- Exception Entry always moves to same or higher EL
- Exception Return always moves to same or lower EL
- Each EL has own SP SPSR ELR registers
- AArch32 and AArch64 switch can happen at EL change
- Secure and Non Secure controlled by EL3
- EL2 and EL3 are implementation defined and optional

### Exception Level Register Access

---

## 5 TrustZone Security

| Feature | Details |
|---------|---------|
| Hardware Isolation | Separate secure and normal worlds |
| Memory Protection | Secure memory not accessible from normal world |
| Peripheral Protection | Secure peripherals isolated via TZPC and TZASC |
| Use Cases | DRM Payments Biometrics Key Storage Secure Boot |
| Transition | Via SMC Secure Monitor Call instruction |
| Bus Signal | NS bit on AXI bus determines secure or non secure access |

### TrustZone Components

| Component | Purpose |
|-----------|---------|
| TZASC | TrustZone Address Space Controller for memory partitioning |
| TZPC | TrustZone Protection Controller for peripheral protection |
| TZMA | TrustZone Memory Adapter for on chip ROM and RAM protection |
| GIC Security | Interrupt groups Group 0 equals Secure and Group 1 equals Non secure |

---

## 6 Instruction Set Architecture A64

All A64 instructions are fixed 32 bit width

### 6.1 Arithmetic Operations

```asm
ADD   X0, X1, X2           ; X0 = X1 + X2
ADDS  X0, X1, X2           ; X0 = X1 + X2 and set flags
SUB   X0, X1, X2           ; X0 = X1 - X2
SUBS  X0, X1, X2           ; X0 = X1 - X2 and set flags
MUL   X0, X1, X2           ; X0 = X1 * X2
SDIV  X0, X1, X2           ; X0 = X1 / X2 signed
UDIV  X0, X1, X2           ; X0 = X1 / X2 unsigned
MADD  X0, X1, X2, X3       ; X0 = X3 + X1 * X2
MSUB  X0, X1, X2, X3       ; X0 = X3 - X1 * X2
SMADDL X0, W1, W2, X3      ; X0 = X3 + W1 * W2 signed long
UMADDL X0, W1, W2, X3      ; X0 = X3 + W1 * W2 unsigned long
SMULH X0, X1, X2           ; X0 = X1 * X2 upper 64 bits signed
UMULH X0, X1, X2           ; X0 = X1 * X2 upper 64 bits unsigned
NEG   X0, X1               ; X0 = -X1
ADC   X0, X1, X2           ; X0 = X1 + X2 + Carry
SBC   X0, X1, X2           ; X0 = X1 - X2 - NOT Carry
6.2 Logical Operations

AND   X0, X1, X2           ; Bitwise AND
ANDS  X0, X1, X2           ; Bitwise AND and set flags
ORR   X0, X1, X2           ; Bitwise OR
EOR   X0, X1, X2           ; Bitwise XOR
BIC   X0, X1, X2           ; Bit Clear AND NOT
ORN   X0, X1, X2           ; OR NOT
EON   X0, X1, X2           ; XOR NOT
MVN   X0, X1               ; Bitwise NOT
6.3 Shift Operations

LSL   X0, X1, #4           ; Logical Shift Left
LSR   X0, X1, #4           ; Logical Shift Right
ASR   X0, X1, #4           ; Arithmetic Shift Right
ROR   X0, X1, #4           ; Rotate Right
LSLV  X0, X1, X2           ; LSL by register value
LSRV  X0, X1, X2           ; LSR by register value
ASRV  X0, X1, X2           ; ASR by register value
RORV  X0, X1, X2           ; ROR by register value
6.4 Bit Manipulation

CLZ   X0, X1               ; Count Leading Zeros
CLS   X0, X1               ; Count Leading Sign bits
RBIT  X0, X1               ; Reverse Bits
REV   X0, X1               ; Reverse Bytes 64 bit
REV16 X0, X1               ; Reverse Bytes in 16 bit halfwords
REV32 X0, X1               ; Reverse Bytes in 32 bit words
EXTR  X0, X1, X2, #shift   ; Extract bitfield
BFM   X0, X1, #r, #s       ; Bitfield Move
SBFM  X0, X1, #r, #s       ; Signed Bitfield Move
UBFM  X0, X1, #r, #s       ; Unsigned Bitfield Move
BFI   X0, X1, #lsb, #width ; Bitfield Insert
BFXIL X0, X1, #lsb, #width ; Bitfield Extract and Insert Low
SBFIZ X0, X1, #lsb, #width ; Signed Bitfield Insert in Zero
UBFIZ X0, X1, #lsb, #width ; Unsigned Bitfield Insert in Zero
SBFX  X0, X1, #lsb, #width ; Signed Bitfield Extract
UBFX  X0, X1, #lsb, #width ; Unsigned Bitfield Extract
6.5 Comparison Operations

CMP   X0, X1               ; Compare using SUBS and discard result
CMN   X0, X1               ; Compare Negative using ADDS and discard
TST   X0, X1               ; Test bits using ANDS and discard result
6.6 Move Operations

MOV   X0, X1               ; Move register
MOV   X0, #0x1234          ; Move immediate
MOVZ  X0, #0x5678          ; Move with zero clear other bits
MOVK  X0, #0x9ABC, LSL #16 ; Move with keep preserve other bits
MOVN  X0, #0x1234          ; Move NOT bitwise invert of immediate
6.7 Address Calculation

ADR   X0, label            ; Form PC relative address within 1MB
ADRP  X0, label            ; Form PC relative page address within 4GB
6.8 Conditional Operations

CSEL  X0, X1, X2, EQ       ; X0 = EQ ? X1 : X2
CSINC X0, X1, X2, NE       ; X0 = NE ? X1 : X2+1
CSINV X0, X1, X2, GE       ; X0 = GE ? X1 : NOT X2
CSNEG X0, X1, X2, LT       ; X0 = LT ? X1 : -X2
CSET  X0, EQ               ; X0 = EQ ? 1 : 0
CSETM X0, EQ               ; X0 = EQ ? -1 : 0
CINC  X0, X1, EQ           ; X0 = EQ ? X1+1 : X1
CNEG  X0, X1, EQ           ; X0 = EQ ? -X1 : X1
CCMP  X0, X1, #nzcv, EQ    ; if EQ then CMP X0 X1 else flags=nzcv
CCMN  X0, X1, #nzcv, EQ    ; if EQ then CMN X0 X1 else flags=nzcv
6.9 Basic Load and Store

LDR   X0, [X1]             ; Load 64 bit from address in X1
STR   X0, [X1]             ; Store 64 bit to address in X1
LDR   W0, [X1]             ; Load 32 bit
STR   W0, [X1]             ; Store 32 bit
LDRB  W0, [X1]             ; Load byte 8 bit zero extend
LDRH  W0, [X1]             ; Load halfword 16 bit zero extend
LDRSB X0, [X1]             ; Load signed byte sign extend to 64
LDRSH X0, [X1]             ; Load signed halfword sign extend to 64
LDRSW X0, [X1]             ; Load signed word sign extend to 64
STRB  W0, [X1]             ; Store byte
STRH  W0, [X1]             ; Store halfword
6.10 Addressing Modes

LDR   X0, [X1, #8]         ; Base + Immediate Offset
LDR   X0, [X1, #8]!        ; Pre index X1 += 8 then load
LDR   X0, [X1], #8         ; Post index load then X1 += 8
LDR   X0, [X1, X2]         ; Register Offset
LDR   X0, [X1, X2, LSL #3] ; Scaled Register Offset
LDR   X0, [X1, W2, SXTW]  ; Sign extended Word Offset
LDR   X0, [X1, W2, UXTW #3] ; Zero extended and scaled offset
LDR   X0, label            ; PC relative literal pool
6.11 Pair Load and Store

LDP   X0, X1, [X2]         ; Load pair of 64 bit registers
STP   X0, X1, [X2]         ; Store pair of 64 bit registers
LDP   X0, X1, [X2, #16]!  ; Pre index pair load
LDP   X0, X1, [X2], #16   ; Post index pair load
LDNP  X0, X1, [X2]         ; Non temporal pair load no cache alloc
STNP  X0, X1, [X2]         ; Non temporal pair store no cache alloc
LDPSW X0, X1, [X2]         ; Load pair signed word
6.12 Exclusive Access

LDXR  X0, [X1]             ; Load Exclusive
STXR  W0, X1, [X2]         ; Store Exclusive W0=0 success 1 fail
LDXP  X0, X1, [X2]         ; Load Exclusive Pair
STXP  W0, X1, X2, [X3]    ; Store Exclusive Pair
LDAXR X0, [X1]             ; Load Acquire Exclusive
STLXR W0, X1, [X2]         ; Store Release Exclusive
CLREX                       ; Clear Exclusive Monitor
6.13 Acquire and Release Semantics

LDAR  X0, [X1]             ; Load Acquire
STLR  X0, [X1]             ; Store Release
LDAPR X0, [X1]             ; Load AcquirePC ARMv8.3
6.14 Atomic Operations ARMv8.1 LSE

LDADD   X0, X1, [X2]       ; Atomic add [X2] += X0 X1 = old value
LDADDAL X0, X1, [X2]       ; Atomic Add Acquire Release
LDADDA  X0, X1, [X2]       ; Atomic Add Acquire
LDADDL  X0, X1, [X2]       ; Atomic Add Release
LDSET   X0, X1, [X2]       ; Atomic OR Set bits
LDCLR   X0, X1, [X2]       ; Atomic AND NOT Clear bits
LDEOR   X0, X1, [X2]       ; Atomic XOR
LDSMAX  X0, X1, [X2]       ; Atomic Signed Maximum
LDSMIN  X0, X1, [X2]       ; Atomic Signed Minimum
LDUMAX  X0, X1, [X2]       ; Atomic Unsigned Maximum
LDUMIN  X0, X1, [X2]       ; Atomic Unsigned Minimum
CAS     X0, X1, [X2]       ; Compare and Swap
CASAL   X0, X1, [X2]       ; CAS Acquire Release
CASP    X0, X1, X2, X3, [X4] ; CAS Pair
SWP     X0, X1, [X2]       ; Swap
SWPAL   X0, X1, [X2]       ; Swap Acquire Release
STADD   X0, [X1]           ; Atomic Add no return
6.15 Unconditional Branches

B     label                 ; Branch PC relative +/- 128MB
BL    label                 ; Branch with Link saves return addr to X30
BR    X0                    ; Branch to Register indirect
BLR   X0                    ; Branch with Link to Register
RET                         ; Return BR X30
RET   X5                    ; Return to address in X5
6.16 Conditional Branches

B.EQ  label                 ; Branch if Equal Z=1
B.NE  label                 ; Branch if Not Equal Z=0
B.CS  label                 ; Branch if Carry Set C=1 same as B.HS
B.CC  label                 ; Branch if Carry Clear C=0 same as B.LO
B.MI  label                 ; Branch if Minus Negative N=1
B.PL  label                 ; Branch if Plus Positive N=0
B.VS  label                 ; Branch if Overflow Set V=1
B.VC  label                 ; Branch if Overflow Clear V=0
B.HI  label                 ; Branch if Higher unsigned greater than
B.LS  label                 ; Branch if Lower or Same unsigned <=
B.GE  label                 ; Branch if Greater or Equal signed >=
B.LT  label                 ; Branch if Less Than signed <
B.GT  label                 ; Branch if Greater Than signed >
B.LE  label                 ; Branch if Less or Equal signed <=
B.AL  label                 ; Branch Always
6.17 Compare and Branch

CBZ   X0, label             ; Compare and Branch if Zero
CBNZ  X0, label             ; Compare and Branch if Not Zero
TBZ   X0, #5, label         ; Test Bit 5 and Branch if Zero
TBNZ  X0, #5, label         ; Test Bit 5 and Branch if Not Zero
6.18 Exception Generation

SVC   #imm16                ; Supervisor Call EL0 to EL1 syscall
HVC   #imm16                ; Hypervisor Call EL1 to EL2
SMC   #imm16                ; Secure Monitor Call to EL3
BRK   #imm16                ; Breakpoint debug exception
HLT   #imm16                ; Halt debug halt
ERET                         ; Exception Return
6.19 System Register Access

MRS   X0, SCTLR_EL1        ; Move from System Register to GP
MSR   SCTLR_EL1, X0        ; Move from GP to System Register
MRS   X0, MPIDR_EL1        ; Read Multiprocessor Affinity Register
MRS   X0, CurrentEL         ; Read Current Exception Level
MRS   X0, DAIF             ; Read interrupt mask flags
MSR   DAIFSet, #0xF        ; Set all interrupt masks
MSR   DAIFClr, #0xF        ; Clear all interrupt masks
MRS   X0, NZCV             ; Read condition flags
MSR   NZCV, X0             ; Write condition flags
6.20 Memory Barriers

DMB   ISH                   ; Data Memory Barrier Inner Shareable
DMB   ISHLD                 ; DMB Load only Inner Shareable
DMB   ISHST                 ; DMB Store only Inner Shareable
DMB   OSH                   ; Data Memory Barrier Outer Shareable
DMB   SY                    ; Data Memory Barrier Full System
DSB   ISH                   ; Data Synchronization Barrier
DSB   SY                    ; Data Synchronization Barrier Full
ISB                         ; Instruction Synchronization Barrier
6.21 Cache Maintenance

DC    CIVAC, X0             ; Clean and Invalidate D Cache by VA to PoC
DC    CVAC, X0              ; Clean D Cache by VA to PoC
DC    CVAU, X0              ; Clean D Cache by VA to PoU
DC    IVAC, X0              ; Invalidate D Cache by VA
DC    ZVA, X0               ; Zero by VA zero entire cache line
DC    CISW, X0              ; Clean and Invalidate by Set Way
DC    CSW, X0               ; Clean by Set Way
DC    ISW, X0               ; Invalidate by Set Way
IC    IALLU                  ; Invalidate All I Cache to PoU
IC    IALLUIS                ; Invalidate All IC to PoU Inner Shareable
IC    IVAU, X0              ; Invalidate IC by VA to PoU
6.22 TLB Maintenance

TLBI  ALLE1                  ; TLB Invalidate All EL1
TLBI  ALLE1IS                ; TLB Invalidate All EL1 Inner Shareable
TLBI  VMALLE1IS              ; TLB Invalidate All EL1 current VMID IS
TLBI  VAE1IS, X0            ; TLB Invalidate by VA EL1 IS
TLBI  ASIDE1IS, X0          ; TLB Invalidate by ASID EL1 IS
TLBI  VAAE1IS, X0           ; TLB Invalidate by VA All ASIDs EL1 IS
TLBI  ALLE2IS                ; TLB Invalidate All EL2 IS
6.23 Address Translation

AT    S1E1R, X0             ; Address Translate Stage 1 EL1 Read
AT    S1E1W, X0             ; Address Translate Stage 1 EL1 Write
AT    S1E0R, X0             ; Address Translate Stage 1 EL0 Read
AT    S12E1R, X0            ; Address Translate Stage 1+2 EL1 Read
6.24 Hint Instructions

NOP                         ; No Operation
WFI                         ; Wait For Interrupt low power sleep
WFE                         ; Wait For Event
SEV                         ; Send Event wake up WFE
SEVL                        ; Send Event Local
YIELD                       ; Yield hint to scheduler or SMT
CLREX                       ; Clear Exclusive Monitor


