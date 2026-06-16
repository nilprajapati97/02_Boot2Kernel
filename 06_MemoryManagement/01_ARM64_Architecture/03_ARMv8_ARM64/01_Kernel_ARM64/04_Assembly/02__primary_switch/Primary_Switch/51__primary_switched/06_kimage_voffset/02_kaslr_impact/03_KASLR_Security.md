# KASLR Security Analysis — Bypasses and Defenses

## What KASLR Protects Against

KASLR is a **probabilistic** defense. It does NOT prevent exploitation outright;
it raises the COST of exploitation by making kernel addresses unpredictable.

### Attack Categories KASLR Mitigates:
1. **ROP/JOP attacks**: Return-oriented or Jump-oriented programming requires
   knowing the addresses of gadgets (small instruction sequences). With KASLR,
   these addresses change every boot.
   
2. **Direct kernel exploitation**: Overwriting kernel function pointers requires
   knowing target addresses. KASLR randomizes them.

3. **Automated kernel exploits**: Scripted attacks that rely on hardcoded kernel
   addresses fail across reboots with KASLR.

### Attack Categories KASLR Does NOT Mitigate:
1. **Information leak attacks**: If the attacker can read kernel memory (even one
   pointer), they can compute the base address. KASLR provides no defense against
   info leaks combined with exploitation.

2. **Same-process exploits**: An attacker with code execution in the kernel already
   knows addresses (they're in the same address space).

3. **Hardware attacks**: CPU side-channels (Spectre, Meltdown) can reveal kernel
   addresses to unprivileged code.

---

## Known KASLR Bypass Techniques (Historical)

### 1. `/proc/kallsyms` Information Leak (Patched)
Early Linux: `/proc/kallsyms` showed real addresses to non-root.
```
$ cat /proc/kallsyms | grep _text
ffff8000481a0000 T _text   # real address leaked!
```
Fix: `kptr_restrict` sysctl — symbols show as 0 to non-root.

### 2. `dmesg` Kernel Pointer Exposure (Partially Patched)
Kernel log messages often printed addresses (`printk("%p", ptr)`).
Fix: `%p` format specifier now hashes kernel pointers by default.
To print real address: use `%px` (privileged contexts only).

### 3. Side-Channel via TSC (Timing)
By timing kernel operations, attackers could sometimes infer the load address.
Linux 4.x mitigation: jitter in timing responses, `CONFIG_GCC_PLUGIN_RANDSTRUCT`.

### 4. `/proc/iomem` Disclosure (Partially patched)
```
$ cat /proc/iomem | grep Kernel
481a0000-49ffffff : Kernel code   # reveals PA of kernel text
```
The PA + known VA → kimage_voffset. Fix: restrict `/proc/iomem` to root.

---

## Defenses That Complement KASLR

### Pointer Authentication (PAC)
ARMv8.3-A hardware feature:
- Signs pointers with a cryptographic signature (uses the upper bits of 64-bit VA)
- Forged pointers fail PAC verification → branch to signed pointer fails
- Makes ROP harder even if kernel address is leaked (can't forge valid signed gadget pointers)

### Shadow Call Stack (SCS)
Protects return addresses from being corrupted even with write-what-where primitives.
KASLR + SCS = both location AND content of return address chain are protected.

### SMAP/PAN (Supervisor Mode Access Prevention)
ARM64: PAN (Privileged Access Never)
- Kernel code cannot directly dereference user-space pointers
- Prevents "ret2usr" attacks even if kernel address is known

### `__ro_after_init` for `kimage_voffset`
An attacker who can write to `kimage_voffset` could make `__pa()` return
attacker-controlled PAs, enabling DMA-based memory corruption. The `__ro_after_init`
protection prevents this write.

---

## KASLR and Crash Dumps — Security vs. Debuggability Tradeoff

For debugging production crashes, KASLR creates a challenge: symbols in the crash
dump need to be resolved using `kimage_voffset`.

**Production (security priority):**
- `kimage_voffset` NOT exposed in `/proc/vmcore` (unless root)
- Crash dumps analyzed only by authorized engineers with root access

**Development (debuggability priority):**
- `CONFIG_RANDOMIZE_BASE=n`: disable KASLR entirely
- `kaslr=off` kernel command line parameter
- `kptr_restrict=0`: expose all kernel addresses in logs

**Crashdump with KASLR:**
```bash
# makedumpfile reads kimage_voffset from VMCOREINFO in the crash dump:
makedumpfile -l --message-level 31 -d 31 /proc/vmcore dumpfile

# crash tool uses KERNELOFFSET from VMCOREINFO:
crash vmlinux dumpfile
crash> p kimage_voffset    # shows the boot-time computed value
```

---

## ARM64-Specific KASLR: Better Than x86?

| Property | ARM64 KASLR | x86-64 KASLR |
|---|---|---|
| Both PA and VA | Yes (independent) | VA only |
| Granule | 2 MB | 2 MB |
| Max entropy | ~29 bits | ~9 bits (VA only, 512 MB range) |
| Hardware RNG | RNDRRS instruction | RDRAND instruction |
| Implementation | head.S + kaslr.c | boot/compressed/kaslr.c |

ARM64 KASLR is stronger than x86-64 KASLR partly because it can independently
randomize both PA and VA, roughly doubling the entropy compared to VA-only schemes.

---

## ARMv8 CPU / Kernel / Memory Context

### CPU Perspective (ARMv8-A)
KASLR (Kernel Address Space Layout Randomization) on ARMv8-A is implemented by choosing a random physical load address (phys_offset) and a random virtual mapping offset (kimage_voffset) at boot time. The CPU does not know or care about randomization: it simply uses whatever address is in TTBR1_EL1 as the root of the kernel page table. The EL1 exception vector base (VBAR_EL1) is also randomized as a consequence of the kernel image being loaded at a random VA. The hardware has no KASLR-awareness.

### Kernel Perspective (Linux ARM64)
KASLR is implemented in __pi_kaslr_early_init (arch/arm64/kernel/pi/kaslr_early.c). It uses the EFI random number generator (or RNDR instruction on ARMv8.5+) to pick a random offset that is aligned to the minimum KASLR granularity (2 MB for 4 KB pages). The offset is applied by:
  1. Choosing a random VA base for the kernel image within the TTBR1_EL1 region.
  2. Updating kimage_voffset = VA - PA.
  3. Updating all ELF RELA relocation entries to reflect the new VA.
  4. Flushing the D-cache for all modified sections.

### Memory Perspective (ARMv8 Memory Model)
KASLR changes the kernel's VA layout but not its PA layout (for a given boot). The TTBR1_EL1 page tables point to the randomized kernel VA. kimage_voffset is a compile-time-unknown runtime constant used to convert between kernel PA and kernel VA: VA = PA + kimage_voffset. Because the kernel uses position-independent code (__pi_ prefix) during the relocation phase, all accesses are PA-relative until the relocations are applied. After __primary_switch, all kernel code runs at the final randomized VA.