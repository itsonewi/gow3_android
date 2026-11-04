/*
 * iOS platform layer for the SpartanPort runtime.
 *
 * Everything here is the Darwin replacement for a Linux facility the stock
 * runtime uses (see platform/sp_compat.h), plus the guest address-space
 * diagnostics the JIT/memory layer and the launcher report.
 */

#include "sp_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include "spartanport_ios.h"

/* The PS4's direct memory is a *physical* window: runtime_memory.c backs it with
 * a sparse file and maps views of it in the guest's user range, which starts at
 * USER_MIN = 0x1000000000 (64 GiB). That is above the 63 GiB map iOS gives an
 * app without the extended-virtual-addressing entitlement, so every guest
 * mapping fails without it. This check reports both halves of that contract. */
#define SP_PS4_USER_MIN UINT64_C(0x1000000000)
#define SP_PS4_USER_MAX UINT64_C(0xfc00000000)

static sp_log_fn g_log;
static void *g_log_user;
static int g_capture;

void sp_set_log(sp_log_fn log, void *user) {
    g_log = log;
    g_log_user = user;
}

/* While the loader's stdio is redirected into the app log, sp_log must not
 * write to stderr: that is the same pipe and the reader would loop. */
void sp_set_log_capture(int on) { g_capture = on; }

void sp_log(int level, const char *fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (g_log) g_log(level, buffer, g_log_user);
    os_log(OS_LOG_DEFAULT, "[spartanport] %{public}s", buffer);
    if (!g_capture) fprintf(stderr, "[sp] %s\n", buffer);
}

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
/* <mach/mach_vm.h> is unsupported on the iOS SDK; libsystem_kernel exports these. */
extern kern_return_t mach_vm_read_overwrite(vm_map_t, mach_vm_address_t, mach_vm_size_t,
                                            mach_vm_address_t, mach_vm_size_t *);
extern kern_return_t mach_vm_region(vm_map_t, mach_vm_address_t *, mach_vm_size_t *,
                                    vm_region_flavor_t, vm_region_info_t,
                                    mach_msg_type_number_t *, mach_port_t *);

/* --- guest memory backing ------------------------------------------------ */

int sp_memfd_create(const char *name, unsigned int flags) {
    (void)flags;
    char directory[PATH_MAX] = {0};
    size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof(directory));
    if (length == 0) snprintf(directory, sizeof(directory), "%s", "/tmp");

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.XXXXXX", directory, name ? name : "spartanport");
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    /* The fd is what matters; the name must not survive the process. */
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int sp_fallocate(int fd, int mode, off_t offset, off_t length) {
#ifdef F_PUNCHHOLE
    if (mode & FALLOC_FL_PUNCH_HOLE) {
        struct fpunchhole hole;
        memset(&hole, 0, sizeof(hole));
        hole.fp_offset = (off_t)offset;
        hole.fp_length = (off_t)length;
        if (fcntl(fd, F_PUNCHHOLE, &hole) == 0) return 0;
        /* APFS/HFS may refuse on some volumes; the pages are still dropped by
         * the guest's munmap, only the de-duplication hint is lost. */
        return 0;
    }
#else
    (void)fd; (void)mode; (void)offset; (void)length;
#endif
    return 0;
}

/* --- threads ------------------------------------------------------------- */

pid_t sp_gettid(void) {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (pid_t)tid;
}

int sp_set_thread_name(const char *name) {
    if (!name) return EINVAL;
    char host[64];
    snprintf(host, sizeof(host), "%s", name);
    return pthread_setname_np(host);
}

int sp_set_thread_priority(int lower) {
    /* Darwin has no per-thread nice value for other threads; PRIO_DARWIN_THREAD
     * applies to the caller (even 0) and PRIO_DARWIN_BG lowers it. */
    return setpriority(PRIO_DARWIN_THREAD, 0, lower ? PRIO_DARWIN_BG : 0);
}

int sp_active_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
}

/* --- diagnostics --------------------------------------------------------- */

int sp_process_read(uintptr_t address, void *out, size_t size) {
    if (!address || !out || !size) return -1;
    mach_vm_size_t got = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)address,
                                              (mach_vm_size_t)size, (mach_vm_address_t)(uintptr_t)out,
                                              &got);
    return kr == KERN_SUCCESS && got == size ? 0 : -1;
}

int sp_jit_is_enabled(void) {
    uint32_t flags = 0;
    if (csops(getpid(), 0 /* CS_OPS_STATUS */, &flags, sizeof(flags)) != 0) return 0;
    return (flags & 0x10000000u /* CS_DEBUGGED */) != 0;
}

/* --- guest address space ------------------------------------------------ */

void sp_app_image_range(uintptr_t *base, uintptr_t *end) {
    extern void jit_app_image_range(uintptr_t *, uintptr_t *);
    jit_app_image_range(base, end);
}

/* Is [address, address+size) inside a mapped region? Read-only query used by
 * the diagnostics page; 1 mapped, 0 free, -1 unknown. */
int sp_region_state(uintptr_t address, uint64_t size) {
    mach_vm_address_t at = (mach_vm_address_t)address;
    mach_vm_size_t span = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &at, &span, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (kr != KERN_SUCCESS) return 0;
    return (address >= at && address + size <= at + span) ? 1 : -1;
}

/* Reports whether the app image and the process address map can hold the PS4
 * user window. Called once before the guest memory pool is created. */
int sp_check_guest_window(void) {
    uintptr_t image_base = 0, image_end = 0;
    sp_app_image_range(&image_base, &image_end);
    sp_log(SP_LOG_INFO, "app image [0x%llx, 0x%llx)", (unsigned long long)image_base,
           (unsigned long long)image_end);
    sp_log(SP_LOG_INFO, "guest user window [0x%llx, 0x%llx)",
           (unsigned long long)SP_PS4_USER_MIN, (unsigned long long)SP_PS4_USER_MAX);

    if (image_end > SP_PS4_USER_MIN && image_base < SP_PS4_USER_MAX) {
        sp_log(SP_LOG_ERROR,
               "the app image overlaps the PS4 user window; the guest cannot map its image, direct "
               "memory or handles. Relink the app below 0x1000000000.");
        return -1;
    }

    uint64_t low = 0, high = 0;
    extern bool jit_task_map_range(uint64_t *, uint64_t *);
    if (jit_task_map_range(&low, &high)) {
        if (high <= SP_PS4_USER_MIN) {
            sp_log(SP_LOG_ERROR,
                   "the address map ends at 0x%llx, below the PS4 user window at 0x%llx: the app "
                   "was signed without com.apple.developer.kernel.extended-virtual-addressing, so "
                   "every guest mapping will fail. Reinstall it with a profile that grants the "
                   "entitlement.", (unsigned long long)high, (unsigned long long)SP_PS4_USER_MIN);
            return -2;
        }
    }
    return 0;
}

void sp_log_address_space(void) {
    uint64_t low = 0, high = 0;
    extern bool jit_task_map_range(uint64_t *, uint64_t *);
    extern uint64_t jit_available_memory(void);
    if (jit_task_map_range(&low, &high)) {
        sp_log(SP_LOG_INFO, "address map [0x%llx, 0x%llx) = %llu GB; available %llu MB",
               (unsigned long long)low, (unsigned long long)high,
               (unsigned long long)((high - low) >> 30),
               (unsigned long long)(jit_available_memory() >> 20));
    } else {
        sp_log(SP_LOG_WARNING, "TASK_VM_INFO did not report the address map");
    }
    sp_log(SP_LOG_INFO, "JIT (CS_DEBUGGED) %s", sp_jit_is_enabled() ? "enabled" : "not enabled");
}
