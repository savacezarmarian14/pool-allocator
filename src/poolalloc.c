#include "poolalloc.h"

void pa_init(void *pool, size_t size)
{
    uintptr_t start = align_up((uintptr_t)pool, PA_ALIGN);
    uintptr_t end   = align_down((uintptr_t)pool + size, PA_ALIGN);
    size_t hdr = align_size_up(sizeof(struct pa_header), PA_ALIGN);

    if (!pool || start >= end || (size_t)(end - start) <= hdr) {
        mm.start_address = NULL;
        mm.end_address   = NULL;
        mm.size          = 0;
        return;
    }

    mm.start_address = (uint8_t *)start;
    mm.end_address   = (uint8_t *)end;
    mm.size          = end - start;          /* total pool */

    struct pa_header *pah = (struct pa_header *)mm.start_address;
    pah->size = mm.size - hdr;               /* data */
    pah->free = 1;
}

void *pa_alloc(size_t size)
{
    size_t hdr = align_size_up(sizeof(struct pa_header), PA_ALIGN);
    size_t need = align_size_up(size, PA_ALIGN);
    uint8_t *cur = mm.start_address;

    if (!mm.start_address || size == 0)
        return NULL;

    while (cur < mm.end_address) {
        struct pa_header *pah = (struct pa_header *)cur;

        if (pah->free && pah->size >= need) {
            size_t remaining = pah->size - need;

            if (remaining > hdr) {           
                pah->size = need;
                struct pa_header *next = (struct pa_header *)(cur + hdr + need);
                next->free = 1;
                next->size = remaining - hdr;
            }
            pah->free = 0;
            return cur + hdr;
        }
        cur += hdr + pah->size;
    }
    return NULL;
}