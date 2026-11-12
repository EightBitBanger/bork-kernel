/*
 * malloc for user space, on top of SYS_SBRK.
 *
 * The kernel maps zeroed pages for [heap start, break) and grows or shrinks
 * that range on request. Everything else happens here.
 *
 * Layout: the heap is one run of chunks, each starting with an 8-byte header
 *
 *     [prev_size][size | flags][payload ...................]
 *      ^ chunk                  ^ pointer handed out (16-byte aligned)
 *
 *   size       whole chunk, header included, a multiple of 16 (>= 16)
 *   INUSE      this chunk is allocated
 *   PREV_INUSE the chunk before it is allocated; when it is not, prev_size
 *              holds that chunk's size, so free() can merge backwards
 *
 * No two free chunks are ever adjacent: free() merges with both neighbors.
 * The last chunk is "top", the free space up to the break. It is never in a
 * bin; it is grown with sbrk when nothing fits, and shrunk again (memory
 * returned to the kernel) once more than TRIM_THRESHOLD of it is free.
 *
 * Free chunks keep their list links in the payload. Bins:
 *   0..30   exact sizes 16, 32, ... 496
 *   31..63  power-of-two ranges [512,1K) [1K,2K) ... (best fit inside)
 * A bitmap of non-empty bins finds the next bin up in one or two scans.
 *
 * If something else moves the break (a direct sbrk call), the heap simply
 * continues in the new region: a permanently allocated "fence" chunk spans
 * the gap, so the chunk walk stays contiguous.
 *
 * Not thread-safe; processes are single-threaded today. Add a lock around
 * the public entry points once threads arrive.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGNMENT       16U
#define HEADER          8U                  // prev_size + size
#define MIN_CHUNK       16U                 // header + two list links
#define INUSE           0x1U
#define PREV_INUSE      0x2U
#define FLAG_MASK       0xFU

#define SMALL_BINS      31U                 // Exact bins for 16..496
#define SMALL_LIMIT     512U
#define NBINS           64U

#define GROW_MIN        (64U * 1024U)       // Smallest sbrk step
#define TRIM_THRESHOLD  (256U * 1024U)      // Free top above this is returned...
#define TRIM_KEEP       (128U * 1024U)      // ...down to this much
#define PAGE            4096U

// Largest request: keeps size arithmetic far from overflow
#define MAX_REQUEST     (0x7FFFFFFFU - 2U * PAGE)

typedef struct chunk {
    size_t prev_size;           // Valid only when the previous chunk is free
    size_t head;                // Size | flags
    struct chunk* fd;           // Free chunks only: next in bin
    struct chunk* bk;           // Free chunks only: previous in bin
} chunk_t;

static chunk_t* bins[NBINS];
static uint32_t binmap[NBINS / 32];

static chunk_t* top;            // NULL until the first allocation
static char*    heap_base;      // First chunk
static char*    brk_end;        // The break as we last left it

static inline size_t   csize(const chunk_t* c)   { return c->head & ~FLAG_MASK; }
static inline chunk_t* cnext(const chunk_t* c)   { return (chunk_t*)((char*)c + csize(c)); }
static inline chunk_t* cprev(const chunk_t* c)   { return (chunk_t*)((char*)c - c->prev_size); }
static inline void*    c2mem(const chunk_t* c)   { return (char*)c + HEADER; }
static inline chunk_t* mem2c(const void* p)      { return (chunk_t*)((char*)p - HEADER); }

static inline uintptr_t align_up(uintptr_t v, uintptr_t a) { return (v + a - 1) & ~(a - 1); }

__attribute__((noreturn)) static void heap_corrupt(const char* what) {
    static const char prefix[] = "malloc: heap corruption: ";
    write(2, prefix, sizeof(prefix) - 1);
    write(2, what, strlen(what));
    write(2, "\n", 1);
    abort();
}

//
// Bins
//

static unsigned bin_index(size_t size) {
    if (size < SMALL_LIMIT)
        return (unsigned)(size / ALIGNMENT) - 1U;
    // 512 = 2^9 -> 31, 1024 -> 32, ...
    unsigned log2 = 31U - (unsigned)__builtin_clz((unsigned)size);
    unsigned idx = SMALL_BINS + (log2 - 9U);
    return idx < NBINS ? idx : NBINS - 1U;
}

static void bin_insert(chunk_t* c) {
    unsigned idx = bin_index(csize(c));
    c->bk = NULL;
    c->fd = bins[idx];
    if (c->fd) c->fd->bk = c;
    bins[idx] = c;
    binmap[idx / 32] |= 1U << (idx % 32);
}

static void bin_remove(chunk_t* c) {
    unsigned idx = bin_index(csize(c));
    if (c->bk) c->bk->fd = c->fd;
    else       bins[idx] = c->fd;
    if (c->fd) c->fd->bk = c->bk;
    if (bins[idx] == NULL)
        binmap[idx / 32] &= ~(1U << (idx % 32));
}

// First non-empty bin with index > idx, or -1
static int bin_next_nonempty(unsigned idx) {
    unsigned i = idx + 1;
    while (i < NBINS) {
        uint32_t bits = binmap[i / 32] & (~0U << (i % 32));
        if (bits)
            return (int)((i & ~31U) + (unsigned)__builtin_ctz(bits));
        i = (i | 31U) + 1;
    }
    return -1;
}

//
// Heap growth
//

static bool heap_init(void) {
    char* base = (char*)sbrk(0);
    if (base == (char*)-1)
        return false;

    // Chunks sit at 8 mod 16 so payloads are 16-byte aligned
    uintptr_t first = align_up((uintptr_t)base + HEADER, ALIGNMENT) - HEADER;
    uintptr_t end = align_up(first + GROW_MIN, PAGE);

    if (sbrk((intptr_t)(end - (uintptr_t)base)) == (void*)-1)
        return false;

    heap_base = (char*)first;
    brk_end   = (char*)end;
    top       = (chunk_t*)first;
    top->prev_size = 0;
    top->head = ((end - first) & ~(uintptr_t)FLAG_MASK) | PREV_INUSE;
    return true;
}

static void chunk_free(chunk_t* c);

// Make top at least `need` bytes. False (errno ENOMEM) if the kernel refuses.
static bool heap_grow(size_t need) {
    size_t have = csize(top);
    if (have >= need)
        return true;

    char* cur = (char*)sbrk(0);
    if (cur == (char*)-1)
        return false;

    if (cur == brk_end) {
        // Contiguous: extend top in place
        size_t want = need - have;
        if (want < GROW_MIN) want = GROW_MIN;
        uintptr_t new_end = align_up((uintptr_t)brk_end + want, PAGE);
        if (new_end < (uintptr_t)brk_end || new_end - (uintptr_t)brk_end > 0x7FFFFFFFU) {
            errno = ENOMEM;
            return false;
        }
        if (sbrk((intptr_t)(new_end - (uintptr_t)brk_end)) == (void*)-1)
            return false;
        brk_end = (char*)new_end;
        top->head = (((uintptr_t)brk_end - (uintptr_t)top) & ~(uintptr_t)FLAG_MASK) | (top->head & PREV_INUSE);
        return csize(top) >= need;
    }

    if (cur < brk_end) {
        // Someone shrank the break under the heap: nothing safe to do
        heap_corrupt("break moved below the heap");
    }

    // Someone else moved the break: continue the heap in a new region
    uintptr_t first = align_up((uintptr_t)cur + HEADER, ALIGNMENT) - HEADER;
    uintptr_t new_end = align_up(first + need + MIN_CHUNK + GROW_MIN, PAGE);
    if (new_end < first || new_end - (uintptr_t)cur > 0x7FFFFFFFU) {
        errno = ENOMEM;
        return false;
    }
    if (sbrk((intptr_t)(new_end - (uintptr_t)cur)) == (void*)-1)
        return false;

    // The old top keeps its last 16 bytes as a fence chunk spanning the
    // gap; the rest of it (if any) becomes an ordinary free chunk
    chunk_t* old = top;
    size_t old_size = csize(old);
    chunk_t* fence = (chunk_t*)((char*)old + old_size - MIN_CHUNK);
    chunk_t* freed = NULL;

    if (old_size >= 2 * MIN_CHUNK) {
        old->head = (old_size - MIN_CHUNK) | INUSE | (old->head & PREV_INUSE);
        fence->head = ((uintptr_t)first - (uintptr_t)fence) | INUSE | PREV_INUSE;
        freed = old;
    } else {
        fence = old;
        fence->head = ((uintptr_t)first - (uintptr_t)fence) | INUSE | (old->head & PREV_INUSE);
    }

    top = (chunk_t*)first;
    top->head = ((new_end - first) & ~(uintptr_t)FLAG_MASK) | PREV_INUSE;
    brk_end = (char*)new_end;

    if (freed)
        chunk_free(freed);      // Goes to a bin: its neighbor is the fence

    return csize(top) >= need;
}

// Give the kernel back what top holds past TRIM_KEEP
static void heap_trim(void) {
    size_t size = csize(top);
    if (size <= TRIM_THRESHOLD)
        return;
    if ((char*)sbrk(0) != brk_end)
        return;     // Someone else owns the break end now

    size_t release = (size - TRIM_KEEP) & ~(size_t)(PAGE - 1);
    if (release == 0)
        return;
    if (sbrk(-(intptr_t)release) == (void*)-1)
        return;     // Keep it; nothing is lost
    brk_end -= release;
    top->head = (size - release) | (top->head & PREV_INUSE);
}

//
// Chunk operations
//

// Request size -> chunk size, or 0 if too large
static size_t request_to_size(size_t n) {
    if (n > MAX_REQUEST)
        return 0;
    size_t s = align_up(n + HEADER, ALIGNMENT);
    return s < MIN_CHUNK ? MIN_CHUNK : s;
}

// c is allocated: cut it down to `size`, freeing the tail if it is big enough
static void chunk_shrink(chunk_t* c, size_t size) {
    size_t cur = csize(c);
    if (cur - size < MIN_CHUNK)
        return;

    chunk_t* tail = (chunk_t*)((char*)c + size);
    c->head = size | INUSE | (c->head & PREV_INUSE);
    tail->head = (cur - size) | INUSE | PREV_INUSE;
    chunk_free(tail);           // Merges with whatever follows
}

// Free a chunk: merge with free neighbors, then bin it or fold it into top
static void chunk_free(chunk_t* c) {
    size_t size = csize(c);
    chunk_t* next = cnext(c);

    if (!(c->head & PREV_INUSE)) {
        chunk_t* prev = cprev(c);
        if (csize(prev) != c->prev_size || (prev->head & INUSE))
            heap_corrupt("bad prev_size");
        bin_remove(prev);
        size += csize(prev);
        c = prev;
    }
    size_t prev_flag = c->head & PREV_INUSE;

    if (next == top) {
        top = c;
        top->head = (size + csize(next)) | prev_flag;
        heap_trim();
        return;
    }

    if (!(next->head & INUSE)) {
        bin_remove(next);
        size += csize(next);
    }

    c->head = size | prev_flag;
    chunk_t* after = (chunk_t*)((char*)c + size);
    after->prev_size = size;
    after->head &= ~(size_t)PREV_INUSE;
    bin_insert(c);
}

static chunk_t* chunk_alloc(size_t size) {
    unsigned idx = bin_index(size);
    chunk_t* c = NULL;

    // Same bin: exact for small sizes, best fit for ranges
    if (bins[idx]) {
        if (idx < SMALL_BINS) {
            c = bins[idx];
        } else {
            for (chunk_t* p = bins[idx]; p; p = p->fd) {
                if (csize(p) >= size && (c == NULL || csize(p) < csize(c))) {
                    c = p;
                    if (csize(p) == size) break;
                }
            }
        }
    }

    // Any chunk in a higher bin is large enough
    if (c == NULL) {
        int j = bin_next_nonempty(idx);
        if (j >= 0) c = bins[j];
    }

    if (c != NULL) {
        bin_remove(c);
        c->head |= INUSE;
        cnext(c)->head |= PREV_INUSE;
        chunk_shrink(c, size);
        return c;
    }

    // Carve from top, always leaving top a valid chunk behind
    if (!heap_grow(size + MIN_CHUNK))
        return NULL;

    c = top;
    size_t rest = csize(top) - size;
    c->head = size | INUSE | (top->head & PREV_INUSE);
    top = (chunk_t*)((char*)c + size);
    top->head = rest | PREV_INUSE;
    return c;
}

// The chunk behind a user pointer, after basic checks
static chunk_t* checked_chunk(void* ptr, const char* who) {
    chunk_t* c = mem2c(ptr);
    if (((uintptr_t)ptr & (ALIGNMENT - 1)) != 0 ||
        (char*)c < heap_base || (char*)c >= (char*)top)
        heap_corrupt(who);
    if (!(c->head & INUSE))
        heap_corrupt(who);
    if ((char*)cnext(c) > (char*)top || !(cnext(c)->head & PREV_INUSE))
        heap_corrupt(who);
    return c;
}

//
// Public API
//

void* malloc(size_t n) {
    size_t size = request_to_size(n);
    if (size == 0) {
        errno = ENOMEM;
        return NULL;
    }
    if (top == NULL && !heap_init())
        return NULL;

    chunk_t* c = chunk_alloc(size);
    return c ? c2mem(c) : NULL;
}

void free(void* ptr) {
    if (ptr == NULL)
        return;
    if (top == NULL)
        heap_corrupt("free() before any malloc()");
    chunk_free(checked_chunk(ptr, "free(): invalid pointer or double free"));
}

void* calloc(size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    void* p = malloc(total);
    if (p) memset(p, 0, total);     // Reused chunks hold old data
    return p;
}

void* realloc(void* ptr, size_t n) {
    if (ptr == NULL)
        return malloc(n);
    if (n == 0) {
        free(ptr);
        return NULL;
    }

    chunk_t* c = checked_chunk(ptr, "realloc(): invalid pointer");
    size_t size = request_to_size(n);
    if (size == 0) {
        errno = ENOMEM;
        return NULL;
    }

    size_t cur = csize(c);
    if (size <= cur) {
        chunk_shrink(c, size);
        return ptr;
    }

    // Grow in place into top
    chunk_t* next = cnext(c);
    if (next == top) {
        if (heap_grow(size - cur + MIN_CHUNK) && cnext(c) == top) {
            size_t rest = csize(top) - (size - cur);
            c->head = size | INUSE | (c->head & PREV_INUSE);
            top = (chunk_t*)((char*)c + size);
            top->head = rest | PREV_INUSE;
            return ptr;
        }
    } else if (!(next->head & INUSE) && cur + csize(next) >= size) {
        // ... or into a free neighbor
        bin_remove(next);
        c->head = (cur + csize(next)) | INUSE | (c->head & PREV_INUSE);
        cnext(c)->head |= PREV_INUSE;
        chunk_shrink(c, size);
        return ptr;
    }

    void* q = malloc(n);
    if (q == NULL)
        return NULL;    // The original block is untouched
    memcpy(q, ptr, cur - HEADER);
    free(ptr);
    return q;
}

void* reallocarray(void* ptr, size_t count, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(count, size, &total)) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(ptr, total);
}

void* memalign(size_t alignment, size_t n) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    if (alignment <= ALIGNMENT)
        return malloc(n);
    if (n > MAX_REQUEST - alignment - MIN_CHUNK || alignment > MAX_REQUEST / 2) {
        errno = ENOMEM;
        return NULL;
    }

    size_t size = request_to_size(n);
    char* p = (char*)malloc(n + alignment + MIN_CHUNK);
    if (p == NULL)
        return NULL;

    chunk_t* c = mem2c(p);
    char* aligned = (char*)align_up((uintptr_t)p, alignment);

    if (aligned != p) {
        // Room for a chunk in front of the aligned one, then free it
        if ((size_t)(aligned - p) < MIN_CHUNK)
            aligned += alignment;
        size_t lead = (size_t)(aligned - p);
        size_t total = csize(c);

        chunk_t* ac = mem2c(aligned);
        c->head = lead | INUSE | (c->head & PREV_INUSE);
        ac->head = (total - lead) | INUSE | PREV_INUSE;
        chunk_free(c);
        c = ac;
    }

    chunk_shrink(c, size);
    return c2mem(c);
}

void* aligned_alloc(size_t alignment, size_t n) {
    return memalign(alignment, n);
}

int posix_memalign(void** out, size_t alignment, size_t n) {
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0)
        return EINVAL;
    int saved = errno;
    void* p = memalign(alignment, n);
    int err = errno;
    errno = saved;
    if (p == NULL)
        return err ? err : ENOMEM;
    *out = p;
    return 0;
}

size_t malloc_usable_size(void* ptr) {
    if (ptr == NULL)
        return 0;
    return csize(checked_chunk(ptr, "malloc_usable_size(): invalid pointer")) - HEADER;
}

// Walk the whole heap and verify every invariant. Returns 0 when the heap is
// consistent, -1 otherwise. Debug aid (not standard); O(heap size).
int malloc_check(void) {
    if (top == NULL)
        return 0;

    size_t free_in_walk = 0;
    bool prev_inuse = true;
    chunk_t* c = (chunk_t*)heap_base;

    while (c != top) {
        if ((char*)c > (char*)top) return -1;
        size_t s = csize(c);
        if (s < MIN_CHUNK || (s & (ALIGNMENT - 1))) return -1;
        if (((c->head & PREV_INUSE) != 0) != prev_inuse) return -1;
        if (!prev_inuse && c->prev_size != csize(cprev(c))) return -1;
        bool inuse = (c->head & INUSE) != 0;
        if (!inuse) {
            if (!prev_inuse) return -1;     // Two free chunks in a row
            free_in_walk++;
        }
        prev_inuse = inuse;
        c = cnext(c);
    }
    if (((top->head & PREV_INUSE) != 0) != prev_inuse) return -1;
    if ((char*)top + csize(top) > brk_end) return -1;

    // Every binned chunk is free, in the right bin, and was seen in the walk
    size_t free_in_bins = 0;
    for (unsigned i = 0; i < NBINS; i++) {
        bool bit = (binmap[i / 32] >> (i % 32)) & 1U;
        if (bit != (bins[i] != NULL)) return -1;
        chunk_t* prev = NULL;
        for (chunk_t* p = bins[i]; p; p = p->fd) {
            if (p->bk != prev || (p->head & INUSE) || bin_index(csize(p)) != i) return -1;
            prev = p;
            free_in_bins++;
        }
    }
    return free_in_walk == free_in_bins ? 0 : -1;
}

