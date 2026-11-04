/*
 * Pool-backed executable memory for FEX on iOS.
 *
 * FEXCore's code buffers call mmap(PROT_READ|PROT_WRITE|PROT_EXEC) through
 * FEXCore::Allocator (VirtualAlloc). iOS refuses that outside a debug session,
 * and even inside one the pages must come from the debugger-allocated pool so
 * they are covered by the JIT authorization and the no-footprint ledger.
 *
 * Instead of patching FEX, the app interposes mmap/munmap for its own image
 * (the JITAllocator's pool is given to this file by the session). Every
 * executable anonymous mapping is carved from the pool; non-executable and
 * file-backed mappings go to libSystem unchanged. munmap of a pool range
 * returns it to a small free list (FEX drops and re-creates code buffers as
 * they grow).
 *
 * This is the same contract Madeira's Wine ntdll gives FEX on iOS: the JIT's
 * VirtualAlloc returns pool memory with RX-to-RW bookkeeping left to the pool.
 */

#include "sp_compat.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "spartanport_ios.h"

typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);
typedef int (*munmap_fn)(void *, size_t);

static mmap_fn real_mmap;
static munmap_fn real_munmap;
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;

static void resolve_real(void) {
    real_mmap = (mmap_fn)dlsym(RTLD_NEXT, "mmap");
    real_munmap = (munmap_fn)dlsym(RTLD_NEXT, "munmap");
    if (!real_mmap) fprintf(stderr, "[sp] cannot resolve libSystem mmap\n");
    if (!real_munmap) fprintf(stderr, "[sp] cannot resolve libSystem munmap\n");
}

#define SP_POOL_ALIGN 0x4000u
#define SP_POOL_MAX_FREE 256

typedef struct {
    uintptr_t address;
    size_t size;
} SpFree;

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t pool_base;
static size_t pool_size;
static size_t pool_used;      /* bump frontier, bytes from pool_base */
static SpFree pool_free[SP_POOL_MAX_FREE];
static size_t pool_free_count;
static size_t pool_allocations;
static size_t pool_bytes_given;

void sp_jit_pool_set(void *rx, void *rw, size_t size) {
    (void)rw; /* the RW alias is the host's view; FEX gets the executable one */
    pthread_mutex_lock(&pool_lock);
    pool_base = (uintptr_t)rx;
    pool_size = size & ~(size_t)(SP_POOL_ALIGN - 1);
    pool_used = 0;
    pool_free_count = 0;
    pool_allocations = 0;
    pool_bytes_given = 0;
    pthread_mutex_unlock(&pool_lock);
    if (pool_base) {
        sp_log(SP_LOG_INFO, "FEX executable allocations come from the JIT pool at 0x%llx (%zu MB)",
               (unsigned long long)pool_base, pool_size >> 20);
    }
}

int sp_jit_pool_active(void) {
    int active;
    pthread_mutex_lock(&pool_lock);
    active = pool_base != 0 && pool_size != 0;
    pthread_mutex_unlock(&pool_lock);
    return active;
}

void sp_jit_pool_stats(uint64_t *used, uint64_t *size, uint64_t *allocations) {
    pthread_mutex_lock(&pool_lock);
    if (used) *used = pool_bytes_given;
    if (size) *size = pool_size;
    if (allocations) *allocations = pool_allocations;
    pthread_mutex_unlock(&pool_lock);
}

static int in_pool(uintptr_t address, size_t length, size_t *offset) {
    if (!pool_base || address < pool_base) return 0;
    if (address - pool_base > pool_size || length > pool_size - (address - pool_base)) return 0;
    if (offset) *offset = (size_t)(address - pool_base);
    return 1;
}

static void *pool_alloc_locked(size_t length) {
    size_t need = (length + SP_POOL_ALIGN - 1) & ~(size_t)(SP_POOL_ALIGN - 1);
    if (!need) need = SP_POOL_ALIGN;

    for (size_t i = 0; i < pool_free_count; ++i) {
        if (pool_free[i].size < need) continue;
        uintptr_t address = pool_free[i].address;
        if (pool_free[i].size > need) {
            /* Split the tail: FEX allocations are page-sized runs. */
            pool_free[i].address += need;
            pool_free[i].size -= need;
        } else {
            pool_free[i] = pool_free[--pool_free_count];
        }
        ++pool_allocations;
        pool_bytes_given += need;
        return (void *)address;
    }

    if (pool_used + need > pool_size) return NULL;
    void *address = (void *)(pool_base + pool_used);
    pool_used += need;
    ++pool_allocations;
    pool_bytes_given += need;
    return address;
}

static void pool_free_locked(uintptr_t address, size_t length) {
    size_t need = (length + SP_POOL_ALIGN - 1) & ~(size_t)(SP_POOL_ALIGN - 1);
    if (!need) need = SP_POOL_ALIGN;
    /* FEX protects the last page of a code buffer as a guard (PROT_NONE).
     * Restore the whole run before it can be handed out again, or the old
     * guard page becomes an inaccessible hole inside the next buffer. */
    mprotect((void *)address, need, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (pool_free_count == SP_POOL_MAX_FREE) return; /* leak inside the pool; it is reused at exit */
    pool_free[pool_free_count].address = address;
    pool_free[pool_free_count].size = need;
    ++pool_free_count;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    pthread_once(&resolve_once, resolve_real);
    (void)addr;

    if ((prot & PROT_EXEC) && (flags & MAP_ANON) && fd == -1) {
        pthread_mutex_lock(&pool_lock);
        if (pool_base && pool_size) {
            void *memory = pool_alloc_locked(length);
            pthread_mutex_unlock(&pool_lock);
            if (memory) return memory;
            sp_log(SP_LOG_WARNING,
                   "JIT pool exhausted for a %zu KB executable mapping; FEX will stop here. "
                   "Raise the pool size in the launcher.", length >> 10);
            errno = ENOMEM;
            return MAP_FAILED;
        }
        pthread_mutex_unlock(&pool_lock);
    }

    if (!real_mmap) {
        errno = ENOSYS;
        return MAP_FAILED;
    }
    return real_mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length) {
    pthread_once(&resolve_once, resolve_real);
    pthread_mutex_lock(&pool_lock);
    if (in_pool((uintptr_t)addr, length, NULL)) {
        pool_free_locked((uintptr_t)addr, length);
        pthread_mutex_unlock(&pool_lock);
        return 0;
    }
    pthread_mutex_unlock(&pool_lock);
    if (!real_munmap) {
        errno = ENOSYS;
        return -1;
    }
    return real_munmap(addr, length);
}
