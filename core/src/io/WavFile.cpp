// Flubsound Pro - dependency-free WAV reader/writer (see WavFile.h).
//
// Reader
//   The RIFF chunk list is walked one 8-byte header at a time with seeks, so
//   the file is never loaded twice and hostile size fields cannot trigger
//   huge allocations or out-of-bounds reads: every chunk size is checked
//   against the real file size, the data chunk is clamped to the bytes that
//   actually exist (streaming writers leave 0xFFFFFFFF or a stale size
//   there) and the sample buffers are sized only from that clamped length.
//   Samples are converted in fixed blocks of kBlockFrames frames.
// Writer
//   Validates everything before the output file is touched, builds the
//   header in memory (exact RIFF / data sizes, pad byte for odd-sized data)
//   and streams the interleaved samples in blocks. Integer formats get TPDF
//   dither from a per-file seeded generator, so renders are reproducible.
#include "flub/io/WavFile.h"

#include "flub/io/FilePath.h"

#include "flub/common/AudioBlock.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>

namespace flub::io
{
namespace
{
constexpr uint16_t kTagPcm = 0x0001;
constexpr uint16_t kTagFloat = 0x0003;
constexpr uint16_t kTagExtensible = 0xFFFE;

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT are {0000000X-0000-0010-8000-00AA00389B71}.
// The first 16-bit word of the GUID is the classic format tag; the other 14
// bytes (as stored little endian in the file) are fixed.
constexpr std::array<uint8_t, 14> kSubFormatGuidTail { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                        0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

// Frames converted per I/O block. Bounds the scratch buffer to 512 KiB (8 ch x 64-bit).
constexpr int64_t kBlockFrames = 8192;

// Only the first 40 bytes of a fmt chunk carry information we use (EXTENSIBLE size).
constexpr uint32_t kMaxFmtBytes = 40;

// Real files have a handful of chunks; a file made of millions of empty chunks would
// otherwise cost two seeks each, so the walk gives up after this many.
constexpr int kMaxChunks = 65536;

// Largest float below 1.0 (1 - 2^-24). PCM32 near full scale rounds to 1.0f when
// converted to float, which would break the [-1, 1) output range of integer formats.
constexpr float kBelowOne = 0.99999994f;

constexpr uint64_t kMaxRiffSize = 0xFFFFFFFFull;

//==============================================================================
uint16_t loadU16 (const uint8_t* p) noexcept
{
    return static_cast<uint16_t> (p[0] | (p[1] << 8));
}

uint32_t loadU32 (const uint8_t* p) noexcept
{
    return static_cast<uint32_t> (p[0]) | (static_cast<uint32_t> (p[1]) << 8) | (static_cast<uint32_t> (p[2]) << 16)
           | (static_cast<uint32_t> (p[3]) << 24);
}

uint64_t loadU64 (const uint8_t* p) noexcept
{
    return static_cast<uint64_t> (loadU32 (p)) | (static_cast<uint64_t> (loadU32 (p + 4)) << 32);
}

void storeU16 (uint8_t* p, uint32_t v) noexcept
{
    p[0] = static_cast<uint8_t> (v & 0xFFu);
    p[1] = static_cast<uint8_t> ((v >> 8) & 0xFFu);
}

void storeU24 (uint8_t* p, uint32_t v) noexcept
{
    p[0] = static_cast<uint8_t> (v & 0xFFu);
    p[1] = static_cast<uint8_t> ((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t> ((v >> 16) & 0xFFu);
}

void storeU32 (uint8_t* p, uint32_t v) noexcept
{
    storeU16 (p, v & 0xFFFFu);
    storeU16 (p + 2, v >> 16);
}

bool idEquals (const uint8_t* p, const char* id) noexcept
{
    return std::memcmp (p, id, 4) == 0;
}

bool isPlausibleChunkId (const uint8_t* p) noexcept
{
    for (int i = 0; i < 4; ++i)
        if (p[i] < 0x20 || p[i] > 0x7E)
            return false;
    return true;
}

/** Chunk IDs that appear in real WAV files. Used only to break the tie when an
    odd-sized chunk may or may not be followed by its pad byte and both candidate
    offsets hold printable IDs (e.g. a missing pad before "data" whose size has a
    printable low byte would otherwise read as the plausible ID "ata?"). */
bool isKnownChunkId (const uint8_t* p) noexcept
{
    static constexpr const char* known[] = { "data", "fmt ", "fact", "LIST", "bext", "JUNK", "junk", "PAD ", "FLLR",
                                             "cue ", "smpl", "inst", "iXML", "id3 ", "ID3 ", "PEAK", "acid", "cart",
                                             "_PMX", "axml", "levl", "umid", "chna", "DISP", "plst", "labl", "ltxt" };
    for (const char* id : known)
        if (std::memcmp (p, id, 4) == 0)
            return true;
    return false;
}

std::string describeId (const uint8_t* p)
{
    std::string s = "'";
    for (int i = 0; i < 4; ++i)
        s += (p[i] >= 0x20 && p[i] <= 0x7E) ? static_cast<char> (p[i]) : '?';
    return s + "'";
}

std::string hex16 (uint32_t v)
{
    char buf[16];
    std::snprintf (buf, sizeof (buf), "0x%04X", static_cast<unsigned> (v));
    return buf;
}

//==============================================================================
struct WavFormat
{
    int numChannels = 0;
    uint32_t sampleRate = 0;
    int bytesPerSample = 0;
    SampleFormat format = SampleFormat::Float32;
};

/** Container bytes per sample for integer PCM. Normally ceil (bits / 8), but a
    non-EXTENSIBLE file may declare e.g. 24 bits while nBlockAlign says each sample
    sits in a 4-byte slot. Ignoring nBlockAlign then misframes every sample into
    garbage, so a per-channel slot that is larger than needed (and at most 4 bytes)
    wins; the valid bits are taken as left-justified, as in WAVE_FORMAT_EXTENSIBLE.
    Nonsense block aligns (smaller than needed, not a multiple of the channel
    count, a bit count written as bytes...) are ignored. */
int pcmContainerBytes (uint32_t bits, uint32_t channels, uint32_t blockAlign) noexcept
{
    const auto needed = (bits + 7) / 8;
    if (blockAlign != 0 && blockAlign % channels == 0)
    {
        const uint32_t slot = blockAlign / channels;
        if (slot > needed && slot <= 4)
            return static_cast<int> (slot);
    }
    return static_cast<int> (needed);
}

/** p holds min (chunkSize, kMaxFmtBytes) bytes of the fmt chunk body. */
bool parseFmt (const uint8_t* p, uint32_t chunkSize, WavFormat& fmt, std::string& message)
{
    if (chunkSize < 16)
    {
        message = "fmt chunk is too small (" + std::to_string (chunkSize) + " bytes, need at least 16)";
        return false;
    }

    uint32_t tag = loadU16 (p);
    const uint32_t channels = loadU16 (p + 2);
    const uint32_t rate = loadU32 (p + 4);
    const uint32_t blockAlign = loadU16 (p + 12);
    const uint32_t bits = loadU16 (p + 14); // container size for EXTENSIBLE

    if (tag == kTagExtensible)
    {
        if (chunkSize < kMaxFmtBytes)
        {
            message = "WAVE_FORMAT_EXTENSIBLE fmt chunk is too small (" + std::to_string (chunkSize) + " bytes, need 40)";
            return false;
        }
        // wValidBitsPerSample (p + 18) is not needed: samples are left-justified in
        // their container, so decoding the full container is exact. dwChannelMask
        // (p + 20) only names speaker positions; channels are returned in file order.
        if (! std::equal (kSubFormatGuidTail.begin(), kSubFormatGuidTail.end(), p + 26))
        {
            message = "unsupported WAVE_FORMAT_EXTENSIBLE sub-format GUID (only PCM and IEEE float are supported)";
            return false;
        }
        tag = loadU16 (p + 24);
    }

    if (channels == 0)
    {
        message = "fmt chunk declares 0 channels";
        return false;
    }
    if (channels > static_cast<uint32_t> (kMaxChannels))
    {
        message = std::to_string (channels) + " channels are not supported (maximum is " + std::to_string (kMaxChannels) + ")";
        return false;
    }
    if (rate == 0)
    {
        message = "fmt chunk declares a sample rate of 0 Hz";
        return false;
    }

    if (tag == kTagPcm)
    {
        // Non-EXTENSIBLE PCM may declare e.g. 20 bits in a 3-byte container; the
        // container is ceil (bits / 8) bytes (or the nBlockAlign slot, see
        // pcmContainerBytes) and the samples are left-justified.
        if (bits >= 1 && bits <= 8)
        {
            message = "8-bit PCM is not supported";
            return false;
        }
        if (bits < 9 || bits > 32)
        {
            message = "unsupported PCM bit depth (" + std::to_string (bits) + ")";
            return false;
        }
        fmt.bytesPerSample = pcmContainerBytes (bits, channels, blockAlign);
        fmt.format = fmt.bytesPerSample == 2 ? SampleFormat::Pcm16 : (fmt.bytesPerSample == 3 ? SampleFormat::Pcm24 : SampleFormat::Pcm32);
    }
    else if (tag == kTagFloat)
    {
        if (bits != 32 && bits != 64)
        {
            message = "unsupported IEEE float bit depth (" + std::to_string (bits) + ", expected 32 or 64)";
            return false;
        }
        fmt.bytesPerSample = static_cast<int> (bits / 8);
        fmt.format = bits == 32 ? SampleFormat::Float32 : SampleFormat::Float64;
    }
    else
    {
        message = "unsupported WAV format tag " + hex16 (tag) + " (only PCM, IEEE float and WAVE_FORMAT_EXTENSIBLE are supported)";
        return false;
    }

    fmt.numChannels = static_cast<int> (channels);
    fmt.sampleRate = rate;
    return true;
}

//==============================================================================
class FileReader
{
public:
    FileReader (std::ifstream& s, int64_t size) noexcept : stream (s), fileSize (size) {}

    /** Reads exactly numBytes at offset; false if that would pass the end of the file. */
    bool readAt (int64_t offset, uint8_t* dst, int64_t numBytes)
    {
        if (offset < 0 || numBytes < 0 || numBytes > fileSize - offset)
            return false;
        stream.clear();
        stream.seekg (static_cast<std::streamoff> (offset), std::ios::beg);
        stream.read (reinterpret_cast<char*> (dst), static_cast<std::streamsize> (numBytes));
        return stream.gcount() == static_cast<std::streamsize> (numBytes);
    }

private:
    std::ifstream& stream;
    const int64_t fileSize;
};

//==============================================================================
float decodeSample (const uint8_t* p, SampleFormat format) noexcept
{
    switch (format)
    {
        case SampleFormat::Pcm16:
            return static_cast<float> (static_cast<int16_t> (loadU16 (p))) * (1.0f / 32768.0f);
        case SampleFormat::Pcm24:
        {
            // Shift the 24-bit word to the top of an int32 and back to sign-extend it.
            const auto u = static_cast<uint32_t> (p[0]) | (static_cast<uint32_t> (p[1]) << 8) | (static_cast<uint32_t> (p[2]) << 16);
            return static_cast<float> (static_cast<int32_t> (u << 8) >> 8) * (1.0f / 8388608.0f);
        }
        case SampleFormat::Pcm32:
        {
            const double v = static_cast<double> (static_cast<int32_t> (loadU32 (p))) * (1.0 / 2147483648.0);
            return std::min (static_cast<float> (v), kBelowOne);
        }
        case SampleFormat::Float32:
        {
            // Passed through unscaled and unclipped (overs are legal in float files);
            // only non-finite values are replaced so NaN/Inf never reach the DSP chain.
            const float v = std::bit_cast<float> (loadU32 (p));
            return std::isfinite (v) ? v : 0.0f;
        }
        case SampleFormat::Float64:
        {
            const double v = std::bit_cast<double> (loadU64 (p));
            if (! std::isfinite (v))
                return 0.0f;
            constexpr double maxFloat = static_cast<double> (std::numeric_limits<float>::max());
            return static_cast<float> (std::clamp (v, -maxFloat, maxFloat));
        }
    }
    return 0.0f;
}

bool readWavImpl (const std::string& path, AudioFileData& out, std::string& error)
{
    const auto fail = [&] (const std::string& message)
    {
        error = path + ": " + message;
        return false;
    };

    // Checked before opening: a directory opens fine on Linux and seeking to
    // its end reports a bogus size (LLONG_MAX on ext4), which would give a
    // baffling "too short" message; on Windows the open itself fails.
    const auto fsPath = pathFromUtf8 (path);
    std::error_code ec;
    if (std::filesystem::is_directory (fsPath, ec))
        return fail ("is a directory");

    std::ifstream in (fsPath, std::ios::binary);
    if (! in)
    {
        error = "cannot open " + path;
        return false;
    }

    in.seekg (0, std::ios::end);
    const auto endPos = static_cast<int64_t> (in.tellg());
    if (endPos < 0)
        return fail ("cannot determine the file size");

    FileReader reader (in, endPos);
    const int64_t fileSize = endPos;

    uint8_t riff[12];
    if (! reader.readAt (0, riff, 12))
        return fail ("file is too short to be a WAV file (" + std::to_string (fileSize) + " bytes)");
    if (idEquals (riff, "RF64"))
        return fail ("RF64 files are not supported");
    if (idEquals (riff, "RIFX"))
        return fail ("big-endian RIFX files are not supported");
    if (! idEquals (riff, "RIFF"))
        return fail ("not a RIFF/WAVE file (header " + describeId (riff) + ")");
    if (! idEquals (riff + 8, "WAVE"))
        return fail ("RIFF file is not WAVE (form type " + describeId (riff + 8) + ")");

    // The RIFF size field is deliberately ignored: streaming writers leave it at 0
    // or 0xFFFFFFFF, so the physical end of the file bounds the chunk walk instead.
    WavFormat fmt;
    bool haveFmt = false, haveData = false;
    int64_t dataOffset = 0, dataBytes = 0;
    int64_t pos = 12;
    int chunksWalked = 0;

    while (pos <= fileSize - 8 && ! (haveFmt && haveData))
    {
        if (++chunksWalked > kMaxChunks)
            return fail ("more than " + std::to_string (kMaxChunks) + " chunks before the audio data (corrupt file?)");

        uint8_t header[8];
        if (! reader.readAt (pos, header, 8))
            break;

        const uint32_t size = loadU32 (header + 4);
        const int64_t body = pos + 8;
        const int64_t available = fileSize - body;

        if (idEquals (header, "fmt ") && ! haveFmt)
        {
            if (static_cast<int64_t> (size) > available)
                return fail ("truncated fmt chunk (" + std::to_string (size) + " bytes declared, " + std::to_string (available) + " present)");

            uint8_t fmtBytes[kMaxFmtBytes] {};
            const uint32_t n = std::min (size, kMaxFmtBytes);
            if (! reader.readAt (body, fmtBytes, n))
                return fail ("read error in fmt chunk");

            std::string message;
            if (! parseFmt (fmtBytes, size, fmt, message))
                return fail (message);
            haveFmt = true;
        }
        else if (idEquals (header, "data") && ! haveData)
        {
            haveData = true;
            dataOffset = body;
            dataBytes = std::min (static_cast<int64_t> (size), available); // clamp oversize / streaming sizes
        }

        // Odd-sized chunks are followed by a pad byte. Some writers forget it, so the
        // pad is skipped unless doing so lands on garbage (or on an unknown ID) while
        // the unpadded offset holds a plausible (or a known) chunk ID.
        int64_t next = body + static_cast<int64_t> (size);
        if ((size & 1u) != 0)
        {
            uint8_t padded[4] {}, unpadded[4] {};
            const bool paddedOk = reader.readAt (next + 1, padded, 4) && isPlausibleChunkId (padded);
            const bool unpaddedOk = reader.readAt (next, unpadded, 4) && isPlausibleChunkId (unpadded);
            const bool padMissing = unpaddedOk && (! paddedOk || (isKnownChunkId (unpadded) && ! isKnownChunkId (padded)));
            if (! padMissing)
                ++next;
        }
        pos = next;
    }

    if (! haveFmt)
        return fail ("missing fmt chunk");
    if (! haveData)
        return fail ("missing data chunk");

    const int64_t frameBytes = static_cast<int64_t> (fmt.numChannels) * fmt.bytesPerSample;
    const int64_t numFrames = dataBytes / frameBytes; // a trailing partial frame is ignored

    AudioFileData result;
    result.sampleRate = static_cast<double> (fmt.sampleRate);
    result.numChannels = fmt.numChannels;
    result.sourceFormat = fmt.format;
    // Sized per channel: assign (n, vector (frames)) would build an extra prototype
    // buffer and raise the peak memory by one channel for long files.
    result.channels.resize (static_cast<size_t> (fmt.numChannels));
    for (auto& ch : result.channels)
        ch.resize (static_cast<size_t> (numFrames));

    std::vector<uint8_t> buffer (static_cast<size_t> (std::min (numFrames, kBlockFrames) * frameBytes));
    for (int64_t frame = 0; frame < numFrames;)
    {
        const int64_t n = std::min (kBlockFrames, numFrames - frame);
        if (! reader.readAt (dataOffset + frame * frameBytes, buffer.data(), n * frameBytes))
            return fail ("read error in data chunk");

        const uint8_t* src = buffer.data();
        for (int64_t i = 0; i < n; ++i)
        {
            for (int c = 0; c < fmt.numChannels; ++c)
            {
                result.channels[static_cast<size_t> (c)][static_cast<size_t> (frame + i)] = decodeSample (src, fmt.format);
                src += fmt.bytesPerSample;
            }
        }
        frame += n;
    }

    out = std::move (result);
    return true;
}

//==============================================================================
/** Speaker mask for WAVE_FORMAT_EXTENSIBLE. Matches the engine's channel order
    (HeadphoneVirtualizer.h): 5.1 = FL FR FC LFE SL SR, 7.1 = FL FR FC LFE BL BR SL SR. */
uint32_t channelMaskFor (int numChannels) noexcept
{
    switch (numChannels)
    {
        case 1: return 0x004; // FC
        case 2: return 0x003; // FL FR
        case 3: return 0x007; // FL FR FC
        case 4: return 0x033; // FL FR BL BR (quad)
        case 5: return 0x607; // FL FR FC SL SR
        case 6: return 0x60F; // FL FR FC LFE SL SR
        case 7: return 0x70F; // FL FR FC LFE BC SL SR
        case 8: return 0x63F; // FL FR FC LFE BL BR SL SR
        default: return 0;
    }
}

/** TPDF dither source. splitmix64 yields 64 well-mixed bits per call, split into two
    independent 32-bit uniforms u1, u2 in [0, 1). d = u1 - u2 = (u1 - 0.5) + (0.5 - u2)
    is the sum of two uniform +-0.5 LSB variables: triangular on (-1, 1) LSB with zero
    mean and variance 1/6 LSB^2. Added before rounding, it makes the mean and variance
    of the quantisation error independent of the signal (no distortion or noise
    modulation); total error power is 1/6 + 1/12 = 1/4 LSB^2 (0.5 LSB rms). */
class TpdfDither
{
public:
    double next() noexcept
    {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        constexpr double scale = 1.0 / 4294967296.0;
        return static_cast<double> (z >> 32) * scale - static_cast<double> (z & 0xFFFFFFFFull) * scale;
    }

private:
    uint64_t state = 0x2545F4914F6CDD1Dull; // fixed seed: identical input -> identical file
};

/** Scales to the integer range (2^(bits-1) per unit, matching the reader), adds
    dither, rounds and clips. NaN becomes 0 (+ dither); +-Inf clips to full scale. */
int32_t quantise (float x, double scale, TpdfDither& dither) noexcept
{
    double v = static_cast<double> (x);
    if (std::isnan (v))
        v = 0.0;
    const double q = std::floor (v * scale + dither.next() + 0.5);
    return static_cast<int32_t> (std::clamp (q, -scale, scale - 1.0));
}

bool writeWavImpl (const std::string& path, const AudioFileData& data, SampleFormat format, std::string& error)
{
    const auto fail = [&] (const std::string& message)
    {
        error = path + ": " + message;
        return false;
    };

    int bytesPerSample = 0;
    switch (format)
    {
        case SampleFormat::Pcm16: bytesPerSample = 2; break;
        case SampleFormat::Pcm24: bytesPerSample = 3; break;
        case SampleFormat::Float32: bytesPerSample = 4; break;
        case SampleFormat::Pcm32:
        case SampleFormat::Float64:
        default: return fail ("unsupported output format (use Pcm16, Pcm24 or Float32)");
    }

    // channels.size() is authoritative; numChannels may be left at 0 or must agree.
    if (data.channels.empty())
        return fail ("no channels to write");
    if (data.channels.size() > static_cast<size_t> (kMaxChannels))
        return fail (std::to_string (data.channels.size()) + " channels are not supported (maximum is " + std::to_string (kMaxChannels) + ")");
    const int numChannels = static_cast<int> (data.channels.size());
    if (data.numChannels != 0 && data.numChannels != numChannels)
        return fail ("numChannels (" + std::to_string (data.numChannels) + ") does not match the number of channel buffers ("
                     + std::to_string (numChannels) + ")");

    const size_t numFrames = data.channels[0].size();
    for (size_t c = 1; c < data.channels.size(); ++c)
        if (data.channels[c].size() != numFrames)
            return fail ("channel " + std::to_string (c) + " has " + std::to_string (data.channels[c].size()) + " frames, expected "
                         + std::to_string (numFrames));

    if (! (data.sampleRate >= 1.0 && data.sampleRate <= 4294967295.0)) // also rejects NaN
        return fail ("invalid sample rate");
    const auto sampleRate = static_cast<uint32_t> (std::llround (data.sampleRate));

    // Checked before any multiplication so a (theoretical) huge frame count cannot
    // wrap the 64-bit size arithmetic below into a small, "valid" header.
    if (numFrames > static_cast<size_t> (kMaxRiffSize))
        return fail ("audio is too long for a RIFF/WAVE file (4 GiB limit; RF64 is not supported)");

    const bool isFloat = format == SampleFormat::Float32;
    const bool extensible = numChannels > 2;
    const uint32_t blockAlign = static_cast<uint32_t> (numChannels * bytesPerSample);
    const uint64_t byteRate = static_cast<uint64_t> (sampleRate) * blockAlign;
    if (byteRate > kMaxRiffSize)
        return fail ("sample rate is too high for the WAV header");

    // PCM: 16-byte WAVEFORMAT. Float: 18-byte WAVEFORMATEX (cbSize = 0) plus a fact
    // chunk, as required for non-PCM formats. > 2 channels: 40-byte EXTENSIBLE.
    const uint32_t fmtSize = extensible ? 40u : (isFloat ? 18u : 16u);
    const bool writeFact = isFloat;
    const uint64_t dataSize = static_cast<uint64_t> (numFrames) * blockAlign;
    const uint64_t padSize = dataSize & 1u;
    const uint64_t headerBytes = 4u + (8u + static_cast<uint64_t> (fmtSize)) + (writeFact ? 12u : 0u) + 8u;
    const uint64_t riffSize = headerBytes + dataSize + padSize;
    if (riffSize > kMaxRiffSize)
        return fail ("audio is too long for a RIFF/WAVE file (4 GiB limit; RF64 is not supported)");

    std::vector<uint8_t> header (static_cast<size_t> (12 + 8 + fmtSize + (writeFact ? 12 : 0) + 8), 0);
    uint8_t* h = header.data();
    const auto putId = [&h] (const char* id)
    {
        std::memcpy (h, id, 4);
        h += 4;
    };
    const auto putU16 = [&h] (uint32_t v)
    {
        storeU16 (h, v);
        h += 2;
    };
    const auto putU32 = [&h] (uint32_t v)
    {
        storeU32 (h, v);
        h += 4;
    };

    const uint32_t bits = static_cast<uint32_t> (bytesPerSample * 8);
    const uint32_t formatTag = isFloat ? kTagFloat : kTagPcm;

    putId ("RIFF");
    putU32 (static_cast<uint32_t> (riffSize));
    putId ("WAVE");
    putId ("fmt ");
    putU32 (fmtSize);
    putU16 (extensible ? kTagExtensible : formatTag);
    putU16 (static_cast<uint32_t> (numChannels));
    putU32 (sampleRate);
    putU32 (static_cast<uint32_t> (byteRate));
    putU16 (blockAlign);
    putU16 (bits);
    if (extensible)
    {
        putU16 (22);   // cbSize
        putU16 (bits); // wValidBitsPerSample
        putU32 (channelMaskFor (numChannels));
        putU16 (formatTag);
        std::memcpy (h, kSubFormatGuidTail.data(), kSubFormatGuidTail.size());
        h += kSubFormatGuidTail.size();
    }
    else if (isFloat)
    {
        putU16 (0); // cbSize
    }
    if (writeFact)
    {
        putId ("fact");
        putU32 (4);
        putU32 (static_cast<uint32_t> (std::min<uint64_t> (numFrames, kMaxRiffSize)));
    }
    putId ("data");
    putU32 (static_cast<uint32_t> (dataSize));

    std::ofstream f (pathFromUtf8 (path), std::ios::binary | std::ios::trunc);
    if (! f)
    {
        error = "cannot write " + path;
        return false;
    }
    f.write (reinterpret_cast<const char*> (header.data()), static_cast<std::streamsize> (header.size()));

    TpdfDither dither;
    const double scale = format == SampleFormat::Pcm16 ? 32768.0 : 8388608.0;
    const auto blockFrames = static_cast<size_t> (kBlockFrames);
    std::vector<uint8_t> buffer (std::min (numFrames, blockFrames) * blockAlign);

    for (size_t frame = 0; frame < numFrames && f;)
    {
        const size_t n = std::min (blockFrames, numFrames - frame);
        uint8_t* dst = buffer.data();
        for (size_t i = 0; i < n; ++i)
        {
            for (int c = 0; c < numChannels; ++c)
            {
                const float x = data.channels[static_cast<size_t> (c)][frame + i];
                if (isFloat)
                {
                    // Bit exact for finite values; NaN -> 0, +-Inf -> +-1 (full scale).
                    const float y = std::isfinite (x) ? x : (std::isnan (x) ? 0.0f : (x > 0.0f ? 1.0f : -1.0f));
                    storeU32 (dst, std::bit_cast<uint32_t> (y));
                }
                else if (format == SampleFormat::Pcm16)
                {
                    storeU16 (dst, static_cast<uint32_t> (quantise (x, scale, dither)) & 0xFFFFu);
                }
                else
                {
                    storeU24 (dst, static_cast<uint32_t> (quantise (x, scale, dither)) & 0xFFFFFFu);
                }
                dst += bytesPerSample;
            }
        }
        f.write (reinterpret_cast<const char*> (buffer.data()), static_cast<std::streamsize> (n * blockAlign));
        frame += n;
    }

    if (padSize != 0)
        f.put ('\0');

    f.flush();
    if (! f)
    {
        f.close();
        // Do not leave a truncated file behind - but only delete regular files: the
        // path may be a device or FIFO (e.g. /dev/full), which must never be unlinked.
        // Both calls take the UTF-8 path (a narrow string is ANSI on Windows).
        std::error_code ec;
        const auto fsPath = pathFromUtf8 (path);
        if (std::filesystem::is_regular_file (fsPath, ec))
            std::filesystem::remove (fsPath, ec);
        return fail ("write error (disk full?)");
    }
    return true;
}
} // namespace

//==============================================================================
bool readWav (const std::string& path, AudioFileData& out, std::string& error)
{
    try
    {
        return readWavImpl (path, out, error);
    }
    catch (const std::bad_alloc&)
    {
        error = path + ": out of memory while reading";
    }
    catch (const std::exception& e)
    {
        error = path + ": " + e.what();
    }
    return false;
}

bool writeWav (const std::string& path, const AudioFileData& data, SampleFormat format, std::string& error)
{
    try
    {
        return writeWavImpl (path, data, format, error);
    }
    catch (const std::bad_alloc&)
    {
        error = path + ": out of memory while writing";
    }
    catch (const std::exception& e)
    {
        error = path + ": " + e.what();
    }
    return false;
}
} // namespace flub::io
