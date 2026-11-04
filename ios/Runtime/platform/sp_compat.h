/*
 * iOS compatibility layer for the SpartanPort runtime.
 *
 * The runtime was written for Linux (nport/Android) and Windows. iOS is Darwin:
 * most POSIX calls exist, but a handful of Linux-only ones do not:
 *
 *   memfd_create()        -> an unlinked sparse file in the app's temp dir
 *   fallocate(PUNCH_HOLE) -> fcntl(F_PUNCHHOLE) where the SDK has it
 *   gettid()/SYS_gettid   -> pthread_threadid_np()
 *   sched_getaffinity()   -> not available; report the active CPU count
 *   /proc/self/task       -> not available; diagnostics degrade to the caller
 *   mallopt()             -> glibc only; no-op on iOS
 *
 * This header is force-included (-include) for the iOS target so the stock
 * sources stay untouched where a macro is enough. The few places that need
 * control flow (thread-name signature, /proc walks) carry an #ifdef __APPLE__.
 */
#ifndef SPARTANPORT_IOS_COMPAT_H
#define SPARTANPORT_IOS_COMPAT_H

#if defined(__APPLE__)
/* probe.c includes <ucontext.h> for the signal frame; the iOS SDK hides the
 * deprecated ucontext API behind _XOPEN_SOURCE. This header is the first
 * include of every stock C file, so the feature macro is early enough. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif

#if defined(__APPLE__)

#include <Availability.h>
#include <TargetConditionals.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- guest memory backing ------------------------------------------------ */

/* memfd_create(2) stand-in: an unlinked, sparse file in NSTemporaryDirectory().
 * The fd is used exactly like a memfd: ftruncate to the pool size, mmap
 * MAP_SHARED views at guest addresses, punch holes to release pages. */
int sp_memfd_create(const char *name, unsigned int flags);
void *sp_memfd_handle(void);

/* fallocate(2) stand-in. Only PUNCH_HOLE|KEEP_SIZE is used by runtime_memory.c;
 * other modes are a no-op returning 0. */
int sp_fallocate(int fd, int mode, off_t offset, off_t length);

/* --- threads ------------------------------------------------------------- */

/* gettid(2) stand-in. */
pid_t sp_gettid(void);

/* pthread_setname_np() differs by platform: Darwin names the calling thread. */
int sp_set_thread_name(const char *name);

/* The guest priority mapping (setpriority(PRIO_PROCESS, tid, ...) on Linux).
 * Darwin only has PRIO_DARWIN_THREAD relative to the calling thread. */
int sp_set_thread_priority(int lower);

/* Active CPU count, the replacement for sched_getaffinity(CPU_COUNT). */
int sp_active_cpu_count(void);

/* --- signals / diagnostics ------------------------------------------------ */

/* process_vm_readv() was used to read guest memory without faulting. iOS is a
 * single address space and has no such call; read through mach_vm_read_overwrite. */
int sp_process_read(uintptr_t address, void *out, size_t size);

/* True when the runtime may map executable memory right now. */
int sp_jit_is_enabled(void);

#ifdef __cplusplus
}
#endif

/* The stock sources call these names directly. */
#define memfd_create sp_memfd_create
#define fallocate sp_fallocate
#define gettid sp_gettid

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#endif

/* Darwin has no MAP_FIXED_NOREPLACE; runtime_memory.c's iOS branch maps with a
 * hint and verifies the result instead. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

/* glibc allocator tuning has no Darwin equivalent. */
#define mallopt(...) ((void)0)
#ifndef M_ARENA_MAX
#define M_ARENA_MAX 0
#endif
#ifndef M_MMAP_THRESHOLD
#define M_MMAP_THRESHOLD 0
#endif

/* Linux's CPU affinity macros are not in the iOS SDK. */
#ifndef CPU_SETSIZE
typedef struct { unsigned long bits[16]; } sp_cpu_set_t;
#define CPU_ZERO(set) ((void)(set))
#define CPU_COUNT(set) sp_active_cpu_count()
#endif

#endif /* __APPLE__ */

#endif /* SPARTANPORT_IOS_COMPAT_H */
