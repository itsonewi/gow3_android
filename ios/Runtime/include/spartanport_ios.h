/*
 * SpartanPort iOS runtime C interface.
 *
 * The Swift app owns the JIT session (StikDebug / spartanport-jit.js) and the
 * game folder; this library owns the guest: PS4 address space, the FEX CPU,
 * the prepared boot image loader and the HLE tables from src/runtime_*.c.
 *
 * A session runs the stock SpartanPort loader (src/probe.c) on a worker thread
 * with argv = [boot.bin, --cpu-only, --app0 ..., --user ...]. The JIT pool is
 * handed over before the thread starts, so FEX's executable allocations come
 * from the debugger-allocated RWX pages instead of mmap(PROT_EXEC), which iOS
 * refuses outside a debug session.
 */
#ifndef SPARTANPORT_IOS_H
#define SPARTANPORT_IOS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SP_LOG_DEBUG = 0,
    SP_LOG_INFO = 1,
    SP_LOG_WARNING = 2,
    SP_LOG_ERROR = 3,
};

/* level is one of SP_LOG_*; the string is NUL-terminated and valid for the
 * duration of the call. Called from the runtime's threads. */
typedef void (*sp_log_fn)(int level, const char *message, void *user);

void sp_set_log(sp_log_fn log, void *user);
void sp_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* 1 while the loader's stdout/stderr are redirected into sp_log. */
void sp_set_log_capture(int on);

typedef struct {
    /* Directory holding boot.bin, the patches and the pipeline cache. On iOS
     * the launcher prepares this folder on a computer (scripts/prepare.py) and
     * imports it, because the port's preparation is Python. */
    const char *game_dir;
    /* Writable per-title directory (saves, logs, cache). */
    const char *user_dir;
    /* The game dump with eboot.bin, sce_sys, sce_module; used for param.sfo,
     * the pause-menu WAD and trophy data. May be the same as game_dir. */
    const char *app0_dir;
    /* 1 = load and run the guest without creating a Vulkan device (the iOS
     * bring-up path; the Android runtime's --cpu-only). */
    int cpu_only;
    /* Extra loader arguments (may be NULL). */
    int extra_argc;
    const char *const *extra_argv;

    /* The dual-mapped JIT pool from sp_pool_prepare(), or NULL. rx must be the
     * executable view FEX writes and executes from. */
    void *jit_rx;
    void *jit_rw;
    size_t jit_size;
} sp_session_config;

/* Starts the runtime on a worker thread. Returns 0 when the thread started,
 * a negative errno-style code otherwise. Only one session at a time. */
int sp_session_start(const sp_session_config *config, sp_log_fn log, void *user);
/* Requests a stop (the guest cannot be unwound safely; see README). */
int sp_session_stop(void);
int sp_session_running(void);
const char *sp_session_last_error(void);

/* Build identification, shown in the diagnostics page. */
const char *sp_build_info(void);

/* JIT pool accounting for the diagnostics page. */
void sp_jit_pool_set(void *rx, void *rw, size_t size);
void sp_session_pool_stats(uint64_t *used, uint64_t *size, uint64_t *allocations);

/* --- address space diagnostics (used by the launcher before a session) --- */
/* 0 when the app image and the process address map can hold the PS4 user
 * window [0x1000000000, 0xfc00000000); a negative reason otherwise. */
int sp_check_guest_window(void);
void sp_log_address_space(void);
void sp_app_image_range(uintptr_t *base, uintptr_t *end);
int sp_region_state(uintptr_t address, uint64_t size);
int sp_jit_is_enabled(void);

/* Read this process's memory without faulting on a bad address (the Darwin
 * replacement for process_vm_readv, used by the fault/watchdog paths). */
int sp_process_read(uintptr_t address, void *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SPARTANPORT_IOS_H */
