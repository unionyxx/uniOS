#include "stdlib.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <uapi/syscalls.h>

#include "syscall.h"

#define PAGE_SIZE 4096
#define ALLOC_ALIGNMENT 16
#define SMALL_REGION_SIZE (64 * 1024)
#define DEDICATED_THRESHOLD (32 * 1024)
#define MIN_SPLIT_SIZE 32
#define REGION_MAGIC 0x524547494f4e4d4dULL
#define BLOCK_MAGIC 0x424c4f434b4d4d31ULL

typedef struct AllocRegion AllocRegion;
typedef struct AllocBlock AllocBlock;

struct AllocBlock
{
    uint64_t magic;
    size_t size;
    int free;
    int _reserved;
    AllocBlock *prev;
    AllocBlock *next;
    AllocRegion *region;
};

struct AllocRegion
{
    uint64_t magic;
    size_t mapped_size;
    int dedicated;
    int _reserved;
    AllocRegion *prev;
    AllocRegion *next;
    AllocBlock *first;
};

static AllocRegion *g_regions = NULL;
static unsigned int g_rand_seed = 1;

static size_t align_up(size_t value, size_t alignment)
{
    size_t mask = alignment - 1;
    if (value > (SIZE_MAX - mask))
        return 0;
    return (value + mask) & ~mask;
}

static size_t block_overhead(void)
{
    return sizeof(AllocRegion) + sizeof(AllocBlock);
}

static size_t region_capacity(size_t mapped_size)
{
    return mapped_size - sizeof(AllocRegion) - sizeof(AllocBlock);
}

static void region_link(AllocRegion *region)
{
    region->prev = NULL;
    region->next = g_regions;
    if (g_regions)
        g_regions->prev = region;
    g_regions = region;
}

static void region_unlink(AllocRegion *region)
{
    if (region->prev)
        region->prev->next = region->next;
    else
        g_regions = region->next;
    if (region->next)
        region->next->prev = region->prev;
}

static AllocRegion *region_create(size_t min_payload, int dedicated)
{
    size_t required = block_overhead();
    if (min_payload > SIZE_MAX - required)
        return NULL;
    required += min_payload;

    size_t mapped_size = dedicated ? required : SMALL_REGION_SIZE;
    if (mapped_size < required)
        mapped_size = required;
    mapped_size = align_up(mapped_size, PAGE_SIZE);
    if (mapped_size == 0 || mapped_size < block_overhead())
        return NULL;

    void *base = (void *)syscall6(SYS_MMAP, 0, mapped_size, 3, 34, (uint64_t)-1, 0);
    if ((uint64_t)base == (uint64_t)-1)
        return NULL;

    AllocRegion *region = (AllocRegion *)base;
    AllocBlock *block = (AllocBlock *)((uint8_t *)base + sizeof(AllocRegion));

    region->magic = REGION_MAGIC;
    region->mapped_size = mapped_size;
    region->dedicated = dedicated;
    region->first = block;
    region_link(region);

    block->magic = BLOCK_MAGIC;
    block->size = region_capacity(mapped_size);
    block->free = 1;
    block->prev = NULL;
    block->next = NULL;
    block->region = region;
    return region;
}

static AllocBlock *find_block(size_t size)
{
    for (AllocRegion *region = g_regions; region; region = region->next) {
        if (region->dedicated)
            continue;
        for (AllocBlock *block = region->first; block; block = block->next) {
            if (block->magic == BLOCK_MAGIC && block->free && block->size >= size)
                return block;
        }
    }
    return NULL;
}

static void split_block(AllocBlock *block, size_t size)
{
    if (!block || block->size < size)
        return;

    size_t remaining = block->size - size;
    if (remaining < sizeof(AllocBlock) + MIN_SPLIT_SIZE)
        return;

    AllocBlock *next = (AllocBlock *)((uint8_t *)(block + 1) + size);
    next->magic = BLOCK_MAGIC;
    next->size = remaining - sizeof(AllocBlock);
    next->free = 1;
    next->prev = block;
    next->next = block->next;
    next->region = block->region;
    if (next->next)
        next->next->prev = next;

    block->size = size;
    block->next = next;
}

static void try_coalesce_forward(AllocBlock *block)
{
    if (!block)
        return;
    while (block->next && block->next->free) {
        AllocBlock *next = block->next;
        uint8_t *expected = (uint8_t *)(block + 1) + block->size;
        if ((uint8_t *)next != expected)
            break;
        block->size += sizeof(AllocBlock) + next->size;
        block->next = next->next;
        if (block->next)
            block->next->prev = block;
    }
}

static AllocBlock *coalesce_block(AllocBlock *block)
{
    if (!block)
        return NULL;

    try_coalesce_forward(block);
    if (block->prev && block->prev->free) {
        block = block->prev;
        try_coalesce_forward(block);
    }
    return block;
}

static int region_is_completely_free(const AllocRegion *region)
{
    if (!region || !region->first)
        return 0;
    const AllocBlock *block = region->first;
    return block->free && block->prev == NULL && block->next == NULL && block->size == region_capacity(region->mapped_size);
}

void *malloc(size_t size)
{
    if (size == 0)
        return NULL;

    size = align_up(size, ALLOC_ALIGNMENT);
    if (size == 0)
        return NULL;

    AllocBlock *block = find_block(size);
    if (!block) {
        int dedicated = (size >= DEDICATED_THRESHOLD) ? 1 : 0;
        AllocRegion *region = region_create(size, dedicated);
        if (!region)
            return NULL;
        block = region->first;
        if (dedicated) {
            block->free = 0;
            return (void *)(block + 1);
        }
    }

    split_block(block, size);
    block->free = 0;
    return (void *)(block + 1);
}

void free(void *ptr)
{
    if (!ptr)
        return;

    AllocBlock *block = ((AllocBlock *)ptr) - 1;
    if (block->magic != BLOCK_MAGIC || block->free)
        return;

    block->free = 1;
    block = coalesce_block(block);
    if (!block || !block->region || block->region->magic != REGION_MAGIC)
        return;

    AllocRegion *region = block->region;
    if (!region_is_completely_free(region))
        return;

    size_t mapped_size = region->mapped_size;
    region_unlink(region);
    syscall2(SYS_MUNMAP, (uint64_t)region, mapped_size);
}

void *calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0)
        return NULL;
    if (nmemb > (SIZE_MAX / size))
        return NULL;

    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (!ptr)
        return NULL;
    memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    size = align_up(size, ALLOC_ALIGNMENT);
    if (size == 0)
        return NULL;

    AllocBlock *block = ((AllocBlock *)ptr) - 1;
    if (block->magic != BLOCK_MAGIC)
        return NULL;

    if (block->size >= size) {
        if (block->region && block->region->dedicated && (block->size - size) >= DEDICATED_THRESHOLD) {
            void *moved = malloc(size);
            if (!moved)
                return NULL;
            memcpy(moved, ptr, size);
            free(ptr);
            return moved;
        }
        split_block(block, size);
        return ptr;
    }

    if (block->next && block->next->free) {
        uint8_t *expected = (uint8_t *)(block + 1) + block->size;
        if ((uint8_t *)block->next == expected) {
            size_t combined = block->size + sizeof(AllocBlock) + block->next->size;
            if (combined >= size) {
                try_coalesce_forward(block);
                split_block(block, size);
                block->free = 0;
                return ptr;
            }
        }
    }

    void *new_ptr = malloc(size);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}

int atoi(const char *str)
{
    int res = 0;
    int sign = 1;
    int i = 0;
    if (str[0] == '-') {
        sign = -1;
        i++;
    }
    for (; str[i] != '\0'; ++i) {
        if (str[i] < '0' || str[i] > '9')
            break;
        res = res * 10 + (str[i] - '0');
    }
    return sign * res;
}

void srand(unsigned int seed)
{
    g_rand_seed = seed;
}

int rand(void)
{
    g_rand_seed = g_rand_seed * 1103515245 + 12345;
    return (int)((g_rand_seed / 65536) % 32768);
}
