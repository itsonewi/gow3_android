// spartanport iOS: FSR is not built into the iPhone port.
//
// The desktop build vendors AMD's FidelityFX SDK (third_party/fsr-vulkan) and
// loads the FSR 4 provider from the graphics driver at run time. Neither exists
// on iOS: MoltenVK exposes no AMD driver, and the FFX SDK's provider loader is
// Windows-only. The renderer still needs the DriverFsrUpscaler type, so this
// stub reports "unavailable" and the settings layer turns every upscaler option
// off (SpartanSettings::ConfigureUpscalerSupport(false, false, "...")).
//
// The game's own TAA and the port's non-FSR upscalers still work; only the FSR
// menu entries are disabled. This file is compiled only when SPARTAN_IOS is on.

#include "video_core/renderer_vulkan/fsr_driver.h"

#include <string>

namespace Vulkan {

struct DriverFsrUpscaler::Impl {};

DriverFsrUpscaler::DriverFsrUpscaler(vk::Device device, vk::PhysicalDevice physical,
                                     PFN_vkGetDeviceProcAddr get_proc)
    : impl(std::make_unique<Impl>()) {
    (void)device;
    (void)physical;
    (void)get_proc;
}

DriverFsrUpscaler::~DriverFsrUpscaler() = default;

bool DriverFsrUpscaler::Available() const {
    return false;
}

bool DriverFsrUpscaler::Record(const Fsr4Upscaler::Frame& frame) {
    (void)frame;
    return false;
}

const std::string& DriverFsrUpscaler::Problem() const {
    static const std::string problem =
        "FSR is not available on iOS: the AMD FidelityFX provider is a desktop driver component.";
    return problem;
}

const std::string& DriverFsrUpscaler::Description() const {
    static const std::string description = "FSR 4";
    return description;
}

} // namespace Vulkan
