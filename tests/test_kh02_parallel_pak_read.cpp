#include "shared/kh02/kh02_parallel_pak_read.h"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <stdexcept>
#include <vector>

int main()
{
    std::atomic<unsigned> active{}, peak{}, completed{};
    std::atomic<bool> fail{}, throw_read{};
    std::barrier overlap(2);
    kh02asset::ParallelPakRead reads(
        [&](uint32_t file, uint64_t offset, std::span<unsigned char> output)
        {
            const auto count = ++active;
            auto previous = peak.load();
            while (previous < count && !peak.compare_exchange_weak(previous, count))
            {
            }
            if (output.size() >= 65'536)
            {
                overlap.arrive_and_wait();
            }
            for (size_t index = 0; index < output.size(); ++index)
            {
                output[index] = static_cast<unsigned char>((offset + index + file) % 251);
            }
            --active;
            ++completed;
            if (throw_read && offset > 1024)
            {
                throw std::runtime_error("native reader failure");
            }
            return !fail;
        });

    for (const size_t size : {33, 262'144, 1'048'575, 1'048'576})
    {
        std::vector<unsigned char> output(size + 2, 0xff);
        assert(reads.read(7, 13, {output.data() + 1, size}));
        assert(output.front() == 0xff && output.back() == 0xff);
        for (size_t index = 0; index < size; ++index)
        {
            assert(output[index + 1] == (13 + index + 7) % 251);
        }
        assert(active == 0);
    }
    assert(peak == 2);
    std::vector<unsigned char> output(262'144);
    auto before = completed.load();
    fail = true;
    assert(!reads.read(7, 13, output));
    assert(completed == before + 2 && active == 0);
    fail = false;
    throw_read = true;
    before = completed.load();
    assert(!reads.read(7, 13, output));
    assert(completed == before + 2 && active == 0);
    throw_read = false;
    assert(reads.read(7, 13, output));
}
