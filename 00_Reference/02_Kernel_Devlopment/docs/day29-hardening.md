# Day 29 — Hardening, Testing & Diagnostics

> **Goal**: Tighten the kernel before release. Add KASLR-lite, stack canaries (`-fstack-protector-strong` for kernel), W^X enforcement, lockdep-lite checks, ubsan, basic kunit-style self-tests, and a robust panic / backtrace / oops dump.
>
> **Why today**: A kernel that boots ≠ a kernel that survives a week of `stress-ng`. This is the day you stop writing features and start breaking your code on purpose.

---

## 1. Background

### 1.1 Defensive layers we add
| Layer | Mechanism |
|---|---|
| Compile-time | `-Wall -Wextra -Werror -Wformat=2 -Wundef -fstack-protector-strong` |
| Link-time | `--no-undefined`, `--orphan-handling=error`, sorted symbol map |
| Runtime | KASLR slide on boot, `__ro_after_init` section, stack canary check, W^X mappings, `BUG()` with stack trace, ubsan handlers |
| Test | `kunit_run_all()` early-init unit tests + `selftests` directory |

### 1.2 KASLR-lite
Pick a random VA slide `delta` (multiple of 2 MiB) at boot from `CNTPCT_EL0` + FDT-provided seed; relink the kernel's high-half base. Implementation in stages:
1. Build PIC-like: use `-fno-pic -mcmodel=large` + `-Wl,-pie` is incompatible — we instead build a position-dependent kernel and re-locate at boot by patching a small relocation table.
2. Pre-compute `R_AARCH64_RELATIVE` entries via linker `--emit-relocs`, then `apply_relocs(delta)` runs before MMU enable.

For Day 29's first pass we settle for **3 candidate slots** (0, 0x100000, 0x200000) and a coin-flip — full ASLR is stretch.

### 1.3 W^X enforcement audit
Scan every kernel mapping after MMU enable and assert:
- `.text` is `PXN=0`, `AP_RO`.
- `.rodata` is `PXN=1`, `AP_RO`.
- `.data`/`.bss` is `PXN=1`, `AP_RW`.
- direct-map of RAM is `PXN=1`. (No execute from RAM heap!)

A boot-time function walks the page tables and `BUG()` if any violation.

### 1.4 Stack canaries
GCC's `-fstack-protector-strong` emits prologue/epilogue checks against `__stack_chk_guard`. Provide:
```c
unsigned long __stack_chk_guard = 0x595e9fbd94fda766UL;  /* randomized at boot */
void __stack_chk_fail(void){ panic("stack canary corrupt"); }
```
At early boot:
```c
__stack_chk_guard = (u64)read_cntpct_el0() * 0x9E3779B97F4A7C15ULL;
```

---

## 2. Implementation

### 2.1 `__ro_after_init`
```c
#define __ro_after_init __attribute__((section(".data..ro_after_init")))
```
Linker:
```
.rodata : { *(.rodata*) *(.data..ro_after_init) }
```
So values written once at init become read-only.

### 2.2 Page-table audit
```c
struct audit_ctx { int violations; };

static void audit_pte(u64 va, u64 pte, struct audit_ctx *c)
{
    bool x = !(pte & PTE_PXN);
    bool w = (pte & PTE_AP_RW_KERNEL);
    if (x && w) {
        printk(KERN_ERR "W^X violation @ 0x%llx (pte=0x%llx)\n", va, pte);
        c->violations++;
    }
}

void wx_audit(void) {
    struct audit_ctx c = {0};
    walk_kernel_pgtables(audit_pte, &c);
    if (c.violations) panic("W^X audit failed");
}
```

### 2.3 BUG()/oops + frame-pointer unwinder
```c
#define BUG() do { printk("BUG at %s:%d\n", __FILE__, __LINE__); dump_stack(); panic("BUG"); } while(0)
#define BUG_ON(c) do { if (c) BUG(); } while(0)

void dump_stack(void)
{
    u64 fp; asm volatile("mov %0, x29":"=r"(fp));
    printk("Call trace:\n");
    for (int i = 0; i < 32 && fp; i++) {
        u64 next_fp = *((u64*)fp);
        u64 lr      = *((u64*)(fp + 8));
        printk("  [<%016llx>] %s\n", lr, kallsyms_lookup(lr));
        if (next_fp <= fp) break;
        fp = next_fp;
    }
}
```

`kallsyms_lookup` is a sorted symbol table generated at build:
```
nm -n vmlinux | awk '{print $1, $3}' > kallsyms.tbl
xxd -i kallsyms.tbl > kernel/kallsyms_data.c
```

### 2.4 UBSAN runtime
Compile with `-fsanitize=undefined -fsanitize-undefined-trap-on-error` is the easiest: every UB triggers `brk`, which our exception handler converts to a printable trap. Otherwise implement specific handlers:
```c
void __ubsan_handle_out_of_bounds(void *data, void *index) { printk(KERN_WARN "ubsan: OOB at %p idx %p\n", data, index); }
void __ubsan_handle_shift_out_of_bounds(void *data, void *l, void *r) { printk(KERN_WARN "ubsan: shift\n"); }
void __ubsan_handle_divrem_overflow(void *data, void*l, void*r){ printk(KERN_WARN "ubsan: div\n"); }
void __ubsan_handle_load_invalid_value(void*d,void*v){ printk(KERN_WARN "ubsan: load_invalid\n"); }
void __ubsan_handle_pointer_overflow(void*d,void*l,void*r){ printk(KERN_WARN "ubsan: ptr_overflow\n"); }
void __ubsan_handle_type_mismatch_v1(void*d,void*p){ printk(KERN_WARN "ubsan: type_mismatch p=%p\n", p); }
```

### 2.5 lockdep-lite
Lightweight tracker: each spinlock has an `id`. On `spin_lock`, push id onto per-CPU stack; on `spin_unlock`, must pop the top. Mismatched order across calls indicates a potential AB-BA. Print warning, not fatal.
```c
#define MAX_HELD 8
struct held_lock { int id; void *caller; };
struct held_lock held[NR_CPUS][MAX_HELD];
int    n_held[NR_CPUS];

void lockdep_acquire(spinlock_t *l) {
    int cpu = smp_processor_id();
    if (n_held[cpu] && held[cpu][n_held[cpu]-1].id > l->dbg_id)
        printk(KERN_WARN "lockdep: inversion holding %d, now %d\n",
               held[cpu][n_held[cpu]-1].id, l->dbg_id);
    held[cpu][n_held[cpu]++] = (struct held_lock){l->dbg_id, __builtin_return_address(0)};
}
```

### 2.6 In-kernel self-tests (`kunit`-lite)
```c
struct ktest { const char *name; int (*fn)(void); };
extern struct ktest __start_ktests[], __stop_ktests[];

#define KTEST(name) \
    static int test_##name(void); \
    static struct ktest __ktest_##name __attribute__((section("__ktests"),used)) = {#name, test_##name}; \
    static int test_##name(void)

KTEST(buddy_alloc_free) {
    phys_addr_t a = alloc_pages(2);
    BUG_ON(!a);
    free_pages(a, 2);
    return 0;
}
KTEST(slab_alloc) {
    void *p = kmalloc(123, 0); BUG_ON(!p); kfree(p); return 0;
}
KTEST(vfs_path_root) {
    BUG_ON(!path_lookup("/"));
    return 0;
}

void run_ktests(void) {
    int pass = 0, fail = 0;
    for (struct ktest *t = __start_ktests; t < __stop_ktests; t++) {
        printk("ktest %s ... ", t->name);
        int r = t->fn();
        if (!r) { printk("OK\n"); pass++; } else { printk("FAIL\n"); fail++; }
    }
    printk("ktests: %d ok, %d fail\n", pass, fail);
}
```

Linker:
```
__ktests : { __start_ktests = .; *(__ktests) __stop_ktests = .; }
```

### 2.7 Userspace selftests
A directory `selftests/` shipped in initramfs:
```
selftests/
  mm_brk        # brk grow + shrink
  io_rw         # write/read a file then verify
  fork_wait     # 100x fork → exit → wait4
  signals       # SIGSEGV expected
  smp_stress    # 4 spinners + 4 yielders
```
`init` (after first shell) runs `/selftests/run` and prints pass/fail summary.

### 2.8 Fuzz inputs
A tiny in-kernel fuzzer for the FDT parser, ELF loader, and cpio parser. Each subsystem takes a `const u8 *blob, u64 size` entry; flip random bits to ensure no panics on malformed input — only `-EINVAL` returns.

---

## 3. Pitfalls

1. **Stack canary before guard is randomized**: GCC emits checks even in `early_init`. Initialize `__stack_chk_guard` *before* the first function returns from `kmain`. Mark `kmain` with `__attribute__((no_stack_protector))`.
2. **Symbols stripped**: `dump_stack` needs symbols. Keep an internal `kallsyms` table even after release; gate verbose backtrace behind config.
3. **ubsan brings out latent bugs**: expect a flood when first enabled. Triage, suppress with `__attribute__((no_sanitize("undefined")))` only when proven safe.
4. **W^X audit too strict for early boot**: turn it on **after** identity map teardown. Otherwise the trampoline page is X+R.
5. **ktest at boot blocks shell**: gate on a kernel cmdline arg `selftest=1` (we parse `/chosen/bootargs` already).

---

## 4. Verification

```
$ qemu ... -append "selftest=1"
[INFO] kallsyms: 412 entries
[INFO] W^X audit: pass
[INFO] ubsan: enabled (trap mode)
ktest buddy_alloc_free ... OK
ktest slab_alloc ... OK
ktest vfs_path_root ... OK
ktest fork_wait ... OK
ktests: 17 ok, 0 fail
```

Inject a deliberate bug (`*(int*)0xDEAD = 1` from kernel) and verify backtrace shows file:line of the culprit.

---

## 5. Stretch

- KASAN-lite shadow byte allocator.
- KCSAN-style data-race detector for spinlocks (random sampling).
- Add `panic_on_warn` cmdline knob.
- Boot-time integrity hash of `.text` printed before going user.

---

## 6. References

- *Linux Kernel Self-Protection* documentation (Documentation/security/self-protection.rst).
- Kees Cook's KSPP talks.
- *xv6 book* — minimal sanity-check patterns.
