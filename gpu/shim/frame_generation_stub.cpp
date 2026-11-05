// spartanport iOS: AMD FSR 3.1 frame generation is not built (it needs the
// FidelityFX SDK bridge, which is a desktop component). The presenter still
// calls FrameGeneration; this stub reports no generated frame, so the real
// frame is shown as-is.
//
// vk_frame_generation.cpp is excluded from the iOS source list and this file is
// added instead.

#include "video_core/renderer_vulkan/vk_frame_generation.h"

namespace Vulkan {

FrameGeneration::FrameGeneration(const Instance& instance, Scheduler& scheduler)
    : instance(instance), scheduler(scheduler) {}

FrameGeneration::~FrameGeneration() = default;

void FrameGeneration::Capture(vk::CommandBuffer cmdbuf, const CaptureInfo& info) {
    (void)cmdbuf;
    (void)info;
}

vk::Image FrameGeneration::Generate(vk::CommandBuffer cmdbuf, bool upscaled, vk::Image final_image,
                                    vk::Format final_format) {
    (void)cmdbuf;
    (void)upscaled;
    (void)final_image;
    (void)final_format;
    return {};
}

vk::ImageView FrameGeneration::GeneratedView(vk::Format format) {
    (void)format;
    return {};
}

} // namespace Vulkan
