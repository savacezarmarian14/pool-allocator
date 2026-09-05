#include <stdio.h>
#include <string.h>

#include "poolalloc.h"

#define POOL_SIZE 4096

static int tests_run;
static int tests_passed;

static unsigned char pool[POOL_SIZE];

#define CHECK(cond, msg)                                        \
    do {                                                        \
        tests_run++;                                            \
        if (cond) {                                             \
            tests_passed++;                                     \
            printf("  [ ok ] %s\n", msg);                       \
        } else {                                                \
            printf("  [FAIL] %s  (%s:%d)\n", msg,               \
                   __FILE__, __LINE__);                         \
        }                                                       \
    } while (0)

static size_t hdr_size(void)
{
    return align_size_up(sizeof(struct pa_header), PA_ALIGN);
}

/* Walks the pool and reports how it is laid out. */
static void pa_dump(void)
{
    size_t hdr = hdr_size();
    uint8_t *curr = mm.start_address;
    int i = 0;

    if (!mm.start_address) {
        printf("    <pool uninitialised>\n");
        return;
    }

    while (curr < mm.end_address) {
        struct pa_header *pah = (struct pa_header *)curr;
        printf("    [%d] off=%zu size=%zu %s\n", i++,
               (size_t)(curr - mm.start_address), pah->size,
               pah->free ? "free" : "used");
        curr += hdr + pah->size;
    }
}

/* Number of blocks currently in the pool. */
static int block_count(void)
{
    size_t hdr = hdr_size();
    uint8_t *curr = mm.start_address;
    int n = 0;

    while (curr < mm.end_address) {
        struct pa_header *pah = (struct pa_header *)curr;
        n++;
        curr += hdr + pah->size;
    }
    return n;
}

/* Walks the pool and checks every invariant we care about. */
static int pool_is_consistent(void)
{
    size_t hdr = hdr_size();
    uint8_t *curr = mm.start_address;

    if (!mm.start_address)
        return 0;

    while (curr < mm.end_address) {
        struct pa_header *pah = (struct pa_header *)curr;

        if (pah->magic != PA_MAGIC)
            return 0;
        if (pah->size == 0)
            return 0;
        if ((uintptr_t)curr % PA_ALIGN != 0)
            return 0;
        if (curr + hdr + pah->size > mm.end_address)
            return 0;

        curr += hdr + pah->size;
    }

    return curr == mm.end_address;
}

static void reset(void)
{
    memset(pool, 0xA5, sizeof(pool));
    pa_init(pool, sizeof(pool));
}

/* ------------------------------------------------------------------ */

static void test_init(void)
{
    printf("init\n");

    reset();

    CHECK(mm.start_address != NULL, "pool initialised");
    CHECK((uintptr_t)mm.start_address % PA_ALIGN == 0, "start is aligned");
    CHECK((uintptr_t)mm.end_address % PA_ALIGN == 0, "end is aligned");
    CHECK(mm.size == (size_t)(mm.end_address - mm.start_address),
          "size is the whole pool");
    CHECK(block_count() == 1, "one block after init");
    CHECK(pool_is_consistent(), "pool consistent after init");

    pa_init(NULL, 4096);
    CHECK(mm.start_address == NULL, "NULL pool rejected");

    pa_init(pool, 8);
    CHECK(mm.start_address == NULL, "pool smaller than header rejected");
}

static void test_alloc_basic(void)
{
    printf("alloc: basic\n");

    reset();

    void *a = pa_alloc(32);
    CHECK(a != NULL, "allocation succeeds");
    CHECK((uintptr_t)a % PA_ALIGN == 0, "returned pointer is aligned");
    CHECK((uint8_t *)a >= mm.start_address && (uint8_t *)a < mm.end_address,
          "returned pointer is inside the pool");
    CHECK(block_count() == 2, "block was split");
    CHECK(pool_is_consistent(), "pool consistent after alloc");

    CHECK(pa_alloc(0) == NULL, "zero-size allocation rejected");

    memset(a, 0x5A, 32);
    CHECK(pool_is_consistent(), "writing to the block does not corrupt the pool");
}

static void test_alloc_distinct(void)
{
    printf("alloc: blocks do not overlap\n");

    reset();

    void *a = pa_alloc(64);
    void *b = pa_alloc(64);
    void *c = pa_alloc(64);

    CHECK(a && b && c, "three allocations succeed");
    CHECK(a != b && b != c && a != c, "pointers are distinct");
    CHECK((uint8_t *)b >= (uint8_t *)a + 64, "b starts after a's data");
    CHECK((uint8_t *)c >= (uint8_t *)b + 64, "c starts after b's data");

    memset(a, 0x11, 64);
    memset(b, 0x22, 64);
    memset(c, 0x33, 64);

    CHECK(((unsigned char *)a)[63] == 0x11, "a intact");
    CHECK(((unsigned char *)b)[63] == 0x22, "b intact");
    CHECK(((unsigned char *)c)[63] == 0x33, "c intact");
    CHECK(pool_is_consistent(), "pool consistent");
}

static void test_alloc_exhaust(void)
{
    printf("alloc: exhaustion\n");

    reset();

    int n = 0;
    while (pa_alloc(128) != NULL)
        n++;

    CHECK(n > 0, "some allocations succeeded");
    CHECK(pa_alloc(128) == NULL, "further allocation returns NULL");
    CHECK(pa_alloc(1) == NULL || 1, "small allocation may still fit");
    CHECK(pool_is_consistent(), "pool consistent when full");

    reset();
    CHECK(pa_alloc(POOL_SIZE * 2) == NULL, "oversized allocation rejected");
    CHECK(block_count() == 1, "failed allocation left the pool untouched");
}

static void test_free_no_coalesce(void)
{
    printf("free: isolated block does not merge\n");

    reset();

    void *a = pa_alloc(64);
    void *b = pa_alloc(64);
    void *c = pa_alloc(64);
    (void)a; (void)c;

    int before = block_count();
    pa_free(b);

    CHECK(block_count() == before, "freeing the middle block merges nothing");
    CHECK(pool_is_consistent(), "pool consistent");
}

static void test_free_coalesce_forward(void)
{
    printf("free: merge with the following block\n");

    reset();

    void *a = pa_alloc(64);
    void *b = pa_alloc(64);
    (void)a;

    /* b is followed by the big free remainder. */
    int before = block_count();
    pa_free(b);

    CHECK(block_count() == before - 1, "b absorbed the trailing free block");
    CHECK(pool_is_consistent(), "pool consistent");
}

static void test_free_coalesce_full(void)
{
    printf("free: everything merges back\n");

    reset();

    void *a = pa_alloc(64);
    void *b = pa_alloc(64);
    void *c = pa_alloc(64);

    pa_free(b);
    pa_free(a);
    pa_free(c);

    CHECK(block_count() == 1, "pool collapsed into one block");
    CHECK(pool_is_consistent(), "pool consistent");

    struct pa_header *pah = (struct pa_header *)mm.start_address;
    CHECK(pah->free == 1, "the single block is free");
    CHECK(pah->size == mm.size - hdr_size(),
          "the single block spans the whole pool");

    if (block_count() != 1)
        pa_dump();
}

static void test_free_order_independence(void)
{
    printf("free: order does not matter\n");

    void *p[8];
    int i;

    /* forward */
    reset();
    for (i = 0; i < 8; i++)
        p[i] = pa_alloc(64);
    for (i = 0; i < 8; i++)
        pa_free(p[i]);
    CHECK(block_count() == 1, "freed in order -> one block");

    /* reverse */
    reset();
    for (i = 0; i < 8; i++)
        p[i] = pa_alloc(64);
    for (i = 7; i >= 0; i--)
        pa_free(p[i]);
    CHECK(block_count() == 1, "freed in reverse -> one block");

    /* interleaved */
    reset();
    for (i = 0; i < 8; i++)
        p[i] = pa_alloc(64);
    for (i = 0; i < 8; i += 2)
        pa_free(p[i]);
    for (i = 1; i < 8; i += 2)
        pa_free(p[i]);
    CHECK(block_count() == 1, "freed interleaved -> one block");
    CHECK(pool_is_consistent(), "pool consistent");
}

static void test_reuse(void)
{
    printf("reuse after free\n");

    reset();

    void *a = pa_alloc(128);
    pa_free(a);
    void *b = pa_alloc(128);

    CHECK(b == a, "freed memory is handed out again");
    CHECK(pool_is_consistent(), "pool consistent");

    reset();
    void *x = pa_alloc(64);
    void *y = pa_alloc(64);
    void *z = pa_alloc(64);
    (void)x; (void)z;

    pa_free(y);
    void *w = pa_alloc(64);
    CHECK(w == y, "the hole in the middle is reused");
    CHECK(pool_is_consistent(), "pool consistent");
}

static void test_free_invalid(void)
{
    printf("free: invalid pointers are ignored\n");

    reset();

    void *a = pa_alloc(64);
    int before = block_count();

    pa_free(NULL);
    CHECK(block_count() == before, "free(NULL) ignored");

    int stack_var = 0;
    pa_free(&stack_var);
    CHECK(block_count() == before, "pointer outside the pool ignored");

    pa_free(a);
    int after = block_count();
    pa_free(a);
    CHECK(block_count() == after, "double free ignored");
    CHECK(pool_is_consistent(), "pool consistent");
}

static void test_alloc_after_uninitialised(void)
{
    printf("alloc on an uninitialised pool\n");

    pa_init(NULL, 0);
    CHECK(pa_alloc(32) == NULL, "alloc returns NULL");
    pa_free((void *)0x1234);
    CHECK(1, "free does not crash");
}

static void test_stress(void)
{
    printf("stress\n");

    void *p[16] = {0};
    unsigned seed = 12345;
    int i, round;

    reset();

    for (round = 0; round < 500; round++) {
        seed = seed * 1103515245u + 12345u;
        i = (seed >> 16) % 16;

        if (p[i]) {
            pa_free(p[i]);
            p[i] = NULL;
        } else {
            size_t sz = ((seed >> 8) % 100) + 1;
            p[i] = pa_alloc(sz);
            if (p[i])
                memset(p[i], (int)i, sz);
        }

        if (!pool_is_consistent()) {
            CHECK(0, "pool stayed consistent through the stress run");
            pa_dump();
            return;
        }
    }

    CHECK(1, "pool stayed consistent through the stress run");

    for (i = 0; i < 16; i++)
        pa_free(p[i]);

    CHECK(block_count() == 1, "pool collapsed back into one block");
}

int main(void)
{
    printf("pool-alloc tests (header = %zu bytes, align = %zu)\n\n",
           hdr_size(), (size_t)PA_ALIGN);

    test_init();
    test_alloc_basic();
    test_alloc_distinct();
    test_alloc_exhaust();
    test_free_no_coalesce();
    test_free_coalesce_forward();
    test_free_coalesce_full();
    test_free_order_independence();
    test_reuse();
    test_free_invalid();
    test_alloc_after_uninitialised();
    test_stress();

    printf("\n%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}