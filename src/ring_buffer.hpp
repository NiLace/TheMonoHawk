#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace hawk {

// Single-producer / single-consumer lock-free float ring buffer.
//
// The audio thread (producer) pushes samples; the detection worker (consumer)
// pops them. Neither blocks nor allocates after construction, so the audio
// thread stays real-time safe. Capacity must be a power of two.
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity_pow2)
        : buffer_(capacity_pow2, 0.0f)
        , mask_(capacity_pow2 - 1)
    {
    }

    // Producer side. Writes up to `count` samples; silently drops any that do
    // not fit (the detector simply works from whatever it receives).
    void push(const float* data, std::size_t count)
    {
        const std::size_t w     = write_.load(std::memory_order_relaxed);
        const std::size_t r     = read_.load(std::memory_order_acquire);
        const std::size_t space = capacity() - (w - r);
        const std::size_t n     = (count < space) ? count : space;
        for (std::size_t i = 0; i < n; ++i) {
            buffer_[(w + i) & mask_] = data[i];
        }
        write_.store(w + n, std::memory_order_release);
    }

    // Consumer side. Reads up to `count` samples into `out`; returns how many
    // were actually available and copied.
    std::size_t pop(float* out, std::size_t count)
    {
        const std::size_t r     = read_.load(std::memory_order_relaxed);
        const std::size_t w     = write_.load(std::memory_order_acquire);
        const std::size_t avail = w - r;
        const std::size_t n     = (count < avail) ? count : avail;
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = buffer_[(r + i) & mask_];
        }
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    std::size_t capacity() const { return mask_ + 1; }

private:
    std::vector<float>       buffer_;
    std::size_t              mask_;
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
};

} // namespace hawk
