# Scenario 2: BUG() / BUG_ON() Assertion Failure

## Symptom

```
[ 5678.234567] ------------[ cut here ]------------
[ 5678.234572] kernel BUG at drivers/my_driver/core.c:245!
[ 5678.234578] Internal error: Oops - BUG: 00000000f2000800 [#1] PREEMPT SMP
[ 5678.234585] Modules linked in: my_module(O) dm_crypt xfs
[ 5678.234592] CPU: 1 PID: 2891 Comm: my_daemon Tainted: G           O      6.8.0 #1
[ 5678.234598] Hardware name: ARM Platform (DT)
[ 5678.234602] pstate: 60400005 (nZCv daif +PAN -UAO -TCO -DIT -SSBS BTYPE=--)
[ 5678.234607] pc : my_state_machine+0x1b0/0x300 [my_module]
[ 5678.234612] lr : my_state_machine+0x1ac/0x300 [my_module]
[ 5678.234617] sp : ffff80001234bc60
[ 5678.234620] ...
[ 5678.234630] Call trace:
[ 5678.234632]  my_state_machine+0x1b0/0x300 [my_module]
[ 5678.234637]  process_command+0x88/0x120 [my_module]
[ 5678.234642]  my_driver_ioctl+0xc4/0x200 [my_module]
[ 5678.234647]  __arm64_sys_ioctl+0xa8/0xe0
[ 5678.234651]  invoke_syscall+0x50/0x120
[ 5678.234655]  el0_svc_common+0x48/0xf0
[ 5678.234659]  do_el0_svc+0x28/0x40
[ 5678.234663]  el0t_64_sync_handler+0x68/0xc0
[ 5678.234667]  el0t_64_sync+0x1a0/0x1a4
[ 5678.234672] Code: aa0003e1 97ffffe0 f9400a60 35000060 (d4210000)
[ 5678.234678]                                            ^^^^^^^^^^
[ 5678.234680]                                            BRK #0x800
[ 5678.234685] ---[ end trace 0000000000000000 ]---
```

### How to Recognize
- **`kernel BUG at <file>:<line>!`** — exact source location provided
- **`Oops - BUG`** (not `Oops - undefined instruction`)
- **`d4210000`** in the Code line = `BRK #0x800` (ARM64 BUG instruction)
- **`------------[ cut here ]------------`** marker before the message
- Developer **intentionally** put `BUG()` or `BUG_ON()` here — it's an assertion

---

## Background: BUG() / BUG_ON() / WARN() / WARN_ON()

### The Assertion Family
```c
/* BUG() — unconditional crash: "this should NEVER happen" */
BUG();
// → Always triggers Oops, kills task (or panics)

/* BUG_ON(condition) — conditional crash */
BUG_ON(list_empty(&my_list));
// → If list is empty: Oops
// → If list not empty: continue normally

/* WARN() — print warning but continue */
WARN(refcount < 0, "negative refcount: %d\n", refcount);
// → Prints stack trace + message, kernel continues

/* WARN_ON(condition) — conditional warning */
WARN_ON(irqs_disabled());
// → If IRQs disabled: print warning, continue
// → Adds taint flag 'W'

/* WARN_ON_ONCE(condition) — warn once only */
WARN_ON_ONCE(size > MAX_SIZE);
// → First time: print warning
// → Subsequent times: silent
```

### ARM64 Implementation
```c
// arch/arm64/include/asm/bug.h

#define __BUG_FLAGS(flags) do {                         \
    asm volatile(                                        \
        "1:     brk     %[imm]\n"                       \
        ".pushsection __bug_table, \"aw\"\n"            \
        "2:     .long   1b - 2b\n"     /* insn offset */\
        "       .long   %[file] - 2b\n" /* file str  */\
        "       .short  %[line]\n"      /* line number*/\
        "       .short  %[flags]\n"     /* flags      */\
        ".popsection\n"                                 \
        : : [imm] "i" (BUG_BRK_IMM),                   \
            [file] "i" (__FILE__),                      \
            [line] "i" (__LINE__),                      \
            [flags] "i" (flags));                       \
    unreachable();                                      \
} while (0)

#define BUG() __BUG_FLAGS(0)
#define BUG_ON(cond) do { if (unlikely(cond)) BUG(); } while (0)

// BUG_BRK_IMM = 0x800
// So: BRK #0x800 → ESR EC = 0x3C (BRK64)
```

### Bug Table Structure
```
Section: __bug_table
┌────────────────────────────────────────────┐
│ instruction offset │ relative to this entry│
│ filename string    │ "drivers/my/core.c"   │
│ line number        │ 245                   │
│ flags              │ 0 = BUG, BUGFLAG_WARN │
└────────────────────────────────────────────┘

When BRK #0x800 is hit:
1. Handler looks up PC in __bug_table
2. Finds file + line → prints "kernel BUG at drivers/my/core.c:245!"
3. Checks flags → BUG or WARN
```

---

## Code Flow: BRK → Oops

```mermaid
flowchart TD
    A["BUG_ON(condition) evaluates TRUE"] --> B["asm: BRK #0x800"]
    B --> C["Exception: ESR EC=0x3C (BRK64)"]
    C --> D["el1h_64_sync_handler()"]
    D --> E["el1_dbg(regs, esr)"]
    E --> F["do_debug_exception()<br>→ call_break_hook()"]
    F --> G["bug_handler(regs, esr)"]
    G --> H["is_valid_bugaddr(regs->pc)?"]
    H -->|Yes| I["report_bug(regs->pc, regs)"]
    I --> J["Lookup in __bug_table"]
    J --> K{Flags?}
    K -->|BUG (no WARN flag)| L["printk: kernel BUG at file:line"]
    L --> M["die('Oops - BUG', regs, esr)"]
    K -->|WARN flag| N["printk: WARNING: at file:line"]
    N --> O["Return — kernel continues"]

    style M fill:#f80,color:#fff
    style O fill:#4a4,color:#fff
```

### bug_handler()
```c
// arch/arm64/kernel/brk.c + kernel/bug.c

static int bug_handler(struct pt_regs *regs, unsigned long esr)
{
    switch (report_bug(regs->pc, regs)) {
    case BUG_TRAP_TYPE_BUG:
        die("Oops - BUG", regs, esr);
        break;
    case BUG_TRAP_TYPE_WARN:
        break;  // WARN — just return
    default:
        break;
    }
    return DBG_HOOK_HANDLED;
}

// kernel/bug.c
enum bug_trap_type report_bug(unsigned long bugaddr, struct pt_regs *regs)
{
    struct bug_entry *bug;

    bug = find_bug(bugaddr);  // look up in __bug_table
    if (!bug)
        return BUG_TRAP_TYPE_NONE;

    if (bug->flags & BUGFLAG_WARNING) {
        // WARN path:
        __warn(file, line, __builtin_return_address(0),
               TAINT_WARN, regs, NULL);
        return BUG_TRAP_TYPE_WARN;
    }

    // BUG path:
    printk(KERN_EMERG "kernel BUG at %s:%u!\n",
           bug->file, bug->line);
    return BUG_TRAP_TYPE_BUG;
}
```

---

## Common Causes

### 1. Invalid State Machine Transition
```c
void my_state_machine(struct my_device *dev, enum event evt)
{
    switch (dev->state) {
    case STATE_IDLE:
        if (evt == EVT_START)
            dev->state = STATE_RUNNING;
        break;
    case STATE_RUNNING:
        if (evt == EVT_STOP)
            dev->state = STATE_IDLE;
        break;
    default:
        BUG();  // Should never reach here — unknown state!
        // If dev->state is corrupted → BUG triggers
    }
}
```

### 2. Unexpected NULL That "Can't Happen"
```c
struct page *alloc_my_page(gfp_t gfp)
{
    struct page *page = alloc_page(gfp | __GFP_NOFAIL);
    BUG_ON(!page);  // __GFP_NOFAIL should never return NULL
    // But under extreme OOM, it CAN fail in some code paths
    return page;
}
```

### 3. Refcount Reaching Impossible Value
```c
void put_my_object(struct my_object *obj)
{
    int old_count = atomic_dec_return(&obj->refcount);
    BUG_ON(old_count < 0);  // Refcount went negative → double-put!
}
```

### 4. Assertion in Core Kernel Code
```c
// mm/page_alloc.c — kernel validates page state:
static inline void __free_one_page(struct page *page, ...)
{
    // Check: page must not be in a bad state
    VM_BUG_ON_PAGE(PageTail(page), page);
    VM_BUG_ON_PAGE(compound_order(page) != order, page);
    // If page metadata is corrupted → BUG
}

// VM_BUG_ON_PAGE() is BUG_ON() that only exists with CONFIG_DEBUG_VM=y
```

### 5. Locking Assertion Failure
```c
void my_critical_section(struct my_device *dev)
{
    lockdep_assert_held(&dev->lock);
    // BUG if we entered without holding dev->lock
    // This is a developer assertion — "caller MUST hold lock"

    // ... access protected data ...
}
```

---

## BUG() vs WARN() — When to Use Which

| Macro | Effect | Use When |
|-------|--------|----------|
| `BUG()` | Oops (kill task or panic) | **State is unrecoverably corrupt** — continuing would be worse |
| `BUG_ON(cond)` | Oops if true | Condition means **data corruption is imminent** |
| `WARN()` | Warning + stack trace | Something wrong but **kernel can recover** |
| `WARN_ON(cond)` | Warning if true | Unexpected but **not dangerous to continue** |
| `WARN_ON_ONCE(cond)` | Warn only first time | Don't flood logs with repeated warnings |

**Kernel community preference**: `WARN()` is almost always preferred over `BUG()`. Linus Torvalds has stated:

> *"BUG() is only appropriate when there is no way to recover and continuing would cause worse damage (e.g., filesystem corruption). In most cases, WARN_ON() + return error is better."*

---

## Debugging Steps

### Step 1: Read the BUG Location
```
kernel BUG at drivers/my_driver/core.c:245!
                ^^^^^^^^^^^^^^^^^^^^^^^^^
                Exact file and line number!

This is the easiest crash to debug — you know EXACTLY where it is.
```

### Step 2: Look at the Source Code
```c
// drivers/my_driver/core.c, line 245:
244:    default:
245:        BUG();  // ← This line
246:    }

// The question is: WHY did execution reach the default case?
// → dev->state must have an unexpected value
```

### Step 3: Check Registers for the State Variable
```
From Oops: check which register holds dev->state
Use objdump to correlate:

objdump -dS my_module.ko | less
# Find the switch statement → which register was loaded
# Then check that register in the Oops register dump
```

### Step 4: Determine if This is a Known Pattern
```bash
# Search kernel bugzilla and mailing lists:
# "kernel BUG at drivers/my_driver/core.c:245"
# Often this exact BUG has been reported and fixed upstream
```

### Step 5: Check if WARN Would Be Better
```c
/* Many BUG() calls should be WARN() + error return: */

/* BEFORE: aggressive BUG */
void process(struct my_device *dev) {
    BUG_ON(dev->state >= MAX_STATES);
    // → kills task, potentially orphans resources
}

/* AFTER: graceful WARN + recovery */
void process(struct my_device *dev) {
    if (WARN_ON(dev->state >= MAX_STATES)) {
        dev->state = STATE_IDLE;  // Reset to known good state
        return;
    }
}
```

### Step 6: Enable CONFIG_DEBUG_BUGVERBOSE
```bash
CONFIG_DEBUG_BUGVERBOSE=y
# Ensures file + line info is included in the BUG message
# Without this, you only get the PC address (harder to debug)
```

---

## WARN() Path — Non-Fatal Alternative

```
[ 5678.234567] ------------[ cut here ]------------
[ 5678.234570] WARNING: CPU: 1 PID: 2891 at drivers/my_driver/core.c:245 my_function+0x44/0x100
[ 5678.234580] Modules linked in: my_module(O)
[ 5678.234585] CPU: 1 PID: 2891 Comm: my_daemon Tainted: G        W  O      6.8.0 #1
                                                          ^        ^
                                                          |        W = warning issued
                                                          G = proprietary module
[ 5678.234600] Call trace:
[ 5678.234602]  my_function+0x44/0x100 [my_module]
[ 5678.234608]  caller_function+0x30/0x80 [my_module]
[ 5678.234612]  ...
[ 5678.234618] ---[ end trace 0000000000000000 ]---
```

**Key differences from BUG():**
- Says `WARNING` not `BUG`
- Taint flag `W` added (not `D`)
- Task is **NOT killed** — execution continues
- No panic regardless of context

---

## Fixes

| Cause | Fix |
|-------|-----|
| Invalid state | Validate state transitions; add bounds checking |
| Corrupt data | Find the corruption source (KASAN, lockdep) |
| Refcount bug | Use `refcount_t`; audit get/put pairs |
| "Can't happen" happened | Handle gracefully; use WARN + return error |
| Third-party module BUG | Update module; report to vendor |
| Overaggressive assertion | Replace `BUG_ON` with `WARN_ON` + recovery |

### Fix Example: Replace BUG with WARN + Recovery
```c
/* BEFORE: crashes the task */
void handle_event(struct my_device *dev, int event)
{
    switch (event) {
    case EVENT_A: handle_a(dev); break;
    case EVENT_B: handle_b(dev); break;
    default:
        BUG();  // Unknown event → Oops!
    }
}

/* AFTER: warn and handle gracefully */
void handle_event(struct my_device *dev, int event)
{
    switch (event) {
    case EVENT_A: handle_a(dev); break;
    case EVENT_B: handle_b(dev); break;
    default:
        WARN(1, "unknown event %d in state %d\n",
             event, dev->state);
        return;  // Skip unknown event, don't crash
    }
}
```

### Fix Example: Replace BUG_ON with Error Return
```c
/* BEFORE: BUG_ON in allocator wrapper */
struct page *my_alloc(gfp_t gfp)
{
    struct page *p = alloc_page(gfp);
    BUG_ON(!p);  // Crashes on OOM!
    return p;
}

/* AFTER: return NULL, let caller handle */
struct page *my_alloc(gfp_t gfp)
{
    struct page *p = alloc_page(gfp);
    if (WARN_ON_ONCE(!p))
        return NULL;  // Caller checks for NULL
    return p;
}
```

---

## Quick Reference

| Item | Value |
|------|-------|
| BUG message | `kernel BUG at <file>:<line>!` |
| Oops type | `Oops - BUG` |
| ARM64 instruction | `BRK #0x800` → `d4210000` |
| ESR EC | `0x3C` (BRK64) — different from undefined (0x00) |
| Data stored in | `__bug_table` section |
| Handler | `bug_handler()` → `report_bug()` |
| WARN taint | `W` flag |
| BUG taint | `D` flag |
| Config | `CONFIG_DEBUG_BUGVERBOSE=y` for file:line info |
| VM_BUG_ON | Only with `CONFIG_DEBUG_VM=y` (debug builds) |
| Kernel policy | Prefer `WARN_ON` over `BUG_ON` in almost all cases |
