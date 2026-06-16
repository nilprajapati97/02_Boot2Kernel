
================================================================================
                    ARMv8 ARCHITECTURE - COMPLETE REFERENCE GUIDE
================================================================================

Author: ARMv8 Reference Documentation
Version: 1.0
Last Updated: 2024
Description: A comprehensive guide to ARMv8 (AArch64) architecture covering
             execution states, registers, exception levels, memory management,
             SIMD/NEON, virtualization, security extensions, and more.

================================================================================
                            TABLE OF CONTENTS
================================================================================

  1.  Overview
  2.  Execution States
  3.  Register File
  4.  Exception Levels
  5.  TrustZone Security
  6.  Instruction Set Architecture (A64)
  7.  NEON/SIMD and Floating Point
  8.  Memory Management
  9.  Virtualization Support
  10. Cryptographic Extensions
  11. Exception and Interrupt Handling
  12. GIC (Generic Interrupt Controller) v3/v4
  13. ARMv8 Sub-Versions and Extensions
  14. LSE Atomics (ARMv8.1)
  15. Pointer Authentication (ARMv8.3)
  16. Memory Tagging Extension (ARMv8.5)
  17. SVE - Scalable Vector Extension (ARMv8.2)
  18. Debug Architecture
  19. ARMv8 vs ARMv7 Comparison
  20. Notable ARMv8 Processor Implementations
  21. ARMv8 Boot Sequence
  22. System Control Registers
  23. Cache Architecture
  24. Power Management
  25. Programming Examples
  26. Quick Reference Summary
  27. Useful Resources

================================================================================
  1. OVERVIEW
================================================================================

  Released:           October 2011
  Developer:          ARM Holdings (now Arm Ltd.)
  Key Change:         First ARM architecture with 64-bit support
  Instruction Set:    AArch64 (64-bit) + AArch32 (32-bit backward compatible)
  Used In:            Smartphones, Servers, Embedded Systems, HPC, IoT
  Notable Processors: Apple M1/M2, Qualcomm Snapdragon, AWS Graviton,
                      Cortex-A53/A57/A72/A76/A78/X1

  Architecture Profiles:
  +-----------+------------------+--------------------------------------+
  | Profile   | Name             | Target                               |
  +-----------+------------------+--------------------------------------+
  | ARMv8-A   | Application      | Smartphones, Servers, PCs            |
  | ARMv8-R   | Real-time        | Automotive, Industrial, Storage      |
  | ARMv8-M   | Microcontroller  | IoT, Embedded, Sensors               |
  +-----------+------------------+--------------------------------------+

================================================================================
  2. EXECUTION STATES
================================================================================

  ARMv8 introduces two execution states:

  +------------------------------------------+
  |              ARMv8 Architecture          |
  |                                          |
  |   +------------+     +------------+      |
  |   |  AArch64   |     |  AArch32   |      |
  |   |  (64-bit)  |     |  (32-bit)  |      |
  |   |            |     |            |      |
  |   |  A64 ISA   |     | A32 + T32  |      |
  |   |            |     |   ISA      |      |
  |   +------------+     +------------+      |
  |                                          |
  +------------------------------------------+

  +---------------------+-----------------------+-----------------------+
  | Feature             | AArch64               | AArch32               |
  +---------------------+-----------------------+-----------------------+
  | Register Width      | 64-bit                | 32-bit                |
  | Instruction Set     | A64                   | A32 (ARM) + T32       |
  | GP Registers        | 31 (X0-X30)           | 15 (R0-R14)           |
  | PC Access           | Not directly accesible| Directly accessible   |
  | Address Space       | 64-bit virtual        | 32-bit virtual        |
  | Backward Compatible | New                   | ARMv7 compatible      |
  | Condition Codes     | Cond branch/select    | IT blocks/predicated  |
  | Instruction Width   | Fixed 32-bit          | 32-bit(A32)/16-bit(T) |
  +---------------------+-----------------------+-----------------------+

  State Switching Rules:
  -----------------------------------------------------------------------
  - Lower EL can be AArch32 if higher EL is AArch64
  - Lower EL CANNOT be AArch64 if higher EL is AArch32
  - State change only happens at exception level change
  - AArch64 -> AArch32: On exception return to lower EL
  - AArch32 -> AArch64: On exception entry to higher EL
  -----------------------------------------------------------------------

================================================================================
  3. REGISTER FILE
================================================================================

  3.1 GENERAL PURPOSE REGISTERS
  -----------------------------------------------------------------------

  General Purpose Registers (64-bit):
  +--------+--------------------------------+
  | X0     | Parameter/Result Register      |
  | X1     | Parameter/Result Register      |
  | ...    | ...                            |
  | X7     | Parameter Register             |
  | X8     | Indirect Result Register       |
  | X9     | Temporary Register             |
  | ...    | ...                            |
  | X15    | Temporary Register             |
  | X16    | IP0 (Intra-procedure call)     |
  | X17    | IP1 (Intra-procedure call)     |
  | X18    | Platform Register              |
  | X19    | Callee-saved Register          |
  | ...    | ...                            |
  | X28    | Callee-saved Register          |
  | X29    | Frame Pointer (FP)             |
  | X30    | Link Register (LR)             |
  | SP     | Stack Pointer                  |
  | PC     | Program Counter (not GP)       |
  | XZR    | Zero Register                  |
  +--------+--------------------------------+

  Lower 32-bit access: W0-W30 (maps to X0-X30)

    63                 31                0
    +------------------+-----------------+
    |    Upper 32-bit  |   W0 (32-bit)  |  = X0 (64-bit)
    +------------------+-----------------+

  +------------------+-------+----------+------------------------+
  | Register Type    | Count | Size     | Purpose                |
  +------------------+-------+----------+------------------------+
  | General Purpose  | 31    | 64-bit   | Data operations        |
  | Zero Register    | 1     | 64/32-bit| Hardwired zero         |
  | Stack Pointer    | 1     | 64-bit   | Stack management       |
  | Program Counter  | 1     | 64-bit   | Instruction address    |
  | SIMD/FP          | 32    | 128-bit  | Float point and NEON   |
  | System Registers | Many  | Various  | Control and status     |
  +------------------+-------+----------+------------------------+

  3.2 AAPCS64 CALLING CONVENTION
  -----------------------------------------------------------------------

  +----------+--------------------------------+-----------+
  | Register | Role                           | Saved By  |
  +----------+--------------------------------+-----------+
  | X0-X7    | Arguments / Return values      | Caller    |
  | X8       | Indirect result location       | Caller    |
  | X9-X15   | Temporary (scratch)            | Caller    |
  | X16-X17  | Intra-procedure-call (linker)  | Caller    |
  | X18      | Platform register              | Platform  |
  | X19-X28  | Callee-saved                   | Callee    |
  | X29      | Frame pointer                  | Callee    |
  | X30      | Link register                  | Callee    |
  | SP       | Stack pointer                  | N/A       |
  +----------+--------------------------------+-----------+

  3.3 SPECIAL REGISTERS
  -----------------------------------------------------------------------

  +-------------+---------------------------------------------+
  | Register    | Purpose                                     |
  +-------------+---------------------------------------------+
  | SP_EL0      | Stack Pointer for EL0                       |
  | SP_EL1      | Stack Pointer for EL1                       |
  | SP_EL2      | Stack Pointer for EL2                       |
  | SP_EL3      | Stack Pointer for EL3                       |
  | ELR_EL1     | Exception Link Register for EL1             |
  | ELR_EL2     | Exception Link Register for EL2             |
  | ELR_EL3     | Exception Link Register for EL3             |
  | SPSR_EL1    | Saved Program Status Register for EL1       |
  | SPSR_EL2    | Saved Program Status Register for EL2       |
  | SPSR_EL3    | Saved Program Status Register for EL3       |
  | VBAR_EL1    | Vector Base Address Register for EL1        |
  | VBAR_EL2    | Vector Base Address Register for EL2        |
  | VBAR_EL3    | Vector Base Address Register for EL3        |
  +-------------+---------------------------------------------+

  3.4 PROCESS STATE (PSTATE)
  -----------------------------------------------------------------------

  PSTATE (Process State):
  +----+----+----+----+------+------+-----+-----+------+------+
  | N  | Z  | C  | V  |  D   |  A   |  I  |  F  | nRW  |  EL  |
  |Flag|Flag|Flag|Flag|Debug |SError| IRQ | FIQ |State |Level |
  +----+----+----+----+------+------+-----+-----+------+------+

  +------+-----------+-------------------------------------------+
  | Flag | Name      | Description                               |
  +------+-----------+-------------------------------------------+
  | N    | Negative  | Result is negative                        |
  | Z    | Zero      | Result is zero                            |
  | C    | Carry     | Carry/borrow occurred                     |
  | V    | Overflow  | Signed overflow occurred                  |
  | D    | Debug     | Debug exception mask                      |
  | A    | SError    | SError interrupt mask                     |
  | I    | IRQ       | IRQ interrupt mask                        |
  | F    | FIQ       | FIQ interrupt mask                        |
  | nRW  | Exec State| 0 = AArch64, 1 = AArch32                  |
  | EL   | Exc Level | Current exception level (0-3)             |
  | SP   | Stack Ptr | 0 = SP_EL0, 1 = SP_ELn                    |
  +------+-----------+-------------------------------------------+

================================================================================
  4. EXCEPTION LEVELS
================================================================================

  +----------------------------------------------+
  |                                              |
  |   EL3 ---- Secure Monitor (Firmware/TZ)      |  Highest Privilege
  |     |                                        |
  |   EL2 ---- Hypervisor (Virtualization)       |
  |     |                                        |
  |   EL1 ---- OS Kernel (Supervisor)            |
  |     |                                        |
  |   EL0 ---- User Applications                 |  Lowest Privilege
  |                                              |
  +----------------------------------------------+

  +------------------+----------------------+-----------------------+-----------+
  | Exception Level  | Purpose              | Example               | Privilege |
  +------------------+----------------------+-----------------------+-----------+
  | EL0              | User/Application     | Apps, Programs        | Lowest    |
  | EL1              | OS Kernel            | Linux, Windows, RTOS  | Privileged|
  | EL2              | Hypervisor           | KVM, Xen, VMware      | Higher    |
  | EL3              | Secure Monitor       | ARM TF, TrustZone     | Highest   |
  +------------------+----------------------+-----------------------+-----------+

  Exception Level Rules:
  -----------------------------------------------------------------------
  - Exception Entry:    Always moves to SAME or HIGHER EL
  - Exception Return:   Always moves to SAME or LOWER EL
  - Per-EL Registers:   Each EL has own SP, SPSR, ELR
  - State Switch:       AArch32/64 switch can happen at EL change
  - Security:           Secure/Non-Secure controlled by EL3
  - Optional Levels:    EL2 and EL3 are implementation-defined
  -----------------------------------------------------------------------

  Exception Level Register Access:
  -----------------------------------------------------------------------
               EL0    EL1    EL2    EL3
  EL0 regs:    RW     RW     RW     RW
  EL1 regs:    --     RW     RW     RW
  EL2 regs:    --     --     RW     RW
  EL3 regs:    --     --     --     RW
  -----------------------------------------------------------------------

================================================================================
  5. TRUSTZONE SECURITY
================================================================================

  +--------------------------------------------------+
  |                Physical Processor                 |
  |                                                   |
  |  +-------------------+  +-------------------+     |
  |  |   Normal World    |  |   Secure World    |     |
  |  |                   |  |                   |     |
  |  |  EL0: Normal App  |  |  EL0: Trusted App |     |
  |  |  EL1: Normal OS   |  |  EL1: Secure OS   |     |
  |  |  EL2: Hypervisor  |  |  (S-EL2: v8.4)    |     |
  |  |                   |  |                   |     |
  |  +---------+---------+  +---------+---------+     |
  |            |                      |               |
  |            +--------+  +---------+                |
  |                     v  v                          |
  |              +---------------+                    |
  |              |     EL3       |                    |
  |              |Secure Monitor |                    |
  |              +---------------+                    |
  +---------------------------------------------------+

  +-----------------------+----------------------------------------------+
  | Feature               | Details                                      |
  +-----------------------+----------------------------------------------+
  | Hardware Isolation    | Separate secure and normal worlds            |
  | Memory Protection     | Secure memory not accessible from normal     |
  | Peripheral Protection | Secure peripherals isolated via TZPC/TZASC   |
  | Use Cases             | DRM, Payments, Biometrics, Key Storage       |
  | Transition            | Via SMC (Secure Monitor Call) instruction    |
  | Bus Signal            | NS bit on AXI bus determines access          |
  +-----------------------+----------------------------------------------+

  TrustZone Components:
  +-----------+--------------------------------------------------------+
  | Component | Purpose                                                |
  +-----------+--------------------------------------------------------+
  | TZASC     | TrustZone Address Space Controller - memory partition  |
  | TZPC      | TrustZone Protection Controller - peripheral protect   |
  | TZMA      | TrustZone Memory Adapter - on-chip ROM/RAM protection  |
  | GIC Sec   | Interrupt groups (Group 0=Secure, Group 1=Non-secure)  |
  +-----------+--------------------------------------------------------+

================================================================================
  6. INSTRUCTION SET ARCHITECTURE (A64)
================================================================================

  All A64 instructions are fixed 32-bit width:

  31                                              0
  +--------+--------+--------+--------+-----------+
  |  op0   |  op1   |   Rd   |   Rn   |  Operand  |
  +--------+--------+--------+--------+-----------+

  6.1 DATA PROCESSING INSTRUCTIONS
  -----------------------------------------------------------------------

  ;; Arithmetic Operations
  ADD   X0, X1, X2           ; X0 = X1 + X2
  ADDS  X0, X1, X2           ; X0 = X1 + X2 (set flags)
  SUB   X0, X1, X2           ; X0 = X1 - X2
  SUBS  X0, X1, X2           ; X0 = X1 - X2 (set flags)
  MUL   X0, X1, X2           ; X0 = X1 * X2
  SDIV  X0, X1, X2           ; X0 = X1 / X2 (signed)
  UDIV  X0, X1, X2           ; X0 = X1 / X2 (unsigned)
  MADD  X0, X1, X2, X3       ; X0 = X3 + (X1 * X2)
  MSUB  X0, X1, X2, X3       ; X0 = X3 - (X1 * X2)
  SMADDL X0, W1, W2, X3      ; X0 = X3 + (W1 * W2) signed long
  UMADDL X0, W1, W2, X3      ; X0 = X3 + (W1 * W2) unsigned long
  SMULH X0, X1, X2           ; X0 = (X1 * X2) >> 64 (signed high)
  UMULH X0, X1, X2           ; X0 = (X1 * X2) >> 64 (unsigned high)
  NEG   X0, X1               ; X0 = -X1
  ADC   X0, X1, X2           ; X0 = X1 + X2 + Carry
  SBC   X0, X1, X2           ; X0 = X1 - X2 - !Carry

  ;; Logical Operations
  AND   X0, X1, X2           ; Bitwise AND
  ANDS  X0, X1, X2           ; Bitwise AND (set flags)
  ORR   X0, X1, X2           ; Bitwise OR
  EOR   X0, X1, X2           ; Bitwise XOR
  BIC   X0, X1, X2           ; Bit Clear (AND NOT)
  ORN   X0, X1, X2           ; OR NOT
  EON   X0, X1, X2           ; XOR NOT
  MVN   X0, X1               ; Bitwise NOT

  ;; Shift Operations
  LSL   X0, X1, #4           ; Logical Shift Left
  LSR   X0, X1, #4           ; Logical Shift Right
  ASR   X0, X1, #4           ; Arithmetic Shift Right
  ROR   X0, X1, #4           ; Rotate Right
  LSLV  X0, X1, X2           ; LSL by register value
  LSRV  X0, X1, X2           ; LSR by register value
  ASRV  X0, X1, X2           ; ASR by register value
  RORV  X0, X1, X2           ; ROR by register value

  ;; Bit Manipulation
  CLZ   X0, X1               ; Count Leading Zeros
  CLS   X0, X1               ; Count Leading Sign bits
  RBIT  X0, X1               ; Reverse Bits
  REV   X0, X1               ; Reverse Bytes (64-bit)
  REV16 X0, X1               ; Reverse Bytes in 16-bit halfwords
  REV32 X0, X1               ; Reverse Bytes in 32-bit words
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

  ;; Comparison Operations
  CMP   X0, X1               ; Compare (SUBS + discard result)
  CMN   X0, X1               ; Compare Negative (ADDS + discard)
  TST   X0, X1               ; Test bits (ANDS + discard result)

  ;; Move Operations
  MOV   X0, X1               ; Move register
  MOV   X0, #0x1234          ; Move immediate
  MOVZ  X0, #0x5678          ; Move with zero (clear other bits)
  MOVK  X0, #0x9ABC, LSL #16 ; Move with keep (preserve other bits)
  MOVN  X0, #0x1234          ; Move NOT (bitwise invert of immediate)

  ;; Address Calculation
  ADR   X0, label            ; Form PC-relative address (+/-1MB)
  ADRP  X0, label            ; Form PC-relative page address (+/-4GB)

  ;; Conditional Operations
  CSEL  X0, X1, X2, EQ       ; X0 = (EQ) ? X1 : X2
  CSINC X0, X1, X2, NE       ; X0 = (NE) ? X1 : X2+1
  CSINV X0, X1, X2, GE       ; X0 = (GE) ? X1 : ~X2
  CSNEG X0, X1, X2, LT       ; X0 = (LT) ? X1 : -X2
  CSET  X0, EQ               ; X0 = (EQ) ? 1 : 0
  CSETM X0, EQ               ; X0 = (EQ) ? -1 : 0
  CINC  X0, X1, EQ           ; X0 = (EQ) ? X1+1 : X1
  CNEG  X0, X1, EQ           ; X0 = (EQ) ? -X1 : X1
  CCMP  X0, X1, #nzcv, EQ    ; if (EQ) CMP X0,X1 else flags=nzcv
  CCMN  X0, X1, #nzcv, EQ    ; if (EQ) CMN X0,X1 else flags=nzcv

  6.2 MEMORY ACCESS INSTRUCTIONS
  -----------------------------------------------------------------------

  ;; Basic Load/Store
  LDR   X0, [X1]             ; Load 64-bit from [X1]
  STR   X0, [X1]             ; Store 64-bit to [X1]
  LDR   W0, [X1]             ; Load 32-bit
  STR   W0, [X1]             ; Store 32-bit
  LDRB  W0, [X1]             ; Load byte (8-bit, zero-extend)
  LDRH  W0, [X1]             ; Load halfword (16-bit, zero-extend)
  LDRSB X0, [X1]             ; Load signed byte (sign-extend to 64)
  LDRSH X0, [X1]             ; Load signed halfword (sign-extend to 64)
  LDRSW X0, [X1]             ; Load signed word (sign-extend to 64)
  STRB  W0, [X1]             ; Store byte
  STRH  W0, [X1]             ; Store halfword

  ;; Addressing Modes
  LDR   X0, [X1, #8]         ; Base + Immediate Offset
  LDR   X0, [X1, #8]!        ; Pre-index: X1 += 8, then load
  LDR   X0, [X1], #8         ; Post-index: load, then X1 += 8
  LDR   X0, [X1, X2]         ; Register Offset
  LDR   X0, [X1, X2, LSL #3] ; Scaled Register Offset
  LDR   X0, [X1, W2, SXTW]  ; Sign-extended Word Offset
  LDR   X0, [X1, W2, UXTW #3] ; Zero-extended + scaled offset
  LDR   X0, label            ; PC-relative (literal pool)

  ;; Pair Load/Store
  LDP   X0, X1, [X2]         ; Load pair of 64-bit registers
  STP   X0, X1, [X2]         ; Store pair of 64-bit registers
  LDP   X0, X1, [X2, #16]!  ; Pre-index pair load
  LDP   X0, X1, [X2], #16   ; Post-index pair load
  LDNP  X0, X1, [X2]         ; Non-temporal pair load (no cache alloc)
  STNP  X0, X1, [X2]         ; Non-temporal pair store (no cache alloc)
  LDPSW X0, X1, [X2]         ; Load pair signed word

  ;; Exclusive Access (for atomics before LSE)
  LDXR  X0, [X1]             ; Load Exclusive
  STXR  W0, X1, [X2]         ; Store Exclusive (W0=0 success, 1 fail)
  LDXP  X0, X1, [X2]         ; Load Exclusive Pair
  STXP  W0, X1, X2, [X3]    ; Store Exclusive Pair
  LDAXR X0, [X1]             ; Load-Acquire Exclusive
  STLXR W0, X1, [X2]         ; Store-Release Exclusive
  CLREX                       ; Clear Exclusive Monitor

  ;; Acquire/Release Semantics
  LDAR  X0, [X1]             ; Load-Acquire
  STLR  X0, [X1]             ; Store-Release
  LDAPR X0, [X1]             ; Load-AcquirePC (ARMv8.3)

  ;; Atomic Operations (ARMv8.1 LSE)
  LDADD   X0, X1, [X2]       ; Atomic: [X2] += X0, X1 = old value
  LDADDAL X0, X1, [X2]       ; Atomic Add (Acquire-Release)
  LDADDA  X0, X1, [X2]       ; Atomic Add (Acquire)
  LDADDL  X0, X1, [X2]       ; Atomic Add (Release)
  LDSET   X0, X1, [X2]       ; Atomic OR (Set bits)
  LDCLR   X0, X1, [X2]       ; Atomic AND NOT (Clear bits)
  LDEOR   X0, X1, [X2]       ; Atomic XOR
  LDSMAX  X0, X1, [X2]       ; Atomic Signed Maximum
  LDSMIN  X0, X1, [X2]       ; Atomic Signed Minimum
  LDUMAX  X0, X1, [X2]       ; Atomic Unsigned Maximum
  LDUMIN  X0, X1, [X2]       ; Atomic Unsigned Minimum
  CAS     X0, X1, [X2]       ; Compare and Swap
  CASAL   X0, X1, [X2]       ; CAS (Acquire-Release)
  CASP    X0, X1, X2, X3, [X4] ; CAS Pair
  SWP     X0, X1, [X2]       ; Swap
  SWPAL   X0, X1, [X2]       ; Swap (Acquire-Release)
  STADD   X0, [X1]           ; Atomic Add (no return)

  6.3 BRANCH INSTRUCTIONS
  -----------------------------------------------------------------------

  ;; Unconditional Branches
  B     label                 ; Branch (PC-relative +/-128MB)
  BL    label                 ; Branch with Link (saves to X30)
  BR    X0                    ; Branch to Register (indirect)
  BLR   X0                    ; Branch with Link to Register
  RET                         ; Return (BR X30)
  RET   X5                    ; Return to address in X5

  ;; Conditional Branches
  B.EQ  label                 ; Branch if Equal (Z=1)
  B.NE  label                 ; Branch if Not Equal (Z=0)
  B.CS  label                 ; Branch if Carry Set (C=1) / B.HS
  B.CC  label                 ; Branch if Carry Clear (C=0) / B.LO
  B.MI  label                 ; Branch if Minus/Negative (N=1)
  B.PL  label                 ; Branch if Plus/Positive (N=0)
  B.VS  label                 ; Branch if Overflow Set (V=1)
  B.VC  label                 ; Branch if Overflow Clear (V=0)
  B.HI  label                 ; Branch if Higher (unsigned >)
  B.LS  label                 ; Branch if Lower or Same (unsigned <=)
  B.GE  label                 ; Branch if Greater or Equal (signed >=)
  B.LT  label                 ; Branch if Less Than (signed <)
  B.GT  label                 ; Branch if Greater Than (signed >)
  B.LE  label                 ; Branch if Less or Equal (signed <=)
  B.AL  label                 ; Branch Always

  ;; Compare and Branch
  CBZ   X0, label             ; Compare and Branch if Zero
  CBNZ  X0, label             ; Compare and Branch if Not Zero
  TBZ   X0, #5, label         ; Test Bit #5 and Branch if Zero
  TBNZ  X0, #5, label         ; Test Bit #5 and Branch if Not Zero

  6.4 SYSTEM INSTRUCTIONS
  -----------------------------------------------------------------------

  ;; Exception Generation
  SVC   #imm16                ; Supervisor Call (EL0 -> EL1, syscall)
  HVC   #imm16                ; Hypervisor Call (EL1 -> EL2)
  SMC   #imm16                ; Secure Monitor Call (-> EL3)
  BRK   #imm16                ; Breakpoint (debug exception)
  HLT   #imm16                ; Halt (debug halt)
  ERET                         ; Exception Return

  ;; System Register Access
  MRS   X0, SCTLR_EL1        ; Move from System Register to GP
  MSR   SCTLR_EL1, X0        ; Move from GP to System Register
  MRS   X0, MPIDR_EL1        ; Read Multiprocessor Affinity Register
  MRS   X0, CurrentEL         ; Read Current Exception Level
  MRS   X0, DAIF             ; Read interrupt mask flags
  MSR   DAIFSet, #0xF        ; Set all interrupt masks
  MSR   DAIFClr, #0xF        ; Clear all interrupt masks
  MRS   X0, NZCV             ; Read condition flags
  MSR   NZCV, X0             ; Write condition flags

  ;; Memory Barriers
  DMB   ISH                   ; Data Memory Barrier (Inner Shareable)
  DMB   ISHLD                 ; DMB - Load only (Inner Shareable)
  DMB   ISHST                 ; DMB - Store only (Inner Shareable)
  DMB   OSH                   ; Data Memory Barrier (Outer Shareable)
  DMB   SY                    ; Data Memory Barrier (Full System)
  DSB   ISH                   ; Data Synchronization Barrier
  DSB   SY                    ; Data Synchronization Barrier (Full)
  ISB                         ; Instruction Synchronization Barrier

  ;; Cache Maintenance
  DC    CIVAC, X0             ; Clean and Invalidate D-Cache by VA to PoC
  DC    CVAC, X0              ; Clean D-Cache by VA to PoC
  DC    CVAU, X0              ; Clean D-Cache by VA to PoU
  DC    IVAC, X0              ; Invalidate D-Cache by VA
  DC    ZVA, X0               ; Zero by VA (zero entire cache line)
  DC    CISW, X0              ; Clean and Invalidate by Set/Way
  DC    CSW, X0               ; Clean by Set/Way
  DC    ISW, X0               ; Invalidate by Set/Way
  IC    IALLU                  ; Invalidate All I-Cache to PoU
  IC    IALLUIS                ; Invalidate All IC to PoU (Inner Shareable)
  IC    IVAU, X0              ; Invalidate IC by VA to PoU

  ;; TLB Maintenance
  TLBI  ALLE1                  ; TLB Invalidate All EL1
  TLBI  ALLE1IS                ; TLB Invalidate All EL1 (Inner Shareable)
  TLBI  VMALLE1IS              ; TLB Invalidate All EL1 (current VMID, IS)
  TLBI  VAE1IS, X0            ; TLB Invalidate by VA EL1 (IS)
  TLBI  ASIDE1IS, X0          ; TLB Invalidate by ASID EL1 (IS)
  TLBI  VAAE1IS, X0           ; TLB Invalidate by VA All ASIDs EL1 (IS)
  TLBI  ALLE2IS                ; TLB Invalidate All EL2 (IS)

  ;; Address Translation
  AT    S1E1R, X0             ; Address Translate Stage 1 EL1 Read
  AT    S1E1W, X0             ; Address Translate Stage 1 EL1 Write
  AT    S1E0R, X0             ; Address Translate Stage 1 EL0 Read
  AT    S12E1R, X0            ; Address Translate Stage 1+2 EL1 Read

  ;; Hint Instructions
  NOP                         ; No Operation
  WFI                         ; Wait For Interrupt (low-power sleep)
  WFE                         ; Wait For Event
  SEV                         ; Send Event (wake up WFE)
  SEVL                        ; Send Event Local
  YIELD                       ; Yield (hint to scheduler/SMT)
  CLREX                       ; Clear Exclusive Monitor

================================================================================
  7. NEON/SIMD AND FLOATING POINT
================================================================================

  7.1 SIMD REGISTER FILE
  -----------------------------------------------------------------------

  128-bit SIMD/FP Registers (V0-V31):

  +----------------------------------------------+
  |                V0 (128-bit)                  |
  +-----------------------+----------------------+
  |      D0 (64-bit)      |                      |
  +------------+----------+                      |
  | S0 (32-bit)|          |                      |
  +------+-----+          |                      |
  |H0    |     |          |                      |
  +---+--+     |          |                      |
  |B0 |  |     |          |                      |
  +---+--+-----+----------+----------------------+

  Access Modes:
    Bn  = 8-bit   (Byte)
    Hn  = 16-bit  (Half-precision float / integer)
    Sn  = 32-bit  (Single-precision float / integer)
    Dn  = 64-bit  (Double-precision float / integer)
    Vn  = 128-bit (Quad/Full Vector)

  7.2 SIMD DATA TYPES
  ------------------------------------------------------------------------

  +-------------------+------------------------+--------------------------+
  | Type              | Elements in 64-bit(Dn) | Elements in 128-bit(Vn)  |
  +-------------------+------------------------+--------------------------+
  | 8-bit Integer (B) | 8                      | 16                       |
  | 16-bit Integer(H) | 4                      | 8                        |
  | 32-bit Integer(S) | 2                      | 4                        |
  | 64-bit Integer(D) | 1                      | 2                        |
  | 16-bit Float (H)  | 4                      | 8                        |
  | 32-bit Float (S)  | 2                      | 4                        |
  | 64-bit Float (D)  | 1                      | 2                        |
  +-------------------+------------------------+--------------------------+

  7.3 SIMD INSTRUCTIONS
  -----------------------------------------------------------------------

  ;; Vector Arithmetic
  ADD    V0.4S, V1.4S, V2.4S      ; Add 4x32-bit integers
  SUB    V0.8H, V1.8H, V2.8H      ; Subtract 8x16-bit integers
  MUL    V0.4S, V1.4S, V2.4S      ; Multiply 4x32-bit integers
  MLA    V0.4S, V1.4S, V2.4S      ; Multiply-Accumulate
  MLS    V0.4S, V1.4S, V2.4S      ; Multiply-Subtract
  ABS    V0.4S, V1.4S             ; Absolute value
  NEG    V0.4S, V1.4S             ; Negate

  ;; Floating-Point Vector
  FADD   V0.4S, V1.4S, V2.4S      ; Add 4x32-bit floats
  FSUB   V0.2D, V1.2D, V2.2D      ; Subtract 2x64-bit doubles
  FMUL   V0.4S, V1.4S, V2.4S      ; Multiply 4x32-bit floats
  FDIV   V0.2D, V1.2D, V2.2D      ; Divide 2x64-bit doubles
  FMLA   V0.4S, V1.4S, V2.4S      ; Fused Multiply-Accumulate
  FMLS   V0.4S, V1.4S, V2.4S      ; Fused Multiply-Subtract
  FABS   V0.4S, V1.4S             ; Absolute value (float)
  FNEG   V0.4S, V1.4S             ; Negate (float)
  FSQRT  V0.2D, V1.2D             ; Square root
  FMAX   V0.4S, V1.4S, V2.4S      ; Maximum (float)
  FMIN   V0.4S, V1.4S, V2.4S      ; Minimum (float)
  FRECPE V0.4S, V1.4S             ; Reciprocal Estimate
  FRSQRTE V0.4S, V1.4S            ; Reciprocal Square Root Estimate
  FCVTZS V0.4S, V1.4S             ; Float to Signed Int (toward zero)
  FCVTZU V0.4S, V1.4S             ; Float to Unsigned Int
  SCVTF  V0.4S, V1.4S             ; Signed Int to Float
  UCVTF  V0.4S, V1.4S             ; Unsigned Int to Float

  ;; Scalar Floating-Point
  FADD   S0, S1, S2               ; Single-precision add
  FADD   D0, D1, D2               ; Double-precision add
  FMUL   S0, S1, S2               ; Single-precision multiply
  FDIV   D0, D1, D2               ; Double-precision divide
  FCMP   S0, S1                   ; Compare (set NZCV)
  FCCMP  S0, S1, #nzcv, EQ        ; Conditional compare
  FCSEL  S0, S1, S2, EQ           ; Conditional select
  FMOV   S0, W0                   ; Move GP to FP register
  FMOV   W0, S0                   ; Move FP to GP register
  FMOV   D0, X0                   ; Move GP 64-bit to FP
  FMOV   X0, D0                   ; Move FP to GP 64-bit
  FCVT   D0, S1                   ; Convert single to double
  FCVT   S0, D1                   ; Convert double to single

  ;; Vector Load/Store
  LD1    {V0.4S}, [X0]             ; Load single 4-element structure
  LD1    {V0.4S, V1.4S}, [X0]     ; Load 2 registers
  LD1    {V0.4S-V3.4S}, [X0]      ; Load 4 registers
  ST1    {V0.4S}, [X0]             ; Store single structure
  LD2    {V0.4S, V1.4S}, [X0]     ; Load 2-element (de-interleave)
  LD3    {V0.4S, V1.4S, V2.4S}, [X0]  ; Load 3-element structures
  LD4    {V0.4S-V3.4S}, [X0]      ; Load 4-element structures
  ST2    {V0.4S, V1.4S}, [X0]     ; Store 2-element (interleave)
  LD1R   {V0.4S}, [X0]             ; Load and replicate to all lanes
  LD1    {V0.S}[2], [X0]          ; Load single lane
  ST1    {V0.S}[2], [X0]          ; Store single lane

  ;; Post-index variants
  LD1    {V0.4S}, [X0], #16       ; Load + advance X0 by 16
  LD1    {V0.4S}, [X0], X1        ; Load + advance X0 by X1

  ;; Vector Comparison
  CMEQ   V0.4S, V1.4S, V2.4S      ; Compare Equal
  CMGT   V0.4S, V1.4S, V2.4S      ; Compare Greater Than (signed)
  CMGE   V0.4S, V1.4S, V2.4S      ; Compare Greater or Equal
  CMHI   V0.4S, V1.4S, V2.4S      ; Compare Higher (unsigned >)
  CMHS   V0.4S, V1.4S, V2.4S      ; Compare Higher or Same
  CMTST  V0.4S, V1.4S, V2.4S      ; Compare Test (AND != 0)
  CMEQ   V0.4S, V1.4S, #0         ; Compare Equal to Zero
  FCMEQ  V0.4S, V1.4S, V2.4S      ; Float Compare Equal
  FCMGT  V0.4S, V1.4S, V2.4S      ; Float Compare Greater Than

  ;; Vector Manipulation
  DUP    V0.4S, W0                 ; Duplicate scalar to all lanes
  DUP    V0.4S, V1.S[2]           ; Duplicate element to all lanes
  INS    V0.S[2], W0               ; Insert GP register into lane
  INS    V0.S[2], V1.S[0]         ; Insert element from another vector
  UMOV   W0, V0.S[2]              ; Extract element (unsigned)
  SMOV   X0, V0.H[3]              ; Extract element (signed)
  EXT    V0.16B, V1.16B, V2.16B, #4  ; Extract (concat + shift)
  TRN1   V0.4S, V1.4S, V2.4S      ; Transpose (even elements)
  TRN2   V0.4S, V1.4S, V2.4S      ; Transpose (odd elements)
  ZIP1   V0.4S, V1.4S, V2.4S      ; Interleave (lower half)
  ZIP2   V0.4S, V1.4S, V2.4S      ; Interleave (upper half)
  UZP1   V0.4S, V1.4S, V2.4S      ; De-interleave (even elements)
  UZP2   V0.4S, V1.4S, V2.4S      ; De-interleave (odd elements)
  REV64  V0.4S, V1.4S             ; Reverse elements within 64-bit
  TBL    V0.16B, {V1.16B}, V2.16B ; Table Lookup
  TBX    V0.16B, {V1.16B}, V2.16B ; Table Lookup (extend)

  ;; Widening / Narrowing
  SADDL  V0.4S, V1.4H, V2.4H      ; Signed Add Long (16->32)
  UADDL  V0.4S, V1.4H, V2.4H      ; Unsigned Add Long
  SSUBL  V0.4S, V1.4H, V2.4H      ; Signed Subtract Long
  ADDHN  V0.4H, V1.4S, V2.4S      ; Add and Narrow High (32->16)
  SMULL  V0.4S, V1.4H, V2.4H      ; Signed Multiply Long
  UMULL  V0.4S, V1.4H, V2.4H      ; Unsigned Multiply Long
  SMLAL  V0.4S, V1.4H, V2.4H      ; Signed Multiply-Accumulate Long
  XTN    V0.4H, V1.4S             ; Extract Narrow
  SQXTN  V0.4H, V1.4S             ; Signed Saturating Extract Narrow
  SXTL   V0.4S, V1.4H             ; Sign Extend Long
  UXTL   V0.4S, V1.4H             ; Zero Extend Long

  ;; Reduction Operations
  ADDV   S0, V1.4S                 ; Add across vector (sum all lanes)
  SADDLV D0, V1.4S                 ; Signed Add Long across vector
  SMAXV  S0, V1.4S                 ; Signed Max across vector
  SMINV  S0, V1.4S                 ; Signed Min across vector
  UMAXV  S0, V1.4S                 ; Unsigned Max across vector
  UMINV  S0, V1.4S                 ; Unsigned Min across vector
  FMAXV  S0, V1.4S                 ; Float Max across vector
  FMINV  S0, V1.4S                 ; Float Min across vector
  FADDP  V0.4S, V1.4S, V2.4S      ; Floating-point Add Pairwise

  ;; Saturating Arithmetic
  SQADD  V0.4S, V1.4S, V2.4S      ; Signed Saturating Add
  UQADD  V0.4S, V1.4S, V2.4S      ; Unsigned Saturating Add
  SQSUB  V0.4S, V1.4S, V2.4S      ; Signed Saturating Subtract
  UQSUB  V0.4S, V1.4S, V2.4S      ; Unsigned Saturating Subtract
  SQABS  V0.4S, V1.4S             ; Signed Saturating Absolute
  SQNEG  V0.4S, V1.4S             ; Signed Saturating Negate
  SQDMULH V0.4S, V1.4S, V2.4S     ; Signed Sat Doubling Multiply High

  ;; Crypto Extensions
  AESE   V0.16B, V1.16B            ; AES single round encryption
  AESD   V0.16B, V1.16B            ; AES single round decryption
  AESMC  V0.16B, V1.16B            ; AES mix columns
  AESIMC V0.16B, V1.16B            ; AES inverse mix columns
  SHA1C  Q0, S1, V2.4S             ; SHA1 hash update (choose)
  SHA1P  Q0, S1, V2.4S             ; SHA1 hash update (parity)
  SHA1M  Q0, S1, V2.4S             ; SHA1 hash update (majority)
  SHA1H  S0, S1                    ; SHA1 fixed rotate
  SHA1SU0 V0.4S, V1.4S, V2.4S     ; SHA1 schedule update 0
  SHA1SU1 V0.4S, V1.4S             ; SHA1 schedule update 1
  SHA256H  Q0, Q1, V2.4S           ; SHA256 hash update (part 1)
  SHA256H2 Q0, Q1, V2.4S           ; SHA256 hash update (part 2)
  SHA256SU0 V0.4S, V1.4S           ; SHA256 schedule update 0
  SHA256SU1 V0.4S, V1.4S, V2.4S   ; SHA256 schedule update 1
  PMULL  V0.1Q, V1.1D, V2.1D      ; Polynomial Multiply Long
  PMULL2 V0.1Q, V1.2D, V2.2D      ; Polynomial Multiply Long (upper)

================================================================================
  8. MEMORY MANAGEMENT
================================================================================

  8.1 VIRTUAL ADDRESS SPACE
  -----------------------------------------------------------------------

  AArch64 Virtual Address Space (48-bit used of 64-bit):

  64-bit Virtual Address:
  +---------+--------+--------+--------+--------+-----------+
  | 63..48  | 47-39  | 38-30  | 29-21  | 20-12  |   11-0    |
  | Sign    |  L0    |  L1    |  L2    |  L3    |  Offset   |
  | Extend  | Index  | Index  | Index  | Index  |  (4KB)    |
  +---------+--------+--------+--------+--------+-----------+

  Two Virtual Address Ranges:
  +----------------------------+  0xFFFF_FFFF_FFFF_FFFF
  |     Kernel Space           |  Translated by TTBR1_EL1
  |     (Upper VA range)       |
  +----------------------------+  0xFFFF_0000_0000_0000
  |                            |
  |     Invalid / Fault        |
  |     (VA Hole)              |
  |                            |
  +----------------------------+  0x0000_FFFF_FFFF_FFFF
  |     User Space             |  Translated by TTBR0_EL1
  |     (Lower VA range)       |
  +----------------------------+  0x0000_0000_0000_0000

  8.2 TRANSLATION TABLES
  -----------------------------------------------------------------------

  4-Level Page Table Walk (4KB Granule):

    Virtual Address
         |
         v
    +---------+     +---------+     +---------+     +---------+
    | Level 0 |---->| Level 1 |---->| Level 2 |---->| Level 3 |---> Physical
    | (512GB) |     |  (1GB)  |     |  (2MB)  |     |  (4KB)  |    Page
    +---------+     +---------+     +---------+     +---------+

  Translation Table Descriptor Format (64-bit entry):

    63    55 54  52 51          12 11        2 1  0
    +-------+------+--------------+-----------+----+
    | Upper | Res  | Output Addr  | Lower     |Type|
    | Attrs |      | [51:12]      | Attrs     |    |
    +-------+------+--------------+-----------+----+

  Type bits [1:0]:
    00 = Invalid (fault)
    01 = Block entry (L1/L2 only - large mapping)
    11 = Table entry (L0/L1/L2) or Page entry (L3)
    10 = Reserved

  Block/Page Descriptor Attributes:

  Lower Attributes (bits [11:2]):
    [11]    nG       - Not Global (ASID-specific if set)
    [10]    AF       - Access Flag
    [9:8]   SH       - Shareability (00=Non, 10=Outer, 11=Inner)
    [7:6]   AP       - Access Permissions
    [5]     NS       - Non-Secure (for Secure state)
    [4:2]   AttrIdx  - Index into MAIR_ELn register

  Upper Attributes (bits [63:52]):
    [54]    XN       - Execute Never
    [53]    PXN      - Privileged Execute Never
    [52]    Contiguous - Contiguous hint for TLB

  Access Permissions (AP[2:1]):
    00 = EL1: RW, EL0: None
    01 = EL1: RW, EL0: RW
    10 = EL1: RO, EL0: None
    11 = EL1: RO, EL0: RO

  8.3 PAGE SIZES
  -----------------------------------------------------------------------

  +--------------+-------------+---------+--------------+
  | Granule Size | Levels Used | VA Bits | Max VA Space |
  +--------------+-------------+---------+--------------+
  | 4KB          | 4 (L0-L3)  | 48-bit  | 256 TB        |
  | 16KB         | 4           | 48-bit  | 256 TB       |
  | 64KB         | 3           | 48-bit  | 256 TB       |
  +--------------+-------------+---------+--------------+

  Block Sizes at Each Level:
  +-------+-------------+--------------+--------------+
  | Level | 4KB Granule | 16KB Granule | 64KB Granule |
  +-------+-------------+--------------+--------------+
  | L0    | 512GB(tbl)  | --           | --           |
  | L1    | 1GB Block   | 64GB(tbl)    | 4TB Block    |
  | L2    | 2MB Block   | 32MB Block   | 512MB Block  |
  | L3    | 4KB Page    | 16KB Page    | 64KB Page    |
  +-------+-------------+--------------+--------------+

  8.4 MEMORY ATTRIBUTES
  -----------------------------------------------------------------------

  +------------------+--------+--------------------------------------------+
  | Attribute        | Type   | Description                                |
  +------------------+--------+--------------------------------------------+
  | Normal           | Cache  | Standard RAM - cacheable, bufferable       |
  | Device-nGnRnE   | Device | Strictly ordered, no gather/reorder/early  |
  | Device-nGnRE    | Device | No gather, no reorder, early write ack     |
  | Device-nGRE     | Device | No gather, reorder allowed, early ack      |
  | Device-GRE      | Device | Gathering, reordering, early write ack     |
  +------------------+--------+--------------------------------------------+

  MAIR (Memory Attribute Indirection Register):

    MAIR_EL1 contains 8 attribute entries (8 bits each):

    Common configurations:
      Attr0 = 0x00  -> Device-nGnRnE
      Attr1 = 0x04  -> Device-nGnRE
      Attr2 = 0x44  -> Normal Non-Cacheable
      Attr3 = 0xFF  -> Normal Write-Back Cacheable (Inner + Outer)
      Attr4 = 0xBB  -> Normal Write-Through Cacheable

  Shareability Domains:
  +------------------+-------+----------------------------------------------+
  | Domain           | Value | Scope                                        |
  +------------------+-------+----------------------------------------------+
  | Non-shareable    | 00    | Single core only                             |
  | Reserved         | 01    | --                                           |
  | Outer Shareable  | 10    | Multiple clusters, GPUs, DMA                 |
  | Inner Shareable  | 11    | Cluster of cores (same coherency domain)     |
  +------------------+-------+----------------------------------------------+

  8.5 MEMORY ORDERING AND BARRIERS
  -----------------------------------------------------------------------

  +---------+------------------+----------------------------------------------+
  | Barrier | Instruction      | Purpose                                      |
  +---------+------------------+----------------------------------------------+
  | DMB     | DMB ISH          | Ensures ordering of memory accesses          |
  | DSB     | DSB ISH          | Ensures completion of all memory accesses     |
  | ISB     | ISB              | Flushes pipeline, ensures changes take effect |
  | LDAR    | LDAR X0, [X1]   | Load-Acquire: subsequent accesses after this  |
  | STLR    | STLR X0, [X1]   | Store-Release: prior accesses before this     |
  +---------+------------------+----------------------------------------------+

  Barrier Scope Options:
  +--------+----------------------------------------------+
  | Option | Meaning                                      |
  +--------+----------------------------------------------+
  | SY     | Full system                                  |
  | ISH    | Inner Shareable domain                       |
  | OSH    | Outer Shareable domain                       |
  | NSH    | Non-shareable (local core)                   |
  | LD     | Load operations only                         |
  | ST     | Store operations only                        |
  +--------+----------------------------------------------+

  ASID and VMID:
  -----------------------------------------------------------------------
  Address Space Identifier (ASID):
    - 8-bit or 16-bit (implementation defined)
    - Tags TLB entries per process
    - Avoids TLB flush on context switch
    - Stored in TTBR0_EL1[63:48]

  Virtual Machine Identifier (VMID):
    - 8-bit or 16-bit
    - Tags TLB entries per VM
    - Used by Stage 2 translation
    - Stored in VTTBR_EL2

================================================================================
  9. VIRTUALIZATION SUPPORT
================================================================================

  +----------------------------------------------+
  |              Hardware (EL3)                  |
  +----------------------------------------------+
  |           Hypervisor (EL2)                   |
  |                                              |
  |  +---------------+  +---------------+        |
  |  |    VM 1       |  |    VM 2       |        |
  |  | +-----------+ |  | +-----------+ |        |
  |  | | Guest OS  | |  | | Guest OS  | |        |
  |  | |  (EL1)    | |  | |  (EL1)    | |        |
  |  | +-----------+ |  | +-----------+ |        |
  |  | | Guest App | |  | | Guest App | |        |
  |  | |  (EL0)    | |  | |  (EL0)    | |        |
  |  | +-----------+ |  | +-----------+ |        |
  |  +---------------+  +---------------+        |
  +----------------------------------------------+

  Two-Stage Address Translation:

    Stage 1 (Guest OS controls):
      Guest VA ----------> Guest PA (IPA = Intermediate Physical Address)

    Stage 2 (Hypervisor controls):
      Guest PA (IPA) ----------> Host PA (Physical Address)

    Combined walk:
      Guest VA --[S1]--> IPA --[S2]--> PA

  +---------------------------+----------------------------------------------+
  | Feature                   | Details                                      |
  +---------------------------+----------------------------------------------+
  | Stage 2 Translation      | Hypervisor controls Guest PA -> Host PA      |
  | VMID                     | Virtual Machine ID for TLB tagging           |
  | Trap and Emulate         | Configurable trapping of guest operations    |
  | Virtual Interrupts       | vIRQ, vFIQ, vSError injected by hypervisor   |
  | Virtual Timer            | Dedicated virtual timer for each VM          |
  | VHE (ARMv8.1)            | Virtualization Host Extensions              |
  | Nested Virt (ARMv8.3)    | Support for nested hypervisors              |
  | Secure EL2 (ARMv8.4)     | Virtualization in Secure world              |
  +---------------------------+----------------------------------------------+

  Key Virtualization Registers:
  +------------------+-----------------------------------------------+
  | Register         | Purpose                                       |
  +------------------+-----------------------------------------------+
  | HCR_EL2          | Hypervisor Configuration Register             |
  | VTTBR_EL2        | Virtualization Translation Table Base         |
  | VTCR_EL2         | Virtualization Translation Control            |
  | VMPIDR_EL2       | Virtualization Multiprocessor ID              |
  | VPIDR_EL2        | Virtualization Processor ID                   |
  | ICH_*            | Virtual interrupt controller interface         |
  +------------------+-----------------------------------------------+

================================================================================
  10. CRYPTOGRAPHIC EXTENSIONS
================================================================================

  +-------------------------------------+
  |      ARMv8 Crypto Extensions        |
  |                                     |
  |  +---------+  +---------+           |
  |  |   AES   |  |  SHA-1  |           |
  |  | Encrypt |  |  Hash   |           |
  |  | Decrypt |  |         |           |
  |  +---------+  +---------+           |
  |                                     |
  |  +---------+  +---------+           |
  |  | SHA-256 |  |  PMULL  |           |
  |  |  Hash   |  | (GF Mul)|           |
  |  +---------+  +---------+           |
  +-------------------------------------+

  +-----------+--------------------------------------+-----------------------+
  | Extension | Instructions                         | Use Case              |
  +-----------+--------------------------------------+-----------------------+
  | AES       | AESE, AESD, AESMC, AESIMC            | AES Encrypt/Decrypt   |
  +-----------+--------------------------------------+-----------------------+
