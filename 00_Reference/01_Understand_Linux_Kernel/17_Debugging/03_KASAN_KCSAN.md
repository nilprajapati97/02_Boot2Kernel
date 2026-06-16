# 03 — KASAN and KCSAN

## 1. KASAN — Kernel Address Sanitizer

**KASAN** detects **memory access bugs** at runtime:
- Use-after-free
- Out-of-bounds accesses (heap, stack, global)
- Use-after-scope

### How it works:
- Shadow memory maps 1 byte → 8 bytes of kernel memory
- Every memory access instrumented to check shadow byte
- ~2x memory overhead, ~2-3x slowdown
- Only for debugging (not production)

---

## 2. KASAN Example Output

```
==================================================================
BUG: KASAN: slab-out-of-bounds in mydriver_write+0x58/0xa0 [mydrv]
Write of size 4 at addr ffff8880362a0060 by task bash/1234

CPU: 2 PID: 1234 Comm: bash
Hardware name: QEMU Standard PC

Call Trace:
 kasan_report+0xf2/0x130
 mydriver_write+0x58/0xa0 [mydrv]
 vfs_write+0xb8/0x1a0
 ksys_write+0x68/0xe0

Allocated by task 1234:
 kmalloc+0x1c/0x30
 mydriver_probe+0x45/0x120

The buggy address belongs to the object at ffff8880362a0040
 which belongs to the cache kmalloc-32 of size 32
==================================================================
```

---

## 3. KASAN Configuration

```bash
CONFIG_KASAN=y
CONFIG_KASAN_GENERIC=y          # Software (all arches)
# or CONFIG_KASAN_HW_TAGS=y     # Hardware (ARM MTE)

# Extra checks:
CONFIG_KASAN_STACK=y            # Stack instrumentation
CONFIG_KASAN_VMALLOC=y          # vmalloc region checks
```

---

## 4. KASAN Modes

| Mode | Description |
|------|-------------|
| Generic | Pure software; works everywhere |
| Software tag-based | ARM64 ptr tagging |
| Hardware tag-based | ARM64 MTE hardware; ~0% overhead |

---

## 5. KCSAN — Kernel Concurrency Sanitizer

**KCSAN** detects **data races** — concurrent unsynchronized accesses:

```
==================================================================
BUG: KCSAN: data-race in mydriver_read / mydriver_write

write to 0xffff8880... of 4 bytes by task 1234 on cpu 1:
 mydriver_write+0x30/0xa0

read to 0xffff8880... of 4 bytes by task 5678 on cpu 0:
 mydriver_read+0x20/0x80
==================================================================
```

---

## 6. KCSAN Configuration

```bash
CONFIG_KCSAN=y
CONFIG_KCSAN_REPORT_ONCE_IN_MS=3000  # Throttle reports
CONFIG_KCSAN_ASSUME_PLAIN_WRITES_ATOMIC=y
```

---

## 7. KCSAN — What it Detects vs Misses

| Scenario | Detected? |
|----------|-----------|
| Plain concurrent read/write | ✅ Yes |
| Declared `__data_racy` | ❌ No (suppressed) |
| WRITE_ONCE / READ_ONCE | ❌ No (intentional) |
| atomic_t operations | ❌ No (safe) |

```c
/* Suppress intentional benign races */
int __data_racy counter;   /* OK to race */

/* Or use WRITE_ONCE/READ_ONCE for lockless code */
WRITE_ONCE(shared_var, val);
val = READ_ONCE(shared_var);
```

---

## 8. KASAN vs KCSAN

| Tool | Detects | Overhead |
|------|---------|----------|
| KASAN | Memory bugs (OOB, UAF) | 2-3x |
| KCSAN | Data races | ~10-30% |
| KFENCE | Sampled OOB/UAF (production) | ~0% |

---

## 9. KFENCE (Production Safety Net)

```bash
# KFENCE: low-overhead sampling-based memory safety tool
CONFIG_KFENCE=y
# Sample one allocation per 100ms (tunable via boot param)
kfence.sample_interval=100
```

---

## 10. Source Files

| File | Description |
|------|-------------|
| `mm/kasan/` | KASAN implementation |
| `kernel/kcsan/` | KCSAN implementation |
| `mm/kfence/` | KFENCE implementation |
| `Documentation/dev-tools/kasan.rst` | KASAN docs |

---

## 11. Related Topics
- [01_printk.md](./01_printk.md)
- [../08_Intro_To_Kernel_Synchronization/02_Race_Conditions.md](../08_Intro_To_Kernel_Synchronization/02_Race_Conditions.md)
