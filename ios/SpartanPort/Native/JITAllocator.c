/*
 * SpartanPort iOS JIT layer - ported from Madeira's app/Madeira/JITAllocator.c.
 *
 * Madeira is GPL-3.0-or-later; SpartanPort is GPL-2.0. The function bodies
 * below are a rewrite of the same Mach/BRK protocol (the protocol itself is
 * dictated by StikDebug, not by Madeira), trimmed to the calls this app makes
 * and extended with the pool hand-off the runtime needs.
 *
 * Everything here follows the reference layer:
 *   - debugger-allocated RX run, vm_remap'ed RW alias;
 *   - VM_LEDGER_FLAG_NO_FOOTPRINT ladder for the pool;
 *   - CS_DEBUGGED / TASK_VM_INFO probes;
 *   - SIGTRAP fallback that only skips BRK #0xf00d;
 *   - BRK #0xf00d with x16 = 1 (prepare) / 0 (detach).
 */

#include "JITAllocator.h"

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <libkern/OSCacheControl.h>
#include <os/log.h>
#include <os/proc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef CS_DEBUGGED
#define CS_DEBUGGED 0x10000000
#endif
#ifndef CS_OPS_STATUS
#define CS_OPS_STATUS 0
#endif
#ifndef CS_GET_TASK_ALLOW
#define CS_GET_TASK_ALLOW 0x00000004
#endif

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

/* <mach/mach_vm.h> is "unsupported" on the iOS SDK, but libsystem_kernel
 * exports these and they are the only calls that report max_protection. */
extern kern_return_t mach_vm_region(vm_map_t, mach_vm_address_t *, mach_vm_size_t *,
                                    vm_region_flavor_t, vm_region_info_t,
                                    mach_msg_type_number_t *, mach_port_t *);
extern kern_return_t mach_vm_protect(vm_map_t, mach_vm_address_t, mach_vm_size_t,
                                     boolean_t, vm_prot_t);
extern kern_return_t mach_memory_entry_ownership(mach_port_t mem_entry, mach_port_t owner,
                                                 int ledger_tag, int ledger_flags);

/* iOS arm64 uses 16 KB pages. The debugger protocol and the private mapping
 * calls work in host pages. */
#define JIT_PAGE_SIZE 0x4000u
#define JIT_PAGE_MASK (JIT_PAGE_SIZE - 1u)

#ifndef VM_LEDGER_TAG_DEFAULT
#define VM_LEDGER_TAG_DEFAULT 0
#endif
#ifndef VM_LEDGER_FLAG_NO_FOOTPRINT
#define VM_LEDGER_FLAG_NO_FOOTPRINT (1 << 0)
#endif

struct JITRegion {
    void *rw_ptr;
    void *rx_ptr;
    size_t size;
    mach_port_t mem_entry;
};

static jit_log_callback_t g_log_callback = NULL;
static char g_last_error[256];

void jit_set_log_callback(jit_log_callback_t callback) { g_log_callback = callback; }

void sp_jit_set_last_error(const char *message) {
    if (!message) message = "";
    strncpy(g_last_error, message, sizeof(g_last_error) - 1);
    g_last_error[sizeof(g_last_error) - 1] = 0;
}

const char *sp_jit_last_error(void) { return g_last_error; }

static void jit_log(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_log_callback) g_log_callback(buf);
    os_log(OS_LOG_DEFAULT, "[spartanport-jit] %{public}s", buf);
    fprintf(stderr, "[JIT] %s\n", buf);
}

static size_t align_to_page(size_t size) { return (size + JIT_PAGE_MASK) & ~(size_t)JIT_PAGE_MASK; }

static uint64_t phys_footprint_now(void) {
    task_vm_info_data_t vmi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &cnt) != KERN_SUCCESS) return 0;
    return vmi.phys_footprint;
}

uint64_t jit_phys_footprint(void) { return phys_footprint_now(); }

/* 1 writable, 0 definitely not, -1 the kernel did not report the region. */
static int region_is_writable(void *address, size_t size) {
    mach_vm_address_t at = (mach_vm_address_t)(uintptr_t)address;
    mach_vm_size_t span = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &at, &span, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (kr != KERN_SUCCESS) return -1;
    if (span < size) return -1;
    return (info.protection & VM_PROT_WRITE) != 0 ? 1 : 0;
}

/* Madeira ml358/ml458: make an already-mapped range jetsam-exempt.
 *
 * The pool gets its pages from the debugger, so it never passes through
 * jit_region_create(). mach_make_memory_entry_64 over the existing range hands
 * back an entry for the same VM object, and the ownership call re-accounts
 * resident pages as well as future ones. The private API is tried with the
 * same ladder of flags/owners as the reference and the first accepted pair
 * wins; phys_footprint is sampled either side so the log says whether the
 * exemption actually took effect. */
bool jit_make_region_no_footprint(void *addr, size_t size, const char *label) {
    if (!addr || !size) return false;

    mach_port_t task = mach_task_self();
    const uint64_t before = phys_footprint_now();

    struct {
        int flags;
        int own_self;
        const char *name;
    } variants[] = {
        { VM_PROT_READ | VM_PROT_WRITE, 0, "plain/TASK_NULL" },
        { VM_PROT_READ | VM_PROT_WRITE, 1, "plain/self" },
        { MAP_MEM_VM_SHARE | VM_PROT_READ | VM_PROT_WRITE, 1, "share/self" },
        { MAP_MEM_VM_SHARE | VM_PROT_READ | VM_PROT_WRITE, 0, "share/TASK_NULL" },
    };

    for (unsigned v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v) {
        memory_object_size_t entry_size = (memory_object_size_t)size;
        mach_port_t entry = MACH_PORT_NULL;
        kern_return_t kr = mach_make_memory_entry_64(
            task, &entry_size, (memory_object_offset_t)(uintptr_t)addr, variants[v].flags, &entry,
            MACH_PORT_NULL);
        if (kr != KERN_SUCCESS || entry == MACH_PORT_NULL) {
            jit_log("[no-footprint] %s: [%s] make_memory_entry kr=%d (%s)", label, variants[v].name, kr,
                    mach_error_string(kr));
            continue;
        }
        kr = mach_memory_entry_ownership(entry, variants[v].own_self ? task : TASK_NULL,
                                         VM_LEDGER_TAG_DEFAULT, VM_LEDGER_FLAG_NO_FOOTPRINT);
        mach_port_deallocate(task, entry);
        if (kr != KERN_SUCCESS) {
            jit_log("[no-footprint] %s: [%s] ownership kr=%d (%s)", label, variants[v].name, kr,
                    mach_error_string(kr));
            continue;
        }
        const uint64_t after = phys_footprint_now();
        jit_log("[no-footprint] %s: [%s] OK for %zu MB | phys_footprint %llu -> %llu MB (delta %lld MB)",
                label, variants[v].name, size >> 20, (unsigned long long)(before >> 20),
                (unsigned long long)(after >> 20), ((long long)after - (long long)before) >> 20);
        return true;
    }

    jit_log("[no-footprint] %s: all %zu variants refused; region stays jetsam-counted", label,
            sizeof(variants) / sizeof(variants[0]));
    return false;
}

JITRegion *jit_region_create(size_t size) {
    size = align_to_page(size);

    JITRegion *region = calloc(1, sizeof(JITRegion));
    if (!region) return NULL;
    region->size = size;
    region->mem_entry = MACH_PORT_NULL;

    mach_port_t task = mach_task_self();
    memory_object_size_t entry_size = (memory_object_size_t)size;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t kr = mach_make_memory_entry_64(
        task, &entry_size, 0,
        MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE, &entry, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) {
        jit_log("jit_region_create: mach_make_memory_entry_64 kr=%d (%s)", kr, mach_error_string(kr));
        free(region);
        return NULL;
    }
    region->mem_entry = entry;

    mach_vm_address_t rw_addr = 0;
    kr = vm_map(task, (vm_address_t *)&rw_addr, size, 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE,
                VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_DEFAULT);
    if (kr != KERN_SUCCESS) {
        jit_log("jit_region_create: vm_map(RW) kr=%d (%s)", kr, mach_error_string(kr));
        mach_port_deallocate(task, entry);
        free(region);
        return NULL;
    }
    region->rw_ptr = (void *)rw_addr;

    mach_vm_address_t rx_addr = 0;
    kr = vm_map(task, (vm_address_t *)&rx_addr, size, 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE,
                VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
                VM_INHERIT_DEFAULT);
    if (kr != KERN_SUCCESS) {
        jit_log("jit_region_create: vm_map(RX) kr=%d (%s)", kr, mach_error_string(kr));
        vm_deallocate(task, (vm_address_t)region->rw_ptr, size);
        mach_port_deallocate(task, entry);
        free(region);
        return NULL;
    }
    region->rx_ptr = (void *)rx_addr;

    jit_log("dual-mapped region: %zu bytes RW=%p RX=%p", size, region->rw_ptr, region->rx_ptr);
    return region;
}

void jit_region_destroy(JITRegion *region) {
    if (!region) return;
    mach_port_t task = mach_task_self();
    if (region->rw_ptr) vm_deallocate(task, (vm_address_t)region->rw_ptr, region->size);
    if (region->rx_ptr) vm_deallocate(task, (vm_address_t)region->rx_ptr, region->size);
    if (region->mem_entry != MACH_PORT_NULL) mach_port_deallocate(task, region->mem_entry);
    free(region);
}

void *jit_region_rw_ptr(JITRegion *region) { return region ? region->rw_ptr : NULL; }
void *jit_region_rx_ptr(JITRegion *region) { return region ? region->rx_ptr : NULL; }
size_t jit_region_size(JITRegion *region) { return region ? region->size : 0; }

void jit_region_invalidate(JITRegion *region, size_t offset, size_t size) {
    if (!region || !region->rx_ptr) return;
    sys_icache_invalidate((char *)region->rx_ptr + offset, size);
}

void *jit_region_write(JITRegion *region, size_t offset, const void *code, size_t code_size) {
    if (!region) return NULL;
    if (offset + code_size > region->size) {
        jit_log("jit_region_write out of bounds: %zu+%zu > %zu", offset, code_size, region->size);
        return NULL;
    }
    memcpy((char *)region->rw_ptr + offset, code, code_size);
    sys_icache_invalidate((char *)region->rx_ptr + offset, code_size);
    return (char *)region->rx_ptr + offset;
}

void jit_app_image_range(uintptr_t *base, uintptr_t *end) {
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    const uint32_t count = _dyld_image_count();
    if (count == 0) {
        if (base) *base = 0;
        if (end) *end = 0;
        return;
    }
    /* Image 0 is the app executable; its __TEXT/__DATA may be several segments,
     * so walk every segment header. */
    const struct mach_header_64 *header =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    const intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    if (header) {
        const uint8_t *p = (const uint8_t *)(header + 1);
        for (uint32_t i = 0; i < header->ncmds; ++i) {
            const struct load_command *lc = (const struct load_command *)p;
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
                uintptr_t a = (uintptr_t)(seg->vmaddr + slide);
                uintptr_t b = a + (uintptr_t)seg->vmsize;
                if (a < lo) lo = a;
                if (b > hi) hi = b;
            }
            if (lc->cmdsize == 0) break;
            p += lc->cmdsize;
        }
    }
    if (hi <= lo) {
        lo = 0;
        hi = 0;
    }
    if (base) *base = lo;
    if (end) *end = hi;
}

bool jit_cs_status(uint32_t *flags) {
    uint32_t value = 0;
    if (csops(getpid(), CS_OPS_STATUS, &value, sizeof(value)) != 0) return false;
    if (flags) *flags = value;
    return true;
}

bool jit_check_debugged(void) {
    uint32_t flags = 0;
    if (!jit_cs_status(&flags)) {
        jit_log("csops failed; assuming not debugged");
        return false;
    }
    const bool debugged = (flags & CS_DEBUGGED) != 0;
    jit_log("CS_DEBUGGED: %s (flags=0x%x)", debugged ? "SET" : "NOT SET", flags);
    return debugged;
}

bool jit_task_map_range(uint64_t *min_address, uint64_t *max_address) {
    task_vm_info_data_t vmi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &cnt) != KERN_SUCCESS ||
        !vmi.max_address) {
        return false;
    }
    if (min_address) *min_address = vmi.min_address;
    if (max_address) *max_address = vmi.max_address;
    return true;
}

uint64_t jit_available_memory(void) { return (uint64_t)os_proc_available_memory(); }

/* SIGTRAP fallback: skip only our own BRK #0xf00d and set x0 = 0. Any other
 * trap (a Swift runtime trap, for example) keeps its default action and
 * returns, so it traps again and produces a crash report. */
static void sigtrap_handler(int sig, siginfo_t *info, void *context) {
    (void)info;
    ucontext_t *uc = (ucontext_t *)context;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    if ((pc & 3) || *(const uint32_t *)(uintptr_t)pc != 0xd43e01a0u /* brk #0xf00d */) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigaction(sig, &dfl, NULL);
        return;
    }
    uc->uc_mcontext->__ss.__pc += 4;
    uc->uc_mcontext->__ss.__x[0] = 0;
}

void jit_install_trap_handler(void) {
    if (jit_check_debugged()) {
        /* A debugger holds the Mach exception port and gets first refusal; a
         * BSD SIGTRAP handler only runs for traps the Mach layer declined, so
         * installing it cannot steal a serviced BRK. */
        jit_log("debugger attached; installing the SIGTRAP fallback anyway");
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = sigtrap_handler;
    sigaction(SIGTRAP, &sa, NULL);
    jit_log("SIGTRAP handler installed");
}

void jit_arm_trap_fallback(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = sigtrap_handler;
    sigaction(SIGTRAP, &sa, NULL);
    jit_log("SIGTRAP fallback armed while CS_DEBUGGED is set");
}

/* iOS BRK-based debugger protocol (BRK #0xf00d). The attached debugger reads
 * x16/x0/x1, performs the operation and writes the result to x0. */
__attribute__((noinline, optnone))
void *jit26_prepare_region(void *addr, size_t len) {
    register void *x0 __asm__("x0") = addr;
    register size_t x1 __asm__("x1") = len;
    __asm__ volatile(
        "mov x16, #1\n"
        "brk #0xf00d\n"
        : "+r"(x0)
        : "r"(x1)
        : "x16", "memory");
    return x0;
}

__attribute__((noinline, optnone))
void jit26_detach(void) {
    __asm__ volatile(
        "mov x16, #0\n"
        "brk #0xf00d\n"
        ::: "x16", "memory");
}

static void sp_jit_fail(int code, const char *message) {
    sp_jit_set_last_error(message);
    jit_log("pool error %d: %s", code, message);
}

/* The pool: debugger-allocated RX + a RW alias of the same VM object.
 *
 * The RX run is requested with the debugger's _M command (x0 = 0). StikDebug's
 * prepare_memory_region() then authorizes those pages for execution. On iOS the
 * returned pages are normally RWX, which is what FEX needs (it writes code and
 * executes it at the same address); rx_writable records what the kernel says.
 * The RW alias is kept for host-side writes and for the no-footprint call. */
int sp_pool_prepare(void *hint, size_t size, SPPool *out) {
    if (!out) return SP_JIT_ALLOC_FAILED;
    memset(out, 0, sizeof(*out));
    sp_jit_set_last_error("");

    size = align_to_page(size);
    if (size < JIT_PAGE_SIZE) size = JIT_PAGE_SIZE;

    if (!jit_check_debugged()) {
        sp_jit_fail(SP_JIT_NO_DEBUGGER,
                    "JIT is not enabled: CS_DEBUGGED is clear. Enable JIT with StikDebug first.");
        return SP_JIT_NO_DEBUGGER;
    }

    void *rx = jit26_prepare_region(hint, size);
    if (!rx) {
        sp_jit_fail(SP_JIT_ALLOC_FAILED,
                    "The debugger did not answer the pool request (no RX memory). Keep StikDebug "
                    "attached and running spartanport-jit.js.");
        return SP_JIT_ALLOC_FAILED;
    }
    if (((uintptr_t)rx & JIT_PAGE_MASK) != 0) {
        sp_jit_fail(SP_JIT_BAD_PLACEMENT, "The debugger returned an unaligned RX address.");
        vm_deallocate(mach_task_self(), (vm_address_t)rx, size);
        return SP_JIT_BAD_PLACEMENT;
    }

    uintptr_t image_base = 0, image_end = 0;
    jit_app_image_range(&image_base, &image_end);
    if ((uintptr_t)rx < image_end && image_base < (uintptr_t)rx + size) {
        char message[192];
        snprintf(message, sizeof(message),
                 "The debugger put the pool at %p, inside the app image [%p,%p). Relaunch.",
                 rx, (void *)image_base, (void *)image_end);
        sp_jit_fail(SP_JIT_BAD_PLACEMENT, message);
        vm_deallocate(mach_task_self(), (vm_address_t)rx, size);
        return SP_JIT_BAD_PLACEMENT;
    }

    int writable = region_is_writable(rx, size);
    if (writable < 0) {
        jit_log("cannot read the pool's protection mask; assuming the debugger's pages are RWX");
        writable = -1;
    }

    mach_port_t task = mach_task_self();
    vm_address_t rw_addr = 0;
    vm_prot_t cur = 0, max = 0;
    /* Prefer the alias directly above the RX run: the distance stays stable and
     * the alias cannot accidentally land in the guest window below. */
    vm_address_t candidates[2] = { (vm_address_t)((uintptr_t)rx + size), 0 };
    kern_return_t kr = KERN_FAILURE;
    for (unsigned i = 0; i < 2; ++i) {
        rw_addr = candidates[i];
        cur = max = 0;
        kr = vm_remap(task, &rw_addr, (vm_size_t)size, 0, VM_FLAGS_ANYWHERE, task,
                      (vm_address_t)rx, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr == KERN_SUCCESS) break;
    }
    if (kr != KERN_SUCCESS) {
        char message[160];
        snprintf(message, sizeof(message), "vm_remap of the RW alias failed (kr=%d).", kr);
        sp_jit_fail(SP_JIT_ALIAS_FAILED, message);
        vm_deallocate(task, (vm_address_t)rx, size);
        return SP_JIT_ALIAS_FAILED;
    }

    if (vm_protect(task, rw_addr, (vm_size_t)size, FALSE, VM_PROT_READ | VM_PROT_WRITE) !=
        KERN_SUCCESS) {
        sp_jit_fail(SP_JIT_PROTECT_FAILED, "The RW alias could not be made writable.");
        vm_deallocate(task, rw_addr, size);
        vm_deallocate(task, (vm_address_t)rx, size);
        return SP_JIT_PROTECT_FAILED;
    }

    /* FEX writes its generated code at the address it executes it from. When
     * the debugger's pages are RWX (the usual case) that is the RX run. If TXM
     * made them RX-only, try the same upgrade on both views before giving up:
     * the RW alias is the same VM object, so TEX can authorize it as well. */
    void *exec = rx;
    if (writable == 0) {
        if (vm_protect(task, (vm_address_t)rx, (vm_size_t)size, FALSE,
                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) == KERN_SUCCESS) {
            jit_log("pool RX was not writable; TXM accepted the RWX upgrade on the RX run");
            writable = 1;
        } else if (vm_protect(task, rw_addr, (vm_size_t)size, FALSE,
                              VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) == KERN_SUCCESS) {
            jit_log("pool RX stayed RX-only; the RW alias is RWX and FEX will run from it");
            exec = (void *)(uintptr_t)rw_addr;
            writable = 1;
        } else {
            sp_jit_fail(SP_JIT_PROTECT_FAILED,
                        "The debugger's pages are not writable and TXM refused RWX on both views; "
                        "FEX cannot write its code.");
            vm_deallocate(task, rw_addr, size);
            vm_deallocate(task, (vm_address_t)rx, size);
            return SP_JIT_PROTECT_FAILED;
        }
    }

    out->rx = rx;
    out->rw = (void *)(uintptr_t)rw_addr;
    out->exec = exec;
    out->size = size;
    out->rx_writable = writable != 0 ? 1 : 0;
    out->no_footprint = jit_make_region_no_footprint(out->rw, size, "spartanport-pool") ? 1 : 0;

    jit_log("JIT pool ready: RX=%p RW=%p size=%zuMB rwx=%d no-footprint=%d", out->rx, out->rw,
            size >> 20, out->rx_writable, out->no_footprint);
    return SP_JIT_OK;
}

int sp_pool_prepare_default(size_t size, SPPool *out) { return sp_pool_prepare(NULL, size, out); }

void sp_pool_detach(void) {
    if (!jit_check_debugged()) {
        jit_log("detach skipped: no debugger session");
        return;
    }
    jit26_detach();
    jit_log("detach requested");
}

void sp_pool_release(SPPool *pool) {
    if (!pool || !pool->size) return;
    mach_port_t task = mach_task_self();
    if (pool->rw) vm_deallocate(task, (vm_address_t)pool->rw, (vm_size_t)pool->size);
    if (pool->rx) vm_deallocate(task, (vm_address_t)pool->rx, (vm_size_t)pool->size);
    memset(pool, 0, sizeof(*pool));
}

bool jit_test_mapping(void) {
    jit_log("=== JIT mapping test (non-executing) ===");

    JITRegion *region = jit_region_create(JIT_PAGE_SIZE);
    if (!region) {
        jit_log("FAIL: no dual-mapped region");
        return false;
    }
    if (region->rw_ptr == region->rx_ptr) {
        jit_log("FAIL: RW and RX are the same address");
        jit_region_destroy(region);
        return false;
    }

    const uint32_t pattern = 0xDEADBEEF;
    memcpy(region->rw_ptr, &pattern, sizeof(pattern));
    uint32_t readback = 0;
    memcpy(&readback, region->rx_ptr, sizeof(readback));
    if (readback != pattern) {
        jit_log("FAIL: dual-map incoherent (wrote 0x%x, read 0x%x)", pattern, readback);
        jit_region_destroy(region);
        return false;
    }

    vm_address_t addr = (vm_address_t)region->rx_ptr;
    vm_size_t span = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = vm_region_64(mach_task_self(), &addr, &span, VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (kr == KERN_SUCCESS && (info.protection & VM_PROT_EXECUTE)) {
        jit_log("OK: RX page has execute permission");
    } else {
        jit_log("WARN: RX page may lack execute permission (kr=%d prot=0x%x)", kr,
                kr == KERN_SUCCESS ? info.protection : 0);
    }

    jit_region_destroy(region);
    jit_log("=== mapping test passed ===");
    return true;
}
