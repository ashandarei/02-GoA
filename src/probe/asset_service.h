#pragma once
#include <string>

namespace kh02
{
    void start_early_asset_service();
    void note_asset_unreal_ready();
    void initialize_asset_service();
    void publish_asset_service(const std::string& failure = {}) noexcept;
    void stop_asset_service();
    auto asset_service_ready() -> bool;
    void notify_asset_service_ready();
} // namespace kh02
