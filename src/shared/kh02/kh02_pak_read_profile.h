#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace kh02asset
{
    // Metadata only. Bounded sampling must never retain decrypted asset bytes.
    class PakReadProfile
    {
      public:
        struct Read
        {
            uint32_t file{}, length{};
            uint64_t offset{}, microseconds{};
            uint64_t started_ms{}, finished_ms{};
        };

        struct Snapshot
        {
            std::vector<Read> reads;
            uint64_t dropped{};
        };

        explicit PakReadProfile(std::size_t capacity = 32'768) : capacity_(capacity)
        {
            reads_.reserve(capacity);
        }

        void record(uint32_t file, uint64_t offset, uint32_t length, uint64_t microseconds, uint64_t started_ms = 0,
                    uint64_t finished_ms = 0)
        {
            std::lock_guard lock(mutex_);
            if (reads_.size() == capacity_)
            {
                ++dropped_;
                return;
            }
            reads_.push_back({file, length, offset, microseconds, started_ms, finished_ms});
        }

        auto snapshot() const -> Snapshot
        {
            std::lock_guard lock(mutex_);
            return {reads_, dropped_};
        }

      private:
        const std::size_t capacity_;
        mutable std::mutex mutex_;
        std::vector<Read> reads_;
        uint64_t dropped_{};
    };
} // namespace kh02asset
