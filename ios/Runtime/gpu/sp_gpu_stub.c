/*
 * GPU library stub for the CPU-only iOS bring-up.
 *
 * The Vulkan path (shadPS4 core + MoltenVK) is not part of this target yet;
 * --cpu-only keeps the stock loader from calling init(). These definitions let
 * the runtime and the loader link, and the launcher shows "GPU: not built" in
 * the diagnostics page instead of pretending a frame was presented.
 */

#include "gpu/spartanportgpu.h"

#include <stdio.h>

/* Weak definitions: when the real shadPS4 GPU library is linked, its strong
 * symbols win; the CPU-only app keeps these. */
#if defined(__APPLE__)
#define SP_WEAK __attribute__((weak))
#else
#define SP_WEAK
#endif

SP_WEAK void spartanportgpu_register_kernel(void) {}

SP_WEAK int spartanportgpu_init(const SpartanGpuConfig *config) {
    (void)config;
    fprintf(stderr, "[iOS] GPU library is not part of the CPU-only bring-up; "
                    "the guest will stop at its first graphics call\n");
    return -1;
}

SP_WEAK uintptr_t spartanportgpu_resolve(const char *scoped_nid) {
    (void)scoped_nid;
    return 0;
}

SP_WEAK int spartanportgpu_handle_fault(void *ucontext, void *address) {
    (void)ucontext;
    (void)address;
    return 0;
}

SP_WEAK void spartanportgpu_dump_guest_writes(void *ucontext) { (void)ucontext; }

SP_WEAK int spartanportgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8) {
    (void)initial_utf8;
    (void)prompt_utf8;
    return 0;
}

SP_WEAK int spartanportgpu_text_input_poll(char *out_utf8, uint64_t size) {
    (void)out_utf8;
    (void)size;
    return 2;
}

SP_WEAK int spartanportgpu_choice_begin(const char *title, const char *const *items, const char *const *images,
                                int count, int focus) {
    (void)title;
    (void)items;
    (void)images;
    (void)count;
    (void)focus;
    return 0;
}

SP_WEAK int spartanportgpu_choice_poll(void) { return -2; }

SP_WEAK int spartanportgpu_choice_confirm(const char *title, const char *const *items, int count, int focus,
                                  const unsigned char *png, uint64_t size, const char *caption,
                                  const char *unavailable) {
    (void)title;
    (void)items;
    (void)count;
    (void)focus;
    (void)png;
    (void)size;
    (void)caption;
    (void)unavailable;
    return 0;
}

SP_WEAK int spartanportgpu_overlay_captures_input(void) { return 0; }
SP_WEAK void spartanportgpu_wait_while_paused(void) {}
SP_WEAK int spartanportgpu_vibration_percent(void) { return 100; }
SP_WEAK int spartanportgpu_menu_confirm_swap(void) { return 0; }
SP_WEAK int spartanportgpu_enemy_health_bar(int toggle) { (void)toggle; return 2; }
SP_WEAK uint64_t spartanportgpu_native_setting(unsigned function) { (void)function; return 0; }
SP_WEAK int spartanportgpu_audio_volume_percent(void) { return 100; }
SP_WEAK int spartanportgpu_audio_channels(void) { return 0; }
SP_WEAK void spartanportgpu_engine_fps_cycle(void) {}
SP_WEAK void spartanportgpu_engine_fps_cap(int fps) { (void)fps; }
SP_WEAK void spartanportgpu_engine_fps_cap_qte(unsigned kinds, int type) { (void)kinds; (void)type; }
SP_WEAK void spartanportgpu_engine_fps_cap_held(unsigned kinds) { (void)kinds; }
SP_WEAK void spartanportgpu_movie_picture(void) {}
SP_WEAK int spartanportgpu_mouse_prompt_distance(void) { return 120; }
SP_WEAK void spartanportgpu_mouse_pause_key(void) {}
SP_WEAK int spartanportgpu_mouse_aim_style(void) { return 0; }
SP_WEAK int spartanportgpu_mouse_aim_sensitivity(void) { return 100; }
SP_WEAK unsigned spartanportgpu_symbol_count(void) { return 0; }
SP_WEAK unsigned long long spartanportgpu_copy_yields(void) { return 0; }

SP_WEAK void spartanportgpu_toast_trophy(int id, const char *title, const char *detail,
                                 const unsigned char *png, uint64_t png_size) {
    (void)id;
    (void)title;
    (void)detail;
    (void)png;
    (void)png_size;
}

SP_WEAK void spartanportgpu_toast_icon(const char *text, const unsigned char *png, uint64_t png_size) {
    (void)text;
    (void)png;
    (void)png_size;
}

SP_WEAK void spartanportgpu_toast(const char *text) { (void)text; }

SP_WEAK void spartanportgpu_trophy_setup(const SpartanGpuTrophyLabels *labels,
                                 const SpartanGpuTrophyInfo *items, int count,
                                 const volatile uint32_t *unlocked) {
    (void)labels;
    (void)items;
    (void)count;
    (void)unlocked;
}
