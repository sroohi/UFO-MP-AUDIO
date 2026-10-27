// Mix recording: the audio thread pushes the post-limiter master into a wait-free ring; a writer
// thread drains it into a WAV file (32-bit float or 24-bit PCM). Starting/stopping happens on the
// message thread.
#pragma once
#include "SpscFifo.h"
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace dj
{

class WavWriter
{
public:
    ~WavWriter() { close(); }
    bool open (const std::string& path, double sampleRate, int channels, int bits); // bits = 24 or 32 (float)
    void write (const float* interleaved, std::size_t frames);
    void close();
    bool isOpen() const noexcept { return file_ != nullptr; }
    std::uint64_t framesWritten() const noexcept { return frames_; }
private:
    void writeHeader();
    std::FILE* file_ = nullptr;
    double sr_ = 48000.0;
    int channels_ = 2, bits_ = 24;
    std::uint64_t frames_ = 0;
};

class Recorder
{
public:
    Recorder() : ring_ (1u << 21) {}
    ~Recorder() { stop(); }

    // message thread
    bool start (const std::string& path, double sampleRate, int bits = 24);
    void stop();
    bool recording() const noexcept { return recording_.load (std::memory_order_acquire); }
    double seconds() const noexcept { return seconds_.load (std::memory_order_relaxed); }
    std::uint64_t droppedFrames() const noexcept { return dropped_.load (std::memory_order_relaxed); }

    // audio thread
    void push (const float* l, const float* r, int n) noexcept
    {
        if (! recording_.load (std::memory_order_acquire)) return;
        for (int i = 0; i < n; ++i)
        {
            if (ring_.capacity() - ring_.size() < 2) { dropped_.fetch_add ((std::uint64_t) (n - i), std::memory_order_relaxed); return; }
            ring_.push (l[i]);
            ring_.push (r[i]);
        }
    }

private:
    void run();
    SpscFifo<float> ring_;
    std::atomic<bool> recording_ { false }, quit_ { false };
    std::atomic<double> seconds_ { 0.0 };
    std::atomic<std::uint64_t> dropped_ { 0 };
    std::thread thread_;
    WavWriter writer_;
};

} // namespace dj
