// spartanport iOS: Darwin shims for the shadPS4 sources.
//
// Force-included (-include) into every GPU translation unit on iOS. The
// renderer was written for Linux, where gettid(), RUSAGE_THREAD and
// pthread_getcpuclockid exist; iOS has equivalents but not the same names.

#pragma once

#if defined(SPARTAN_IOS)

#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static inline pid_t gettid(void) {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (pid_t)tid;
}

static inline int spartanport_pthread_getcpuclockid(pthread_t thread, clockid_t* clock_id) {
    (void)thread;
    // Darwin has no per-thread CPU clock; the process clock is the closest.
    *clock_id = CLOCK_PROCESS_CPUTIME_ID;
    return 0;
}

// Darwin's getrusage has no RUSAGE_THREAD.
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD RUSAGE_SELF
#endif

// The renderer calls pthread_getcpuclockid directly.
#define pthread_getcpuclockid spartanport_pthread_getcpuclockid

#endif // SPARTAN_IOS
