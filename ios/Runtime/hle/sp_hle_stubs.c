/*
 * Stubs for the modules the iOS bring-up does not build yet.
 *
 * The prepared boot image resolves imports through runtime_resolve(); the
 * modules that need libavcodec (videodec), SDL audio/pad or LibAtrac9 (ajm,
 * mp3) are left out of the first iOS target, so their resolve/report entry
 * points are present but always return "not found". Everything else (memory,
 * threads, mutexes, semaphores, time, files, savedata, kernel, services) is the
 * stock HLE and is compiled in.
 */

#include <stdint.h>

uintptr_t runtime_audio_resolve(const char *name) { (void)name; return 0; }
void runtime_audio_report(void) {}

uintptr_t runtime_pad_resolve(const char *name) { (void)name; return 0; }
void runtime_pad_report(void) {}
int runtime_pad_describe(void) { return 0; }

uintptr_t runtime_videodec_resolve(const char *name) { (void)name; return 0; }

uintptr_t runtime_ajm_resolve(const char *name) { (void)name; return 0; }
void runtime_ajm_report(void) {}

/* probe.c --vulkan-only; the session never asks for it. */
int vulkan_smoke(void) { return 1; }
