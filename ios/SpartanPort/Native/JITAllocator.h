/*
 * SpartanPort iOS JIT layer.
 *
 * This is the same layer Madeira (https://github.com/willfaust/Madeira,
 * app/Madeira/JITAllocator.c) uses for jailed iOS:
 *
 *   - executable memory is only obtainable while a debugger is attached
 *     (CS_DEBUGGED); StikDebug runs spartanport-jit.js, which answers the
 *     BRK #0xf00d protocol below;
 *   - the JIT pool is one debugger-allocated RX run (16 KB pages) with a
 *     vm_remap'ed RW alias of the same VM object;
 *   - the pool is marked VM_LEDGER_FLAG_NO_FOOTPRINT so the code cache does not
 *     count against the app's jetsam limit (Memory+);
 *   - BRK without a debugger is skipped by a SIGTRAP handler that only accepts
 *     this protocol's BRK, so an unserviced request fails instead of crashing.
 *
 * BRK protocol (spartanport-jit.js):
 *   x16 = 1, x0 = address (0 = let the debugger allocate), x1 = length
 *        -> debugger maps RX there, prepares it for execution and returns the
 *           address in x0;
 *   x16 = 0 -> debugger detaches.
 */
#ifndef SPARTANPORT_JIT_ALLOCATOR_H
#define SPARTANPORT_JIT_ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes for sp_pool_prepare(). */
enum {
    SP_JIT_OK = 0,
    SP_JIT_NO_DEBUGGER = -1,   /* no debugger answered the BRK */
    SP_JIT_ALLOC_FAILED = -2,  /* the debugger refused to map RX */
    SP_JIT_ALIAS_FAILED = -3,  /* vm_remap of the RW alias failed */
    SP_JIT_PROTECT_FAILED = -4,/* the RW alias could not be made writable */
    SP_JIT_BAD_PLACEMENT = -5, /* the debugger put RX where we cannot use it */
};

typedef struct {
    void *rx;          /* executable view the debugger allocated */
    void *rw;          /* writable alias of the same pages (host side) */
    void *exec;        /* what FEX must use: rx when the pages are RWX, else rw if
                          TXM let the alias become RWX (both are the same pages) */
    size_t size;       /* 16 KB aligned */
    int rx_writable;   /* 1 when the debugger's pages are RWX (usual case) */
    int no_footprint;  /* 1 when the jetsam exemption was accepted */
} SPPool;

/* Allocate the dual-mapped pool through the attached debugger. `hint` may be 0.
 * size is rounded up to 16 KB. The debugger stays attached; call
 * sp_pool_detach() once the pool exists. */
int sp_pool_prepare(void *hint, size_t size, SPPool *out);
int sp_pool_prepare_default(size_t size, SPPool *out);
void sp_pool_detach(void);
void sp_pool_release(SPPool *pool);

/* The dual-mapped region API, kept for the diagnostics probe. */
typedef struct JITRegion JITRegion;
JITRegion *jit_region_create(size_t size);
void jit_region_destroy(JITRegion *region);
void *jit_region_rw_ptr(JITRegion *region);
void *jit_region_rx_ptr(JITRegion *region);
size_t jit_region_size(JITRegion *region);
void *jit_region_write(JITRegion *region, size_t offset, const void *code, size_t code_size);
void jit_region_invalidate(JITRegion *region, size_t offset, size_t size);

/* Mark an already-mapped range jetsam-exempt (VM_LEDGER_FLAG_NO_FOOTPRINT).
 * Walks the same small ladder of entry flags/owners as Madeira and logs every
 * step together with phys_footprint either side. */
bool jit_make_region_no_footprint(void *addr, size_t size, const char *label);

/* Process state. */
bool jit_check_debugged(void);
bool jit_cs_status(uint32_t *flags);
bool jit_task_map_range(uint64_t *min_address, uint64_t *max_address);
uint64_t jit_available_memory(void);
uint64_t jit_phys_footprint(void);

/* The app's own Mach-O image, so the runtime can refuse a guest window that
 * would sit on top of it. */
void jit_app_image_range(uintptr_t *base, uintptr_t *end);

/* SIGTRAP fallback and the debugger protocol. */
void jit_install_trap_handler(void);
void jit_arm_trap_fallback(void);
void *jit26_prepare_region(void *addr, size_t len);
void jit26_detach(void);

/* Non-executing dual-map/cache-coherency probe, safe without JIT enabled. */
bool jit_test_mapping(void);

/* Why the last sp_pool_prepare() failed, as a static string. */
const char *sp_jit_last_error(void);
void sp_jit_set_last_error(const char *message);

typedef void (*jit_log_callback_t)(const char *message);
void jit_set_log_callback(jit_log_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* SPARTANPORT_JIT_ALLOCATOR_H */
