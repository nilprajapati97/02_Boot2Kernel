# UBSAN and Undefined Behavior Sanitizer Deep Dive

Category: Debugging and Profiling Tools  
Platform: ARM64 (AArch64), undefined behavior detection

---

## 1. Concept Foundation

UBSAN (Undefined Behavior Sanitizer) detects undefined behavior in kernel code.

Targets:
- integer overflows
- null pointer dereferences
- out-of-bounds access
- signed integer wraps
- division by zero

---

## 2. ARM64 Hardware Detail

### 2.1 Undefined behavior on ARM64

ARM64 spec leaves many behaviors unspecified:
- integer overflow behavior
- null dereference behavior (trap or silent fail)

UBSAN catches these at compile time via instrumentation.

### 2.2 Instrumentation

Compiler adds checks at potentially problematic operations.
Minimal performance impact if checks are not triggered.

---

## 3. Linux Kernel Implementation

### 3.1 Detection points

Compiler instruments:
- integer arithmetic (overflow checks)
- pointer dereferences (null/alignment checks)
- array access (bounds checks)
- shifts (invalid shift amounts)

### 3.2 Report generation

On undefined behavior detected:
1. collect stack trace
2. log violation details
3. optionally panic or continue

---

## 4. Hardware-Software Interaction

UBSAN catching bug:
1. code does unsigned integer arithmetic that wraps
2. UBSAN hook detects wrap
3. reports violation with line number and values
4. developer investigates and fixes

---

## 5. Interview Q and A

Q1: How is UBSAN different from bounds checkers?
UBSAN is broader; covers undefined behavior beyond just memory access.

Q2: Why is integer overflow a kernel concern?
Overflow bugs can cause security vulnerabilities and memory corruption.

Q3: Can UBSAN catch all undefined behavior?
No; only behavior reachable during testing and instrumented by compiler.

Q4: What is false positive risk for UBSAN?
Low; most reports are real issues, though some may be benign in practice.

Q5: How do you handle UBSAN warnings?
Fix or annotate as intended (unlikely) with checked arithmetic.

Q6: How expensive is UBSAN at runtime?
Minimal if violations not triggered; instrumentation calls are fast.

---

## 6. Pitfalls and Gotchas

- Assuming UBSAN catches all bugs (test coverage dependent).
- Ignoring UBSAN warnings as "not real" (they often are).
- Not updating kernel code to use checked_* arithmetic functions.
- Running UBSAN only in debug builds (should be enabled in CI).

---

## 7. Quick Reference Table

| Violation | Typical cause |
|---|---|
| unsigned integer overflow | wraparound in counter or size calculation |
| signed integer overflow | undefined in C standard |
| null dereference | pointer not checked before use |
| out-of-bounds | array access beyond allocated size |
| division by zero | input not validated |

| Response | Action |
|---|---|
| overflow detected | use saturating or checked arithmetic |
| null dereference | add null check |
| bounds violation | validate index before access |
