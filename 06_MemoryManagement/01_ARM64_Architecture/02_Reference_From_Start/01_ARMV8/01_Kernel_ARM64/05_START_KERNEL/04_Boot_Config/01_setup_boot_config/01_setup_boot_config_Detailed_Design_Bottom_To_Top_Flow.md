# setup_boot_config() — Detailed Design: Bottom-To-Top Flow
## Source: init/main.c | Kernel: Linux ARM/ARM64

---

## Table of Contents

1. [Purpose and Position in Boot Sequence](#1-purpose-and-position-in-boot-sequence)
2. [Bootconfig Concept — What Problem It Solves](#2-bootconfig-concept--what-problem-it-solves)
3. [Initrd Memory Layout and Trailer Format](#3-initrd-memory-layout-and-trailer-format)
4. [Bottom Layer: get_boot_config_from_initrd()](#4-bottom-layer-get_boot_config_from_initrd)
5. [Bottom Layer: xbc_get_embedded_bootconfig()](#5-bottom-layer-xbc_get_embedded_bootconfig)
6. [Bottom Layer: parse_args() + bootconfig_params()](#6-bottom-layer-parse_args--bootconfig_params)
7. [Middle Layer: xbc_init() — Building the Parse Tree](#7-middle-layer-xbc_init--building-the-parse-tree)
8. [Middle Layer: xbc_make_cmdline()](#8-middle-layer-xbc_make_cmdline)
9. [Top Layer: setup_boot_config() — Full Control Flow](#9-top-layer-setup_boot_config--full-control-flow)
10. [Downstream: How extra_command_line Flows into setup_command_line()](#10-downstream-how-extra_command_line-flows-into-setup_command_line)
11. [Cleanup: exit_boot_config() and xbc_exit()](#11-cleanup-exit_boot_config-and-xbc_exit)
12. [CONFIG_BOOT_CONFIG Disabled Path](#12-config_boot_config-disabled-path)
13. [Global Variable Lifecycle Table](#13-global-variable-lifecycle-table)
14. [Complete Call Graph](#14-complete-call-graph)

---

## 1. Purpose and Position in Boot Sequence

### Position in start_kernel()

```
start_kernel()
    │
    ├─ setup_arch(&command_line)        ← arch-specific HW init, DTB parse
    │
    ├─ setup_boot_config()              ← THIS FUNCTION
    │       Finds bootconfig in initrd (or embedded)
    │       Parses it into a key-value tree
    │       Extracts kernel.* → extra_command_line
    │       Extracts init.*   → extra_init_args
    │
    ├─ setup_command_line(command_line) ← merges extra_command_line + boot_command_line
    │
    ├─ parse_early_param()
    ├─ parse_args("Booting kernel", ...)
    │
    └─ ...
```

### Why It Must Run After setup_arch() and Before setup_command_line()

- `setup_arch()` populates `initrd_start` and `initrd_end` (physical → virtual mapped
  addresses of the initrd image in memory). `setup_boot_config()` reads the end of the
  initrd to detect the bootconfig trailer — so initrd must be located first.
- `setup_command_line()` builds `saved_command_line` and `static_command_line` by
  prepending `extra_command_line` (set by `setup_boot_config()`). If bootconfig ran
  after `setup_command_line()`, the extra kernel parameters would be lost.

---

## 2. Bootconfig Concept — What Problem It Solves

### The Kernel Command Line Length Limit

The kernel command line (`boot_command_line[]`) is limited to `COMMAND_LINE_SIZE`
(typically 2048 bytes on ARM, 4096 bytes on ARM64). This limit is set at compile time:

```c
/* arch/arm64/include/asm/setup.h */
#define COMMAND_LINE_SIZE 2048

/* Some configs raise this to 4096 */
```

For embedded systems with many device-specific parameters, or for complex container/
cloud-native boot environments where dozens of `sysctl.*` settings must be passed,
this 2 KB limit is insufficient.

### The Bootconfig Solution

Bootconfig is a structured key-value configuration format appended to the initrd image.
It is **not** subject to the command line length limit. The bootconfig data:

- Can be up to `XBC_DATA_MAX` = 32767 bytes (32 KB - 1)
- Uses a hierarchical dot-notation key format: `kernel.printk = "7"`
- Is appended to the initrd as a structured trailer with magic, size, and checksum
- Keys under `kernel.*` namespace become additional kernel parameters
- Keys under `init.*` namespace become additional init(1) arguments

### Bootconfig File Format Example

```
# /etc/bootconfig (assembled by mkinitramfs/dracut)
kernel {
    printk.devkmsg = "on"
    loglevel = "7"
    panic = "5"
    crashkernel = "256M"
}

init {
    systemd.unified_cgroup_hierarchy = "1"
    rd.lvm.lv = "vg/root"
}
```

After parsing, `kernel.printk.devkmsg=on kernel.loglevel=7 ...` is prepended to
`boot_command_line` as `extra_command_line`.

---

## 3. Initrd Memory Layout and Trailer Format

### Physical Memory Layout After Bootloader Loads Initrd

```
Physical RAM (example ARM64 system, 4 GB)
┌──────────────────────────────────────────┐  0x0000_0000_0000_0000
│  Reserved (first 1MB on many SoCs)       │
├──────────────────────────────────────────┤  0x0000_0000_0010_0000
│  Kernel Image (vmlinuz / Image)          │  loaded by bootloader
│  e.g., 0x80000 (ARM64 default load addr) │
├──────────────────────────────────────────┤
│  ...                                     │
├──────────────────────────────────────────┤  initrd_start
│  initrd / initramfs image                │
│  ┌──────────────────────────────────┐    │
│  │ cpio archive (rootfs)            │    │
│  ├──────────────────────────────────┤    │
│  │ [BOOTCONFIG DATA]                │    │
│  │   e.g., "kernel {\n loglevel=7}" │    │
│  ├──────────────────────────────────┤    │
│  │ [padding to 4-byte alignment]    │    │
│  ├──────────────────────────────────┤    │
│  │ u32 size  (LE, bootconfig bytes) │    │
│  │ u32 csum  (LE, byte-sum checksum)│    │
│  ├──────────────────────────────────┤    │
│  │ "#BOOTCONFIG\n" (12 bytes magic) │    │  ← initrd_end - 12
│  └──────────────────────────────────┘    │  ← initrd_end
├──────────────────────────────────────────┤  initrd_end
│  ...                                     │
└──────────────────────────────────────────┘
```

### Trailer Structure in Detail

```
Offset from initrd_end:
  [initrd_end - 12]  = "#BOOTCONFIG\n"     (BOOTCONFIG_MAGIC, 12 bytes)
  [initrd_end - 12 - 4] = checksum (u32 LE)
  [initrd_end - 12 - 8] = size     (u32 LE)
  [initrd_end - 12 - 8 - size] = bootconfig text data (ASCII)
```

### GRUB Alignment Issue

The comment in the code notes:
```c
/*
 * Since Grub may align the size of initrd to 4, we must
 * check the preceding 3 bytes as well.
 */
for (i = 0; i < 4; i++) {
    if (!memcmp(data, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN))
        goto found;
    data--;
}
```

GRUB2 may pad the initrd to a 4-byte aligned size. So `initrd_end` might point
3 bytes past the actual `#BOOTCONFIG\n` magic. The loop walks back up to 3 extra
bytes to find the real magic position.

### Checksum Algorithm

```c
static inline uint32_t xbc_calc_checksum(void *data, uint32_t size)
{
    unsigned char *p = data;
    uint32_t ret = 0;
    while (size--)
        ret += *p++;
    return ret;
}
```

Simple byte summation — not cryptographic. Purpose is only data integrity (not
security). The bootconfig tool (`tools/bootconfig/bootconfig.c`) appends this
trailer when building the initrd.

---

## 4. Bottom Layer: get_boot_config_from_initrd()

### Source (init/main.c, #ifdef CONFIG_BLK_DEV_INITRD)

```c
static void * __init get_boot_config_from_initrd(size_t *_size)
{
    u32 size, csum;
    char *data;
    u32 *hdr;
    int i;

    if (!initrd_end)
        return NULL;

    data = (char *)initrd_end - BOOTCONFIG_MAGIC_LEN;   // Step 1
    for (i = 0; i < 4; i++) {                           // Step 2
        if (!memcmp(data, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN))
            goto found;
        data--;
    }
    return NULL;

found:
    hdr = (u32 *)(data - 8);           // Step 3: point to [size, csum]
    size = le32_to_cpu(hdr[0]);
    csum = le32_to_cpu(hdr[1]);

    data = ((void *)hdr) - size;       // Step 4: point to text start
    if ((unsigned long)data < initrd_start) {
        pr_err("bootconfig size %d is greater than initrd size %ld\n",
            size, initrd_end - initrd_start);
        return NULL;
    }

    if (xbc_calc_checksum(data, size) != csum) {   // Step 5: verify
        pr_err("bootconfig checksum failed\n");
        return NULL;
    }

    initrd_end = (unsigned long)data;  // Step 6: shrink initrd
    if (_size)
        *_size = size;

    return data;
}
```

### Step-by-Step Walkthrough

| Step | Action | Detail |
|------|---------|--------|
| 1 | Point `data` to `initrd_end - 12` | The last 12 bytes should be `#BOOTCONFIG\n` |
| 2 | Search magic in 4-byte window | Handles GRUB 4-byte alignment padding |
| 3 | Read 8-byte header before magic | `hdr[0]` = size (LE u32), `hdr[1]` = checksum (LE u32) |
| 4 | Compute text start | `data = magic_ptr - 8 - size` |
| 5 | Validate checksum | Byte-sum of text must match stored checksum |
| 6 | Trim initrd | `initrd_end` is moved back to exclude bootconfig from the cpio view |

### Critical Side Effect: initrd_end Trimming

The function **modifies `initrd_end`** even when called with `_size = NULL`
(the `!CONFIG_BOOT_CONFIG` path). This is intentional — the bootconfig trailer
must always be removed from the initrd view so the cpio/squashfs extractor in
userspace does not see garbage at the end of the archive.

This is why even in the `!CONFIG_BOOT_CONFIG` stub, the call is made:
```c
static void __init setup_boot_config(void)
{
    /* Remove bootconfig data from initrd */
    get_boot_config_from_initrd(NULL);
}
```

---

## 5. Bottom Layer: xbc_get_embedded_bootconfig()

### Source (lib/bootconfig.c, CONFIG_BOOT_CONFIG_EMBED)

```c
extern __visible const char embedded_bootconfig_data[];
extern __visible const char embedded_bootconfig_data_end[];

const char * __init xbc_get_embedded_bootconfig(size_t *size)
{
    *size = embedded_bootconfig_data_end - embedded_bootconfig_data;
    return (*size) ? embedded_bootconfig_data : NULL;
}
```

### What is Embedded Bootconfig?

When `CONFIG_BOOT_CONFIG_EMBED=y`, a bootconfig file is baked directly into the
kernel image at build time. The linker script includes `bootconfig-data.S` which
`.incbin`s the config file. The symbols `embedded_bootconfig_data` and
`embedded_bootconfig_data_end` mark the boundaries.

This is useful for:
- Factory/manufacturing builds that always need specific params
- Systems without initrd (XIP kernels, some deeply embedded ARM Cortex-M/R)
- Testing without rebuilding initrd

### Fallback Logic in setup_boot_config()

```c
data = get_boot_config_from_initrd(&size);   // Try initrd first
if (!data)
    data = xbc_get_embedded_bootconfig(&size); // Fall back to embedded
```

If initrd has bootconfig → use it (initrd wins).
If no initrd bootconfig → check if kernel was built with embedded config.
If neither → `data == NULL` → proceed without bootconfig.

---

## 6. Bottom Layer: parse_args() + bootconfig_params()

### Purpose

Before investing effort in parsing the bootconfig data, the kernel must check
whether the user actually **wants** bootconfig active. The activation token is
the word `bootconfig` on the kernel command line.

### bootconfig_params() Callback

```c
static int __init bootconfig_params(char *param, char *val,
                                    const char *unused, void *arg)
{
    if (strcmp(param, "bootconfig") == 0) {
        bootconfig_found = true;
    }
    return 0;
}
```

### How parse_args() Is Called Here

```c
static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
err = parse_args("bootconfig", tmp_cmdline, NULL, 0, 0, 0, NULL,
                 bootconfig_params);
```

Key points:
- `boot_command_line` is copied to `tmp_cmdline` because `parse_args()` modifies
  the string in-place (replaces `=` with NUL terminators).
- The `NULL` kernel param table and `0, 0` min/max level mean: no registered
  `__param` table is consulted — only the unknown handler (`bootconfig_params`)
  is called for every token.
- Every parameter in the command line is passed to `bootconfig_params()`.
  Only `"bootconfig"` flips the flag.

### initargs_offs: Tracking '--' Position

`parse_args()` stops when it encounters `--` and returns a pointer into the
`tmp_cmdline` string at the character after `--`. This is captured as:

```c
if (err)
    initargs_offs = err - tmp_cmdline;
```

`initargs_offs` records the byte offset of the init arguments section in
`boot_command_line`. Later in `setup_command_line()`, this is used to correctly
splice `extra_init_args` (from `init.*` bootconfig keys) **before** the
cmdline-specified init args that appear after `--`.

---

## 7. Middle Layer: xbc_init() — Building the Parse Tree

### Source (lib/bootconfig.c)

```c
int __init xbc_init(const char *data, size_t size,
                    const char **emsg, int *epos)
{
    // 1. Guard: not already initialized, size valid
    // 2. memblock_alloc() a copy of data (size+1 bytes, NUL terminated)
    // 3. memblock_alloc() xbc_nodes[] array (XBC_NODE_MAX = 8192 nodes)
    // 4. xbc_parse_tree()   — tokenize + build node tree
    // 5. xbc_verify_tree()  — semantic validation
    // returns node count on success, -errno on failure
}
```

### Memory Allocations Made by xbc_init()

Both allocations use `memblock_alloc()` — the early boot memory allocator that
operates before the page allocator is up:

| Allocation | Size | Purpose |
|-----------|------|---------|
| `xbc_data` | `size + 1` bytes | Copy of raw bootconfig text, NUL-terminated |
| `xbc_nodes` | `8192 * sizeof(xbc_node)` = 8192 * 8 = 65536 bytes | Node tree |

Total worst case: ~96 KB from memblock. This is acceptable since memblock manages
all free memory reported by the bootloader/DTB.

### xbc_node Structure

```c
struct xbc_node {
    uint16_t next;    // index of next sibling node (0 = none)
    uint16_t child;   // index of first child node  (0 = none)
    uint16_t parent;  // index of parent node (XBC_NODE_MAX = no parent)
    uint16_t data;    // offset into xbc_data[] | XBC_VALUE flag
} __attribute__((__packed__));  // 8 bytes per node
```

Node types:
- **Key node**: `data & XBC_VALUE == 0` → `data` is offset into `xbc_data` for key string
- **Value node**: `data & XBC_VALUE != 0` → `data & ~XBC_VALUE` is offset for value string

### Example Tree for Bootconfig Text

Input text:
```
kernel {
    loglevel = "7"
    printk.devkmsg = "on"
}
init {
    systemd.unified_cgroup_hierarchy = "1"
}
```

Tree structure:
```
[root/implicit]
├── KEY "kernel"
│   ├── KEY "loglevel"
│   │   └── VALUE "7"
│   └── KEY "printk"
│       └── KEY "devkmsg"
│           └── VALUE "on"
└── KEY "init"
    └── KEY "systemd"
        └── KEY "unified_cgroup_hierarchy"
            └── VALUE "1"
```

### xbc_parse_tree() Tokenizer States

The parser is a single-pass character scanner. Key grammar tokens:
- `{` → open brace, increase depth, set `last_parent`
- `}` → close brace, restore parent from `open_brace[]` stack
- `=` → value assignment
- `"..."` → quoted string value
- `[...]` → array value
- `#` or `//` → comment to end of line
- `;` or newline → end of key-value statement
- `.` → key path separator (creates intermediate key nodes)

### xbc_verify_tree()

After parsing, the tree is verified:
- No duplicate keys at same level
- No empty key names
- Value nodes only appear as children of key nodes
- Tree depth does not exceed `XBC_DEPTH_MAX` = 16

---

## 8. Middle Layer: xbc_make_cmdline()

### Source (init/main.c)

```c
static char * __init xbc_make_cmdline(const char *key)
{
    struct xbc_node *root;
    char *new_cmdline;
    int ret, len = 0;

    root = xbc_find_node(key);       // Find "kernel" or "init" subtree
    if (!root)
        return NULL;

    len = xbc_snprint_cmdline(NULL, 0, root);   // Dry run: count bytes
    if (len <= 0)
        return NULL;

    new_cmdline = memblock_alloc(len + 1, SMP_CACHE_BYTES);
    // ... error check ...

    ret = xbc_snprint_cmdline(new_cmdline, len + 1, root);  // Real print
    // ... error check ...

    return new_cmdline;
}
```

### Two-Pass Pattern

`xbc_snprint_cmdline()` is called twice:
1. **Dry run** (`buf=NULL, size=0`): `snprintf(NULL, 0, ...)` returns the required
   length without writing. This gives the exact byte count.
2. **Real run**: `memblock_alloc()` the exact size, then fill it.

This avoids over-allocation and is the standard Linux kernel pattern for dynamic
strings in early boot (no `asprintf()` available at this stage).

### xbc_snprint_cmdline() Logic

```c
static int __init xbc_snprint_cmdline(char *buf, size_t size,
                                       struct xbc_node *root)
{
    xbc_node_for_each_key_value(root, knode, val) {
        xbc_node_compose_key_after(root, knode, xbc_namebuf, XBC_KEYLEN_MAX);
        // xbc_namebuf = "loglevel", "printk.devkmsg", etc.

        vnode = xbc_node_get_child(knode);
        if (!vnode) {
            // Key with no value: emit "keyname "
            snprintf(buf, ..., "%s ", xbc_namebuf);
        }
        xbc_array_for_each_value(vnode, val) {
            // Key with value(s): emit 'keyname="value" '
            snprintf(buf, ..., "%s=\"%s\" ", xbc_namebuf, val);
        }
    }
}
```

### Example Output

For the `kernel {}` subtree:
```
loglevel="7" printk.devkmsg="on"
```

This string is assigned to `extra_command_line`. Note the trailing space — this is
harmless because the command line parser skips whitespace between tokens.

For the `init {}` subtree:
```
systemd.unified_cgroup_hierarchy="1"
```

Assigned to `extra_init_args`.

---

## 9. Top Layer: setup_boot_config() — Full Control Flow

### Complete Annotated Source

```c
static void __init setup_boot_config(void)
{
    static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;
    const char *msg, *data;
    int pos, ret;
    size_t size;
    char *err;

    // ─── PHASE 1: Locate bootconfig data ───────────────────────────────
    data = get_boot_config_from_initrd(&size);
    if (!data)
        data = xbc_get_embedded_bootconfig(&size);
    // data == NULL is OK here. We still need to scan cmdline for "bootconfig"
    // keyword. If found but data==NULL, that's a user error to report.

    // ─── PHASE 2: Scan command line for "bootconfig" keyword ────────────
    strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
    err = parse_args("bootconfig", tmp_cmdline, NULL, 0, 0, 0, NULL,
                     bootconfig_params);
    // After this: bootconfig_found == true  IFF "bootconfig" is on cmdline

    // ─── PHASE 3: Early exit if not needed ──────────────────────────────
    if (IS_ERR(err) ||
        !(bootconfig_found || IS_ENABLED(CONFIG_BOOT_CONFIG_FORCE)))
        return;
    // Exit if:
    //   - parse_args() returned an error (should not happen, handler returns 0)
    //   - "bootconfig" NOT on cmdline AND CONFIG_BOOT_CONFIG_FORCE not set

    // ─── PHASE 4: Record init args offset ───────────────────────────────
    if (err)
        initargs_offs = err - tmp_cmdline;
    // err points to the char after "--" in tmp_cmdline if "--" was present.
    // initargs_offs captures where cmdline init args begin (for splicing later).

    // ─── PHASE 5: Data presence check ───────────────────────────────────
    if (!data) {
        if (bootconfig_found)
            pr_err("'bootconfig' found on command line, but no bootconfig found\n");
        else
            pr_info("No bootconfig data provided, so skipping bootconfig");
        return;
    }

    // ─── PHASE 6: Size guard ─────────────────────────────────────────────
    if (size >= XBC_DATA_MAX) {     // XBC_DATA_MAX = 32767
        pr_err("bootconfig size %ld greater than max size %d\n",
               (long)size, XBC_DATA_MAX);
        return;
    }

    // ─── PHASE 7: Parse bootconfig text into XBC tree ────────────────────
    ret = xbc_init(data, size, &msg, &pos);
    if (ret < 0) {
        // pos < 0: initialization error (memory, already initialized)
        // pos >= 0: text parse error at byte offset pos
        if (pos < 0)
            pr_err("Failed to init bootconfig: %s.\n", msg);
        else
            pr_err("Failed to parse bootconfig: %s at %d.\n", msg, pos);
    } else {
        // ─── PHASE 8: Extract kernel.* and init.* ────────────────────────
        xbc_get_info(&ret, NULL);
        pr_info("Load bootconfig: %ld bytes %d nodes\n", (long)size, ret);

        extra_command_line = xbc_make_cmdline("kernel");
        // e.g., extra_command_line = "loglevel=\"7\" printk.devkmsg=\"on\" "

        extra_init_args = xbc_make_cmdline("init");
        // e.g., extra_init_args = "systemd.unified_cgroup_hierarchy=\"1\" "
    }
    return;
}
```

### State Machine Diagram

```
                    [Entry]
                       │
                       ▼
          ┌─────────────────────────┐
          │ get_boot_config_from_   │
          │ initrd() → data/size    │
          └─────────────────────────┘
                       │
                       ▼
          ┌─────────────────────────┐
          │ if !data:               │
          │ xbc_get_embedded_       │
          │ bootconfig() → data     │
          └─────────────────────────┘
                       │
                       ▼
          ┌─────────────────────────┐
          │ parse_args(cmdline) →   │
          │ bootconfig_found flag   │
          └─────────────────────────┘
                       │
          ┌────────────┴────────────┐
          │ IS_ERR(err) OR          │
          │ !bootconfig_found AND   │
          │ !BOOT_CONFIG_FORCE      │
          └────────────┬────────────┘
                    YES│                    NO
                       ▼                    ▼
                   [return]    ┌─────────────────────────┐
                               │ if !data:               │
                               │   log error/info        │
                               │   return                │
                               └─────────────────────────┘
                                            │
                               ┌────────────┴────────────┐
                               │ size >= XBC_DATA_MAX?   │
                               └────────────┬────────────┘
                                         YES│              NO
                                            ▼              ▼
                                        [return]  ┌────────────────┐
                                                  │ xbc_init()     │
                                                  └────────────────┘
                                                          │
                                             ┌────────────┴────────────┐
                                             │ ret < 0?                │
                                             └────────────┬────────────┘
                                                       YES│             NO
                                                          ▼             ▼
                                                   [log error]  ┌──────────────┐
                                                                │ xbc_make_    │
                                                                │ cmdline      │
                                                                │ ("kernel")   │
                                                                │ ("init")     │
                                                                └──────────────┘
                                                                       │
                                                                   [return]
```

---

## 10. Downstream: How extra_command_line Flows into setup_command_line()

### setup_command_line() in init/main.c

```c
static void __init setup_command_line(char *command_line)
{
    size_t len, xlen = 0, ilen = 0;

    if (extra_command_line)
        xlen = strlen(extra_command_line);
    if (extra_init_args)
        ilen = strlen(extra_init_args) + 4; /* for " -- " */

    len = xlen + strlen(boot_command_line) + 1;

    saved_command_line = memblock_alloc(len + ilen, SMP_CACHE_BYTES);
    static_command_line = memblock_alloc(len, SMP_CACHE_BYTES);

    if (xlen) {
        strcpy(saved_command_line, extra_command_line);  // prepend
        strcpy(static_command_line, extra_command_line); // prepend
    }
    strcpy(saved_command_line + xlen, boot_command_line);
    strcpy(static_command_line + xlen, command_line);

    if (ilen) {
        if (initargs_offs) {
            // "--" was present in cmdline: splice extra_init_args before
            // existing init args
            len = xlen + initargs_offs;
            strcpy(saved_command_line + len, extra_init_args);
            len += ilen - 4;
            strcpy(saved_command_line + len,
                   boot_command_line + initargs_offs - 1);
        } else {
            // No "--" in cmdline: append " -- extra_init_args"
            len = strlen(saved_command_line);
            strcpy(saved_command_line + len, " -- ");
            len += 4;
            strcpy(saved_command_line + len, extra_init_args);
        }
    }
    saved_command_line_len = strlen(saved_command_line);
}
```

### Memory Layout of saved_command_line

Case A: bootconfig has `kernel.*` params only (no `init.*`):
```
saved_command_line:
┌──────────────────────────┬─────────────────────────────────┐
│ extra_command_line        │ boot_command_line               │
│ "loglevel=\"7\" "         │ "console=ttyS0 root=/dev/sda1"  │
└──────────────────────────┴─────────────────────────────────┘
```

Case B: bootconfig has both `kernel.*` and `init.*`, cmdline has `-- /sbin/init`:
```
saved_command_line:
┌──────────────────┬────────────────────────────┬──────────────────────┬──────────────┐
│ extra_cmdline    │ boot_command_line (pre "--")│ extra_init_args      │ "-- /sbin/init" │
└──────────────────┴────────────────────────────┴──────────────────────┴──────────────┘
```

### Why extra_command_line Must Be Prepended (Not Appended)

The comment in `setup_command_line()` explains:
> "We have to put extra_command_line before boot command lines because there
> could be dashes (separator of init command line) in the command lines."

If `extra_command_line` were appended after `boot_command_line` and
`boot_command_line` contained `--`, the bootconfig kernel params would be
placed in the init argument section, not the kernel param section.

---

## 11. Cleanup: exit_boot_config() and xbc_exit()

### When Cleanup Happens

```c
static int __ref kernel_init(void *unused)
{
    wait_for_completion(&kthreadd_done);
    kernel_init_freeable();
    async_synchronize_full();
    system_state = SYSTEM_FREEING_INITMEM;
    ...
    exit_boot_config();    // ← HERE: after async init, before free_initmem()
    free_initmem();        // reclaims __init sections
    ...
}
```

`exit_boot_config()` calls `xbc_exit()`:

```c
void __init xbc_exit(void)
{
    xbc_free_mem(xbc_data, xbc_data_size);     // free text copy
    xbc_data = NULL;
    xbc_data_size = 0;
    xbc_node_num = 0;
    xbc_free_mem(xbc_nodes,                    // free node array
                 sizeof(struct xbc_node) * XBC_NODE_MAX);
    xbc_nodes = NULL;
    brace_index = 0;
}
```

`memblock_free()` is used — matching the `memblock_alloc()` at `xbc_init()` time.

### Why `__init` Despite Being Called After Scheduling?

`xbc_exit()` is marked `__init` (placed in `.init.text`). It is called from
`kernel_init()` which is marked `__ref` (not freed early). The call to `xbc_exit()`
happens before `free_initmem()`. Once `free_initmem()` executes, the `.init.text`
section is unmapped/freed. The ordering is correct: `xbc_exit()` runs while
`.init.text` is still valid.

---

## 12. CONFIG_BOOT_CONFIG Disabled Path

When `CONFIG_BOOT_CONFIG` is not set in `.config`:

```c
#else   /* !CONFIG_BOOT_CONFIG */

static void __init setup_boot_config(void)
{
    /* Remove bootconfig data from initrd */
    get_boot_config_from_initrd(NULL);
}

static int __init warn_bootconfig(char *str)
{
    pr_warn("WARNING: 'bootconfig' found on the kernel command line "
            "but CONFIG_BOOT_CONFIG is not set.\n");
    return 0;
}

#define exit_boot_config()  do {} while (0)

#endif  /* CONFIG_BOOT_CONFIG */
```

| Symbol | When CONFIG_BOOT_CONFIG=y | When CONFIG_BOOT_CONFIG=n |
|--------|--------------------------|--------------------------|
| `bootconfig_found` | `static bool` variable | `#define false` |
| `initargs_offs` | `static size_t` variable | `#define 0` |
| `setup_boot_config()` | Full implementation | Stub — only trims initrd |
| `exit_boot_config()` | Calls `xbc_exit()` | Empty macro |
| `extra_command_line` | Set by `xbc_make_cmdline()` | Remains NULL |
| `extra_init_args` | Set by `xbc_make_cmdline()` | Remains NULL |

---

## 13. Global Variable Lifecycle Table

| Variable | Type | Set By | Consumed By | Freed/Cleared By |
|----------|------|--------|-------------|-----------------|
| `extra_command_line` | `char *` | `xbc_make_cmdline("kernel")` | `setup_command_line()` | Lives forever (in `saved_command_line`) |
| `extra_init_args` | `char *` | `xbc_make_cmdline("init")` | `setup_command_line()`, `parse_args(extra_init_args)` | Lives forever |
| `bootconfig_found` | `bool` | `bootconfig_params()` | `setup_boot_config()` early exit check | `__init`, freed with `.init.data` |
| `initargs_offs` | `size_t` | `parse_args()` return value | `setup_command_line()` | `__init` |
| `xbc_data` | `char *` (global in lib/bootconfig.c) | `xbc_init()` | `xbc_node_get_data()` | `xbc_exit()` → `memblock_free()` |
| `xbc_nodes` | `xbc_node *` | `xbc_init()` | `xbc_find_node()`, `xbc_make_cmdline()` | `xbc_exit()` → `memblock_free()` |

---

## 14. Complete Call Graph

```
start_kernel()
└── setup_boot_config()                              [init/main.c]
    ├── get_boot_config_from_initrd()                [init/main.c]
    │   ├── memcmp(data, BOOTCONFIG_MAGIC, 12)       [string.h]
    │   ├── le32_to_cpu()                            [byteorder.h]
    │   └── xbc_calc_checksum()                      [bootconfig.h inline]
    ├── xbc_get_embedded_bootconfig()                [lib/bootconfig.c]
    │   └── (returns pointer to .init.rodata section)
    ├── strscpy(tmp_cmdline, boot_command_line, ...) [string.h]
    ├── parse_args("bootconfig", tmp_cmdline, ...)   [kernel/params.c]
    │   └── bootconfig_params()                      [init/main.c callback]
    ├── xbc_init(data, size, &msg, &pos)             [lib/bootconfig.c]
    │   ├── memblock_alloc(size+1)                   [mm/memblock.c]
    │   ├── memcpy(xbc_data, data, size)             [string.h]
    │   ├── memblock_alloc(XBC_NODE_MAX * 8)         [mm/memblock.c]
    │   ├── xbc_parse_tree()                         [lib/bootconfig.c]
    │   └── xbc_verify_tree()                        [lib/bootconfig.c]
    └── xbc_make_cmdline("kernel")                   [init/main.c]
        ├── xbc_find_node("kernel")                  [lib/bootconfig.c]
        │   └── xbc_node_find_subkey()               [lib/bootconfig.c]
        ├── xbc_snprint_cmdline(NULL, 0, root)       [init/main.c] ← size probe
        ├── memblock_alloc(len+1)                    [mm/memblock.c]
        └── xbc_snprint_cmdline(buf, len+1, root)    [init/main.c] ← real fill
            ├── xbc_node_for_each_key_value()        [bootconfig.h macro]
            ├── xbc_node_compose_key_after()         [lib/bootconfig.c]
            └── xbc_array_for_each_value()           [bootconfig.h macro]

    (same xbc_make_cmdline pattern for "init" → extra_init_args)

    └── exit_boot_config()  [called later from kernel_init()]
        └── xbc_exit()                               [lib/bootconfig.c]
            ├── memblock_free(xbc_data)              [mm/memblock.c]
            └── memblock_free(xbc_nodes)             [mm/memblock.c]
```

---

*Document End*
*Source reference: init/main.c, lib/bootconfig.c, include/linux/bootconfig.h*
*Kernel version: Linux 6.x (ARM/ARM64)*
