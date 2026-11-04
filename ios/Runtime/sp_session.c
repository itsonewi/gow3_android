/*
 * The iOS session: one SpartanPort runtime run.
 *
 * The stock loader (src/probe.c) does the real work: it reads the prepared
 * boot.bin, maps the guest image below 1 TiB, resolves the HLE imports from
 * src/runtime_*.c, starts FEXCore and enters the game's x86-64 entry point.
 * This file only:
 *
 *   - builds the loader's argv from the Swift configuration;
 *   - hands the JIT pool to FEX (sp_jit_pool_set);
 *   - captures the runtime's stdout/stderr into the app's log;
 *   - runs the loader on a worker thread with a large stack.
 *
 * A fatal runtime error calls exit()/abort() on this thread; on iOS that ends
 * the app, which is the behaviour of the stock loader on every platform. The
 * launcher reports "runtime stopped" and the next launch starts cleanly.
 */

#include "sp_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "spartanport_ios.h"

extern int spartanport_loader_main(int argc, char **argv);
extern void sp_set_log_capture(int on);

/* sp_jit_mmap.c */
extern void sp_jit_pool_set(void *rx, void *rw, size_t size);
extern void sp_jit_pool_stats(uint64_t *used, uint64_t *size, uint64_t *allocations);

static pthread_t session_thread;
static pthread_t log_thread;
static atomic_int session_running;
static atomic_int log_running;
static int log_pipe[2] = {-1, -1};
static sp_log_fn session_log;
static void *session_log_user;
static char last_error[512];
static char *owned_argv[32];
static int owned_argc;
static char build_info[160];

const char *sp_build_info(void) {
    if (!build_info[0]) {
        snprintf(build_info, sizeof(build_info),
                 "SpartanPort iOS " __DATE__ " " __TIME__
                 " | pool-backed FEX | 16K pages | CPU-only bring-up");
    }
    return build_info;
}

const char *sp_session_last_error(void) { return last_error; }
int sp_session_running(void) { return atomic_load(&session_running); }

static void *log_reader(void *argument) {
    (void)argument;
    char buffer[4096];
    size_t used = 0;
    for (;;) {
        ssize_t got = read(log_pipe[0], buffer + used, sizeof(buffer) - used - 1);
        if (got <= 0) break;
        used += (size_t)got;
        buffer[used] = 0;
        char *line = buffer;
        char *newline;
        while ((newline = strchr(line, '\n'))) {
            *newline = 0;
            if (*line && session_log) session_log(SP_LOG_INFO, line, session_log_user);
            line = newline + 1;
        }
        size_t left = strlen(line);
        memmove(buffer, line, left + 1);
        used = left;
        if (used == sizeof(buffer) - 1) {
            if (session_log) session_log(SP_LOG_INFO, buffer, session_log_user);
            used = 0;
        }
    }
    atomic_store(&log_running, 0);
    return NULL;
}

static void *session_worker(void *argument) {
    (void)argument;
    int argc = owned_argc;
    char **argv = owned_argv;
    sp_log(SP_LOG_INFO, "runtime starting: %d arguments", argc);
    int result = spartanport_loader_main(argc, argv);
    sp_log(result == 0 ? SP_LOG_WARNING : SP_LOG_ERROR, "runtime returned %d", result);
    atomic_store(&session_running, 0);
    return NULL;
}

static void free_owned(void) {
    for (int i = 0; i < owned_argc; ++i) free(owned_argv[i]);
    owned_argc = 0;
}

static int append_arg(const char *value) {
    if (owned_argc >= (int)(sizeof(owned_argv) / sizeof(owned_argv[0])) - 1) return -1;
    owned_argv[owned_argc] = strdup(value);
    if (!owned_argv[owned_argc]) return -1;
    ++owned_argc;
    return 0;
}

int sp_session_start(const sp_session_config *config, sp_log_fn log, void *user) {
    if (!config || !config->game_dir) {
        snprintf(last_error, sizeof(last_error), "no game directory");
        return -EINVAL;
    }
    if (atomic_load(&session_running)) {
        snprintf(last_error, sizeof(last_error), "a session is already running");
        return -EBUSY;
    }

    session_log = log;
    session_log_user = user;
    sp_set_log(log, user);

    char boot[1024];
    snprintf(boot, sizeof(boot), "%s/boot.bin", config->game_dir);
    if (access(boot, R_OK) != 0) {
        snprintf(last_error, sizeof(last_error),
                 "boot.bin is missing in %s. Prepare the game folder on a computer and import "
                 "the prepared folder.", config->game_dir);
        sp_log(SP_LOG_ERROR, "%s", last_error);
        return -ENOENT;
    }

    free_owned();
    if (append_arg(boot)) {
        free_owned();
        snprintf(last_error, sizeof(last_error), "argument list too long");
        return -E2BIG;
    }
    if (config->cpu_only && append_arg("--cpu-only")) {
        free_owned();
        return -E2BIG;
    }
    if (append_arg("--timeout") || append_arg("0")) {
        free_owned();
        snprintf(last_error, sizeof(last_error), "argument list too long");
        return -E2BIG;
    }
    if (config->app0_dir && config->app0_dir[0]) {
        if (append_arg("--app0") || append_arg(config->app0_dir)) {
            free_owned();
            return -E2BIG;
        }
    }
    if (config->user_dir && config->user_dir[0]) {
        if (append_arg("--user") || append_arg(config->user_dir)) {
            free_owned();
            return -E2BIG;
        }
        setenv("SPARTAN_USER_DIR", config->user_dir, 1);
    }
    for (int i = 0; i < config->extra_argc && config->extra_argv; ++i) {
        if (append_arg(config->extra_argv[i])) {
            free_owned();
            return -E2BIG;
        }
    }
    owned_argv[owned_argc] = NULL;

    sp_jit_pool_set(config->jit_rx, config->jit_rw, config->jit_size);
    sp_check_guest_window();
    sp_log_address_space();

    /* Capture the loader's output. sp_log itself no longer writes to stderr
     * while capturing (platform.c), so there is no feedback loop. */
    if (pipe(log_pipe) != 0) {
        snprintf(last_error, sizeof(last_error), "cannot create the log pipe: %s", strerror(errno));
        return -errno;
    }
    sp_set_log_capture(1);
    fflush(NULL);
    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    close(log_pipe[1]);
    log_pipe[1] = -1;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    atomic_store(&log_running, 1);
    pthread_create(&log_thread, NULL, log_reader, NULL);
    pthread_detach(log_thread);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    atomic_store(&session_running, 1);
    int created = pthread_create(&session_thread, &attr, session_worker, NULL);
    pthread_attr_destroy(&attr);
    if (created != 0) {
        atomic_store(&session_running, 0);
        snprintf(last_error, sizeof(last_error), "cannot start the runtime thread: %s", strerror(created));
        sp_log(SP_LOG_ERROR, "%s", last_error);
        return -created;
    }
    pthread_detach(session_thread);
    return 0;
}

int sp_session_stop(void) {
    /* The guest and FEX have no safe unwind point (signals, JIT code and guest
     * threads are live). A "stop" is therefore a request the UI reports; the
     * process ends with the runtime when the guest exits. */
    if (!atomic_load(&session_running)) return 0;
    sp_log(SP_LOG_WARNING, "stop requested; the guest has no unwind point, close the app to end it");
    return -ENOTSUP;
}

/* Called by the diagnostics page. */
void sp_session_pool_stats(uint64_t *used, uint64_t *size, uint64_t *allocations) {
    sp_jit_pool_stats(used, size, allocations);
}
