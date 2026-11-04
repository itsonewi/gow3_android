/* JIT layer for SpartanPort iOS.
 *
 * Ported from Madeira (https://github.com/willfaust/Madeira,
 * app/Madeira/JITAllocator.c) and trimmed to what this app needs: the
 * dual-mapped region allocator, the VM_LEDGER_FLAG_NO_FOOTPRINT exemption,
 * the CS_DEBUGGED/address-map probes, the SIGTRAP fallback and the iOS 26
 * BRK #0xf00d debugger protocol. The Wine-specific test harness and the
 * early-VA constructor are intentionally left in Madeira.
 *
 * iOS allows executable memory only for a debugged process. StikDebug (or a
 * built-in StikJIT helper) attaches, runs spartanport-jit.js, and services the
 * BRK requests made here: it allocates RX pages with _M and marks them
 * executable via prepare_memory_region(). This app then vm_remaps a writable
 * alias of the same VM object.
 */

#include "JITAllocator.h"

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <os/log.h>
#include <os/proc.h>

/* csops(2) - used to check the CS_DEBUGGED flag. */
#ifndef CS_DEBUGGED
#define CS_DEBUGGED 0x10000000
#endif
#ifndef CS_OPS_STATUS
#define CS_OPS_STATUS 0
#endif
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

/* <mach/mach_vm.h> is "unsupported" on the iOS SDK, but these are exported by
 * libsystem_kernel and are the only calls that report max_protection. */
extern kern_return_t mach_vm_region(vm_map_t, mach_vm_address_t *, mach_vm_size_t *,
                                    vm_region_flavor_t, vm_region_info_t,
                                    mach_msg_type_number_t *, mach_port_t *);
extern kern_return_t mach_vm_protect(vm_map_t, mach_vm_address_t, mach_vm_size_t,
                                     boolean_t, vm_prot_t);

/* iOS arm64 uses 16 KB pages. Kept explicit because the debugger protocol and
 * the private mapping calls work in host pages. */
#define JIT_PAGE_SIZE 0x4000

#ifndef VM_LEDGER_TAG_DEFAULT
#define VM_LEDGER_TAG_DEFAULT 0
#endif
#ifndef VM_LEDGER_FLAG_NO_FOOTPRINT
#define VM_LEDGER_FLAG_NO_FOOTPRINT (1 << 0)
#endif

extern kern_return_t mach_memory_entry_ownership(
    mach_port_t mem_entry,
    mach_port_t owner,
    int ledger_tag,
    int ledger_flags);

struct JITRegion {
    void *rw_ptr;           /* writable view */
    void *rx_ptr;           /* executable view */
    size_t size;
    mach_port_t mem_entry;
};

static jit_log_callback_t g_log_callback = NULL;

void jit_set_log_callback(jit_log_callback_t callback) {
    g_log_callback = callback;
}

static void jit_log(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_log_callback) {
        g_log_callback(buf);
    }
    os_log(OS_LOG_DEFAULT, "[JIT] %{public}s", buf);
    fprintf(stderr, "[JIT] %s\n", buf);
}

static size_t align_to_page(size_t size) {
    return (size + JIT_PAGE_SIZE - 1) & ~(size_t)(JIT_PAGE_SIZE - 1);
}

JITRegion *jit_region_create(size_t size) {
    size = align_to_page(size);

    JITRegion *region = calloc(1, sizeof(JITRegion));
    if (!region) {
        jit_log("failed to allocate JITRegion struct");
        return NULL;
    }
    region->size = size;
    region->mem_entry = MACH_PORT_NULL;

    kern_return_t kr;
    mach_port_t task = mach_task_self();

    /* Create a named memory entry, mark it no-footprint, then map RW and RX
     * views of the same VM object. */
    memory_object_size_t entry_size = (memory_object_size_t)size;
    mach_port_t mem_entry = MACH_PORT_NULL;

    kr = mach_make_memory_entry_64(
        task,
        &entry_size,
        0,
        MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
        &mem_entry,
        MACH_PORT_NULL);

    if (kr != KERN_SUCCESS) {
        jit_log("mach_make_memory_entry_64 failed: %s (kr=%d)", mach_error_string(kr), kr);
        free(region);
        return NULL;
    }

    region->mem_entry = mem_entry;
    jit_log("created memory entry on port %d, size=%zu", mem_entry, (size_t)entry_size);

    kr = mach_memory_entry_ownership(mem_entry, TASK_NULL,
                                     VM_LEDGER_TAG_DEFAULT,
                                     VM_LEDGER_FLAG_NO_FOOTPRINT);
    if (kr != KERN_SUCCESS) {
        jit_log("mach_memory_entry_ownership failed (non-fatal): %s (kr=%d)",
                mach_error_string(kr), kr);
    } else {
        jit_log("memory entry marked no-footprint");
    }

    mach_vm_address_t rw_addr = 0;
    kr = vm_map(task,
                (vm_address_t *)&rw_addr,
                size,
                0,
                VM_FLAGS_ANYWHERE,
                mem_entry,
                0,
                FALSE,
                VM_PROT_READ | VM_PROT_WRITE,
                VM_PROT_READ | VM_PROT_WRITE,
                VM_INHERIT_DEFAULT);
    if (kr != KERN_SUCCESS) {
        jit_log("vm_map (RW) failed: %s (kr=%d)", mach_error_string(kr), kr);
        mach_port_deallocate(task, mem_entry);
        free(region);
        return NULL;
    }
    region->rw_ptr = (void *)rw_addr;

    mach_vm_address_t rx_addr = 0;
    kr = vm_map(task,
                (vm_address_t *)&rx_addr,
                size,
                0,
                VM_FLAGS_ANYWHERE,
                mem_entry,
                0,
                FALSE,
                VM_PROT_READ | VM_PROT_EXECUTE,
                VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
                VM_INHERIT_DEFAULT);
    if (kr != KERN_SUCCESS) {
        jit_log("vm_map (RX) failed: %s (kr=%d)", mach_error_string(kr), kr);
        vm_deallocate(task, (vm_address_t)region->rw_ptr, size);
        mach_port_deallocate(task, mem_entry);
        free(region);
        return NULL;
    }
    region->rx_ptr = (void *)rx_addr;

    jit_log("dual-mapped JIT region: size=%zu RW=%p RX=%p", size, region->rw_ptr, region->rx_ptr);
    return region;
}

/* Madeira ml358: make an already-mapped region jetsam-exempt.
 *
 * The production pool gets its RX pages from StikDebug and vm_remaps the RW
 * alias, so it never passes through jit_region_create(). mach_make_memory_entry_64
 * over the existing range hands back an entry for the same VM object, so the
 * ownership call re-accounts resident pages as well as future ones.
 *
 * Private API: walk a small ladder of entry flags/owners and keep the pair the
 * kernel accepts. Every step is logged; phys_footprint is sampled either side
 * so the log says whether the exemption actually worked. */
bool jit_make_region_no_footprint(void *addr, size_t size, const char *label) {
    mach_port_t task = mach_task_self();
    kern_return_t kr;

    uint64_t fp_before = 0, fp_after = 0;
    task_vm_info_data_t vmi;
    mach_msg_type_number_t vmi_cnt = TASK_VM_INFO_COUNT;
    if (task_info(task, TASK_VM_INFO, (task_info_t)&vmi, &vmi_cnt) == KERN_SUCCESS) {
        fp_before = vmi.phys_footprint;
    }

    struct { unsigned int flags; int own_self; const char *name; } variants[] = {
        { VM_PROT_READ | VM_PROT_WRITE,                    0, "plain/TASK_NULL" },
        { VM_PROT_READ | VM_PROT_WRITE,                    1, "plain/self" },
        { MAP_MEM_VM_SHARE | VM_PROT_READ | VM_PROT_WRITE, 1, "share/self" },
        { MAP_MEM_VM_SHARE | VM_PROT_READ | VM_PROT_WRITE, 0, "share/TASK_NULL" },
    };

    for (unsigned v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        memory_object_size_t entry_size = (memory_object_size_t)size;
        mach_port_t entry = MACH_PORT_NULL;
        kr = mach_make_memory_entry_64(task,
                                       &entry_size,
                                       (memory_object_offset_t)(uintptr_t)addr,
                                       variants[v].flags,
                                       &entry,
                                       MACH_PORT_NULL);
        if (kr != KERN_SUCCESS || entry == MACH_PORT_NULL) {
            jit_log("[no-footprint] %s: [%s] make_memory_entry failed kr=%d (%s)",
                    label, variants[v].name, kr, mach_error_string(kr));
            continue;
        }

        kr = mach_memory_entry_ownership(entry,
                                         variants[v].own_self ? task : TASK_NULL,
                                         VM_LEDGER_TAG_DEFAULT,
                                         VM_LEDGER_FLAG_NO_FOOTPRINT);
        mach_port_deallocate(task, entry);
        if (kr != KERN_SUCCESS) {
            jit_log("[no-footprint] %s: [%s] ownership failed kr=%d (%s)",
                    label, variants[v].name, kr, mach_error_string(kr));
            continue;
        }

        vmi_cnt = TASK_VM_INFO_COUNT;
        if (task_info(task, TASK_VM_INFO, (task_info_t)&vmi, &vmi_cnt) == KERN_SUCCESS) {
            fp_after = vmi.phys_footprint;
        }
        jit_log("[no-footprint] %s: [%s] OK for %zu MB | phys_footprint %llu MB -> %llu MB (delta %lld MB)",
                label, variants[v].name, size >> 20,
                (unsigned long long)(fp_before >> 20),
                (unsigned long long)(fp_after >> 20),
                ((long long)fp_after - (long long)fp_before) >> 20);
        return true;
    }

    jit_log("[no-footprint] %s: all %zu variants refused; region stays jetsam-counted",
            label, sizeof(variants) / sizeof(variants[0]));
    return false;
}

void jit_region_destroy(JITRegion *region) {
    if (!region) return;
    mach_port_t task = mach_task_self();
    if (region->rw_ptr) {
        vm_deallocate(task, (vm_address_t)region->rw_ptr, region->size);
    }
    if (region->rx_ptr) {
        vm_deallocate(task, (vm_address_t)region->rx_ptr, region->size);
    }
    if (region->mem_entry != MACH_PORT_NULL) {
        mach_port_deallocate(task, region->mem_entry);
    }
    free(region);
}

void *jit_region_rw_ptr(JITRegion *region) {
    return region ? region->rw_ptr : NULL;
}

void *jit_region_rx_ptr(JITRegion *region) {
    return region ? region->rx_ptr : NULL;
}

size_t jit_region_size(JITRegion *region) {
    return region ? region->size : 0;
}

void jit_region_invalidate(JITRegion *region, size_t offset, size_t size) {
    if (!region || !region->rx_ptr) return;
    sys_icache_invalidate((char *)region->rx_ptr + offset, size);
}

void *jit_region_write(JITRegion *region, size_t offset, const void *code, size_t code_size) {
    if (!region) return NULL;
    if (offset + code_size > region->size) {
        jit_log("write out of bounds: offset=%zu size=%zu region=%zu",
                offset, code_size, region->size);
        return NULL;
    }
    memcpy((char *)region->rw_ptr + offset, code, code_size);
    sys_icache_invalidate((char *)region->rx_ptr + offset, code_size);
    return (char *)region->rx_ptr + offset;
}

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
        jit_log("debugger attached; installing SIGTRAP fallback anyway");
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

bool jit_cs_status(uint32_t *flags) {
    uint32_t value = 0;
    if (csops(getpid(), CS_OPS_STATUS, &value, sizeof(value)) != 0) return false;
    *flags = value;
    return true;
}

bool jit_task_map_range(uint64_t *min_address, uint64_t *max_address) {
    task_vm_info_data_t vmi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &cnt) != KERN_SUCCESS ||
        !vmi.max_address) {
        return false;
    }
    *min_address = vmi.min_address;
    *max_address = vmi.max_address;
    return true;
}

uint64_t jit_available_memory(void) {
    return (uint64_t)os_proc_available_memory();
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

bool jit_check_debugged(void) {
    uint32_t flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) != 0) {
        jit_log("csops failed; assuming not debugged");
        return false;
    }
    bool debugged = (flags & CS_DEBUGGED) != 0;
    jit_log("CS_DEBUGGED: %s (flags=0x%x)", debugged ? "SET" : "NOT SET", flags);
    return debugged;
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

    uint32_t pattern = 0xDEADBEEF;
    memcpy(region->rw_ptr, &pattern, sizeof(pattern));
    uint32_t readback = 0;
    memcpy(&readback, region->rx_ptr, sizeof(readback));
    if (readback != pattern) {
        jit_log("FAIL: dual-map incoherent (wrote 0x%x, read 0x%x)", pattern, readback);
        jit_region_destroy(region);
        return false;
    }

    vm_address_t addr = (vm_address_t)region->rx_ptr;
    vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    kern_return_t kr = vm_region_64(mach_task_self(), &addr, &region_size,
                                    VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                                    &info_count, &object_name);
    if (kr == KERN_SUCCESS && (info.protection & VM_PROT_EXECUTE)) {
        jit_log("OK: RX page has execute permission");
    } else {
        jit_log("WARN: RX page may lack execute permission (kr=%d prot=0x%x)",
                kr, kr == KERN_SUCCESS ? info.protection : 0);
    }

    jit_region_destroy(region);
    jit_log("=== mapping test passed ===");
    return true;
}
