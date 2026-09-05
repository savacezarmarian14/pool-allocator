# pool-alloc

A fixed-pool memory allocator written in C11, with no dependency on the C standard library allocator, no dependency on the operating system, and no dynamic memory of its own.

You hand it a block of memory. It hands out pieces of that block and takes them back.

```c
static unsigned char pool[64 * 1024];

pa_init(pool, sizeof(pool));

void *p = pa_alloc(1024);
/* ... */
pa_free(p);
```

---

## Why a pool

The obvious way to write an allocator is to build it on `brk`/`sbrk` or `mmap` and grow the heap on demand. That is what `malloc` does, and it is the wrong starting point for anything that has to run outside a hosted Linux process.

Taking the memory as a parameter instead means the allocator does not care where the memory came from. On a microcontroller it is a static array in `.bss`. In a hosted process it is whatever `mmap` returned. In a kernel it is a region carved out at boot. The core stays identical; only the caller changes.

That inversion is the entire design decision. Everything else follows from it:

- No syscalls, so it works freestanding.
- No global heap, so several independent pools can coexist with different lifetimes.
- Deterministic footprint, decided at compile time or at startup, never growing behind your back.
- Testable without a process, because a `static unsigned char[]` is a perfectly good pool.

Sources of memory (`sbrk`, `mmap`) belong in separate translation units layered *above* the allocator, not inside it.

---

## Design

### Layout

The pool is a flat sequence of blocks. Every block is a header followed by its data:

```
+--------+------------------+--------+--------------+--------+---------+
| header |       data       | header |     data     | header |  data   |
+--------+------------------+--------+--------------+--------+---------+
^                                                                      ^
start_address                                              end_address
```

There is no separate metadata region and no side table. The block list is implicit: the next header always sits at `current + header_size + current->size`, so walking the pool is pointer arithmetic and nothing else.

The header is 16 bytes on x86-64:

```c
struct pa_header {
    size_t   size;    /* usable bytes in this block, excluding the header */
    uint8_t  free;
    uint32_t magic;
};
```

`free` and `magic` fit in padding that `size` would have forced anyway, so the validation field is effectively free.

### Alignment

Every address the allocator returns is aligned to `_Alignof(max_align_t)` — 16 bytes on x86-64. This is maintained by induction rather than by fixing things up at the end:

1. `pa_init` rounds the start of the pool up and the end down.
2. The header size is a multiple of the alignment, enforced at compile time:

   ```c
   _Static_assert(sizeof(struct pa_header) % PA_ALIGN == 0,
                  "pa_header size must be a multiple of PA_ALIGN");
   ```

3. Every requested size is rounded up before it is used.

Given an aligned block start, `start + header + size` is therefore aligned too, and so is every block after it. Rounding is done with bitmasks (`(x + (a - 1)) & ~(a - 1)`), not division — `PA_ALIGN` is always a power of two, and division on a small MCU is not free.

Addresses and sizes get separate helpers (`align_up` on `uintptr_t`, `align_size_up` on `size_t`) so that the two are never accidentally interchanged.

### Allocation

First fit. `pa_alloc` walks the implicit block list and takes the first free block large enough for the rounded-up request.

If the remainder after carving out the request is larger than a header, the block is split in two and the tail becomes a new free block. If it is not, the leftover bytes stay attached to the allocated block as internal fragmentation — splitting off a fragment too small to ever hold a header would produce a block that can never be used.

First fit was chosen over best fit deliberately. Best fit needs a full scan on every allocation and, in practice, does not fragment measurably less. Address-ordered first fit is what most production allocators approximate anyway.

### Freeing and coalescing

Freeing without merging is not freeing. A pool that has served and released ten thousand 64-byte requests should be able to satisfy a 4 KB request afterwards; if adjacent free blocks are never joined, it cannot.

`pa_free` marks the block free and then makes a single pass over the pool, merging every run of adjacent free blocks it finds. This is a stronger guarantee than merging only the neighbours of the block just freed: the pool is fully coalesced after every `pa_free`, so there is no ordering of frees that can leave two adjacent free blocks behind. The test suite checks this directly, in forward, reverse, and interleaved order.

The cost is O(n) in the number of blocks rather than O(1). For a pool allocator this is the right trade: the block count is bounded by the pool size, the pass is a linear walk over memory that is already hot, and the alternative — boundary tags with a footer on every block — costs space on every block to speed up an operation that is not on the hot path.

### Validating pointers

`pa_free` rejects a pointer unless all of the following hold:

- it is not `NULL`, and the pool is initialised
- the header it implies lies inside the pool
- that header carries the magic value
- the block is not already free

This catches stray pointers, pointers into the middle of a block, and the common case of a double free. It is a sanity check, not a security boundary — see the limitations below.

---

## Building

```
make          # build build/libpoolalloc.a
make check    # build and run the test suite
make san      # run the same tests under AddressSanitizer + UBSan
make clean
```

Builds clean with `-std=c11 -Wall -Wextra -Wpedantic`. No warnings, no GCC extensions — in particular, no arithmetic on `void *`, which is why every internal pointer is `uint8_t *`.

Header dependencies are generated automatically (`-MMD -MP`), so editing `poolalloc.h` rebuilds what it should.

---

## Tests

53 assertions across 12 groups, all passing, clean under ASan and UBSan.

| Group | What it establishes |
|---|---|
| init | Start and end alignment, whole-pool sizing, rejection of `NULL` and of pools too small to hold a header |
| alloc, basic | Returned pointers are aligned and inside the pool, blocks split correctly, zero-size requests rejected |
| alloc, distinct | Three live allocations do not overlap — verified by writing distinct patterns and reading them back |
| alloc, exhaustion | Allocation returns `NULL` when full; a failed allocation leaves the pool untouched |
| free, no coalesce | A freed block with two live neighbours merges with nothing |
| free, forward | A freed block absorbs the free block after it |
| free, full | Freeing everything collapses the pool back into a single block of the original size |
| free, order | Forward, reverse, and interleaved free orders all collapse to one block |
| reuse | Freed memory is handed out again; a hole in the middle is reused |
| free, invalid | `NULL`, out-of-pool pointers, and double frees are all ignored |
| uninitialised | `pa_alloc` returns `NULL` and `pa_free` does not crash before `pa_init` |
| stress | 500 pseudo-random alloc/free operations, with every pool invariant re-checked after each one |

The stress test is the one that matters. After every single operation it walks the pool and verifies that every header carries the magic value, every block is aligned, no size is zero, no block extends past the end, and the block sizes sum to exactly the pool size. A coalescing bug that corrupts a single `size` field breaks that last invariant immediately.

Two helpers in the test file are useful on their own: `pa_dump`, which prints the block layout, and `pool_is_consistent`, which is the invariant check described above.

---

## Complexity

| Operation | Cost |
|---|---|
| `pa_init` | O(1) |
| `pa_alloc` | O(n) in the number of blocks |
| `pa_free` | O(n) in the number of blocks |

Space overhead is 16 bytes per block on x86-64, plus up to `PA_ALIGN - 1` bytes of internal fragmentation per allocation from rounding.

---

## Limitations

Stated plainly, because an allocator that does not tell you where its edges are is worse than one with fewer features.

**Not thread-safe.** There is no locking, and the global `mm` state means one pool per program. Concurrent use requires external synchronisation. A per-pool handle passed by the caller would fix both, and is the natural next change.

**Double free is detected, not prevented.** Once a freed block has been merged into its predecessor, its header no longer exists — a second `pa_free` on the same pointer lands in the middle of the merged block's data. The magic check almost always rejects it, but if the application happened to write the magic value at exactly that offset, the check passes and the pool is corrupted. Reliable detection needs a quarantine list or a separate metadata region, which is a different design.

**No `realloc` or `calloc`.** Not hard to add. `realloc` in particular could grow in place when the following block is free, which is where most of the value is.

**First fit fragments.** Adjacent free blocks are always merged, but a pattern of long-lived small allocations interleaved with short-lived large ones will still leave the pool unable to satisfy a large request. This is inherent to any allocator that cannot move live blocks.

**No size class or bin structure.** Every allocation is a linear scan. For a pool with thousands of live blocks and an allocation-heavy workload, segregated free lists would be a large win.

---

## Layout

```
inc/poolalloc.h        public API, alignment helpers, struct definitions
src/poolalloc.c        pa_init, pa_alloc, pa_free
tests/                 test suite with pa_dump and the invariant checker
Makefile
```

---

## License

MIT
