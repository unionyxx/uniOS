#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "debug.h"
#include "spinlock.h"

// Heap lock for thread safety
static Spinlock heap_lock = SPINLOCK_INIT;

// Bucket allocator implementation
// Buckets for sizes: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
#define MIN_BUCKET_SIZE 16
#define MAX_BUCKET_SIZE 4096
#define NUM_BUCKETS 9

struct FreeBlock {
    FreeBlock* next;
};

struct AllocHeader {
    size_t size; // Size of the user data + header, rounded up to bucket size
    uint64_t magic;
};

#define HEAP_MAGIC 0xC0FFEE1234567890

static FreeBlock* buckets[NUM_BUCKETS];

static int get_bucket_index(size_t size) {
    if (size <= 16) return 0;
    if (size <= 32) return 1;
    if (size <= 64) return 2;
    if (size <= 128) return 3;
    if (size <= 256) return 4;
    if (size <= 512) return 5;
    if (size <= 1024) return 6;
    if (size <= 2048) return 7;
    if (size <= 4096) return 8;
    return -1;
}

static size_t get_bucket_size(int index) {
    return 16 << index;
}

void heap_init(void* start, size_t size) {
    // We don't use the initial blob anymore, we allocate pages on demand from PMM
    // But we can use the provided initial memory to seed the allocator if needed.
    // For now, just initialize buckets to null.
    for (int i = 0; i < NUM_BUCKETS; i++) {
        buckets[i] = nullptr;
    }
    (void)start; // Unused
    (void)size;  // Unused
}

static void* heap_alloc_large(size_t size) {
    size_t pages = (size + sizeof(AllocHeader) + 4095) / 4096;
    void* ptr = pmm_alloc_frames(pages);
    if (!ptr) return nullptr;
    
    // Convert physical to virtual address (HHDM)
    uint64_t virt = vmm_phys_to_virt((uint64_t)ptr);
    AllocHeader* header = (AllocHeader*)virt;
    header->size = pages * 4096;
    header->magic = HEAP_MAGIC;
    
    return (void*)(header + 1);
}

// Internal malloc without lock (for recursive calls within locked context)
static void* malloc_unlocked(size_t size);

static void* malloc_unlocked(size_t size) {
    if (size == 0) return nullptr;
    
    size_t total_size = size + sizeof(AllocHeader);
    
    if (total_size > MAX_BUCKET_SIZE) {
        return heap_alloc_large(size);
    }
    
    int bucket_idx = get_bucket_index(total_size);
    size_t bucket_size = get_bucket_size(bucket_idx);
    
    if (buckets[bucket_idx]) {
        FreeBlock* block = buckets[bucket_idx];
        buckets[bucket_idx] = block->next;
        
        AllocHeader* header = (AllocHeader*)block;
        header->size = bucket_size;
        header->magic = HEAP_MAGIC;
        
        return (void*)(header + 1);
    }
    
    // Bucket empty, allocate a new page and split it
    void* page_phys = pmm_alloc_frame();
    if (!page_phys) return nullptr;
    
    uint64_t page_virt = vmm_phys_to_virt((uint64_t)page_phys);
    
    // Split page into blocks
    size_t num_blocks = 4096 / bucket_size;
    for (size_t i = 0; i < num_blocks; i++) {
        FreeBlock* block = (FreeBlock*)(page_virt + i * bucket_size);
        block->next = buckets[bucket_idx];
        buckets[bucket_idx] = block;
    }
    
    // Recursive call within locked context
    return malloc_unlocked(size);
}

void* malloc(size_t size) {
    spinlock_acquire(&heap_lock);
    void* result = malloc_unlocked(size);
    spinlock_release(&heap_lock);
    return result;
}

// Allocate memory with specified alignment
// alignment must be a power of 2 and >= sizeof(void*)
void* aligned_alloc(size_t alignment, size_t size) {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    
    // Allocate extra space for alignment and storing original pointer
    size_t total = size + alignment + sizeof(void*);
    void* raw = malloc(total);
    if (!raw) return nullptr;
    
    // Align the pointer
    uintptr_t raw_addr = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned_addr = (raw_addr + alignment - 1) & ~(alignment - 1);
    
    // Store original pointer just before the aligned address
    ((void**)aligned_addr)[-1] = raw;
    
    return (void*)aligned_addr;
}

// Free memory allocated with aligned_alloc
void aligned_free(void* ptr) {
    if (!ptr) return;
    // Retrieve original pointer stored before aligned address
    void* raw = ((void**)ptr)[-1];
    free(raw);
}

void free(void* ptr) {
    if (!ptr) return;
    
    spinlock_acquire(&heap_lock);
    
    AllocHeader* header = (AllocHeader*)ptr - 1;
    if (header->magic != HEAP_MAGIC) {
        spinlock_release(&heap_lock);
        DEBUG_ERROR("Heap corruption detected at %p (magic: %lx)", ptr, header->magic);
        return;
    }
    
    size_t size = header->size;
    
    if (size > MAX_BUCKET_SIZE) {
        // Large allocation - convert virt to phys and free all pages
        size_t pages = size / 4096;
        uint64_t virt = (uint64_t)header;
        uint64_t phys = vmm_virt_to_phys(virt);
        
        for (size_t i = 0; i < pages; i++) {
             pmm_free_frame((void*)(phys + i * 4096));
        }
        spinlock_release(&heap_lock);
        return;
    }
    
    int bucket_idx = get_bucket_index(size);
    
    FreeBlock* block = (FreeBlock*)header;
    block->next = buckets[bucket_idx];
    buckets[bucket_idx] = block;
    
    spinlock_release(&heap_lock);
}

void* operator new(size_t size) {
    return malloc(size);
}

void* operator new[](size_t size) {
    return malloc(size);
}

void operator delete(void* ptr) {
    free(ptr);
}

void operator delete[](void* ptr) {
    free(ptr);
}

void operator delete(void* ptr, size_t size) {
    (void)size;
    free(ptr);
}

void operator delete[](void* ptr, size_t size) {
    (void)size;
    free(ptr);
}
