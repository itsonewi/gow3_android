// spartanport iOS: AvPlayer's source and file streamer are FFmpeg clients
// (libavformat/libavcodec/libswresample/libswscale). The iPhone build does not
// ship libav*, so those two files are excluded from the iOS source list and
// this stand-in reports "no source" for every request.
//
// The HLE entry points (sceAvPlayer*) still exist: the guest gets the usual
// error result instead of a missing import, and the game can continue to the
// scene renderer. Real movie playback needs an iOS FFmpeg build.
//
// AvPlayerFileStreamer is only instantiated by avplayer_source.cpp, so it does
// not need a stub of its own.

#include "core/libraries/avplayer/avplayer_source.h"

namespace Libraries::AvPlayer {

AvPlayerSource::AvPlayerSource(AvPlayerStateCallback& state) {
    (void)state;
}

AvPlayerSource::~AvPlayerSource() = default;

bool AvPlayerSource::Init(const AvPlayerInitData& init_data, std::string_view path) {
    (void)init_data;
    (void)path;
    return false;
}

bool AvPlayerSource::FindStreams() {
    return false;
}

s32 AvPlayerSource::GetStreamCount() {
    return 0;
}

bool AvPlayerSource::GetStreamInfo(u32 stream_index, AvPlayerStreamInfo& info) {
    (void)stream_index;
    (void)info;
    return false;
}

bool AvPlayerSource::EnableStream(u32 stream_index) {
    (void)stream_index;
    return false;
}

void AvPlayerSource::SetLooping(bool is_looping) {
    (void)is_looping;
}

std::optional<bool> AvPlayerSource::HasFrames(u32 num_frames) {
    (void)num_frames;
    return std::nullopt;
}

bool AvPlayerSource::Start() {
    return false;
}

bool AvPlayerSource::Stop() {
    return true;
}

void AvPlayerSource::Pause() {}

void AvPlayerSource::Resume() {}

bool AvPlayerSource::GetAudioData(AvPlayerFrameInfo& audio_info) {
    (void)audio_info;
    return false;
}

bool AvPlayerSource::GetVideoData(AvPlayerFrameInfo& video_info) {
    (void)video_info;
    return false;
}

bool AvPlayerSource::GetVideoData(AvPlayerFrameInfoEx& video_info) {
    (void)video_info;
    return false;
}

u64 AvPlayerSource::CurrentTime() {
    return 0;
}

bool AvPlayerSource::IsActive() {
    return false;
}

} // namespace Libraries::AvPlayer
