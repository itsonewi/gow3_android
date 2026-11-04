#ifndef JIT_ALLOCATOR_H
#define JIT_ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a dual-mapped JIT region. */
typedef struct JITRegion JITRegion;

/* Create a dual-mapped JIT region of the given size. RW is for writing generated
 * code, RX is for executing it. Returns NULL on failure. */
JITRegion *jit_region_create(size_t size);

void jit_region_destroy(JITRegion *region);
void *jit_region_rw_ptr(JITRegion *region);
void *jit_region_rx_ptr(JITRegion *region);
size_t jit_region_size(JITRegion *region);
void *jit_region_write(JITRegion *region, size_t offset, const void *code, size_t code_size);
void jit_region_invalidate(JITRegion *region, size_t offset, size_t size);

/* Mark an already-mapped range jetsam-exempt (VM_LEDGER_FLAG_NO_FOOTPRINT).
 * Used for the pool whose RX pages come from StikDebug and whose RW view is a
 * vm_remap alias. Non-fatal: returns false when the private API refuses. */
bool jit_make_region_no_footprint(void *addr, size_t size, const char *label);

/* CS_DEBUGGED / CS_OPS_STATUS, via csops(2). */
bool jit_check_debugged(void);
bool jit_cs_status(uint32_t *flags);

/* The process user address map [min, max) from TASK_VM_INFO. 0xfc0000000
 * (63 GB) without extended virtual addressing, 0x8000000000 (512 GB) with it. */
bool jit_task_map_range(uint64_t *min_address, uint64_t *max_address);

/* Bytes this process may still allocate under the current jetsam limit. */
uint64_t jit_available_memory(void);

/* SIGTRAP fallback for an unattached debugger: skip BRK #0xf00d and return 0. */
void jit_install_trap_handler(void);
void jit_arm_trap_fallback(void);

/* iOS BRK-based debugger protocol used by Madeira:
 *   x16 = 1, x0 = addr (or 0), x1 = len -> debugger prepares RX memory
 *   x16 = 0                              -> debugger detaches
 * The debugger (StikDebug / StikJIT) must be attached and running the bundled
 * spartanport-jit.js script. With no debugger the SIGTRAP fallback returns 0. */
void *jit26_prepare_region(void *addr, size_t len);
void jit26_detach(void);

/* Non-executing dual-map/cache-coherency probe, safe without JIT enabled. */
bool jit_test_mapping(void);

typedef void (*jit_log_callback_t)(const char *message);
void jit_set_log_callback(jit_log_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* JIT_ALLOCATOR_H */
