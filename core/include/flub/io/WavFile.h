// Flubsound Pro - dependency-free WAV reader/writer for batch processing.
//
// Reads RIFF/WAVE (and RF64 is a roadmap item): PCM 16/24/32-bit integer,
// IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE with PCM/float sub-formats, any
// channel count up to kMaxChannels, skipping unknown chunks (LIST, bext...).
// Writes float32 or PCM16/24 with TPDF dither (+-1 LSB triangular) for
// integer formats. It is the CLI's and the tests' only file format, and the
// app's Export / batch process dialog writes its WAV files with it (other
// formats go through JUCE's AudioFormatManager there, app/Source/export).
// This class keeps the CLI and tests free of JUCE.
#pragma once

#include <cstddef>
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

/** readWav on a WAV file's bytes in memory (the same parser; errors name
    "<memory>"). Lets tests and fuzzers exercise the parser without the
    file system, whose cost on Windows dominates thousands of small reads. */
bool readWavMemory (const uint8_t* data, size_t size, AudioFileData& out, std::string& error);

/** format must be Pcm16, Pcm24 or Float32. Integer formats are TPDF dithered. */
bool writeWav (const std::string& path, const AudioFileData& data, SampleFormat format, std::string& error);
} // namespace flub::io
