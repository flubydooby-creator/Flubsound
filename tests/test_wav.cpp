// Tests for the WAV reader/writer: round trips for every output format,
// header layout, hand-built files for every supported input format, TPDF
// dither statistics, and hostile input (truncation, garbage, fuzzed headers).
//
// The Processor-style checks map onto file I/O as follows: (a) there is no
// real-time path in this module (reader/writer allocate by design), so the
// allocation-free contract is covered for the JSON accessors in
// test_json.cpp; (b) robustness = extreme/non-finite sample values at every
// sample rate; (c) block-size invariance = frame counts around the internal
// conversion block (8192 frames) and deterministic output; (d) latency =
// impulses keep their exact frame and channel (zero offset).
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/io/WavFile.h"

#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

#if defined(__linux__)
    #include <sys/stat.h>
    #include <sys/sysmacros.h>
#endif

using namespace flub;
using namespace flub::io;
using namespace flubtest;

namespace
{
/** Unique file in the system temp directory, removed on destruction. */
class TempFile
{
public:
    TempFile()
    {
        static int counter = 0;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto name = "flub_wav_test_" + std::to_string (ticks) + "_" + std::to_string (std::random_device {}()) + "_"
                          + std::to_string (++counter) + ".wav";
        path = (std::filesystem::temp_directory_path() / name).string();
    }

    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove (path, ec);
    }

    TempFile (const TempFile&) = delete;
    TempFile& operator= (const TempFile&) = delete;

    std::string path;
};

/** Little-endian byte builder for hand-made WAV files. */
struct Bytes
{
    std::vector<uint8_t> v;

    Bytes& id (const char* s)
    {
        v.insert (v.end(), s, s + 4);
        return *this;
    }
    Bytes& u8 (uint32_t x)
    {
        v.push_back (static_cast<uint8_t> (x & 0xFFu));
        return *this;
    }
    Bytes& u16 (uint32_t x) { return u8 (x).u8 (x >> 8); }
    Bytes& u24 (uint32_t x) { return u16 (x).u8 (x >> 16); }
    Bytes& u32 (uint32_t x) { return u16 (x).u16 (x >> 16); }
    Bytes& u64 (uint64_t x) { return u32 (static_cast<uint32_t> (x)).u32 (static_cast<uint32_t> (x >> 32)); }
    Bytes& f32 (float x) { return u32 (std::bit_cast<uint32_t> (x)); }
    Bytes& f64 (double x) { return u64 (std::bit_cast<uint64_t> (x)); }
    Bytes& append (const Bytes& b)
    {
        v.insert (v.end(), b.v.begin(), b.v.end());
        return *this;
    }
    /** Appends a chunk; odd bodies get the RIFF pad byte unless `pad` is false. */
    Bytes& chunk (const char* ckId, const Bytes& body, bool pad = true)
    {
        id (ckId).u32 (static_cast<uint32_t> (body.v.size())).append (body);
        if (pad && (body.v.size() & 1u) != 0)
            u8 (0);
        return *this;
    }
};

Bytes fmtBody (uint32_t tag, uint32_t channels, uint32_t rate, uint32_t bits)
{
    const uint32_t blockAlign = channels * ((bits + 7) / 8);
    Bytes b;
    b.u16 (tag).u16 (channels).u32 (rate).u32 (rate * blockAlign).u16 (blockAlign).u16 (bits);
    return b;
}

const uint8_t kGuidTail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

Bytes fmtExtensibleBody (uint32_t subTag, uint32_t channels, uint32_t rate, uint32_t containerBits, uint32_t validBits, uint32_t mask)
{
    Bytes b = fmtBody (0xFFFE, channels, rate, containerBits);
    b.u16 (22).u16 (validBits).u32 (mask).u16 (subTag);
    b.v.insert (b.v.end(), kGuidTail, kGuidTail + 14);
    return b;
}

std::vector<uint8_t> riff (const Bytes& chunks)
{
    Bytes b;
    b.id ("RIFF").u32 (static_cast<uint32_t> (chunks.v.size() + 4)).id ("WAVE").append (chunks);
    return b.v;
}

void writeBytes (const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream f (path, std::ios::binary | std::ios::trunc);
    f.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
}

std::vector<uint8_t> readBytes (const std::string& path)
{
    std::ifstream f (path, std::ios::binary | std::ios::ate);
    std::vector<uint8_t> bytes (static_cast<size_t> (std::max<std::streamoff> (0, f.tellg())));
    f.seekg (0);
    f.read (reinterpret_cast<char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
    return bytes;
}

uint32_t le16 (const std::vector<uint8_t>& b, size_t at) { return static_cast<uint32_t> (b[at] | (b[at + 1] << 8)); }
uint32_t le32 (const std::vector<uint8_t>& b, size_t at) { return le16 (b, at) | (le16 (b, at + 2) << 16); }
bool idAt (const std::vector<uint8_t>& b, size_t at, const char* id) { return std::memcmp (b.data() + at, id, 4) == 0; }

/** Deterministic multichannel test signal: a different sine + noise per channel, peak < 0.9. */
AudioFileData makeSignal (int numChannels, int numFrames, double sampleRate)
{
    AudioFileData d;
    d.sampleRate = sampleRate;
    d.numChannels = numChannels;
    for (int c = 0; c < numChannels; ++c)
    {
        auto s = sine (137.0 * (c + 1), sampleRate, numFrames, 0.6f, 0.3 * c);
        const auto n = whiteNoise (numFrames, 0.29f, static_cast<uint32_t> (1000 + c));
        for (size_t i = 0; i < s.size(); ++i)
            s[i] += n[i];
        d.channels.push_back (std::move (s));
    }
    return d;
}

double maxAbsError (const AudioFileData& a, const AudioFileData& b)
{
    double e = 0.0;
    for (size_t c = 0; c < a.channels.size(); ++c)
        for (size_t i = 0; i < a.channels[c].size(); ++i)
            e = std::max (e, std::abs (static_cast<double> (a.channels[c][i]) - static_cast<double> (b.channels[c][i])));
    return e;
}

bool bitExact (const AudioFileData& a, const AudioFileData& b)
{
    if (a.channels.size() != b.channels.size())
        return false;
    for (size_t c = 0; c < a.channels.size(); ++c)
    {
        if (a.channels[c].size() != b.channels[c].size())
            return false;
        for (size_t i = 0; i < a.channels[c].size(); ++i)
            if (std::bit_cast<uint32_t> (a.channels[c][i]) != std::bit_cast<uint32_t> (b.channels[c][i]))
                return false;
    }
    return true;
}

bool allFinite (const AudioFileData& d)
{
    for (const auto& ch : d.channels)
        for (float x : ch)
            if (! std::isfinite (x))
                return false;
    return true;
}

bool contains (const std::string& s, const char* needle) { return s.find (needle) != std::string::npos; }

/** Writes `bytes` to a temp file and reads it back. */
bool readBytesAsWav (const std::vector<uint8_t>& bytes, AudioFileData& out, std::string& error)
{
    TempFile f;
    writeBytes (f.path, bytes);
    return readWav (f.path, out, error);
}

bool roundTrip (const AudioFileData& in, SampleFormat format, AudioFileData& out)
{
    TempFile f;
    std::string error;
    if (! writeWav (f.path, in, format, error))
    {
        std::cerr << "    write failed: " << error << "\n";
        return false;
    }
    if (! readWav (f.path, out, error))
    {
        std::cerr << "    read failed: " << error << "\n";
        return false;
    }
    return true;
}
} // namespace

//==============================================================================
TEST_CASE ("WavFile: Float32 multichannel round trip is bit exact and header fields read back")
{
    for (int channels : { 1, 2, 3, 6, 8 })
    {
        auto in = makeSignal (channels, 1500, 48000.0);
        // Values a float file must carry untouched: overs, signed zero, denormals, huge values.
        in.channels[0][10] = 1.5f;
        in.channels[0][11] = -2.0f;
        in.channels[0][12] = -0.0f;
        in.channels[0][13] = std::numeric_limits<float>::denorm_min();
        in.channels[0][14] = 3.0e38f;
        in.channels[0][15] = 1.0e-30f;

        AudioFileData out;
        REQUIRE (roundTrip (in, SampleFormat::Float32, out));
        CHECK (out.numChannels == channels);
        CHECK (out.channels.size() == static_cast<size_t> (channels));
        CHECK (out.sampleRate == 48000.0);
        CHECK (out.sourceFormat == SampleFormat::Float32);
        CHECK (out.numFrames() == 1500);
        CHECK (bitExact (in, out));
    }
}

TEST_CASE ("WavFile: PCM24 round trip error is below 2^-22")
{
    for (int channels : { 1, 3, 8 })
    {
        const auto in = makeSignal (channels, 2001, 96000.0); // odd length: mono needs a pad byte
        AudioFileData out;
        REQUIRE (roundTrip (in, SampleFormat::Pcm24, out));
        CHECK (out.numChannels == channels);
        CHECK (out.sampleRate == 96000.0);
        CHECK (out.sourceFormat == SampleFormat::Pcm24);
        CHECK (out.numFrames() == 2001);
        CHECK_LE (maxAbsError (in, out), std::ldexp (1.0, -22) * 0.999);
    }
}

TEST_CASE ("WavFile: PCM16 round trip error is below 2^-14 including dither")
{
    for (int channels : { 1, 2, 5 })
    {
        const auto in = makeSignal (channels, 3000, 44100.0);
        AudioFileData out;
        REQUIRE (roundTrip (in, SampleFormat::Pcm16, out));
        CHECK (out.numChannels == channels);
        CHECK (out.sampleRate == 44100.0);
        CHECK (out.sourceFormat == SampleFormat::Pcm16);
        CHECK (out.numFrames() == 3000);
        const double err = maxAbsError (in, out);
        CHECK_LE (err, std::ldexp (1.0, -14) * 0.999);
        CHECK_GE (err, std::ldexp (1.0, -16)); // dither + rounding really happened
    }
}

TEST_CASE ("WavFile: writer produces exact RIFF sizes, fmt fields and channel masks")
{
    TempFile f;
    std::string error;

    // Stereo PCM16: plain 16-byte WAVEFORMAT.
    {
        auto d = makeSignal (2, 10, 44100.0);
        REQUIRE (writeWav (f.path, d, SampleFormat::Pcm16, error));
        const auto b = readBytes (f.path);
        REQUIRE (b.size() == 44 + 40);
        CHECK (idAt (b, 0, "RIFF") && idAt (b, 8, "WAVE") && idAt (b, 12, "fmt ") && idAt (b, 36, "data"));
        CHECK (le32 (b, 4) == b.size() - 8);
        CHECK (le32 (b, 16) == 16);
        CHECK (le16 (b, 20) == 1);        // PCM
        CHECK (le16 (b, 22) == 2);        // channels
        CHECK (le32 (b, 24) == 44100);    // rate
        CHECK (le32 (b, 28) == 44100 * 4); // byte rate
        CHECK (le16 (b, 32) == 4);        // block align
        CHECK (le16 (b, 34) == 16);       // bits
        CHECK (le32 (b, 40) == 40);       // data size
    }

    // Mono PCM24 with an odd frame count: odd data size -> pad byte counted in the RIFF size.
    {
        auto d = makeSignal (1, 5, 48000.0);
        REQUIRE (writeWav (f.path, d, SampleFormat::Pcm24, error));
        const auto b = readBytes (f.path);
        REQUIRE (b.size() == 44 + 15 + 1);
        CHECK (le32 (b, 4) == b.size() - 8);
        CHECK (le16 (b, 20) == 1);
        CHECK (le16 (b, 32) == 3);
        CHECK (le16 (b, 34) == 24);
        CHECK (le32 (b, 40) == 15);
        CHECK (b.back() == 0);
    }

    // Stereo float: 18-byte WAVEFORMATEX with cbSize 0 plus a fact chunk.
    {
        auto d = makeSignal (2, 7, 192000.0);
        REQUIRE (writeWav (f.path, d, SampleFormat::Float32, error));
        const auto b = readBytes (f.path);
        REQUIRE (b.size() == 12 + 26 + 12 + 8 + 56);
        CHECK (le32 (b, 4) == b.size() - 8);
        CHECK (le32 (b, 16) == 18);
        CHECK (le16 (b, 20) == 3); // IEEE float
        CHECK (le32 (b, 24) == 192000);
        CHECK (le16 (b, 34) == 32);
        CHECK (le16 (b, 36) == 0); // cbSize
        CHECK (idAt (b, 38, "fact") && le32 (b, 42) == 4 && le32 (b, 46) == 7);
        CHECK (idAt (b, 50, "data") && le32 (b, 54) == 56);
    }

    // 5.1 float: WAVE_FORMAT_EXTENSIBLE, mask FL FR FC LFE SL SR, float sub-format GUID.
    {
        auto d = makeSignal (6, 3, 96000.0);
        REQUIRE (writeWav (f.path, d, SampleFormat::Float32, error));
        const auto b = readBytes (f.path);
        REQUIRE (b.size() == 12 + 48 + 12 + 8 + 72);
        CHECK (le32 (b, 4) == b.size() - 8);
        CHECK (le32 (b, 16) == 40);
        CHECK (le16 (b, 20) == 0xFFFE);
        CHECK (le16 (b, 22) == 6);
        CHECK (le32 (b, 28) == 96000 * 24);
        CHECK (le16 (b, 32) == 24);
        CHECK (le16 (b, 34) == 32);
        CHECK (le16 (b, 36) == 22);    // cbSize
        CHECK (le16 (b, 38) == 32);    // valid bits
        CHECK (le32 (b, 40) == 0x60F); // channel mask
        CHECK (le16 (b, 44) == 3);     // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
        CHECK (std::memcmp (b.data() + 46, kGuidTail, 14) == 0);
        CHECK (idAt (b, 60, "fact") && le32 (b, 68) == 3);
        CHECK (idAt (b, 72, "data") && le32 (b, 76) == 72);
    }

    // 7.1 PCM16: EXTENSIBLE with the PCM sub-format and the 7.1 mask.
    {
        auto d = makeSignal (8, 4, 48000.0);
        REQUIRE (writeWav (f.path, d, SampleFormat::Pcm16, error));
        const auto b = readBytes (f.path);
        REQUIRE (b.size() == 12 + 48 + 8 + 64);
        CHECK (le32 (b, 4) == b.size() - 8);
        CHECK (le16 (b, 20) == 0xFFFE);
        CHECK (le32 (b, 40) == 0x63F);
        CHECK (le16 (b, 44) == 1); // KSDATAFORMAT_SUBTYPE_PCM
        CHECK (idAt (b, 60, "data") && le32 (b, 64) == 64);
    }
}

TEST_CASE ("WavFile: reads WAVE_FORMAT_EXTENSIBLE float32, float64 and 24-in-32 PCM files")
{
    AudioFileData out;
    std::string error;

    {
        Bytes data;
        const float values[] = { 0.25f, -0.5f, 0.75f, 1.25f, -1.0f, 0.0f };
        for (float v : values)
            data.f32 (v);
        Bytes chunks;
        chunks.chunk ("fmt ", fmtExtensibleBody (3, 3, 88200, 32, 32, 0x7)).chunk ("data", data);
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        CHECK (out.numChannels == 3);
        CHECK (out.sampleRate == 88200.0);
        CHECK (out.sourceFormat == SampleFormat::Float32);
        REQUIRE (out.numFrames() == 2);
        CHECK (out.channels[0][0] == 0.25f && out.channels[1][0] == -0.5f && out.channels[2][0] == 0.75f);
        CHECK (out.channels[0][1] == 1.25f && out.channels[1][1] == -1.0f && out.channels[2][1] == 0.0f);
    }
    {
        Bytes data;
        data.f64 (0.125).f64 (-0.375);
        Bytes chunks;
        chunks.chunk ("fmt ", fmtExtensibleBody (3, 1, 44100, 64, 64, 0x4)).chunk ("data", data);
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        CHECK (out.sourceFormat == SampleFormat::Float64);
        REQUIRE (out.numFrames() == 2);
        CHECK (out.channels[0][0] == 0.125f && out.channels[0][1] == -0.375f);
    }
    {
        // 24 valid bits left-justified in 32-bit containers.
        Bytes data;
        data.u32 (0x40000000u).u32 (0x80000000u).u32 (0x00000100u).u32 (0xFFFFFF00u);
        Bytes chunks;
        chunks.chunk ("fmt ", fmtExtensibleBody (1, 2, 48000, 32, 24, 0x3)).chunk ("data", data);
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        CHECK (out.sourceFormat == SampleFormat::Pcm32);
        REQUIRE (out.numFrames() == 2);
        CHECK (out.channels[0][0] == 0.5f);
        CHECK (out.channels[1][0] == -1.0f);
        CHECK (out.channels[0][1] == static_cast<float> (std::ldexp (1.0, -23)));
        CHECK (out.channels[1][1] == static_cast<float> (-std::ldexp (1.0, -23)));
    }
    {
        Bytes data;
        data.u16 (0x4000).u16 (0xC000);
        Bytes chunks;
        chunks.chunk ("fmt ", fmtExtensibleBody (1, 2, 32000, 16, 16, 0x3)).chunk ("data", data);
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        CHECK (out.sourceFormat == SampleFormat::Pcm16);
        CHECK (out.sampleRate == 32000.0);
        REQUIRE (out.numFrames() == 1);
        CHECK (out.channels[0][0] == 0.5f && out.channels[1][0] == -0.5f);
    }
}

TEST_CASE ("WavFile: plain PCM16/24/32 and float32/64 decode to exact normalised values")
{
    AudioFileData out;
    std::string error;
    const auto readMono = [&] (const Bytes& fmt, const Bytes& data) -> bool
    {
        Bytes chunks;
        chunks.chunk ("fmt ", fmt).chunk ("data", data);
        return readBytesAsWav (riff (chunks), out, error);
    };

    {
        Bytes data;
        data.u16 (0x8000).u16 (0xFFFF).u16 (0).u16 (1).u16 (0x4000).u16 (0x7FFF);
        REQUIRE (readMono (fmtBody (1, 1, 48000, 16), data));
        CHECK (out.sourceFormat == SampleFormat::Pcm16);
        const float expected[] = { -1.0f, -1.0f / 32768.0f, 0.0f, 1.0f / 32768.0f, 0.5f, 32767.0f / 32768.0f };
        REQUIRE (out.numFrames() == 6);
        for (size_t i = 0; i < 6; ++i)
            CHECK (out.channels[0][i] == expected[i]);
    }
    {
        Bytes data;
        data.u24 (0x800000).u24 (0x7FFFFF).u24 (0x400000).u24 (1).u24 (0xFFFFFF);
        REQUIRE (readMono (fmtBody (1, 1, 48000, 24), data));
        CHECK (out.sourceFormat == SampleFormat::Pcm24);
        const float expected[] = { -1.0f, 8388607.0f / 8388608.0f, 0.5f, 1.0f / 8388608.0f, -1.0f / 8388608.0f };
        REQUIRE (out.numFrames() == 5);
        for (size_t i = 0; i < 5; ++i)
            CHECK (out.channels[0][i] == expected[i]);
    }
    {
        // 20-bit samples in a 3-byte container (left-justified) decode through the 24-bit path.
        Bytes data;
        data.u24 (0x400000);
        REQUIRE (readMono (fmtBody (1, 1, 48000, 20), data));
        CHECK (out.sourceFormat == SampleFormat::Pcm24);
        CHECK (out.channels[0][0] == 0.5f);
    }
    {
        Bytes data;
        data.u32 (0x80000000u).u32 (0x7FFFFFFFu).u32 (0x40000000u).u32 (1);
        REQUIRE (readMono (fmtBody (1, 1, 48000, 32), data));
        CHECK (out.sourceFormat == SampleFormat::Pcm32);
        REQUIRE (out.numFrames() == 4);
        CHECK (out.channels[0][0] == -1.0f);
        CHECK (out.channels[0][1] < 1.0f); // full scale stays inside [-1, 1) after float rounding
        CHECK (out.channels[0][1] > 0.9999999f);
        CHECK (out.channels[0][2] == 0.5f);
        CHECK (out.channels[0][3] == static_cast<float> (std::ldexp (1.0, -31)));
    }
    {
        Bytes data;
        data.f32 (0.25f).f32 (-3.5f).f32 (std::numeric_limits<float>::quiet_NaN()).f32 (std::numeric_limits<float>::infinity()).f32 (-0.0f);
        REQUIRE (readMono (fmtBody (3, 1, 48000, 32), data));
        CHECK (out.sourceFormat == SampleFormat::Float32);
        REQUIRE (out.numFrames() == 5);
        CHECK (out.channels[0][0] == 0.25f);
        CHECK (out.channels[0][1] == -3.5f); // float data is not clipped
        CHECK (out.channels[0][2] == 0.0f);  // NaN / Inf are replaced by silence
        CHECK (out.channels[0][3] == 0.0f);
        CHECK (std::signbit (out.channels[0][4]));
    }
    {
        Bytes data;
        data.f64 (0.1).f64 (1.0e300).f64 (-1.0e300).f64 (std::numeric_limits<double>::quiet_NaN()).f64 (-std::numeric_limits<double>::infinity());
        REQUIRE (readMono (fmtBody (3, 1, 48000, 64), data));
        CHECK (out.sourceFormat == SampleFormat::Float64);
        REQUIRE (out.numFrames() == 5);
        CHECK (out.channels[0][0] == 0.1f);
        CHECK (out.channels[0][1] == std::numeric_limits<float>::max());
        CHECK (out.channels[0][2] == -std::numeric_limits<float>::max());
        CHECK (out.channels[0][3] == 0.0f);
        CHECK (out.channels[0][4] == 0.0f);
    }
}

TEST_CASE ("WavFile: unknown chunks, pad bytes, missing pads and oversize data chunks are handled")
{
    AudioFileData out;
    std::string error;
    Bytes stereo16;
    stereo16.u16 (0x4000).u16 (0xC000).u16 (0x2000).u16 (0xE000).u16 (0x1000).u16 (0xF000);
    const auto checkFrames = [&] (int64_t frames)
    {
        REQUIRE (out.numFrames() == frames);
        CHECK (out.numChannels == 2);
        CHECK (out.channels[0][0] == 0.5f && out.channels[1][0] == -0.5f);
        if (frames > 1)
            CHECK (out.channels[0][1] == 0.25f && out.channels[1][1] == -0.25f);
    };

    {
        // Odd-sized LIST with pad before fmt, junk between fmt and data, LIST after data.
        Bytes list;
        list.id ("INFO").u8 ('x');
        Bytes junk;
        junk.u32 (0).u32 (0);
        Bytes chunks;
        chunks.chunk ("LIST", list).chunk ("fmt ", fmtBody (1, 2, 48000, 16)).chunk ("junk", junk).chunk ("data", stereo16).chunk ("LIST", list);
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        checkFrames (3);
    }
    {
        // Odd-sized chunk written without its pad byte (a common writer bug).
        Bytes odd;
        odd.u8 (1).u8 (2).u8 (3);
        Bytes chunks;
        chunks.chunk ("bext", odd, false).chunk ("fmt ", fmtBody (1, 2, 48000, 16)).chunk ("data", stereo16);
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        checkFrames (3);
    }
    {
        // Streaming writers leave 0xFFFFFFFF in the data size (and 0 in the RIFF size).
        Bytes chunks;
        chunks.chunk ("fmt ", fmtBody (1, 2, 48000, 16)).id ("data").u32 (0xFFFFFFFFu).append (stereo16);
        auto bytes = riff (chunks);
        bytes[4] = bytes[5] = bytes[6] = bytes[7] = 0;
        REQUIRE (readBytesAsWav (bytes, out, error));
        checkFrames (3);
    }
    {
        // Data size larger than the file, plus a trailing partial frame: clamp, drop the partial frame.
        Bytes data = stereo16;
        data.u16 (0x1234).u8 (0x56);
        Bytes chunks;
        chunks.chunk ("fmt ", fmtBody (1, 2, 48000, 16)).id ("data").u32 (1000).append (data);
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        checkFrames (3);
    }
    {
        // data before fmt (non-conformant but seen in the wild), with a fact chunk.
        Bytes fact;
        fact.u32 (3);
        Bytes chunks;
        chunks.chunk ("data", stereo16).chunk ("fact", fact).chunk ("fmt ", fmtBody (1, 2, 22050, 16));
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        checkFrames (3);
        CHECK (out.sampleRate == 22050.0);
    }
    {
        // Many small unknown chunks before the audio are walked; an absurd number is refused.
        Bytes many;
        for (int i = 0; i < 1000; ++i)
            many.chunk ("junk", Bytes {});
        Bytes chunks = many;
        chunks.chunk ("fmt ", fmtBody (1, 2, 48000, 16)).chunk ("data", stereo16);
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        checkFrames (3);

        Bytes tooMany;
        for (int i = 0; i < 70; ++i)
            tooMany.append (many);
        tooMany.chunk ("fmt ", fmtBody (1, 2, 48000, 16)).chunk ("data", stereo16);
        CHECK (! readBytesAsWav (riff (tooMany), out, error));
        CHECK (contains (error, "chunks before the audio data"));
    }
    {
        // A zero-length data chunk is a valid, empty file.
        Bytes chunks;
        chunks.chunk ("fmt ", fmtBody (1, 2, 48000, 16)).chunk ("data", Bytes {});
        REQUIRE (readBytesAsWav (riff (chunks), out, error));
        CHECK (out.numChannels == 2);
        CHECK (out.channels.size() == 2);
        CHECK (out.numFrames() == 0);
    }
}

TEST_CASE ("WavFile: every truncation of a valid file is rejected or clamped without crashing")
{
    TempFile src;
    std::string error;
    const auto in = makeSignal (4, 20, 48000.0);
    REQUIRE (writeWav (src.path, in, SampleFormat::Pcm24, error));
    const auto full = readBytes (src.path);
    const size_t headerSize = 12 + 48 + 8; // RIFF + EXTENSIBLE fmt + data header
    REQUIRE (full.size() == headerSize + 20 * 12);

    AudioFileData reference;
    REQUIRE (readWav (src.path, reference, error));

    TempFile f;
    for (size_t length = 0; length <= full.size(); ++length)
    {
        writeBytes (f.path, std::vector<uint8_t> (full.begin(), full.begin() + static_cast<std::ptrdiff_t> (length)));
        AudioFileData out;
        error.clear();
        const bool ok = readWav (f.path, out, error);
        if (length < headerSize)
        {
            CHECK (! ok);
            CHECK (! error.empty());
        }
        else
        {
            REQUIRE (ok);
            const auto frames = static_cast<int64_t> ((length - headerSize) / 12);
            REQUIRE (out.numFrames() == frames);
            for (int c = 0; c < 4; ++c)
                for (int64_t i = 0; i < frames; ++i)
                    CHECK (out.channels[static_cast<size_t> (c)][static_cast<size_t> (i)]
                           == reference.channels[static_cast<size_t> (c)][static_cast<size_t> (i)]);
        }
    }
}

TEST_CASE ("WavFile: garbage, bad fmt chunks and unsupported formats are rejected with messages")
{
    AudioFileData out;
    std::string error;
    const auto rejects = [&] (const std::vector<uint8_t>& bytes, const char* expected) -> bool
    {
        error.clear();
        const bool ok = readBytesAsWav (bytes, out, error);
        if (ok || ! contains (error, expected))
        {
            std::cerr << "    expected an error containing \"" << expected << "\", got ok=" << ok << " error=\"" << error << "\"\n";
            return false;
        }
        return true;
    };
    const auto withFmt = [] (const Bytes& fmt)
    {
        Bytes data;
        data.u32 (0).u32 (0);
        Bytes chunks;
        chunks.chunk ("fmt ", fmt).chunk ("data", data);
        return riff (chunks);
    };

    CHECK (! readWav ("/nonexistent-dir-flub/none.wav", out, error) && contains (error, "cannot open"));
    CHECK (rejects ({}, "too short"));
    CHECK (rejects ({ 'R', 'I', 'F', 'F' }, "too short"));
    CHECK (rejects (riff (Bytes {}), "missing fmt"));
    CHECK (rejects (withFmt (fmtBody (1, 0, 48000, 16)), "0 channels"));
    CHECK (rejects (withFmt (fmtBody (1, 9, 48000, 16)), "9 channels"));
    CHECK (rejects (withFmt (fmtBody (1, 64, 48000, 16)), "maximum is 8"));
    CHECK (rejects (withFmt (fmtBody (1, 2, 0, 16)), "0 Hz"));
    CHECK (rejects (withFmt (fmtBody (1, 2, 48000, 8)), "8-bit"));
    CHECK (rejects (withFmt (fmtBody (1, 2, 48000, 0)), "bit depth"));
    CHECK (rejects (withFmt (fmtBody (1, 2, 48000, 48)), "bit depth"));
    CHECK (rejects (withFmt (fmtBody (3, 2, 48000, 16)), "float bit depth"));
    CHECK (rejects (withFmt (fmtBody (2, 2, 48000, 4)), "format tag 0x0002"));
    CHECK (rejects (withFmt (fmtBody (0x55, 2, 48000, 0)), "format tag 0x0055"));

    {
        Bytes shortFmt;
        shortFmt.u16 (1).u16 (2).u32 (48000);
        CHECK (rejects (withFmt (shortFmt), "fmt chunk is too small"));
    }
    {
        Bytes ext = fmtBody (0xFFFE, 2, 48000, 16);
        ext.u16 (0);
        CHECK (rejects (withFmt (ext), "EXTENSIBLE fmt chunk is too small"));
    }
    {
        Bytes ext = fmtExtensibleBody (1, 2, 48000, 16, 16, 3);
        ext.v[26 + 5] = 0x11; // corrupt the GUID tail
        CHECK (rejects (withFmt (ext), "sub-format"));
    }
    {
        Bytes chunks;
        chunks.id ("fmt ").u32 (16).u16 (1).u16 (2); // fmt body cut short by the end of the file
        CHECK (rejects (riff (chunks), "truncated fmt"));
    }
    {
        Bytes chunks;
        chunks.chunk ("fmt ", fmtBody (1, 2, 48000, 16));
        CHECK (rejects (riff (chunks), "missing data"));
    }
    {
        auto bytes = riff (Bytes {});
        std::memcpy (bytes.data(), "RF64", 4);
        CHECK (rejects (bytes, "RF64"));
        std::memcpy (bytes.data(), "RIFX", 4);
        CHECK (rejects (bytes, "RIFX"));
        std::memcpy (bytes.data(), "RIFF", 4);
        std::memcpy (bytes.data() + 8, "AVI ", 4);
        CHECK (rejects (bytes, "not WAVE"));
    }

    // Random garbage of many lengths: always an error, never a crash.
    FastRandom rng (77);
    for (int n = 0; n < 60; ++n)
    {
        std::vector<uint8_t> garbage (static_cast<size_t> (1 + n * 7));
        for (auto& b : garbage)
            b = static_cast<uint8_t> (rng.nextU32() >> 24);
        error.clear();
        CHECK (! readBytesAsWav (garbage, out, error));
        CHECK (! error.empty());
    }

    // A failed read leaves the output untouched.
    AudioFileData keep;
    keep.numChannels = 1;
    keep.sampleRate = 12345.0;
    keep.channels = { { 0.5f } };
    TempFile f;
    writeBytes (f.path, withFmt (fmtBody (1, 0, 48000, 16)));
    CHECK (! readWav (f.path, keep, error));
    CHECK (keep.numChannels == 1 && keep.sampleRate == 12345.0 && keep.numFrames() == 1 && keep.channels[0][0] == 0.5f);
}

TEST_CASE ("WavFile: fuzzed headers never crash and successful reads are well-formed")
{
    Bytes list;
    list.id ("INFO").id ("ISFT").u32 (3).u8 ('a').u8 ('b').u8 (0).u8 (0);
    Bytes data;
    for (int i = 0; i < 32 * 2; ++i)
        data.u16 (static_cast<uint32_t> (i * 997));
    Bytes chunks;
    chunks.chunk ("LIST", list).chunk ("fmt ", fmtExtensibleBody (1, 2, 48000, 16, 16, 3)).chunk ("data", data);
    const auto base = riff (chunks);

    TempFile f;
    FastRandom rng (4242);
    int accepted = 0;
    for (int iter = 0; iter < 2500; ++iter)
    {
        auto bytes = base;
        const int mutations = 1 + static_cast<int> (rng.nextU32() % 4);
        for (int m = 0; m < mutations; ++m)
        {
            // Mostly hit the header region (first 104 bytes), where the parser makes decisions.
            const uint32_t span = (rng.nextU32() & 3u) != 0 ? 104u : static_cast<uint32_t> (bytes.size());
            bytes[rng.nextU32() % span] = static_cast<uint8_t> (rng.nextU32() >> 24);
        }
        if ((rng.nextU32() & 7u) == 0)
            bytes.resize (rng.nextU32() % bytes.size());
        writeBytes (f.path, bytes);

        AudioFileData out;
        std::string error;
        if (readWav (f.path, out, error))
        {
            ++accepted;
            CHECK (out.numChannels >= 1 && out.numChannels <= 8);
            CHECK (out.channels.size() == static_cast<size_t> (out.numChannels));
            CHECK (out.sampleRate > 0.0);
            for (const auto& ch : out.channels)
                CHECK (ch.size() == out.channels[0].size());
            CHECK (allFinite (out));
        }
        else
        {
            CHECK (! error.empty());
        }
    }
    CHECK (accepted > 0); // benign mutations (e.g. inside sample data) must still load
}

TEST_CASE ("WavFile: TPDF dither is zero-mean with 0.5 LSB rms error independent of the signal")
{
    // e = Q(x + d) - x with d triangular on (-1, 1) LSB: E[e] = 0, E[e^2] = 1/6 + 1/12 = 1/4 for
    // every x (rectangular dither would give 1/6, i.e. 0.408 LSB rms, and signal-dependent power).
    const int n = 40000;
    for (auto format : { SampleFormat::Pcm16, SampleFormat::Pcm24 })
    {
        const double lsb = format == SampleFormat::Pcm16 ? 1.0 / 32768.0 : 1.0 / 8388608.0;
        for (double level : { 0.0, 1000.25, 1000.5, -3.75, 20000.9 })
        {
            AudioFileData in;
            in.sampleRate = 48000.0;
            in.channels = { std::vector<float> (static_cast<size_t> (n), static_cast<float> (level * lsb)) };
            AudioFileData out;
            REQUIRE (roundTrip (in, format, out));

            double sum = 0.0, sumSq = 0.0, maxErr = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double e = (static_cast<double> (out.channels[0][static_cast<size_t> (i)]) - static_cast<double> (in.channels[0][0])) / lsb;
                sum += e;
                sumSq += e * e;
                maxErr = std::max (maxErr, std::abs (e));
            }
            CHECK_NEAR (sum / n, 0.0, 0.02);
            CHECK_NEAR (std::sqrt (sumSq / n), 0.5, 0.02);
            CHECK_LE (maxErr, 1.5 + 1.0e-6);
        }
    }
}

TEST_CASE ("WavFile: extreme and non-finite samples at every sample rate read back finite and bounded")
{
    const float inf = std::numeric_limits<float>::infinity();
    const std::vector<float> extremes = { std::numeric_limits<float>::quiet_NaN(), inf, -inf, 1.0e30f, -1.0e30f, 1.5f, -1.5f,
                                          std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min(), 1.0f, -1.0f };
    const auto noise = whiteNoise (256, 1.0f, 99);

    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        AudioFileData in;
        in.sampleRate = rate;
        in.channels.resize (2);
        for (auto& ch : in.channels)
        {
            ch.insert (ch.end(), 64, 0.0f);                 // silence
            ch.insert (ch.end(), 64, 0.5f);                 // DC
            ch.insert (ch.end(), noise.begin(), noise.end()); // full-scale white noise
            ch.insert (ch.end(), 16, 0.0f);
            ch.push_back (1.0f); // single-sample impulse
            ch.insert (ch.end(), 16, 0.0f);
            ch.insert (ch.end(), extremes.begin(), extremes.end());
        }
        const size_t extremesAt = in.channels[0].size() - extremes.size();

        for (auto format : { SampleFormat::Pcm16, SampleFormat::Pcm24, SampleFormat::Float32 })
        {
            AudioFileData out;
            REQUIRE (roundTrip (in, format, out));
            CHECK (out.sampleRate == rate);
            CHECK (out.numFrames() == static_cast<int64_t> (in.channels[0].size()));
            CHECK (allFinite (out));

            const auto& ch = out.channels[1];
            if (format == SampleFormat::Float32)
            {
                CHECK (ch[extremesAt + 0] == 0.0f);  // NaN -> 0
                CHECK (ch[extremesAt + 1] == 1.0f);  // +Inf -> +full scale
                CHECK (ch[extremesAt + 2] == -1.0f); // -Inf -> -full scale
                CHECK (ch[extremesAt + 3] == 1.0e30f);
                for (size_t i = 0; i < 64; ++i)
                    CHECK (ch[i] == 0.0f);
            }
            else
            {
                const double lsb = format == SampleFormat::Pcm16 ? 1.0 / 32768.0 : 1.0 / 8388608.0;
                for (float x : ch)
                    CHECK (x >= -1.0f && x < 1.0f);
                CHECK_LE (std::abs (ch[extremesAt + 0]), 1.0 * lsb); // NaN -> 0 (+ dither)
                CHECK (ch[extremesAt + 1] == static_cast<float> (1.0 - lsb));
                CHECK (ch[extremesAt + 2] == -1.0f);
                CHECK (ch[extremesAt + 3] == static_cast<float> (1.0 - lsb));
                CHECK (ch[extremesAt + 4] == -1.0f);
                CHECK (ch[extremesAt + 7] == static_cast<float> (1.0 - lsb));
                CHECK_LE (peakAbs (ch.data(), 64), 1.0 * lsb); // dithered silence stays within +-1 LSB
            }
        }
    }
}

TEST_CASE ("WavFile: frame counts around the conversion block size round trip; output is deterministic")
{
    for (int frames : { 1, 7, 64, 512, 8191, 8192, 8193, 20000 })
    {
        const auto in = makeSignal (3, frames, 48000.0);
        AudioFileData out;
        REQUIRE (roundTrip (in, SampleFormat::Float32, out));
        CHECK (out.numFrames() == frames);
        CHECK (bitExact (in, out));

        REQUIRE (roundTrip (in, SampleFormat::Pcm24, out));
        CHECK (out.numFrames() == frames);
        CHECK_LE (maxAbsError (in, out), std::ldexp (1.0, -22));
    }

    // The dither generator is seeded per file: the same input always gives the same bytes.
    const auto in = makeSignal (2, 9000, 44100.0);
    TempFile a, b;
    std::string error;
    REQUIRE (writeWav (a.path, in, SampleFormat::Pcm16, error));
    REQUIRE (writeWav (b.path, in, SampleFormat::Pcm16, error));
    CHECK (readBytes (a.path) == readBytes (b.path));
}

TEST_CASE ("WavFile: impulses keep their exact frame and channel (zero latency, no channel swap)")
{
    const int channels = 8, frames = 400;
    AudioFileData in;
    in.sampleRate = 48000.0;
    in.numChannels = channels;
    in.channels.assign (channels, std::vector<float> (frames, 0.0f));
    const auto impulseAt = [] (int c) { return static_cast<size_t> (20 + 31 * c); };
    for (int c = 0; c < channels; ++c)
        in.channels[static_cast<size_t> (c)][impulseAt (c)] = 0.5f;

    for (auto format : { SampleFormat::Float32, SampleFormat::Pcm24, SampleFormat::Pcm16 })
    {
        AudioFileData out;
        REQUIRE (roundTrip (in, format, out));
        REQUIRE (out.numChannels == channels);
        const double lsb = format == SampleFormat::Pcm16 ? 1.0 / 32768.0 : 1.0 / 8388608.0;
        for (int c = 0; c < channels; ++c)
        {
            const auto& ch = out.channels[static_cast<size_t> (c)];
            size_t peakIndex = 0;
            for (size_t i = 1; i < ch.size(); ++i)
                if (std::abs (ch[i]) > std::abs (ch[peakIndex]))
                    peakIndex = i;
            CHECK (peakIndex == impulseAt (c));
            if (format == SampleFormat::Float32)
                CHECK (ch[peakIndex] == 0.5f);
            else
                CHECK_NEAR (ch[peakIndex], 0.5, 1.5 * lsb);
        }
    }
}

TEST_CASE ("WavFile: writer rejects invalid requests without creating a file")
{
    TempFile f;
    std::string error;
    const auto good = makeSignal (2, 16, 48000.0);
    const auto rejects = [&] (const AudioFileData& d, SampleFormat format, const char* expected) -> bool
    {
        error.clear();
        const bool ok = writeWav (f.path, d, format, error);
        const bool created = std::filesystem::exists (f.path);
        if (ok || created || ! contains (error, expected))
        {
            std::cerr << "    expected \"" << expected << "\", got ok=" << ok << " created=" << created << " error=\"" << error << "\"\n";
            return false;
        }
        return true;
    };

    CHECK (rejects (good, SampleFormat::Pcm32, "unsupported output format"));
    CHECK (rejects (good, SampleFormat::Float64, "unsupported output format"));

    AudioFileData d = good;
    d.channels.clear();
    d.numChannels = 0;
    CHECK (rejects (d, SampleFormat::Float32, "no channels"));

    d = makeSignal (9, 16, 48000.0);
    CHECK (rejects (d, SampleFormat::Float32, "maximum is 8"));

    d = good;
    d.channels[1].pop_back();
    CHECK (rejects (d, SampleFormat::Pcm16, "channel 1 has 15 frames"));

    d = good;
    d.numChannels = 3;
    CHECK (rejects (d, SampleFormat::Pcm16, "does not match"));

    for (double rate : { 0.0, -48000.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1.0e12 })
    {
        d = good;
        d.sampleRate = rate;
        CHECK (rejects (d, SampleFormat::Pcm24, "sample rate"));
    }

    CHECK (! writeWav ("/nonexistent-dir-flub/out.wav", good, SampleFormat::Float32, error));
    CHECK (contains (error, "cannot write"));

    // numChannels == 0 is accepted (channels.size() is authoritative); zero frames is a valid empty file.
    d = good;
    d.numChannels = 0;
    REQUIRE (writeWav (f.path, d, SampleFormat::Pcm16, error));
    d.channels.assign (2, {});
    REQUIRE (writeWav (f.path, d, SampleFormat::Pcm24, error));
    AudioFileData out;
    REQUIRE (readWav (f.path, out, error));
    CHECK (out.numChannels == 2 && out.numFrames() == 0);
}

// ---- adversarial review tests ----

TEST_CASE ("WavFile review: a missing pad byte before 'data' is detected even when the data size looks like text")
{
    // An odd-sized chunk written without its pad, followed by a data chunk whose size
    // has a printable low byte (100 = 'd'): both the padded offset ("ata" + 'd') and the
    // unpadded one ("data") look like chunk IDs, so only knowing real IDs resolves it.
    Bytes odd;
    odd.u8 (1).u8 (2).u8 (3);
    Bytes data;
    for (int i = 0; i < 50; ++i)
        data.u16 (static_cast<uint32_t> (i * 256));
    REQUIRE (data.v.size() == 100);
    Bytes chunks;
    chunks.chunk ("fmt ", fmtBody (1, 1, 48000, 16)).chunk ("bext", odd, false).chunk ("data", data);

    AudioFileData out;
    std::string error;
    REQUIRE (readBytesAsWav (riff (chunks), out, error));
    REQUIRE (out.numFrames() == 50);
    CHECK (out.channels[0][1] == 256.0f / 32768.0f);
    CHECK (out.channels[0][49] == 49.0f * 256.0f / 32768.0f);

    // The same file written correctly (with the pad) still reads identically, even
    // with a printable, non-zero pad byte.
    Bytes padded;
    padded.chunk ("fmt ", fmtBody (1, 1, 48000, 16)).chunk ("bext", odd, false).u8 ('z').chunk ("data", data);
    AudioFileData out2;
    REQUIRE (readBytesAsWav (riff (padded), out2, error));
    REQUIRE (out2.numFrames() == 50);
    CHECK (out2.channels[0][49] == out.channels[0][49]);
}

TEST_CASE ("WavFile review: nBlockAlign frames 24-bit samples stored in 4-byte slots")
{
    // Plain PCM, wBitsPerSample = 24 but nBlockAlign = 8 for stereo: each sample sits
    // left-justified in 4 bytes. Before the fix this was decoded with 3-byte framing,
    // turning the whole file into garbage.
    Bytes fmt;
    fmt.u16 (1).u16 (2).u32 (48000).u32 (48000 * 8).u16 (8).u16 (24);
    Bytes data;
    data.u32 (0x40000000u).u32 (0xC0000000u).u32 (0x20000000u).u32 (0xE0000000u);
    Bytes chunks;
    chunks.chunk ("fmt ", fmt).chunk ("data", data);
    AudioFileData out;
    std::string error;
    REQUIRE (readBytesAsWav (riff (chunks), out, error));
    REQUIRE (out.numFrames() == 2);
    CHECK (out.channels[0][0] == 0.5f && out.channels[1][0] == -0.5f);
    CHECK (out.channels[0][1] == 0.25f && out.channels[1][1] == -0.25f);

    // Nonsense block aligns (bits instead of bytes, channel count forgotten, 0) are ignored.
    for (uint32_t badAlign : { 32u, 2u, 0u, 3u })
    {
        Bytes f16;
        f16.u16 (1).u16 (2).u32 (48000).u32 (48000 * 4).u16 (badAlign).u16 (16);
        Bytes d16;
        d16.u16 (0x4000).u16 (0xC000);
        Bytes c16;
        c16.chunk ("fmt ", f16).chunk ("data", d16);
        REQUIRE (readBytesAsWav (riff (c16), out, error));
        REQUIRE (out.numFrames() == 1);
        CHECK (out.channels[0][0] == 0.5f && out.channels[1][0] == -0.5f);
    }
}

TEST_CASE ("WavFile review: directories and special files give clear errors; a failing device is never unlinked")
{
    AudioFileData out;
    std::string error;
    const auto dir = std::filesystem::temp_directory_path().string();
    CHECK (! readWav (dir, out, error));
    CHECK (contains (error, "is a directory"));

    AudioFileData d = makeSignal (2, 20000, 48000.0);
    CHECK (! writeWav (dir, d, SampleFormat::Pcm16, error));
    CHECK (contains (error, "cannot write"));

#if defined(__linux__)
    // A private copy of /dev/full (writes fail with ENOSPC). The writer used to
    // std::remove() the path after a failed write, which deletes the device node
    // itself (as root, writeWav ("/dev/full", ...) removed /dev/full).
    TempFile node;
    if (::mknod (node.path.c_str(), S_IFCHR | 0600, makedev (1, 7)) == 0)
    {
        CHECK (! writeWav (node.path, d, SampleFormat::Pcm16, error));
        CHECK (contains (error, "write error"));
        CHECK (std::filesystem::exists (node.path));
        CHECK (std::filesystem::is_character_file (node.path));
    }
#endif
}

TEST_CASE ("WavFile review: TPDF dither has the triangular shape and is independent across channels")
{
    // For an input exactly on an integer level the error is round (d) with d triangular
    // on (-1, 1): P(e = +1) = P(e = -1) = P(d > 0.5) = 1/8 and P(e = 0) = 3/4.
    // Rectangular +-0.5 LSB dither would give e = 0 always; +-1 LSB rectangular 1/4 each.
    const int n = 80000;
    AudioFileData in;
    in.sampleRate = 44100.0;
    in.channels = { std::vector<float> (static_cast<size_t> (n), 0.0f), std::vector<float> (static_cast<size_t> (n), 100.0f / 32768.0f) };
    AudioFileData out;
    REQUIRE (roundTrip (in, SampleFormat::Pcm16, out));

    int counts[2][3] = {};
    double sum0 = 0.0, sum1 = 0.0, sum01 = 0.0, sq0 = 0.0, sq1 = 0.0;
    for (size_t i = 0; i < static_cast<size_t> (n); ++i)
    {
        const double e0 = std::round (static_cast<double> (out.channels[0][i]) * 32768.0);
        const double e1 = std::round (static_cast<double> (out.channels[1][i]) * 32768.0) - 100.0;
        REQUIRE (std::abs (e0) <= 1.0 && std::abs (e1) <= 1.0);
        ++counts[0][static_cast<int> (e0) + 1];
        ++counts[1][static_cast<int> (e1) + 1];
        sum0 += e0;
        sum1 += e1;
        sum01 += e0 * e1;
        sq0 += e0 * e0;
        sq1 += e1 * e1;
    }
    for (auto& c : counts)
    {
        CHECK_NEAR (c[0] / static_cast<double> (n), 0.125, 0.006);
        CHECK_NEAR (c[1] / static_cast<double> (n), 0.75, 0.008);
        CHECK_NEAR (c[2] / static_cast<double> (n), 0.125, 0.006);
    }
    const double cov = sum01 / n - (sum0 / n) * (sum1 / n);
    const double corr = cov / std::sqrt ((sq0 / n - (sum0 / n) * (sum0 / n)) * (sq1 / n - (sum1 / n) * (sum1 / n)));
    CHECK_LE (std::abs (corr), 0.02); // interleaved sequential draws: no inter-channel correlation
}

TEST_CASE ("WavFile review: odd-sized EXTENSIBLE PCM24 data gets a counted pad byte and reads back")
{
    TempFile f;
    std::string error;
    const auto in = makeSignal (3, 7, 48000.0); // 3 ch x 3 bytes x 7 frames = 63 bytes
    REQUIRE (writeWav (f.path, in, SampleFormat::Pcm24, error));
    const auto b = readBytes (f.path);
    REQUIRE (b.size() == 12 + 48 + 8 + 63 + 1);
    CHECK (le32 (b, 4) == b.size() - 8);
    CHECK (le16 (b, 20) == 0xFFFE);
    CHECK (le32 (b, 40) == 0x007);
    CHECK (le32 (b, 64) == 63);
    CHECK (b.back() == 0);

    // A chunk appended after the padded data is still found by a strict reader walk.
    auto withTail = b;
    Bytes tail;
    tail.chunk ("LIST", Bytes {}.id ("INFO"));
    withTail.insert (withTail.end(), tail.v.begin(), tail.v.end());
    AudioFileData out;
    REQUIRE (readBytesAsWav (withTail, out, error));
    CHECK (out.numFrames() == 7);
    CHECK_LE (maxAbsError (in, out), std::ldexp (1.0, -22));
}

TEST_CASE ("WavFile review: PCM round trips stay within spec at full scale and at every sample rate")
{
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        AudioFileData in;
        in.sampleRate = rate;
        in.channels.assign (2, {});
        for (float x : { -1.0f, -0.99999f, 0.99995f, 32767.0f / 32768.0f, 1.0e-9f, -1.0e-9f, 0.5f })
            for (auto& ch : in.channels)
                ch.insert (ch.end(), 50, x);
        for (auto format : { SampleFormat::Pcm16, SampleFormat::Pcm24 })
        {
            AudioFileData out;
            REQUIRE (roundTrip (in, format, out));
            CHECK (out.sampleRate == rate);
            const double limit = format == SampleFormat::Pcm16 ? std::ldexp (1.0, -14) : std::ldexp (1.0, -22);
            CHECK_LE (maxAbsError (in, out), limit * 0.999);
        }
    }
}
