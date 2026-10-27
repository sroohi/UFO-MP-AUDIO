// DEEJ-AY — wait-free single-producer / single-consumer FIFO.
// Used for UI→audio commands, audio→UI events and the recorder tap.
#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace dj
{

template <typename T>
class SpscFifo
{
public:
    explicit SpscFifo (std::size_t minimumCapacity = 256)
        : buffer_ (roundUpPow2 (minimumCapacity)), mask_ (buffer_.size() - 1) {}

    // Producer side. Never blocks, never allocates.
    bool push (const T& value) noexcept
    {
        const auto w = write_.load (std::memory_order_relaxed);
        const auto r = read_.load (std::memory_order_acquire);
        if (w - r >= buffer_.size())
            return false;
        buffer_[w & mask_] = value;
        write_.store (w + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Never blocks, never allocates.
    bool pop (T& out) noexcept
    {
        const auto r = read_.load (std::memory_order_relaxed);
        const auto w = write_.load (std::memory_order_acquire);
        if (r == w)
            return false;
        out = buffer_[r & mask_];
        read_.store (r + 1, std::memory_order_release);
        return true;
    }

    std::size_t size() const noexcept
    {
        return write_.load (std::memory_order_acquire) - read_.load (std::memory_order_acquire);
    }

    std::size_t capacity() const noexcept { return buffer_.size(); }

private:
    static std::size_t roundUpPow2 (std::size_t n) noexcept
    {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    std::vector<T> buffer_;
    std::size_t mask_;
    alignas (64) std::atomic<std::size_t> write_ { 0 };
    alignas (64) std::atomic<std::size_t> read_ { 0 };
};

} // namespace dj
