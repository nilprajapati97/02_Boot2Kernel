# `setup_log_buf()` — printk Ring Buffer Allocation

## Purpose

Allocates the persistent printk ring buffer from `memblock`. Before this call, kernel messages are stored in a tiny static buffer. After this call, the full-sized ring buffer is available so early boot messages are not lost.

## Source File

`kernel/printk/printk.c`

## The printk Ring Buffer

The kernel log is stored in a structured ring buffer. Prior to kernel 5.10, this was a simple circular character buffer. Since 5.10 it is a structured log with separate message records:

```c
struct printk_ringbuffer {
    struct prb_desc_ring  desc_ring;   // Message descriptors
    struct prb_data_ring  text_data_ring; // Message text
    atomic_long_t         fail;
};
```

## Static vs Dynamic Buffer

### Before `setup_log_buf()`

A small static buffer is used:

```c
#define LOG_BUF_LEN_DEFAULT (1 << CONFIG_LOG_BUF_SHIFT)
static char __log_buf[LOG_BUF_LEN_DEFAULT] __log_align;
```

`CONFIG_LOG_BUF_SHIFT` default is 17 → 128 KB.

### After `setup_log_buf()`

A larger buffer is allocated from `memblock` (early) or from the buddy allocator (late). Size is determined by:

1. `log_buf_len=` command line parameter
2. Number of CPUs (more CPUs = bigger default buffer)
3. `CONFIG_LOG_BUF_SHIFT` Kconfig option

## Two-Phase Allocation

`setup_log_buf()` is called **twice**:

1. **Early** (from `start_kernel()` before `mm_core_init()`):
   - Uses `memblock_alloc()` for the buffer
   - `setup_log_buf(1)` — early mode

2. **Late** (from `log_buf_len_setup()` if `log_buf_len=` given):
   - Uses `memblock` or buddy allocator depending on timing

## Pre-conditions

- `setup_arch()` must have run (memblock available)
- `nr_cpu_ids` should be set (for CPU-count-based sizing)

## Post-conditions

- `log_buf` points to the new larger buffer
- `log_buf_len` reflects the new size
- Early log messages preserved in new buffer

## Cross-references

- [Phase overview](../README.md)
- `console_init()` — [../../13_console_locking/console_init/README.md](../../13_console_locking/console_init/README.md)
