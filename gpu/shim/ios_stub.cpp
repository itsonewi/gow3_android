// spartanport iOS: the two FFmpeg clients (PNG icons and trophy media) are not
// linked into the iPhone build. The overlay and trophy toast still call their
// interfaces, so this file answers "nothing to decode" instead of pulling in
// libavcodec/format/swscale for pictures that only decorate the menu.
//
// The game's own video output path (videoout/avplayer) does not use FFmpeg; it
// is the guest's VideoOut HLE and the Vulkan presenter.

#include "spartanport_png.h"
#include "spartanport_trophy_media.h"

#include <vector>

namespace SpartanPng {

bool DecodeRgba(const unsigned char* data, size_t size, Image& out) {
    (void)data;
    (void)size;
    out = {};
    return false;
}

bool LoadRgba(const char* path, Image& out) {
    (void)path;
    out = {};
    return false;
}

} // namespace SpartanPng

namespace SpartanToastMedia {

std::vector<float> DecodeSound(const std::filesystem::path& path) {
    (void)path;
    return {};
}

Animation DecodeAnimation(const std::filesystem::path& path, const AnimationLimits& limits) {
    (void)path;
    (void)limits;
    return {};
}

Sheet PackFrames(const Animation& animation) {
    (void)animation;
    return {};
}

} // namespace SpartanToastMedia
