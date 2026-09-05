#ifndef __POOLALLOC_H__
#define __POOLALLOC_H__

#include <stddef.h>
#include <stdint.h>

#define PA_ALIGN _Alignof(max_align_t)
#define PA_MAGIC 0x0C0FFEE0u

/* for addresses */
static inline uintptr_t align_up(uintptr_t x, size_t a) {
    return (x + (a - 1)) & ~(uintptr_t)(a - 1);
}

static inline uintptr_t align_down(uintptr_t x, size_t a) {
    return x & ~(uintptr_t)(a - 1);
}

/* for sizes */
static inline size_t align_size_up(size_t n, size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

static inline size_t align_size_down(size_t n, size_t a) {
    return n & ~(a - 1);
}

struct pa_header {
    size_t   size;
    uint8_t  free;
    uint32_t magic;
};

struct pa_state {
    uint8_t *start_address;
    uint8_t *end_address;
    size_t   size;
};

extern struct pa_state mm;

_Static_assert(sizeof(struct pa_header) % PA_ALIGN == 0 ||
               PA_ALIGN % _Alignof(struct pa_header) == 0,
               "pa_header alignment");

void pa_init(void *pool, size_t size);
void *pa_alloc(size_t size);
void pa_free(void *ptr);

#endif /* __POOLALLOC_H__ */