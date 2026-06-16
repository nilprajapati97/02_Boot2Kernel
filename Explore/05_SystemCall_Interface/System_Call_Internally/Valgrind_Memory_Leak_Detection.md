# Valgrind for Memory Leak Detection — Deep Dive (Embedded Linux Interview Guide)

**Valgrind** is an instrumentation framework that runs your program on a
*synthetic CPU* and watches every memory access and every heap allocation. Its
flagship tool, **Memcheck**, catches the entire family of C/C++ memory bugs —
**leaks**, out-of-bounds reads/writes, use-after-free, double-free, mismatched
`malloc`/`free` vs `new`/`delete`, and reads of uninitialized memory — *without
recompiling your source* and *without you writing a single test assertion*.

For a kernel / embedded-Linux engineer this matters because the bugs Valgrind
finds are exactly the ones that don't crash immediately: a 16-byte leak per
request is invisible on a desktop but kills a router after three weeks of
uptime; an uninitialized read "works on my board" until the compiler or the next
silicon revision lays out the stack differently.

This document explains, from scratch and in depth, **how Valgrind works**, **how
Memcheck detects leaks and bad accesses**, **how to use it in practice**, and —
importantly — **how to apply it to embedded Linux targets** (cross-compiling,
running on-target, resource limits, and when to reach for AddressSanitizer
instead). It is framed around **Why? When? Where? How?** and finishes with an
interview Q&A and cheat sheets.

> Scope: Memcheck on Linux, both `x86_64` development hosts and `arm`/`aarch64`
> targets. Other Valgrind tools (Helgrind, DRD, Massif, Cachegrind) are
> mentioned only for context. Flag names refer to Valgrind ≥ 3.15.

---

## 0. Table of Contents

1. [The Big Picture (Layers)](#1-the-big-picture-layers)
2. [WHY Valgrind Exists (The Bug Class)](#2-why-valgrind-exists-the-bug-class)
3. [WHEN to Use It & WHAT It Detects](#3-when-to-use-it--what-it-detects)
4. [WHERE It Sits — DBI vs Compile-Time Instrumentation](#4-where-it-sits--dbi-vs-compile-time-instrumentation)
5. [HOW Valgrind Works Internally (Core + VEX JIT)](#5-how-valgrind-works-internally-core--vex-jit)
6. [Memcheck Internals — A-bits & V-bits (Shadow Memory)](#6-memcheck-internals--a-bits--v-bits-shadow-memory)
7. [Leak Detection Mechanism & Leak Kinds](#7-leak-detection-mechanism--leak-kinds)
8. [Practical Usage — Build, Run, Flags](#8-practical-usage--build-run-flags)
9. [Reading a Memcheck Report](#9-reading-a-memcheck-report)
10. [Embedded Linux — Cross-Compile, Run On-Target, Limits](#10-embedded-linux--cross-compile-run-on-target-limits)
11. [Alternatives & Complements (ASan, LSan, mtrace, Static)](#11-alternatives--complements-asan-lsan-mtrace-static)
12. [Pitfalls, False Positives & Suppressions](#12-pitfalls-false-positives--suppressions)
13. [Interview Q&A](#13-interview-qa)
14. [Quick-Reference Cheat Sheets](#14-quick-reference-cheat-sheets)

---

## 1. The Big Picture (Layers)

Valgrind does **not** run your program directly on the hardware. It loads your
binary into its own address space, *disassembles* it block by block, *adds
instrumentation*, *re-compiles* it back to host machine code, and runs **that**.
Your program never touches the real CPU's instruction stream unsupervised.

```
┌──────────────────────────────────────────────────────────────┐
│ Your program (unmodified a.out, compiled with -g)            │
│   p = malloc(16);   *p = 5;   free(p);   /* maybe forgot? */ │
├──────────────────────────────────────────────────────────────┤
│ ░░░ Valgrind CORE ("coregrind") ░░░                          │
│   • Loads the guest binary + intercepts the loader           │
│   • Owns the real process; schedules guest threads           │
│   • Intercepts malloc/free/mmap/brk and syscalls             │
├──────────────────────────────────────────────────────────────┤
│ VEX — the JIT / dynamic binary translator                    │
│   guest machine code ──disassemble──▶ VEX IR (SSA-like)      │
│        VEX IR ──INSTRUMENT (tool adds checks)──▶ VEX IR'      │
│        VEX IR' ──recompile──▶ HOST machine code  ──▶ run      │
├──────────────────────────────────────────────────────────────┤
│ TOOL plugin  (here: MEMCHECK)                                │
│   • Inserts a check before every load/store                  │
│   • Maintains SHADOW MEMORY (A-bits + V-bits)                │
│   • Replaces malloc/free to track every heap block           │
├──────────────────────────────────────────────────────────────┤
│ SHADOW MEMORY (Valgrind's private bookkeeping)               │
│   For each byte of guest memory: "is it addressable?"        │
│   For each bit  of guest data:   "is it initialized?"        │
└──────────────────────────────────────────────────────────────┘
                 ▼ at exit (or on demand)
        LEAK CHECK: scan the heap for unreachable blocks
```

Key mental model: **Valgrind is a CPU emulator with a conscience.** It runs
every one of your instructions, but inserts extra instructions around the
memory-touching ones to ask "is this access legal? is this data initialized?"
and it remembers every heap block you allocated so it can tell you, at the end,
which ones you never freed and can no longer reach.

This is why Valgrind needs **no source changes and no recompilation** — it works
on the *binary*. It is also why it is **slow** (typically 10–50× under Memcheck):
every memory access becomes several instructions plus a shadow-memory lookup.

---

## 2. WHY Valgrind Exists (The Bug Class)

C and C++ give you raw pointers and manual memory management. That power creates
a class of bugs the language itself will not catch:

| Bug | What happens | Why it's nasty |
|-----|--------------|----------------|
| **Memory leak** | You `malloc` and never `free`; the pointer is lost | No crash. Process RSS grows slowly until OOM-kill — hours/days later, far from the cause |
| **Use-after-free** | You `free(p)` then read/write `*p` | Sometimes "works" (memory not yet reused), sometimes corrupts the allocator |
| **Buffer overflow/underflow** | Write past the end/start of a heap block | Silent heap corruption; crash appears in an *unrelated* later allocation |
| **Uninitialized read** | Use a variable/heap byte before writing it | Non-deterministic; depends on stack/heap residue. "Heisenbug" |
| **Double free** | `free(p)` twice | Corrupts allocator metadata; may be exploitable |
| **Mismatched alloc/free** | `free` a `new[]` pointer, or `delete` a `malloc` pointer | Undefined behaviour; allocator/destructor mismatch |
| **Invalid free** | `free` a stack/global/middle-of-block pointer | Allocator corruption |

The unifying theme: **these bugs don't fail where they're caused.** The crash
(if any) happens later, somewhere else, often non-deterministically. Traditional
debuggers show you the *symptom site*, not the *cause site*. Valgrind flips that
— it reports the bug **at the instruction that committed it**, with the
allocation stack trace attached.

### Why this is acute on embedded / long-running systems

- **Uptime is measured in months.** A daemon, a network stack, a media pipeline
  runs continuously. A tiny per-event leak is a guaranteed eventual outage.
- **No swap, little RAM.** A 256 MB target has no headroom; the OOM killer
  reaps your process (or worse, an innocent one) long before a desktop would
  notice.
- **Field debugging is painful.** Reproducing a 3-week leak on a deployed device
  is far more expensive than catching it in CI with Valgrind.

---

## 3. WHEN to Use It & WHAT It Detects

**When:** during development and especially in **CI/regression testing**, run
your unit tests / integration harness under Valgrind with `--error-exitcode` so
any new memory error fails the build. Use it when you observe growing RSS,
intermittent crashes, or "impossible" data corruption.

**What Memcheck detects (the complete list):**

- **Reads/writes of unaddressable memory** — past heap-block bounds, freed
  blocks, unmapped pages → `Invalid read`/`Invalid write`.
- **Use of uninitialized values** — when an uninit value *affects program
  behaviour* (a branch, a syscall argument, an address) → `Conditional jump or
  move depends on uninitialised value(s)` / `Use of uninitialised value`.
- **Illegal frees** — double free, free of non-heap or interior pointers →
  `Invalid free`.
- **Mismatched allocation/deallocation** — `malloc`+`delete`, `new`+`free`,
  `new[]`+`delete` → `Mismatched free() / delete / delete []`.
- **Overlapping `src`/`dst`** in `memcpy` and friends.
- **Bad arguments** to `malloc`/`memalign` etc.
- **Memory leaks** at exit (or on demand) — the focus of this guide.

> Memcheck is *dynamic*: it only sees code paths that **actually execute**. A
> leak on an error path you never trigger will not be reported. Combine with
> good test coverage.

---

## 4. WHERE It Sits — DBI vs Compile-Time Instrumentation

There are two fundamentally different ways to add memory checking. Knowing the
trade-off is a classic interview discriminator.

```
        COMPILE-TIME (AddressSanitizer)          RUN-TIME / DBI (Valgrind)
        ──────────────────────────────          ─────────────────────────
  source ─▶ compiler inserts checks         binary (already built) ─▶ Valgrind
           ─▶ instrumented binary                  JIT-instruments at run time
  needs recompile + relink                   no recompile; works on any binary
  ~2× slowdown, ~2–3× memory                  ~10–50× slowdown, large memory
  redzones + shadow (1/8 scale)               full shadow + emulated CPU
  catches stack & global overflows well       great on heap; weaker on stack/global
  can't instrument what you can't rebuild      runs closed-source/3rd-party binaries
```

**Valgrind is Dynamic Binary Instrumentation (DBI):** it instruments machine
code *as it runs*. Advantages: no rebuild, sees *all* code including libraries
and dlopen'd plugins, and its uninitialized-value tracking (V-bits) is more
precise than ASan's. Disadvantages: heavy slowdown and RAM use, and it
serializes threads onto one core (bad for race detection coverage, fine for
leaks).

**AddressSanitizer (ASan)** is compile-time: faster and better at stack/global
overflows, but needs to rebuild the whole stack and instruments only what you
recompile. On embedded, the choice often comes down to *can you cross-build the
whole image with `-fsanitize=address`?* (ASan) vs *can you fit and tolerate
Valgrind's overhead on the target?* (Valgrind). See §11.

---

## 5. HOW Valgrind Works Internally (Core + VEX JIT)

Valgrind's architecture is a **core** plus a swappable **tool**:

1. **Coregrind** takes over the process. It maps your executable, takes over the
   ELF entry point, intercepts the C library's `malloc`/`free` family, traps all
   system calls (so it knows when the kernel reads/writes your memory), and
   implements its own thread scheduler (guest threads are serialized onto a
   single host thread — only one runs at a time).

2. **The disassemble–instrument–recompile loop (VEX):**
   - Execution proceeds one **basic block** (a "superblock") at a time.
   - The block's **guest** machine code (ARM, AArch64, x86-64…) is decoded into
     **VEX IR**, an architecture-neutral, SSA-like intermediate representation.
   - The active **tool** (Memcheck) is handed the IR and **instruments** it —
     e.g., before every `LD`/`ST`, it inserts a helper call that checks the
     shadow state.
   - VEX **recompiles** the instrumented IR into **host** machine code and
     caches the translation. Subsequent runs of the same block reuse the cache.
   - The translated block runs natively on the real CPU, then control returns to
     the scheduler for the next block.

```
   guest insns ─▶ [VEX front end] ─▶ VEX IR ─▶ [Memcheck instrument] ─▶ IR'
        ▲                                                                │
        │                                                  [VEX back end]│
        └──────────────── run on host CPU ◀── host machine code ◀────────┘
                              (cached in translation table)
```

Because every memory operation is wrapped with a check, and shadow state is
updated on every store, Memcheck typically runs **20–30× slower** than native
and uses roughly **2× the memory** (shadow + redzones + translation cache). That
overhead *is the price of admission* for byte-precise, bit-precise checking with
zero source changes.

> The same core also lets Valgrind interpose its own `malloc` so it can place
> **redzones** (guard bytes) before and after every heap block — that's how it
> detects off-by-one overflows.

---

## 6. Memcheck Internals — A-bits & V-bits (Shadow Memory)

Memcheck answers two independent questions for memory you touch:

1. **"Am I allowed to access this address?"** → **A-bits (Addressability).**
   One bit *per byte* of guest address space: addressable or not. Set when you
   `malloc`/`mmap`, cleared on `free`/`munmap`. The redzones around heap blocks
   are marked *un*addressable, so a one-byte overflow lands on an unaddressable
   byte and is reported instantly.

2. **"Is the data I'm reading actually initialized?"** → **V-bits (Validity /
   "defined-ness").** One shadow bit *per bit* of guest data (registers and
   memory). A freshly `malloc`'d block is all-undefined; writing a value makes
   those bits defined; copying memory copies the V-bits too.

```
   Guest byte at 0x6001a0:   value = 0x?? (just malloc'd)
   ───────────────────────────────────────────────────────
   A-bit  : 1   → addressable (inside a live heap block)
   V-bits : 8×"undefined"  → not yet written

   After  *p = 5;
   A-bit  : 1
   V-bits : 8×"defined"    → safe to read & use
```

The crucial subtlety — and the reason Memcheck is so precise — is that **using
an uninitialized value is not, by itself, an error.** Memcheck *propagates*
undefined-ness through copies and arithmetic and only **complains when an
undefined value actually changes observable behaviour**: it controls a branch
(`if`/`switch`), is used as a memory **address**, or is passed to a **syscall**.
That's why the message is "*Conditional jump or move depends on uninitialised
value(s)*" rather than firing at the read itself. This eliminates a flood of
false positives from benign copies of padding bytes.

> **`--track-origins=yes`** extends this: when an undefined value is reported,
> Memcheck traces *where the undefined-ness came from* (which allocation or stack
> slot). It roughly doubles slowdown but turns "somewhere" into a concrete line.

---

## 7. Leak Detection Mechanism & Leak Kinds

### How the leak check works

Memcheck records **every** live heap block (address, size, and the **allocation
stack trace**) in an internal table. When the leak check runs — at exit, or on
demand via `vgdb` — it performs a **conservative garbage-collection-style scan**:

1. Identify the **roots**: CPU registers and all *addressable* memory that is not
   itself heap (static/global data, the stacks of all threads).
2. **Scan** those roots for any value that *looks like a pointer* into a known
   heap block. Follow it; mark that block reachable; recursively scan inside it
   for more pointers (transitive reachability).
3. Any heap block **not** reached by this scan is a **leak** — nothing in the
   program can possibly free it anymore.

This is *conservative*: a random integer that happens to equal a heap address
will be treated as a pointer (avoiding false leak reports at the cost of
occasionally missing a real leak). That conservatism is why Valgrind reports
leaks reliably but classifies some as "possibly".

### The four leak kinds (memorize these)

| Kind | Meaning | Typical cause |
|------|---------|---------------|
| **Definitely lost** | No pointer to the block exists anywhere | Classic leak — you overwrote/lost the only pointer. **Fix these.** |
| **Indirectly lost** | Reachable only *through* a definitely-lost block | Children of a leaked parent (e.g., a leaked linked-list head; the nodes are indirectly lost). Fixing the parent fixes these. |
| **Possibly lost** | Only an **interior pointer** points into the block | Pointer into the *middle* of the block (offset pointer, some C++ multiple-inheritance / aligned-vector layouts). Investigate. |
| **Still reachable** | A valid pointer still exists at exit, but you never freed it | Global caches, singletons, one-time allocations. Usually benign but untidy; some shops require zero. |

> `--show-leak-kinds=all` prints all four. `--leak-check=full` gives a stack
> trace per leaking block. **Definitely** and **indirectly** lost are real bugs;
> **still reachable** is a policy choice; **possibly lost** needs a human look.

---

## 8. Practical Usage — Build, Run, Flags

### Step 1 — Build for good diagnostics

```sh
# -g  : debug info → file:line and symbol names in stack traces
# -O0 : no optimization → traces line up with source; -O1 is usually OK,
#       but aggressive inlining/optimization can blur stacks & cause
#       spurious uninitialized-value reports.
gcc -g -O0 -o myapp myapp.c
```

### Step 2 — Run under Memcheck

```sh
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --error-exitcode=1 \
         --log-file=valgrind.out \
         ./myapp arg1 arg2
```

### The flags that matter

| Flag | Effect |
|------|--------|
| `--tool=memcheck` | Default tool; usually omitted. |
| `--leak-check=full` | Report each leaking block **with a stack trace** (vs `summary`). |
| `--show-leak-kinds=all` | Show definite/indirect/possible/reachable (default shows definite+possible). |
| `--track-origins=yes` | Trace the origin of uninitialized values. ~2× slower; very worth it. |
| `--error-exitcode=N` | Exit non-zero if any error found → **fails CI builds**. |
| `--errors-for-leak-kinds=definite,indirect` | Which leak kinds count as "errors" for the exit code. |
| `--log-file=FILE` | Write the report to a file (use `%p` for PID: `valgrind.%p.log`). |
| `--xml=yes --xml-file=FILE` | Machine-readable output for CI dashboards. |
| `--suppressions=FILE` | Load known-false-positive suppressions (see §12). |
| `--gen-suppressions=all` | Print ready-to-paste suppression entries for each error. |
| `--num-callers=N` | Stack-trace depth (default 12; raise for deep call chains). |
| `--child-silent-after-fork=yes` | Don't trace children after `fork` (useful for servers). |
| `--trace-children=yes` | Follow `exec`'d child processes (e.g., scripts launching binaries). |
| `--fair-sched=yes` | Fairer thread scheduling on multi-threaded programs. |
| `--vgdb=full` / `--vgdb-error=0` | Enable the gdb bridge (see §10). |

### Step 3 — Run your *test suite* under it

The highest-value use is in CI: run the unit/integration tests under Valgrind
with `--error-exitcode=1`. A new leak or invalid access then **breaks the
build**, catching the bug the day it's introduced rather than in the field.

---

## 9. Reading a Memcheck Report

### A leak block

```
==12345== 16 bytes in 1 blocks are definitely lost in loss record 1 of 1
==12345==    at 0x4C2FB0F: malloc (vg_replace_malloc.c:309)
==12345==    by 0x108671: make_widget (widget.c:42)
==12345==    by 0x1086A3: main (main.c:11)
```

Read it bottom-up as a call stack: `main` (main.c:11) called `make_widget`
(widget.c:42), which called `malloc`. **16 bytes** allocated there were never
freed and are now unreachable (**definitely lost**). The fix lives at
`widget.c:42` — that allocation has no matching `free`. `12345` is the PID.

### An invalid access

```
==12345== Invalid write of size 4
==12345==    at 0x108693: main (main.c:14)        ← where the bad write happened
==12345==  Address 0x5204044 is 0 bytes after a block of size 16 alloc'd
==12345==    at 0x4C2FB0F: malloc (vg_replace_malloc.c:309)
==12345==    by 0x108671: main (main.c:11)        ← where that block was allocated
```

This says: at `main.c:14` you wrote 4 bytes **0 bytes after** a 16-byte block —
a classic off-by-one (`a[4]` on `int a[4]`). The report pairs the *fault site*
with the *allocation site*, so you see both the crime and the victim.

### The leak summary

```
==12345== LEAK SUMMARY:
==12345==    definitely lost: 16 bytes in 1 blocks
==12345==    indirectly lost: 0 bytes in 0 blocks
==12345==      possibly lost: 0 bytes in 0 blocks
==12345==    still reachable: 72,704 bytes in 1 blocks   ← often libc one-time init
==12345==         suppressed: 0 bytes in 0 blocks
```

A clean run ends with `All heap blocks were freed -- no leaks are possible` and
`ERROR SUMMARY: 0 errors from 0 contexts`. (The 72 KB "still reachable" is
usually libc/stdio's one-time buffers — benign.)

---

## 10. Embedded Linux — Cross-Compile, Run On-Target, Limits

This is where embedded engineers earn their keep. Valgrind runs the **guest on
its synthetic CPU, so it must support your target architecture** (it does:
`arm`, `aarch64`, plus `x86`, `amd64`, `ppc`, `mips`, `s390x`). Valgrind itself
runs **on the target** — there is no "remote emulate from the host"; the guest
and Valgrind share one process on the device.

### Option A — Use your build system's package

Most embedded build systems ship a Valgrind recipe — the path of least
resistance:

- **Yocto / OpenEmbedded:** add `valgrind` to your image
  (`IMAGE_INSTALL:append = " valgrind"`). It's in `meta-oe`/core.
- **Buildroot:** enable `BR2_PACKAGE_VALGRIND` in `menuconfig`
  (Target packages → Debugging, profiling and benchmark → valgrind).

These handle the cross-compile for you and place Valgrind plus its support files
(the `.../lib/valgrind/` directory with `vgpreload_*.so` and default
suppressions) onto the rootfs.

### Option B — Cross-compile Valgrind by hand

```sh
# Example: build Valgrind for aarch64 with a cross toolchain.
export CROSS=aarch64-linux-gnu
./configure --host=aarch64-unknown-linux-gnu \
            CC=${CROSS}-gcc \
            CFLAGS="-static-libgcc" \
            --prefix=/usr
make -j"$(nproc)"
make install DESTDIR=$PWD/_install
# Then copy _install/usr/{bin,lib}/... (the whole lib/valgrind dir) to the target.
```

Key points:

- `--host` selects the **target** triple; the build still runs on your dev host.
- The **`lib/valgrind/` directory is mandatory** on the target — it contains the
  preload shims and the default suppression file. Copy the whole tree, not just
  the `valgrind` binary.
- Build your **application** with the matching cross toolchain and `-g`.

### Then, on the target

```sh
# On the device (over ssh / serial console):
valgrind --leak-check=full --show-leak-kinds=all ./myapp
```

### Resource reality check (the embedded gotchas)

| Constraint | Consequence | Mitigation |
|-----------|-------------|------------|
| **RAM** | Memcheck roughly **doubles** memory use (shadow + redzones + translation cache). A 64–128 MB target may OOM. | Test a smaller workload; reduce `--num-callers`; use `--freelist-vol` lower; or run on a higher-RAM dev board with the same arch. |
| **CPU / time** | **10–50× slowdown.** Real-time deadlines will be missed; watchdogs may fire. | Disable hardware watchdog during the run; relax timeouts; reduce dataset. |
| **Flash space** | Valgrind + `lib/valgrind/` is several MB. | Put it on an NFS mount / SD card / tmpfs rather than internal flash. |
| **No FPU/odd ISA extensions** | VEX may not model exotic SIMD/coproc ops. | Compile the app without those extensions for the Valgrind run, or check Valgrind's arch support notes. |
| **Watchdog/RT** | Timing-sensitive code behaves differently under 30× slowdown. | Stub timers; understand results are about *memory*, not *timing*. |

### Remote debugging with `vgdb` (powerful on embedded)

Valgrind exposes a gdb stub so you can drive it from gdb — and trigger a leak
check **on demand**, mid-run, without exiting:

```sh
# On target: start under Valgrind, paused for gdb, error-triggered:
valgrind --vgdb=yes --vgdb-error=0 ./myapp
# (Valgrind prints the vgdb command to attach.)

# From a gdb (cross-gdb on host, or gdb on target):
(gdb) target remote | vgdb
(gdb) monitor leak_check full reachable any   # run leak check NOW
(gdb) monitor get_vbits 0x........ 16          # inspect V-bits of an address
(gdb) monitor block_list <loss_record_nr>      # list blocks of a leak record
```

`monitor leak_check` mid-execution is invaluable for long-running daemons: snapshot
leaks at two points and diff them, instead of waiting for process exit.

### When Valgrind is *not* feasible on the target

If the device is too small or too timing-critical, your options are:

- Run the same code on a **bigger board of the same architecture** (e.g., a
  dev/eval board) under Valgrind.
- Run the **logic in a host-side unit-test harness** (same source, native or
  QEMU-user) under Valgrind, isolating it from real hardware drivers.
- Switch to **compile-time sanitizers** (ASan/LSan) which are far lighter — see
  §11.

---

## 11. Alternatives & Complements (ASan, LSan, mtrace, Static)

Valgrind is not the only tool, and a strong answer names the trade-offs.

| Tool | Type | Slowdown | Rebuild? | Strengths | Embedded fit |
|------|------|----------|----------|-----------|--------------|
| **Valgrind/Memcheck** | DBI (run-time) | 10–50× | No | Heap leaks, uninit-value precision, any binary incl. libs | Heavy; needs RAM+CPU headroom on target |
| **AddressSanitizer (ASan)** | Compile-time | ~2× | Yes (`-fsanitize=address`) | Heap **and** stack/global overflows, use-after-return | Good if you can cross-build the whole image; modest overhead |
| **LeakSanitizer (LSan)** | Compile-time (part of ASan, or standalone) | Near-zero for leaks | Yes (`-fsanitize=leak`) | Pure leak detection, very light | Excellent when rebuild is possible |
| **MemorySanitizer (MSan)** | Compile-time | ~3× | Yes (needs *all* deps instrumented) | Uninitialized reads | Hard on embedded (must instrument libc/deps) |
| **glibc `mtrace`** | Library hook | Low | Link/env only (`MALLOC_TRACE`) | Lightweight malloc/free trace → `mtrace` script finds leaks | Works on tiny targets; only leaks, no bounds/uninit |
| **glibc `MALLOC_CHECK_` / `mcheck`** | Library | Low | Env var | Catches some heap corruption/double-free cheaply | Very light, coarse |
| **Static analyzers** (clang-tidy, cppcheck, Coverity, `scan-build`) | Compile-time, no exec | None (build-time) | Source needed | Finds bugs on *all* paths, including untaken ones | No runtime cost — run in CI on host |

**Rules of thumb for embedded:**

- **Can you rebuild the whole image?** Prefer **ASan/LSan** — much lighter,
  catches stack/global bugs Valgrind misses, and runs closer to real time.
- **Closed-source/third-party binary, or can't rebuild?** Use **Valgrind** — it
  works on the binary as-is.
- **Tiny target, only need leaks?** **`mtrace`** or **LSan** are the light path.
- **Want zero-runtime-cost, all-paths coverage?** Add **static analysis** in CI.
  Static and dynamic tools are *complementary*, not substitutes — static finds
  untaken-path bugs; dynamic confirms real, executed ones.

> ASan and Valgrind are mutually exclusive at run time (don't run an ASan binary
> under Valgrind). Pick one per run.

---

## 12. Pitfalls, False Positives & Suppressions

- **"Still reachable" is usually fine.** libc, the dynamic loader, and some
  libraries keep one-time allocations alive until process exit. These are not
  leaks in any harmful sense. Decide your policy (many teams require zero
  *definitely/indirectly* lost but tolerate *still reachable*).
- **Optimized builds blur traces.** `-O2`+ inlining and dead-store elimination
  can cause inaccurate line numbers and occasional spurious uninit reports.
  Prefer `-g -O0` (or `-O1`) for Valgrind runs.
- **Custom allocators hide blocks.** If you pool memory or use a slab allocator,
  Memcheck sees one big `malloc`, not your sub-blocks. Use the **client request
  macros** (`VALGRIND_MALLOCLIKE_BLOCK` / `FREELIKE_BLOCK` from
  `<valgrind/memcheck.h>`) to teach Memcheck about your allocator.
- **Third-party / driver noise.** Closed libraries (GPU drivers, vendor blobs)
  often trip benign warnings. Suppress them rather than chasing them.

### Suppression files

A suppression tells Valgrind "this known, benign error — ignore it":

```sh
# 1. Auto-generate suppression text for every reported error:
valgrind --leak-check=full --gen-suppressions=all ./myapp

# 2. Paste the generated stanzas into a file, e.g. my.supp:
#    {
#       libfoo_known_leak
#       Memcheck:Leak
#       match-leak-kinds: reachable
#       fun:malloc
#       fun:foo_init
#       ...
#    }

# 3. Apply it on subsequent runs:
valgrind --suppressions=my.supp --leak-check=full ./myapp
```

Keep suppressions **narrow** (match the specific call chain) and **commented**
(why is this safe?), or they'll mask real future bugs.

---

## 13. Interview Q&A

**Q1. How does Valgrind work without recompiling my code?**
It's a *dynamic binary instrumentation* tool: it disassembles your binary one
basic block at a time into VEX IR, lets the active tool (Memcheck) insert checks,
recompiles to host code, and runs that on a synthetic CPU. It operates on the
binary, so no source or rebuild is needed.

**Q2. What's the difference between A-bits and V-bits?**
A-bits track **addressability** — one bit per byte: may I touch this address?
V-bits track **validity/defined-ness** — one bit per data bit: is this value
initialized? A-bit violations are reported immediately; V-bit (uninitialized)
problems are reported only when the value influences a branch, an address, or a
syscall.

**Q3. Why doesn't Memcheck flag *every* read of uninitialized memory?**
Because copying uninitialized bytes (e.g., struct padding) is harmless. Memcheck
*propagates* undefined-ness and only complains when it actually affects observable
behaviour, dramatically cutting false positives.

**Q4. Explain the four leak kinds.**
*Definitely lost* — no pointer remains (real bug). *Indirectly lost* — reachable
only via a definitely-lost block (fix the parent). *Possibly lost* — only an
interior pointer points into it (investigate). *Still reachable* — a valid
pointer exists at exit but you never freed it (often benign one-time allocs).

**Q5. How does the leak check actually find leaks?**
At exit it does a conservative GC-style scan: roots = registers + non-heap
addressable memory + thread stacks; it scans for values that look like pointers
into known heap blocks, follows them transitively, and any block never reached is
a leak.

**Q6. Why is Valgrind so slow, and roughly how slow?**
Every memory access becomes a check plus a shadow-memory update, and all code is
JIT-translated through VEX. Typically 10–50× slower (Memcheck ~20–30×) with ~2×
memory.

**Q7. Valgrind vs AddressSanitizer — when do you pick which?**
ASan is compile-time, ~2×, catches stack/global overflows too, but needs a
rebuild and only covers recompiled code. Valgrind is run-time, no rebuild, works
on any binary including third-party libs, with more precise uninit tracking, but
much heavier. On embedded: ASan if you can rebuild the image; Valgrind for
unrebuildable binaries or when you need its precision.

**Q8. How do you run Valgrind on an ARM embedded target?**
Valgrind supports `arm`/`aarch64`, and it runs *on* the device. Get it via Yocto
(`IMAGE_INSTALL`) or Buildroot (`BR2_PACKAGE_VALGRIND`), or cross-compile with
`./configure --host=aarch64-...`. Copy the whole `lib/valgrind/` tree to the
target, build the app with `-g`, then run `valgrind ./app` on the device. Watch
RAM (≈2×) and the 10–50× slowdown vs watchdogs/RT deadlines.

**Q9. The target is too small to run Valgrind. What now?**
Run the same arch on a bigger eval board under Valgrind; or extract the logic
into a host/QEMU-user unit-test harness and Valgrind that; or switch to lighter
tools — LSan/ASan (if you can rebuild) or `mtrace` for leaks only; add static
analysis in CI.

**Q10. How do you integrate Valgrind into CI?**
Run the test suite under `valgrind --leak-check=full --error-exitcode=1` (often
`--errors-for-leak-kinds=definite,indirect`). Any new memory error returns
non-zero and fails the build. Use `--xml=yes` for dashboards and suppression
files for known-benign noise.

**Q11. What's `--track-origins=yes` and its cost?**
It records *where* an uninitialized value originated (which alloc/stack slot), so
the report points to the root cause instead of just the use site. Costs roughly
2× extra slowdown.

**Q12. You have a custom pool/slab allocator — Memcheck shows one big block.
How do you get per-object tracking?**
Annotate the allocator with the client-request macros
`VALGRIND_MALLOCLIKE_BLOCK` / `VALGRIND_FREELIKE_BLOCK` from
`<valgrind/memcheck.h>` so Memcheck treats your sub-allocations as real blocks
with redzones and leak tracking.

**Q13. Can you trigger a leak check while the program is still running?**
Yes — start with `--vgdb=yes --vgdb-error=0`, attach gdb via
`target remote | vgdb`, and issue `monitor leak_check full reachable any`. Ideal
for long-running daemons; snapshot and diff leaks over time.

**Q14. Why `-g` and why avoid `-O2` for Valgrind runs?**
`-g` gives file/line/symbol info for readable stack traces. High optimization
inlines and reorders code, blurring traces and occasionally producing spurious
uninitialized-value reports; `-O0`/`-O1` keeps reports aligned with source.

**Q15. Does Memcheck catch stack buffer overflows?**
Poorly, compared to heap. Its strength is the heap (redzones around `malloc`
blocks). Stack and global overflows are better caught by ASan. Use both tools as
complements.

---

## 14. Quick-Reference Cheat Sheets

### Essential command

```sh
gcc -g -O0 -o app app.c
valgrind --leak-check=full --show-leak-kinds=all \
         --track-origins=yes --error-exitcode=1 ./app
```

### Flag cheat sheet

| Goal | Flag |
|------|------|
| Per-block leak traces | `--leak-check=full` |
| All four leak kinds | `--show-leak-kinds=all` |
| Origin of uninit values | `--track-origins=yes` |
| Fail CI on any error | `--error-exitcode=1` |
| Only definite/indirect count as errors | `--errors-for-leak-kinds=definite,indirect` |
| Write report to file (per PID) | `--log-file=vg.%p.log` |
| Machine-readable | `--xml=yes --xml-file=vg.xml` |
| Generate suppressions | `--gen-suppressions=all` |
| Apply suppressions | `--suppressions=my.supp` |
| Deeper stack traces | `--num-callers=40` |
| Follow exec'd children | `--trace-children=yes` |
| On-demand leak check via gdb | `--vgdb=yes --vgdb-error=0` |

### Leak-kind decision table

| Report says | Action |
|-------------|--------|
| **definitely lost** | Real bug — add the missing `free`. |
| **indirectly lost** | Fix the parent block; these vanish. |
| **possibly lost** | Inspect — interior pointer; may be intentional. |
| **still reachable** | Usually benign (globals/libc); decide by policy. |

### Embedded workflow

```
1. Add Valgrind to image   → Yocto IMAGE_INSTALL / Buildroot BR2_PACKAGE_VALGRIND
                              (or cross-compile: ./configure --host=aarch64-...)
2. Copy lib/valgrind/ tree  → onto target rootfs (mandatory)
3. Build app with -g        → matching cross toolchain
4. Run on target            → valgrind --leak-check=full ./app   (mind RAM ≈2×, CPU 10–50×)
5. Long-running daemon?      → --vgdb + monitor leak_check, snapshot & diff
6. Too small to run?         → bigger same-arch board, host/QEMU harness, or ASan/LSan/mtrace
```

### Internals in one line each

- **Core (coregrind):** owns the process, intercepts malloc/syscalls, schedules guest threads.
- **VEX:** disassemble guest → IR → instrument → recompile to host, cached per block.
- **Memcheck:** A-bits (addressable?) + V-bits (initialized?) + heap-block table.
- **Leak check:** conservative pointer scan from roots → unreached blocks = leaks.

---

## See Also

- [ARM64 Exception Handling in the Linux Kernel](Exception_Handling_ARM64.md) —
  the hardware entry mechanism beneath syscalls like `brk`/`mmap` that the heap
  allocator (and thus Valgrind's tracking) ultimately rides on.
- Valgrind manual: `man valgrind`, and the Memcheck section of the official user
  manual (`docs/` in the Valgrind tree).
