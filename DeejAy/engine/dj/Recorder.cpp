#include "Recorder.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace dj
{

namespace
{
    void put16 (std::FILE* f, std::uint16_t v) { const unsigned char b[2] { (unsigned char) v, (unsigned char) (v >> 8) }; std::fwrite (b, 1, 2, f); }
    void put32 (std::FILE* f, std::uint32_t v) { const unsigned char b[4] { (unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16), (unsigned char) (v >> 24) }; std::fwrite (b, 1, 4, f); }
}

bool WavWriter::open (const std::string& path, double sampleRate, int channels, int bits)
{
    close();
    file_ = std::fopen (path.c_str(), "wb");
    if (file_ == nullptr) return false;
    sr_ = sampleRate; channels_ = channels; bits_ = bits == 32 ? 32 : 24; frames_ = 0;
    writeHeader();
    return true;
}

void WavWriter::writeHeader()
{
    const std::uint32_t bytesPerSample = (std::uint32_t) bits_ / 8;
    const std::uint64_t dataBytes64 = frames_ * (std::uint64_t) channels_ * bytesPerSample;
    const auto dataBytes = (std::uint32_t) std::min<std::uint64_t> (dataBytes64, 0xFFFFFFF0ull);
    std::fseek (file_, 0, SEEK_SET);
    std::fwrite ("RIFF", 1, 4, file_);
    put32 (file_, 36 + dataBytes);
    std::fwrite ("WAVEfmt ", 1, 8, file_);
    put32 (file_, 16);
    put16 (file_, bits_ == 32 ? 3 : 1);                       // IEEE float or PCM
    put16 (file_, (std::uint16_t) channels_);
    put32 (file_, (std::uint32_t) std::lround (sr_));
    put32 (file_, (std::uint32_t) std::lround (sr_) * (std::uint32_t) channels_ * bytesPerSample);
    put16 (file_, (std::uint16_t) (channels_ * (int) bytesPerSample));
    put16 (file_, (std::uint16_t) bits_);
    std::fwrite ("data", 1, 4, file_);
    put32 (file_, dataBytes);
}

void WavWriter::write (const float* x, std::size_t frames)
{
    if (file_ == nullptr || frames == 0) return;
    const std::size_t n = frames * (std::size_t) channels_;
    if (bits_ == 32)
        std::fwrite (x, sizeof (float), n, file_);   // little-endian hosts (macOS arm64/x86_64)
    else
    {
        std::vector<unsigned char> buf (n * 3);
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto v = (std::int32_t) std::lround (std::clamp (x[i], -1.0f, 1.0f) * 8388607.0f);
            buf[i * 3] = (unsigned char) v;
            buf[i * 3 + 1] = (unsigned char) (v >> 8);
            buf[i * 3 + 2] = (unsigned char) (v >> 16);
        }
        std::fwrite (buf.data(), 1, buf.size(), file_);
    }
    frames_ += frames;
}

void WavWriter::close()
{
    if (file_ == nullptr) return;
    writeHeader();
    std::fclose (file_);
    file_ = nullptr;
}

bool Recorder::start (const std::string& path, double sampleRate, int bits)
{
    stop();
    if (! writer_.open (path, sampleRate, 2, bits)) return false;
    float discard;
    while (ring_.pop (discard)) {}
    seconds_.store (0.0);
    dropped_.store (0);
    quit_.store (false);
    recording_.store (true, std::memory_order_release);
    thread_ = std::thread ([this, sampleRate]
    {
        std::vector<float> chunk;
        chunk.reserve (8192);
        for (;;)
        {
            const bool quitting = quit_.load (std::memory_order_acquire);
            float v;
            while (chunk.size() < 8192 && ring_.pop (v)) chunk.push_back (v);
            if (chunk.size() >= 2)
            {
                const std::size_t frames = chunk.size() / 2;
                writer_.write (chunk.data(), frames);
                chunk.erase (chunk.begin(), chunk.begin() + (std::ptrdiff_t) (frames * 2));
                seconds_.store ((double) writer_.framesWritten() / sampleRate, std::memory_order_relaxed);
            }
            if (quitting && ring_.size() == 0) break;
            if (ring_.size() < 4096) std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }
        writer_.close();
    });
    return true;
}

void Recorder::stop()
{
    recording_.store (false, std::memory_order_release);
    quit_.store (true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

} // namespace dj
