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

/* Weak: the GPU app compiles the real SDL3 pad/audio modules, whose strong
 * definitions then override these. */
#if defined(__APPLE__)
#define SP_WEAK __attribute__((weak))
#else
#define SP_WEAK
#endif

SP_WEAK uintptr_t runtime_audio_resolve(const char *name) { (void)name; return 0; }
SP_WEAK void runtime_audio_report(void) {}

SP_WEAK uintptr_t runtime_pad_resolve(const char *name) { (void)name; return 0; }
SP_WEAK void runtime_pad_report(void) {}
SP_WEAK int runtime_pad_describe(void) { return 0; }

uintptr_t runtime_videodec_resolve(const char *name) { (void)name; return 0; }

uintptr_t runtime_ajm_resolve(const char *name) { (void)name; return 0; }
void runtime_ajm_report(void) {}

/* probe.c --vulkan-only; the session never asks for it. */
int vulkan_smoke(void) { return 1; }

/* The cheat/QTE/actor status hooks normally live in gpu/shim (spartanport_orbs,
 * spartanport_actors, spartanportgpu) and in the SDL pad module. The CPU-only
 * app still references them from probe.c; weak definitions keep the link and
 * let the GPU app's strong symbols win. */
static unsigned char sp_qte_state[4096];

SP_WEAK uintptr_t spartanport_actor_slots_address(void) { return 0; }
SP_WEAK void spartanport_actor_status(int supported) { (void)supported; }
SP_WEAK uintptr_t spartanport_auto_qte_flag(void) { return 0; }
SP_WEAK uintptr_t spartanport_cheat_flag(unsigned id) { (void)id; return 0; }
SP_WEAK void spartanport_cheat_status(unsigned id) { (void)id; }
SP_WEAK uintptr_t spartanport_damage_multiplier(int taken) { (void)taken; return 100; }
SP_WEAK void spartanport_engine_fps_globals(uintptr_t fps, uintptr_t rounded) {
    (void)fps;
    (void)rounded;
}
SP_WEAK uintptr_t spartanport_orb_pickup_multiplier(unsigned kind) { (void)kind; return 100; }
SP_WEAK void spartanport_orbs_status(int supported) { (void)supported; }
SP_WEAK float spartanport_orbs_gain(void *player, float requested) {
    (void)player;
    return requested;
}
SP_WEAK uintptr_t spartanport_qte_state_address(void) { return (uintptr_t)sp_qte_state; }
SP_WEAK void spartanport_qte_status(int supported) { (void)supported; }
SP_WEAK void spartanport_stat_multipliers_status(int damage, int orb_pickup) {
    (void)damage;
    (void)orb_pickup;
}
