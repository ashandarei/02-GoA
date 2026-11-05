#pragma once
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

namespace kh02asset
{
    // One request in flight. The caller and worker receive disjoint spans and
    // must open independent native handles. Both finish before returning.
    class ParallelPakRead
    {
      public:
        using Reader = std::function<bool(uint32_t, uint64_t, std::span<unsigned char>)>;

        explicit ParallelPakRead(Reader reader) : reader_(std::move(reader)), worker_([this] { consume(); }) {}

        ParallelPakRead(const ParallelPakRead&) = delete;
        auto operator=(const ParallelPakRead&) -> ParallelPakRead& = delete;

        ~ParallelPakRead()
        {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            available_.notify_one();
            worker_.join();
        }

        auto read(uint32_t file, uint64_t offset, std::span<unsigned char> output) -> bool
        {
            std::lock_guard request_lock(request_mutex_);
            constexpr std::size_t alignment = 65'536;
            const auto first_length = (output.size() / 2 / alignment) * alignment;
            if (!first_length || first_length == output.size())
            {
                return invoke(file, offset, output);
            }
            {
                std::lock_guard lock(mutex_);
                file_ = file;
                offset_ = offset + first_length;
                output_ = output.subspan(first_length);
                pending_ = true;
                complete_ = false;
            }
            available_.notify_one();
            const bool first_ok = invoke(file, offset, output.first(first_length));
            std::unique_lock lock(mutex_);
            completed_.wait(lock, [this] { return complete_; });
            return first_ok && successful_;
        }

      private:
        auto invoke(uint32_t file, uint64_t offset, std::span<unsigned char> output) noexcept -> bool
        {
            try
            {
                return reader_(file, offset, output);
            }
            catch (...)
            {
                return false;
            }
        }

        void consume()
        {
            for (;;)
            {
                uint32_t file{};
                uint64_t offset{};
                std::span<unsigned char> output;
                {
                    std::unique_lock lock(mutex_);
                    available_.wait(lock, [this] { return pending_ || stopping_; });
                    if (!pending_)
                    {
                        return;
                    }
                    file = file_;
                    offset = offset_;
                    output = output_;
                    pending_ = false;
                }
                const bool successful = invoke(file, offset, output);
                {
                    std::lock_guard lock(mutex_);
                    successful_ = successful;
                    complete_ = true;
                }
                completed_.notify_one();
            }
        }

        Reader reader_;
        std::mutex request_mutex_, mutex_;
        std::condition_variable available_, completed_;
        uint32_t file_{};
        uint64_t offset_{};
        std::span<unsigned char> output_;
        bool pending_{}, complete_{}, successful_{}, stopping_{};
        std::thread worker_;
    };
} // namespace kh02asset
