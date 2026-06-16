# `pcpu_chunk_relocate()` — Free-Size Slot Management

## Source Reference
- `mm/percpu.c:555` — function definition
- Called by: `pcpu_setup_first_chunk()` after creating first chunk

---

## Function Signature

```c
/* mm/percpu.c:555 */
/**
 * pcpu_chunk_relocate - put chunk in the appropriate chunk slot
 * @chunk: chunk of interest
 * @oslot: the previous slot it was on
 *
 * This function is called after an allocation or free changed @chunk.
 * New @chunk place is determined by the number of free bytes in @chunk.
 * The @oslot should be the slot @chunk was on.  If @chunk is new and
 * not on any slot, the caller should pass -1 as @oslot.
 *
 * CONTEXT:
 * pcpu_lock held.
 */
static void pcpu_chunk_relocate(struct pcpu_chunk *chunk, int oslot)
```

---

## Purpose: Organizing Chunks for O(1) Allocation

`pcpu_chunk_relocate()` maintains the **slot array** (`pcpu_slot[]`) — an array of
linked lists where each list contains chunks with a similar amount of free space.

The design goal: when `pcpu_alloc(size)` is called, find a suitable chunk in O(1)
by jumping directly to the slot that should have chunks with `size` free bytes.

---

## The Slot Index Formula

```c
/* mm/percpu.c:~550 */
static int __pcpu_size_to_slot(int size)
{
    int highbit = fls(size);    /* find highest set bit */
    return max(highbit - PCPU_SLOT_BASE_SHIFT, 1);
}

#define PCPU_SLOT_BASE_SHIFT    5   /* slots indexed from 2^5 = 32 bytes */

/* Slot 0:  free_bytes = 0          (full chunk, no free space) */
/* Slot 1:  free_bytes = 1-31       (tiny free space) */
/* Slot 2:  free_bytes = 32-63      */
/* Slot 3:  free_bytes = 64-127     */
/* Slot 4:  free_bytes = 128-255    */
/* ...                              */
/* Slot K:  free_bytes = 2^(K+4) .. 2^(K+5)-1 */
```

Examples:
```
free_bytes = 0:       slot = 0
free_bytes = 28672:   fls(28672) = 15, slot = max(15-5, 1) = 10
free_bytes = 20480:   fls(20480) = 15, slot = max(15-5, 1) = 10
free_bytes = 8192:    fls(8192)  = 14, slot = max(14-5, 1) = 9
```

---

## `pcpu_chunk_relocate()` Implementation

```c
/* mm/percpu.c:555 */
static void pcpu_chunk_relocate(struct pcpu_chunk *chunk, int oslot)
{
    int nslot = pcpu_chunk_slot(chunk);
    /* pcpu_chunk_slot uses free_bytes to compute the target slot */

    if (chunk == pcpu_reserved_chunk || oslot == nslot)
        return;
    /* reserved chunk is not managed in slot lists */
    /* if slot hasn't changed, no movement needed */

    if (oslot < PCPU_SLOT_BASE_SHIFT)
        list_del_init(&chunk->list);

    if (nslot < PCPU_SLOT_BASE_SHIFT)
        list_add_tail(&chunk->list, &pcpu_slot[nslot]);
    else if (oslot != nslot)
        list_move_tail(&chunk->list, &pcpu_slot[nslot]);
}
```

For the **first chunk** (called from `pcpu_setup_first_chunk` with `oslot = -1`):
```c
/* oslot = -1 → chunk is not in any list yet */
/* nslot = computed from chunk->free_bytes (dynamic region size ~20KB+) */
/* Action: list_add_tail(chunk, pcpu_slot[nslot]) */
```

---

## How Allocation Uses the Slot Array

```c
/* pcpu_alloc() — simplified */
int slot = pcpu_size_to_slot(size);  /* compute minimum slot needed */

/* Search from 'slot' upward until we find a chunk */
while (slot < pcpu_nr_slots) {
    if (!list_empty(&pcpu_slot[slot])) {
        chunk = list_first_entry(&pcpu_slot[slot], struct pcpu_chunk, list);
        /* Try to allocate from this chunk */
        if (pcpu_alloc_area(chunk, size, align, ...)) {
            pcpu_chunk_relocate(chunk, slot);  /* relocate after space changed */
            return success;
        }
    }
    slot++;
}
/* All slots exhausted → allocate new chunk */
```

This gives **O(log(free_bytes))** worst case, typically O(1) for common allocation sizes.

---

## Initial State After `pcpu_setup_first_chunk()`

```
pcpu_slot[]:
  [0]: empty  (no full chunks)
  [1]: empty
  ...
  [9]: [pcpu_first_chunk] ← free_bytes = ~20KB → fls(20480) = 15 → slot 10
  or
  [10]: [pcpu_first_chunk]
  ...
  [N]: empty
```

The exact slot depends on `PERCPU_DYNAMIC_RESERVE` (20KB by default):
```
free_bytes = 20480 = 0x5000
fls(20480) = 15
slot = max(15 - 5, 1) = 10
→ pcpu_first_chunk goes into pcpu_slot[10]
```

---

## Slot Movement During Runtime

```
Initial:  pcpu_first_chunk in slot 10  (free_bytes = 20480)

alloc_percpu(4096):  (4KB allocation)
  → free_bytes decreases: 20480 - 4096 = 16384
  → fls(16384) = 15 → slot = 10  (same slot!)
  → no relocation needed

alloc_percpu(8192):  (8KB allocation)
  → free_bytes decreases: 16384 - 8192 = 8192
  → fls(8192) = 14 → slot = 9  (moved down!)
  → pcpu_chunk_relocate(chunk, 10) → moves to slot[9]

alloc_percpu(8192):  (another 8KB)
  → free_bytes decreases: 8192 - 8192 = 0
  → slot = 0  (full!)
  → pcpu_chunk_relocate(chunk, 9) → moves to slot[0]
  → New dynamic chunk must be allocated for future requests
```

---

## Interview Quick Facts

| Question | Answer |
|---|---|
| What is pcpu_slot[]? | Array of list_head, indexed by log2(free_bytes) |
| What does relocate do with oslot=-1? | Inserts chunk into correct slot (new chunk) |
| What is PCPU_SLOT_BASE_SHIFT? | 5 — slots indexed from 32-byte granularity |
| How does allocation use slots? | Jumps to slot ≥ needed size, finds first available chunk |
| Is pcpu_reserved_chunk in slots? | No — excluded (its allocations managed separately) |
| What happens when all slots are exhausted? | New chunk created via pcpu_create_chunk() |
