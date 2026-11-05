#pragma once
#include "shared/kh02/kh02_control_protocol.h"
#include <filesystem>

namespace kh02
{
    // Called only by the verified parked startup thread after the control event.
    // Decoded audio/video stays in RAM and the source renderer remains silent.
    void execute_movie_request(const std::filesystem::path& bridge_directory, const kh02control::Request& request);
} // namespace kh02
