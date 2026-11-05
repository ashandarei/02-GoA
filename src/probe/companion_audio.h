#pragma once
#include <string>

namespace kh02
{
    void start_companion_audio(bool companion);
    void stop_companion_audio();
    auto companion_audio_status() -> std::string;
} // namespace kh02
