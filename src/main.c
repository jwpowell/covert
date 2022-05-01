#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

/**!
 * Returns the discrete log of the value `n` rounded down to the nearest whole
 * number. Equivallently, returns the position of the most significant one.
 *
 * `n` is assumed to be non-zero.
 *
 * Yes, I know there's a branchless algorithm to do this.
 */
uint32_t dlog2(size_t n)
{
    uint32_t k = 0;

    while (n != 0) {
        n >>= 1;
        k += 1;
    }

    return k - 1;
}

/**
 * Executes the CLFLUSH instruction for the byte at `ptr`.
 *
 * The MFENCE was necessary to observe precise timings for `timed_read()`.
 */
void clflush(uint8_t* ptr)
{
    __asm__ __volatile__(
        "clflush (%[ptr])\n"
        "mfence\n"
        :
        : [ptr] "r"(ptr));
}

/**
 * Invokes `clflush()`.
 *
 * This is an abstraction for the purpose of experimentation, but currently
 * redundant.
 */
void cache_flush(uint8_t* ptr)
{
    clflush(ptr);
}

/**
 * Simply reads from the byte and throws it away.
 *
 * Probably can be more simply done with a volatile read of `ptr`.
 */
void cache_fill(uint8_t* ptr)
{
    __asm__ __volatile__("mov (%[ptr]), %%al\n" : : [ptr] "r"(ptr) : "rax");
}

/**
 * Times a read to the byte at `ptr`.
 *
 * This does not have to be accurate. In fact, the two requiremetns are for it
 * to be precise (that is, low deviation) and for the difference between an L1
 * cache hit and all other accesses scenarios.
 */
uint64_t timed_read(uint8_t* ptr)
{
    uint64_t t0[2];
    uint64_t t1[2];

    __asm__ __volatile__(
        "rdtscp\n"
        "lfence\n"
        "mov %%rax, %[t0_0]\n"
        "mov %%rdx, %[t0_1]\n"
        "mov (%[ptr]), %%al\n"
        "rdtscp\n"
        "lfence\n"
        "mov %%rax, %[t1_0]\n"
        "mov %%rdx, %[t1_1]\n"
        : [t0_0] "=r"(t0[0]), [t0_1] "=r"(t0[1]), [t1_0] "=g"(t1[0]),
          [t1_1] "=g"(t1[1])
        : [ptr] "r"(ptr)
        : "rax", "rcx", "rdx", "rbx");

    return (t1[0] | (t1[1] << 32)) - (t0[0] | (t0[1] << 32));
}

/**
 * Metadata and resources used for manipulating the cache
 */
typedef struct cache {
    /// Size of the cache in bytes
    size_t size;

    /// Size of a cache line in bytes
    size_t line_size;

    /// Size of a cache set in bytes
    size_t set_size;

    /// Associativity of the cache - the nubmer of ways in a set.
    size_t assoc;

    /// Number of sets in the cache
    size_t nsets;

    /// Block offset mask for the address
    uintptr_t offset_mask;

    /// Index mask for the address
    uintptr_t index_mask;

    /// Tag mask for the address
    uintptr_t tag_mask;

    /// LSB of the `index_mask`
    int index_shift;

    /// LSB of the `tag_mask`
    int tag_shift;

    /// Measured latency of cache hits
    uint64_t hit_latency;

    /// Measured latency of cache misses
    uint64_t miss_latency;

    /// Heuristic threshold for determining if a timed read is a hit or not.
    /// Currently the average of the hit and miss latency.
    uint64_t hit_threshold;

    /// Size of `buffer` in bytes
    size_t buffer_size;

    /// A buffer a multiple size of the cache used for manipulation of the cache
    uint8_t* buffer;
} cache_t;

/**
 * Initialzie the `cache` structure
 */
int cache_init(cache_t* cache)
{
    memset(cache, 0, sizeof(*cache));

    cache->size = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    cache->line_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    cache->assoc = sysconf(_SC_LEVEL1_DCACHE_ASSOC);

    cache->set_size = (cache->line_size * cache->assoc);
    cache->nsets = cache->size / cache->set_size;

    cache->index_shift = dlog2(cache->line_size);
    cache->tag_shift = dlog2(cache->nsets) + cache->index_shift;

    cache->offset_mask = cache->line_size - 1;
    cache->index_mask = (cache->nsets - 1) << cache->index_shift;
    cache->tag_mask = (~0UL) << cache->tag_shift;

    cache->buffer_size = cache->size * cache->assoc;
    cache->buffer = aligned_alloc(cache->size, cache->buffer_size);

    if (cache->buffer == NULL) {
        return -1;
    }

    const int NTRIALS = 1024;
    uint64_t mean;

    mean = 0;

    for (int trial = 0; trial < NTRIALS; trial++) {
        cache_fill(&cache->buffer[0]);
        mean += timed_read(&cache->buffer[0]);
    }

    cache->hit_latency = mean / NTRIALS;

    mean = 0;

    for (int trial = 0; trial < NTRIALS; trial++) {
        cache_flush(&cache->buffer[0]);
        mean += timed_read(&cache->buffer[0]);
    }

    cache->miss_latency = mean / NTRIALS;

    cache->hit_threshold = (cache->hit_latency + cache->miss_latency) / 2;

    memset(cache->buffer, 0, cache->size);

    return 0;
}

/**
 *  Tear down the `cache` structure
 */
int cache_deinit(cache_t* cache)
{
    free(cache->buffer);

    return 0;
}

/**
 * Flush all ways in a set.
 *
 * Note: This function only makes sense when this process has filled all the
 * ways before this call. As such, this will not likely invalidate lines filled
 * by other proceses. If none of `cache->buffer` is present in the cache, all
 * the CLFLUSHes are allowed to be no-ops. To invalidate lines of another
 * process, use the `cache_fill_set()` function to *take* the lines from the
 * other process.
 */
int cache_flush_set(cache_t* cache, size_t setno)
{
    if (setno >= cache->nsets) {
        return -1;
    }

    uint8_t* ptr = cache->buffer + (setno << cache->index_shift);

    for (size_t k = 0; k < cache->assoc; k++) {
        cache_flush(ptr);
        ptr += cache->nsets << cache->index_shift;
    }

    return 0;
}

/**
 * Fill all ways in a set.
 *
 * This works by reading N distinct blocks in a given index, where N is the
 * associativity of the cache.
 *
 * This function works under the assumptions that the cache replacement policy
 * is LRU.
 */
int cache_fill_set(cache_t* cache, size_t setno)
{
    if (setno >= cache->nsets) {
        return -1;
    }

    uint8_t* ptr = cache->buffer + (setno << cache->index_shift);

    for (size_t k = 0; k < cache->assoc; k++) {
        cache_fill(ptr);
        ptr += cache->nsets << cache->index_shift;
    }

    return 0;
}

/**
 * Performs a timed read on each block in the cache and counts how many blocks
 * are heuristically determined as present.
 *
 * This is useful after a `cache_fill_set()` invocation on the same set. After
 * that call finishes, this process `owns` all the ways in the set. Therefore,
 * if this function is called shortly after `cache_fill_set()` on the same set,
 * one should expect this function to return a number close to `cache->assoc`.
 */
int cache_count_hits(cache_t* cache, size_t setno)
{
    if (setno >= cache->nsets) {
        return -1;
    }

    uint8_t* ptr = cache->buffer + (setno << cache->index_shift);

    int count = 0;

    for (size_t k = 0; k < cache->assoc; k++) {
        uint64_t dur = timed_read(ptr);

        if (cache->hit_threshold >= dur) {
            count += 1;
        }

        ptr += cache->nsets << cache->index_shift;

        if (!(ptr < (cache->buffer + cache->buffer_size))) {
            printf("sadface.\n");
        }
    }

    return count;
}

int pin_current_thread(int cpuno)
{
    pthread_t current = pthread_self();
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(cpuno, &cpuset);

    pthread_setaffinity_np(current, CPU_SETSIZE, &cpuset);

    return 0;
}

/**
 * Transmit the message over the covert channel.
 *
 * `tx` is the set number for transmitting the message. `rx` is the set number
 * for receiving sync messages from receiver.
 */
int transmit(cache_t* cache, int setno, char* msg)
{
    return 0;
}

int receive(cache_t* cache, int setno)
{
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        return 1;
    }

    char* role = argv[1];
    int setno = atoi(argv[2]);
    int cpuno = atoi(argv[3]);

    pin_current_thread(cpuno);

    cache_t cache;

    cache_init(&cache);

    printf("Set:    %d\n", setno);
    printf("CPU:   %d\n", cpuno);

    if (strcmp(role, "transmit") == 0) {
        printf("Role:  TRANSMIT\n");
        transmit(&cache, setno, "hello world!");
    } else if (strcmp(role, "receive") == 0) {
        printf("Role:  RECEIVE\n");
        receive(&cache, setno);
    } else {
        printf("Invalid role: %s\n", role);
    }

    cache_deinit(&cache);

    return 0;
}
