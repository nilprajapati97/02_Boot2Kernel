# ARM64 KASLR Deep Dive

Category: KASLR and Kernel Boot Memory Setup  
Platform: ARM64 (AArch64), Linux 6.x

---

## 1. Concept Foundation

KASLR randomizes kernel placement to reduce reliability of code-reuse attacks.

Goal:
- Make fixed gadget addresses unavailable to attackers.
- Increase exploitation cost for memory corruption vulnerabilities.

ARM64 KASLR dimensions:
- Virtual KASLR for kernel image base
- Physical KASLR depending on boot path and platform support
- Module region randomization around kernel image

Limits:
- KASLR is probabilistic hardening, not a complete memory safety fix.

---

## 2. ARM64 Hardware Detail

### 2.1 Address-space layout context

Kernel virtual placement depends on:
- configured VA size
- linear map layout
- kernel image alignment constraints

Randomization must preserve:
- alignment needed for section mappings
- coexistence with fixmap, vmemmap, modules, and vmalloc areas

### 2.2 Entropy sources

Typical entropy sources during early boot:
- EFI RNG protocol where available
- architecture random instructions such as RNDR on supported CPUs
- mixed timing or firmware-provided seeds as fallback

Entropy quality directly impacts practical brute-force difficulty.

### 2.3 Symbol exposure interactions

Even with randomized base, information leaks can reveal effective addresses.
Hardening companions:
- pointer visibility restrictions in proc and sysfs
- strict logging policy
- controlled kallsyms exposure behavior

---

## 3. Linux Kernel Implementation

### 3.1 Early boot flow

High-level logic in early arm64 boot code:
1. Determine whether KASLR is enabled by config and boot args.
2. Gather entropy.
3. Compute randomized image offset within allowed window.
4. Relocate or map kernel accordingly.
5. Record effective offset for internal symbol resolution.

Common symbols and concepts:
- kimage_voffset
- kaslr_offset computation paths in early assembly and setup code
- disable path via boot parameter such as nokaslr

### 3.2 Module randomization

Kernel module allocation is also randomized in module region policy:
- Helps prevent stable module gadget targeting.
- Usually constrained by branch-range and architecture mapping rules.

### 3.3 Debug and observability behavior

Security controls reduce accidental disclosure:
- kptr_restrict policy
- restricted kallsyms in non-privileged contexts
- crash dump and debug workflows may intentionally retain privileged visibility

---

## 4. Hardware-Software Interaction

Boot sequence with KASLR enabled:
1. Firmware enters kernel image.
2. Early setup collects entropy and computes offset.
3. Kernel virtual base is adjusted.
4. Page tables are built around randomized placement.
5. Runtime symbol references use relocated base.

Impact on attackers:
- Absolute addresses vary per boot.
- Exploit payloads requiring fixed gadgets become less portable.
- Combined with PAC, BTI, and strict pointer exposure, exploitation cost increases significantly.

---

## 5. Interview Q and A

Q1: Is KASLR enough to stop ROP?
No. It raises difficulty but leaks can recover layout. It should be combined with PAC, BTI, and memory-safety hardening.

Q2: What is kimage_voffset?
A runtime offset used to translate between linked and actual loaded kernel virtual addresses.

Q3: Why can KASLR be disabled with boot args?
For deterministic debugging, crash triage, or compatibility scenarios. Production security posture usually keeps it enabled.

Q4: Does module randomization matter if kernel is randomized?
Yes. Modules provide additional gadget surfaces; randomizing their placement adds another uncertainty layer.

Q5: What harms KASLR effectiveness most?
Address disclosure bugs and verbose logs that leak pointers.

Q6: How do you validate KASLR quickly?
Compare kernel base addresses across cold boots and inspect restricted symbol exposure policies.

---

## 6. Pitfalls and Gotchas

- Confusing relocation offset with physical memory randomization details.
- Assuming all boot modes provide equal entropy quality.
- Leaving debug interfaces open in production and leaking kernel addresses.
- Using only reboot-in-place tests instead of full power-cycle entropy validation.
- Benchmarking security posture without checking pointer leak channels.

---

## 7. Quick Reference Table

| Item | Description |
|---|---|
| KASLR | Randomizes kernel location at boot |
| kimage_voffset | Effective runtime kernel virtual offset |
| nokaslr | Boot argument disabling KASLR |
| Module randomization | Randomized placement for loadable modules |
| kptr_restrict | Limits pointer disclosure in user-visible outputs |
