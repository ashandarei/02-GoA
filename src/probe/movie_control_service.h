#pragma once
#include "shared/kh02/kh02_control_protocol.h"
#define NOMINMAX
#include <Windows.h>
#include <filesystem>
#include <optional>

namespace kh02
{
    void initialize_movie_control();
    void stop_movie_control();
    void mark_movie_control_ready();
    void signal_asset_readiness();
    auto movie_control_event() -> HANDLE;
    auto movie_control_owner() -> HANDLE;
    auto movie_control_identity() -> const kh02control::Identity&;
    auto take_movie_control_request() -> std::optional<kh02control::Request>;
    void complete_movie_control_request();
} // namespace kh02
