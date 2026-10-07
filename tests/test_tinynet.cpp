// Tests for the TinyNet inference runtime (flub/neural/TinyNet.h): layer math
// against numpy reference outputs exported by tools/neural/train_voice_cleanup.py
// (tests/neural_reference_data.h), model file parsing including corrupt and
// hostile files, allocation-free deterministic inference, and the shipped voice
// cleanup model's front end and outputs against the same numpy reference.
#include "TestFramework.h"
#include "neural_reference_data.h"

#include "flub/neural/TinyNet.h"
#include "flub/neural/VoiceCleanupRunner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
std::vector<uint8_t> bytesOf (const unsigned char* data, size_t size)
{
    return std::vector<uint8_t> (data, data + size);
}

void putU32 (std::vector<uint8_t>& b, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b[at + static_cast<size_t> (i)] = static_cast<uint8_t> ((v >> (8 * i)) & 0xFFu);
}

uint32_t getU32 (const std::vector<uint8_t>& b, size_t at)
{
    return static_cast<uint32_t> (b[at]) | (static_cast<uint32_t> (b[at + 1]) << 8) | (static_cast<uint32_t> (b[at + 2]) << 16)
         | (static_cast<uint32_t> (b[at + 3]) << 24);
}

/** Re-signs the payload after a deliberate edit, so the parser's own checks (not the CRC) must refuse it. */
void fixCrc (std::vector<uint8_t>& b)
{
    putU32 (b, 20, nn::crc32 (b.data() + 64, b.size() - 64));
}

std::string loadError (const std::vector<uint8_t>& b)
{
    nn::TinyNet net;
    std::string error;
    if (net.load (b.data(), b.size(), error))
        return {};
    CHECK (! net.isLoaded());
    CHECK (! error.empty());
    return error.empty() ? std::string ("(no reason)") : error;
}

/** Runs a reference net frame by frame (reset at resetAt) and returns the largest
    deviation from the numpy outputs. */
float referenceDeviation (const unsigned char* model, size_t size, const float* input, const float* expected, int frames, int resetAt)
{
    nn::TinyNet net;
    std::string error;
    REQUIRE (net.load (model, size, error));
    REQUIRE (net.numInputs() == 5);
    REQUIRE (net.numOutputs() == 3);
    for (int o = 0; o < 3; ++o)
        REQUIRE (net.outputSize (o) == neuralref::kNetOutputSizes[o]);
    float worst = 0.0f;
    for (int t = 0; t < frames; ++t)
    {
        if (t == resetAt)
            net.reset();
        net.run (input + t * 5);
        int k = 0;
        for (int o = 0; o < 3; ++o)
            for (int i = 0; i < net.outputSize (o); ++i, ++k)
                worst = std::max (worst, std::abs (net.output (o)[i] - expected[t * 9 + k]));
    }
    return worst;
}
} // namespace

TEST_CASE ("TinyNet: Dense, Conv1D and GRU layers with every activation match the numpy reference (float32 and int8 weights)")
{
    const float f32 = referenceDeviation (neuralref::kNetF32, sizeof (neuralref::kNetF32), neuralref::kNetF32Input, neuralref::kNetF32Output,
                                          neuralref::kNetF32Frames, neuralref::kNetF32ResetAt);
    const float i8 = referenceDeviation (neuralref::kNetInt8, sizeof (neuralref::kNetInt8), neuralref::kNetInt8Input, neuralref::kNetInt8Output,
                                         neuralref::kNetInt8Frames, neuralref::kNetInt8ResetAt);
    std::printf ("    largest deviation from numpy: float32 weights %.3g, int8 weights %.3g\n", f32, i8);
    CHECK_LE (f32, 2.0e-6f);
    CHECK_LE (i8, 2.0e-6f);
}

TEST_CASE ("TinyNet: the header and outputs of a valid file are reported; run() before load() is a no-op")
{
    nn::TinyNet empty;
    CHECK (! empty.isLoaded());
    float in[5] = {};
    empty.run (in); // must not crash
    CHECK (empty.output (0) == nullptr);
    CHECK (empty.outputSize (0) == 0);

    nn::TinyNet net;
    std::string error;
    REQUIRE (net.load (neuralref::kNetF32, sizeof (neuralref::kNetF32), error));
    CHECK (net.info().formatVersion == 1u);
    CHECK (net.info().modelVersion == 3u);
    CHECK (net.info().numLayers == 7u);
    CHECK (net.info().name == "reference F32");
    CHECK (net.output (3) == nullptr);
    CHECK (net.output (-1) == nullptr);
}

TEST_CASE ("TinyNet: truncated, corrupt and hostile model files are refused with a reason")
{
    const auto good = bytesOf (neuralref::kNetF32, sizeof (neuralref::kNetF32));
    const auto good8 = bytesOf (neuralref::kNetInt8, sizeof (neuralref::kNetInt8));
    REQUIRE (loadError (good).empty());
    REQUIRE (loadError (good8).empty());

    // Every truncation, and a byte too many.
    for (size_t n = 0; n < good.size(); ++n)
        CHECK (! loadError (std::vector<uint8_t> (good.begin(), good.begin() + static_cast<std::ptrdiff_t> (n))).empty());
    auto longer = good;
    longer.push_back (0);
    CHECK (loadError (longer).find ("payload") != std::string::npos);

    // Every single-byte change in the payload: the CRC refuses it.
    for (size_t i = 64; i < good.size(); ++i)
    {
        auto b = good;
        b[i] ^= 0x5Au;
        CHECK (loadError (b).find ("checksum") != std::string::npos);
    }

    struct Edit
    {
        const char* what;
        size_t at;
        uint32_t value;
        bool int8;
        const char* expect; // a word in the reason
        bool resign;        // fix the CRC so the semantic check is what refuses it
    };
    const Edit edits[] = {
        { "magic", 0, 0x4B4C4F46u, false, "FLUBTNET", false },
        { "format version", 8, 2u, false, "version", false },
        { "header size", 12, 80u, false, "header", false },
        { "layer count 0", 44, 0u, false, "layer count", false },
        { "layer count 33", 44, 33u, false, "layer count", false },
        { "input size 0", 40, 0u, false, "input size", false },
        { "output count 0", 48, 0u, false, "output count", false },
        { "parameter count", 52, 0u, false, "parameters", false },
        { "flags", 56, 1u, false, "flags", false },
        { "layer type", 64, 9u, false, "layer type", true },
        { "activation", 68, 7u, false, "activation", true },
        { "output size 0", 72, 0u, false, "output size", true },
        { "output size huge", 72, 5000u, false, "output size", true },
        { "kernel", 76, 99u, false, "kernel", true },
        { "input count", 80, 0u, false, "input count", true },
        { "forward reference", 84, 5u, false, "earlier tensor", true },
        { "weight format", 88, 2u, false, "weight format", true },
        { "NaN weight", 92, 0x7FC00000u, false, "finite", true },
        { "negative int8 scale", 92, 0xBF800000u, true, "weights", true },
    };
    for (const auto& e : edits)
    {
        auto b = e.int8 ? good8 : good;
        putU32 (b, e.at, e.value);
        if (e.resign)
            fixCrc (b);
        const std::string why = loadError (b);
        std::printf ("    %-20s -> %s\n", e.what, why.c_str());
        CHECK (why.find (e.expect) != std::string::npos);
    }

    // An int8 value of -128 (the writer's range is symmetric): layer 0 has 6 rows, so the
    // int8 block starts after 6 scales.
    {
        auto b = good8;
        b[92 + 6 * 4] = 0x80u;
        fixCrc (b);
        CHECK (! loadError (b).empty());
    }
    // A GRU with an activation field.
    {
        auto b = good;
        // find layer 2 (the first GRU): walk the records like the parser
        nn::TinyNet probe;
        std::string err;
        REQUIRE (probe.load (b.data(), b.size(), err));
        size_t at = 64;
        for (int l = 0; l < 2; ++l)
        {
            const uint32_t type = getU32 (b, at), out = getU32 (b, at + 8), kernel = getU32 (b, at + 12), nIn = getU32 (b, at + 16);
            uint32_t in = 0;
            for (uint32_t i = 0; i < nIn; ++i)
            {
                const uint32_t id = getU32 (b, at + 20 + 4 * i);
                in += id == 0 ? 5u : (id == 1 ? 6u : 5u);
            }
            const size_t rows = out, cols = static_cast<size_t> (type == 2 ? kernel * in : in);
            at += 20 + 4 * nIn + 4 + 4 * (rows * cols + rows);
        }
        REQUIRE (getU32 (b, at) == 3u); // GRU
        putU32 (b, at + 4, 2u);
        fixCrc (b);
        CHECK (loadError (b).find ("GRU") != std::string::npos);
    }
    // Trailing bytes inside a re-signed payload.
    {
        auto b = good;
        b.insert (b.end(), { 0, 0, 0, 0 });
        putU32 (b, 16, static_cast<uint32_t> (b.size() - 64));
        fixCrc (b);
        CHECK (loadError (b).find ("unexpected bytes") != std::string::npos);
    }
    // Null data.
    {
        nn::TinyNet net;
        std::string error;
        CHECK (! net.load (nullptr, 100, error));
    }
}

TEST_CASE ("TinyNet: run() and reset() allocate nothing and give the same bits on every run")
{
    nn::TinyNet net;
    std::string error;
    REQUIRE (net.load (voiceCleanupModelData(), voiceCleanupModelSize(), error));
    std::vector<float> in (static_cast<size_t> (net.numInputs()));
    std::vector<float> first, second;
    for (int pass = 0; pass < 2; ++pass)
    {
        auto& out = pass == 0 ? first : second;
        out.reserve (200 * 23);
        uint32_t s = 7;
        AllocationGuard guard;
        net.reset();
        for (int t = 0; t < 200; ++t)
        {
            for (auto& v : in)
            {
                s = 1664525u * s + 1013904223u;
                v = static_cast<float> (s >> 8) / 16777216.0f * 8.0f - 6.0f;
            }
            net.run (in.data());
            for (int o = 0; o < net.numOutputs(); ++o)
                out.insert (out.end(), net.output (o), net.output (o) + net.outputSize (o));
        }
        CHECK (guard.allocations() == 0);
    }
    REQUIRE (first.size() == second.size());
    CHECK (std::memcmp (first.data(), second.data(), first.size() * sizeof (float)) == 0);
}

TEST_CASE ("VoiceCleanup: the embedded model is presets/neural/voice-cleanup.fnn byte for byte and fits the front end")
{
#ifdef FLUB_NEURAL_MODEL_FILE
    std::ifstream f (FLUB_NEURAL_MODEL_FILE, std::ios::binary);
    REQUIRE (f.good());
    const std::vector<char> file ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    REQUIRE (file.size() == voiceCleanupModelSize());
    CHECK (std::memcmp (file.data(), voiceCleanupModelData(), file.size()) == 0);
#endif
    VoiceCleanupRunner runner;
    REQUIRE (runner.isValid());
    const auto& info = runner.getModelInfo();
    std::printf ("    model: %s, version %u, %u parameters, %zu bytes\n", info.name.c_str(), info.modelVersion, info.parameterCount,
                 voiceCleanupModelSize());
    CHECK (info.featureSet == 1u);
    CHECK (info.sampleRate == 48000u);
    CHECK (info.frameSize == 240u);
    const ModelDescription d = runner.describe();
    CHECK (d.frameSize == 240);
    CHECK (d.numControls == 22);
    CHECK (d.controlKind == ControlKind::BandGains);
    CHECK (d.fftSize == 512);
    CHECK (d.sampleRate == 48000.0);
}

TEST_CASE ("VoiceCleanup: features, band gains and voice activity match the numpy reference on a test signal")
{
    // The signal the export step used (see neural_reference_data.h).
    std::vector<float> x (static_cast<size_t> (neuralref::kSignalSamples));
    uint32_t s = 12345;
    for (int i = 0; i < neuralref::kSignalSamples; ++i)
    {
        s = 1664525u * s + 1013904223u;
        const double lcg = static_cast<double> (s >> 8) / 16777216.0 - 0.5;
        const double t = i / 48000.0;
        const double pi = 3.14159265358979323846;
        x[static_cast<size_t> (i)] = static_cast<float> (0.3 * std::sin (2 * pi * 150.0 * t) * (0.5 + 0.5 * std::sin (2 * pi * 3.0 * t))
                                                         + 0.1 * std::sin (2 * pi * (300.0 * t + 4000.0 * t * t)) + 0.05 * lcg);
    }
    VoiceCleanupRunner runner;
    REQUIRE (runner.isValid());
    runner.reset();
    float worstBand = 0.0f, worstVoicing = 0.0f, worstGain = 0.0f, worstVad = 0.0f;
    std::vector<float> gains (22);
    REQUIRE (neuralref::kSignalFrames * 240 == neuralref::kSignalSamples);
    for (int k = 0; k < neuralref::kSignalFrames; ++k)
    {
        runner.processFrame (x.data() + k * 240, gains.data());
        const float* f = runner.getLastFeatures();
        for (int b = 0; b < 22; ++b)
            worstBand = std::max (worstBand, std::abs (f[b] - neuralref::kSignalFeatures[k * 23 + b]));
        worstVoicing = std::max (worstVoicing, std::abs (f[22] - neuralref::kSignalFeatures[k * 23 + 22]));
        for (int b = 0; b < 22; ++b)
            worstGain = std::max (worstGain, std::abs (gains[static_cast<size_t> (b)] - neuralref::kSignalGains[k * 22 + b]));
        worstVad = std::max (worstVad, std::abs (runner.getLastVoiceActivity() - neuralref::kSignalVad[k]));
    }
    std::printf ("    largest deviation from numpy over %d frames: band log-energy %.3g, voicing %.3g, gain %.3g, voice activity %.3g\n",
                 neuralref::kSignalFrames, worstBand, worstVoicing, worstGain, worstVad);
    CHECK_LE (worstBand, 1.0e-4f);   // log10 units
    CHECK_LE (worstVoicing, 1.0e-4f);
    CHECK_LE (worstGain, 1.0e-4f);
    CHECK_LE (worstVad, 1.0e-4f);
}
