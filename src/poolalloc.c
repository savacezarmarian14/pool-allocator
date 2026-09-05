#include "poolalloc.h"

struct pa_state mm;

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
    pah->magic = PA_MAGIC;
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
                struct pa_header *next = 
                    (struct pa_header *)(cur + hdr + need);
                next->free = 1;
                next->size = remaining - hdr;
                next->magic = PA_MAGIC;
            }
            pah->free = 0;
            return cur + hdr;
        }
        cur += hdr + pah->size;
    }
    return NULL;
}

void pa_free(void *ptr)
{
    size_t hdr = align_size_up(sizeof(struct pa_header), PA_ALIGN);
    uint8_t *curr = mm.start_address;
    struct pa_header *pah;

    if (ptr == NULL || mm.start_address == NULL) {
        return;
    }

    pah = (struct pa_header *)((uint8_t *)ptr - hdr);

    if ((uint8_t *)pah < mm.start_address ||
        (uint8_t *)pah + hdr >= mm.end_address) {
        return;
    }

    if (pah->magic != PA_MAGIC || pah->free == 1) {
        return;
    }

    pah->free = 1;

    while (curr < mm.end_address)
    {
        struct pa_header *curr_pah = (struct pa_header *)curr;
        struct pa_header *next_pah =
            (struct pa_header *)(curr + hdr + curr_pah->size);

        if (curr_pah->free == 1) {
            while ((uint8_t *)next_pah < mm.end_address && next_pah->free == 1) {
                curr_pah->size += hdr + next_pah->size;
                next_pah = (struct pa_header *)(curr + hdr + curr_pah->size);
            }
        }

        curr += hdr + curr_pah->size;
    }
}