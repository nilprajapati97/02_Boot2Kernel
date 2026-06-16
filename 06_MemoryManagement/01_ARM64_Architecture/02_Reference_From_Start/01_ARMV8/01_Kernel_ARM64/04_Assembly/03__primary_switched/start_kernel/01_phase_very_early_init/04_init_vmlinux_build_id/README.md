# `init_vmlinux_build_id()` — Kernel Build ID

## Overview

| Attribute    | Value                                        |
|-------------|-----------------------------------------------|
| **Function** | `init_vmlinux_build_id(void)`                |
| **Source**   | `kernel/buildid.c`                           |
| **Purpose**  | Initialize and expose the kernel's unique build identifier (GNU Build ID) |

---

## Why It Exists

A **Build ID** is a cryptographic hash (usually SHA-1 or MD5) of significant parts of an ELF binary, embedded in the `.note.gnu.build-id` ELF section. It uniquely identifies a specific kernel build.

Used for:
1. **Crash analysis**: When a kernel crash dump is captured, the Build ID tells crash tools (`crash`, `kdump`, GDB) exactly which kernel binary to use for symbol resolution
2. **Perf profiling**: `perf record` embeds the Build ID in perf.data so symbols can be resolved offline on a different machine
3. **BPF tracing**: BPF CO-RE (Compile Once Run Everywhere) uses Build IDs to verify kernel version compatibility
4. **Kernel live patching**: `kpatch`, `livepatch` tools need the exact Build ID to generate compatible patches

---

## Internal Deep Dive

```c
// kernel/buildid.c
void __init init_vmlinux_build_id(void)
{
    extern const void __start_notes __weak;
    extern const void __stop_notes __weak;
    
    // Parse the .note.gnu.build-id ELF note embedded in the kernel
    build_id_parse_buf(&__start_notes,
                       &__stop_notes - &__start_notes,
                       vmlinux_build_id);
}
```

The Build ID is stored in `vmlinux_build_id[]` — a global array of up to 20 bytes (SHA-1 build IDs are 20 bytes).

### ELF Note Format

```
ELF Note (.note.gnu.build-id):
┌──────────────────────────────┐
│ namesz = 4 ("GNU\0")         │
│ descsz = 20 (SHA-1 length)   │
│ type   = NT_GNU_BUILD_ID (3) │
│ name   = "GNU\0"             │
│ desc   = [20 bytes SHA-1]    │
└──────────────────────────────┘
```

---

## Access From Userspace

```bash
# View kernel build ID
cat /sys/kernel/notes | xxd | grep -A2 "GNU"

# Or via /proc
cat /proc/sys/kernel/build_id  # (some kernels)

# Via perf
perf buildid-list --kernel
```

---

## Interview Q&A

### Q1: How is the Build ID generated for the kernel?
**A:** During the kernel link step, the linker (`ld`) computes a hash over the output ELF file's loadable sections and embeds it as a `.note.gnu.build-id` ELF note. The default is SHA-1 of the kernel `.text`, `.data`, `.rodata` etc. sections. This happens at build time, not runtime — `init_vmlinux_build_id()` just **reads** the pre-embedded note into a kernel variable.

### Q2: Why must `init_vmlinux_build_id()` run so early?
**A:** Because crash dump handlers (`kdump`, `kexec`) and BPF programs can run at any time after the kernel is fully initialized. Those subsystems need the Build ID readily available in a global variable — not parsed on demand. Running it early ensures `vmlinux_build_id[]` is valid before any of those subsystems start.

### Q3: What happens if two kernels have the same Build ID?
**A:** This can only happen if the binary is exactly identical byte-for-byte (same compiler, same config, same source, same link order). In practice, Build IDs are unique per build. If you rebuild the kernel with a single character change, the hash changes completely. Identical Build IDs from different builds would indicate a deterministic build system (reproducible builds).
