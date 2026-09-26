// Flubsound Pro - dependency-free WAV reader/writer for batch processing.
//
// Reads RIFF/WAVE (and RF64 is a roadmap item): PCM 16/24/32-bit integer,
// IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE with PCM/float sub-formats, any
// channel count up to kMaxChannels, skipping unknown chunks (LIST, bext...).
// Writes float32 or PCM16/24 with TPDF dither (+-1 LSB triangular) for
// integer formats. WAV is the only file format today (CLI, tests); FLAC /
// MP3 / AIFF / Ogg through JUCE's AudioFormatManager in the app's batch UI
// are roadmap (docs/07-roadmap.md 2.10). This class keeps the CLI and tests
// free of JUCE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flub::io
{
enum class SampleFormat
{
    Pcm16,
    Pcm24,
    Pcm32,
    Float32,
    Float64
};

struct AudioFileData
{
    double sampleRate = 48000.0;
    int numChannels = 0;
    SampleFormat sourceFormat = SampleFormat::Float32;
    std::vector<std::vector<float>> channels; // planar, [channel][frame]

    int64_t numFrames() const noexcept { return channels.empty() ? 0 : static_cast<int64_t> (channels[0].size()); }
};

/** Returns false and fills `error` on failure. */
bool readWav (const std::string& path, AudioFileData& out, std::string& error);

/** format must be Pcm16, Pcm24 or Float32. Integer formats are TPDF dithered. */
bool writeWav (const std::string& path, const AudioFileData& data, SampleFormat format, std::string& error);
} // namespace flub::io
