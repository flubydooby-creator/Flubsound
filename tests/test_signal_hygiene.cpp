// Signal hygiene (docs/11 E10): aliasing of the oversampled saturator per
// latency profile and sample rate, the residual-path DC blockers, DC-free
// harmonics from the bass harmonics generator and air exciter, and the
// capture FIFO's input sanitiser.
//
// Alias metric: worstAliasDbc (tools/flubsound-cli/Analysis.h, shared with
// `flubsound-cli quality --rate`): a sine on an odd bin of a 65536-point
// Blackman-Harris analysis, the largest line in 20 Hz .. 20 kHz that is not
// within 4 bins of one of its harmonics, dB re the fundamental. Rows:
//
//   * the rate-aware saturator table (Oversampler::forProfile) at Warmth 100
//     (Tape, 9 dB drive), -6 dBFS 1 / 5 / 7 / 10 kHz sines, 44.1 / 48 / 96 /
//     192 kHz: <= -70 dBc (Quality / Balanced), <= -60 dBc (Low Latency),
//     in the latency the fixed 2x designs had
//   * the same through the whole chain at 44.1 kHz (Music, Warmth 100)
//   * extreme settings (docs/11 E10 Phase 2: first-order ADAA everywhere,
//     8x in Quality up to 48 kHz): 24 dB of drive on -6 dBFS tones and 9 dB
//     on 0 dBFS tones, every type, <= -70 dBc in Quality at every rate and
//     in every profile from 88.2 kHz
//   * KNOWN_GAP rows for Balanced / Low Latency at 24 dB and 44.1 / 48 kHz,
//     which 4x with ADAA cannot bring below -70 dBc (8x would double the
//     curve's CPU there), pinned so they cannot get worse unnoticed
#include "TestFramework.h"
#include "TestSignals.h"

#include "Analysis.h"
#include "CliOptions.h"
#include "OfflineRenderer.h"

// DriftCompensatedFifo.cpp itself is compiled into test_drift_fifo.cpp's
// translation unit (it needs no app headers); this file uses the class.
#include "../app/Source/engine/DriftCompensatedFifo.h"

#include "flub/common/Math.h"
#include "flub/dsp/BassEngine.h"
#include "flub/dsp/ClarityEnhancer.h"
#include "flub/dsp/Oversampler.h"
#include "flub/dsp/Saturator.h"
#include "flub/engine/Parameters.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr int kAliasN = 65536;

void measured (const std::string& name, double value, const char* unit)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.2f", value);
    std::cout << "    measured " << name << " = " << buf << " " << unit << "\n";
}

/** Worst alias over the 1 / 5 / 7 / 10 kHz rows of a mono saturator. */
double saturatorWorstAliasDbc (const Oversampler::Design& design, double fs, SaturationType type, float driveDb, float amplitude)
{
    double worst = -200.0;
    for (double hz : { 1000.0, 5000.0, 7000.0, 10000.0 })
    {
        const int bin = cli::aliasToneBin (hz, fs, kAliasN);
        Saturator sat;
        sat.setOversampling (design);
        sat.prepare ({ fs, 512, 1 });
        SaturatorParams p;
        p.type = type;
        p.driveDb = driveDb;
        sat.setParams (p);
        sat.reset();
        const int n = static_cast<int> (fs / 4) + kAliasN;
        Planar buf (1, n);
        const double f0 = bin * fs / kAliasN;
        for (int i = 0; i < n; ++i)
            buf.ch[0][static_cast<size_t> (i)] = static_cast<float> (amplitude * std::sin (kTwoPi * f0 * i / fs));
        processInBlocks (sat, buf, 512);
        worst = std::max (worst, cli::worstAliasDbc (buf.ch[0].data() + (n - kAliasN), kAliasN, fs, bin));
    }
    return worst;
}

std::vector<float> asymmetricTwoTone (double f1, double f2, double fs, int n, double amplitude)
{
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t> (i)] = static_cast<float> (amplitude * std::sin (kTwoPi * f1 * i / fs) + amplitude * std::cos (kTwoPi * f2 * i / fs));
    return x;
}

double dcOfSecondHalf (const std::vector<float>& y)
{
    const int half = static_cast<int> (y.size() / 2);
    return cli::dcDbfs (y.data() + half, static_cast<int> (y.size()) - half);
}

const char* profileName (Oversampler::Profile p)
{
    return p == Oversampler::Profile::Quality ? "Quality" : (p == Oversampler::Profile::Balanced ? "Balanced" : "Low Latency");
}
} // namespace

// =============================================================================
// Rate-aware oversampling (step 3)
// =============================================================================
TEST_CASE ("Signal hygiene: the rate-aware saturator table keeps the 2x designs' latency (32 / 16 samples) at every rate")
{
    // Every chain latency (docs/01 5.1, test_engine.cpp) is therefore
    // unchanged. Factor: 8x in Quality below 88.2 kHz, else 4x below
    // 176.4 kHz and 2x from there; every row runs the ADAA curves.
    for (const double fs : { 8000.0, 16000.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
        for (const auto profile : { Oversampler::Profile::Quality, Oversampler::Profile::Balanced, Oversampler::Profile::LowLatency })
        {
            const auto design = Oversampler::forProfile (profile, fs);
            Saturator sat;
            sat.setOversampling (design);
            sat.prepare ({ fs, 256, 2 });
            CHECK (sat.latencySamples() == (profile == Oversampler::Profile::Quality ? 32 : 16));
            const int factor = fs >= 176400.0 ? 2 : (profile == Oversampler::Profile::Quality && fs < 88200.0 ? 8 : 4);
            CHECK (design.factor == factor);
            CHECK (design.adaa);
        }
}

TEST_CASE ("Signal hygiene: Warmth 100 through the saturator - worst alias of 1 / 5 / 7 / 10 kHz <= -70 dBc (Quality, Balanced) / -60 dBc (Low Latency) at 44.1 / 48 / 96 / 192 kHz")
{
    // Before (the fixed 2x designs: Quality 2x High, Balanced / Low Latency
    // 2x Low; same stimulus): 44.1 kHz -32.2 / -32.3 dBc, 48 kHz -41.2 /
    // -41.1, 96 kHz -83.1 / -83.1, 192 kHz -148.9 / -118.1 dBc. Below
    // 176.4 kHz Balanced and Low Latency share one design (16 samples).
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto profile : { Oversampler::Profile::Quality, Oversampler::Profile::Balanced })
        {
            const double worst = saturatorWorstAliasDbc (Oversampler::forProfile (profile, fs), fs, SaturationType::Tape, 9.0f, 0.5f);
            measured (std::string ("saturator alias, Warmth 100, ") + profileName (profile) + " at " + std::to_string (static_cast<int> (fs)) + " Hz", worst, "dBc");
            CHECK_LE (worst, -70.0);
        }
    for (const double fs : { 44100.0, 192000.0 })
        CHECK (Oversampler::forProfile (Oversampler::Profile::LowLatency, fs) == Oversampler::forProfile (Oversampler::Profile::Balanced, fs));
}

TEST_CASE ("Signal hygiene: Tube and Digital at 9 dB drive stay <= -70 dBc at 44.1 kHz with the 16-sample design")
{
    // Before (2x Low): Tube -55.4, Digital -86.0 dBc (a -6 dBFS sine stays
    // below Digital's knee at 9 dB: only its cubic term works).
    const auto design = Oversampler::forProfile (Oversampler::Profile::Balanced, 44100.0);
    for (const auto type : { SaturationType::Tube, SaturationType::Digital })
    {
        const double worst = saturatorWorstAliasDbc (design, 44100.0, type, 9.0f, 0.5f);
        measured (std::string ("saturator alias, 9 dB drive, ") + (type == SaturationType::Tube ? "Tube" : "Digital") + ", Balanced at 44100 Hz", worst, "dBc");
        CHECK_LE (worst, -70.0);
    }
}

TEST_CASE ("Signal hygiene: Warmth 100 through the chain at 44.1 kHz - the 10 kHz alias is <= -70 dBc in every profile")
{
    // Music, Warmth 100 (sat.drive 9, bass harmonics, bass boost), the
    // worst tone of the saturator rows. Before: -36.6 (Quality), -36.6
    // (Balanced, Low Latency) dBc; after: -91.0 / -74.7 / -74.7.
    constexpr double fs = 44100.0;
    const int bin = cli::aliasToneBin (10000.0, fs, kAliasN);
    const double f0 = bin * fs / kAliasN;
    const int n = static_cast<int> (fs / 2) + kAliasN;
    io::AudioFileData in;
    in.sampleRate = fs;
    in.numChannels = 2;
    in.channels.assign (2, std::vector<float> (static_cast<size_t> (n)));
    for (int i = 0; i < n; ++i)
        in.channels[0][static_cast<size_t> (i)] = in.channels[1][static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (kTwoPi * f0 * i / fs));

    for (const auto profile : { param::LatencyProfileValue::Quality, param::LatencyProfileValue::Balanced, param::LatencyProfileValue::LowLatency })
    {
        cli::RenderOptions o;
        o.mode = param::ModeValue::Music;
        o.macros.push_back ({ "5", 100.0f });
        o.profile = profile;
        cli::ResolvedParameters p;
        std::string error;
        REQUIRE (cli::buildParameters (o, p, error));
        std::vector<std::vector<float>> out;
        int latency = 0;
        REQUIRE (cli::renderPass (in, p.values, 512, out, latency, error));
        const double dbc = cli::worstAliasDbc (out[0].data() + (n - kAliasN), kAliasN, fs, bin);
        measured (std::string ("chain alias, Warmth 100, 10 kHz, profile ") + std::to_string (static_cast<int> (profile)), dbc, "dBc");
        CHECK_LE (dbc, -70.0);
    }
}

TEST_CASE ("Signal hygiene: extreme settings in Quality - 24 dB of drive and 0 dBFS at 9 dB, every type, <= -70 dBc at 44.1 / 48 / 96 / 192 kHz")
{
    // docs/11 E10 Phase 2 (ADAA, and 8x up to 48 kHz). Before (4x / 2x without
    // ADAA), 44.1 kHz: Tape 24 dB -28.2, Tube -38.0, Digital -35.6, Tape at
    // 0 dBFS -45.9 dBc; 192 kHz: -46.1 / -75.0 / -59.8 / -94.4 dBc.
    struct Row
    {
        SaturationType type;
        float drive, amplitude;
    };
    for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto& r : { Row { SaturationType::Tape, 24.0f, 0.5f }, Row { SaturationType::Tube, 24.0f, 0.5f },
                               Row { SaturationType::Digital, 24.0f, 0.5f }, Row { SaturationType::Tape, 9.0f, 1.0f } })
        {
            const double worst = saturatorWorstAliasDbc (Oversampler::forProfile (Oversampler::Profile::Quality, fs), fs, r.type, r.drive, r.amplitude);
            if (fs == 44100.0 || fs == 192000.0)
                measured ("saturator alias, Quality, type " + std::to_string (static_cast<int> (r.type)) + ", " + std::to_string (static_cast<int> (r.drive)) + " dB, "
                              + (r.amplitude < 1.0f ? "-6" : "0") + " dBFS at " + std::to_string (static_cast<int> (fs)) + " Hz",
                          worst, "dBc");
            CHECK_LE (worst, -70.0);
        }
}

TEST_CASE ("Signal hygiene: extreme settings in Balanced / Low Latency - <= -70 dBc from 88.2 kHz, 0 dBFS at 9 dB <= -70 dBc at 44.1 kHz")
{
    // One design serves both profiles (16 samples). Before: 96 kHz Tape 24 dB
    // -46.1, Digital -59.7 dBc; 44.1 kHz Tape at 0 dBFS -45.8 dBc (the
    // KnownGap row it closes).
    for (const double fs : { 88200.0, 96000.0, 192000.0 })
        for (const auto type : { SaturationType::Tape, SaturationType::Tube, SaturationType::Digital })
            CHECK_LE (saturatorWorstAliasDbc (Oversampler::forProfile (Oversampler::Profile::Balanced, fs), fs, type, 24.0f, 0.5f), -70.0);
    const double fullScale = saturatorWorstAliasDbc (Oversampler::forProfile (Oversampler::Profile::Balanced, 44100.0), 44100.0, SaturationType::Tape, 9.0f, 1.0f);
    measured ("saturator alias, Balanced, Tape, 9 dB, 0 dBFS at 44100 Hz", fullScale, "dBc");
    CHECK_LE (fullScale, -70.0);
}

TEST_CASE ("Signal hygiene KnownGap: 24 dB of drive at 44.1 / 48 kHz in Balanced / Low Latency still aliases above -70 dBc (4x with ADAA)")
{
    // KNOWN_GAP: target <= -70 dBc (Balanced), <= -60 dBc (Low Latency) per
    // docs/11 E10. Pinned at today's value + 0.5 dB, so a change can only
    // improve them. 8x (Quality's design) meets them at twice the curve's
    // CPU. Before ADAA (2x Low, then 4x): Tape -15.0 -> -28.0 -> -54.1,
    // Digital -18.5 -> -35.5 -> -62.6, Tube -37.9 -> -64.3 dBc at 44.1 kHz.
    struct Row
    {
        const char* name;
        double fs;
        SaturationType type;
        double recordedDbc;
    };
    for (const auto& r : { Row { "Tape, 44.1 kHz", 44100.0, SaturationType::Tape, -54.1 }, Row { "Digital, 44.1 kHz", 44100.0, SaturationType::Digital, -62.6 },
                           Row { "Tube, 44.1 kHz", 44100.0, SaturationType::Tube, -64.3 }, Row { "Tape, 48 kHz", 48000.0, SaturationType::Tape, -59.3 } })
    {
        const double worst = saturatorWorstAliasDbc (Oversampler::forProfile (Oversampler::Profile::Balanced, r.fs), r.fs, r.type, 24.0f, 0.5f);
        measured (std::string ("saturator alias KnownGap, Balanced, 24 dB, -6 dBFS, ") + r.name, worst, "dBc");
        CHECK_LE (worst, r.recordedDbc + 0.5);
    }
}

// =============================================================================
// Residual-path DC blockers and DC-free harmonics (step 1)
// =============================================================================
TEST_CASE ("Signal hygiene: the saturator's residual-path DC blocker - 100 + 200 Hz at 12 / 24 dB leaves <= -120 dBFS DC in every type")
{
    // 0.35 sin (100 Hz) + 0.35 cos (200 Hz): no DC in, an asymmetric
    // waveform (the E10 DC stimulus). Before: Tape -29.8 / -36.2, Digital
    // -28.2 / -35.9 dBFS at 12 / 24 dB; Tube's 10 Hz blocker already held
    // it at -150 dBFS.
    constexpr double fs = 48000.0;
    for (const auto type : { SaturationType::Tape, SaturationType::Tube, SaturationType::Digital })
        for (const float drive : { 12.0f, 24.0f })
        {
            Saturator sat;
            sat.setOversampling (Oversampler::forProfile (Oversampler::Profile::Balanced, fs));
            sat.prepare ({ fs, 512, 1 });
            SaturatorParams p;
            p.type = type;
            p.driveDb = drive;
            sat.setParams (p);
            sat.reset();
            Planar buf (1, static_cast<int> (4 * fs));
            buf.ch[0] = asymmetricTwoTone (100.0, 200.0, fs, buf.numSamples(), 0.35);
            buf.rebind();
            processInBlocks (sat, buf, 512);
            const double dc = dcOfSecondHalf (buf.ch[0]);
            measured ("saturator DC, type " + std::to_string (static_cast<int> (type)) + ", drive " + std::to_string (static_cast<int> (drive)), dc, "dBFS");
            CHECK_LE (dc, -120.0);
        }
}

TEST_CASE ("Signal hygiene: the bass harmonics generator and the air exciter add no DC (post high-passes)")
{
    // Both shape a band-limited copy and high-pass the result (HP2 at the
    // harmonics cutoff; HP4 at 7 kHz), so even harmonics' DC never reaches
    // the output: no residual-path blocker is needed there.
    constexpr double fs = 48000.0;
    const int n = static_cast<int> (4 * fs);
    {
        BassEngine bass;
        bass.prepare ({ fs, 512, 1 });
        BassEngineParams p;
        p.harmonicsAmount = 1.0f;
        p.harmonicsCharacter = 0.0f; // even: T2 / T4 carry DC
        p.subsonicHz = 0.0f;
        bass.setParams (p);
        bass.reset();
        Planar buf (1, n);
        buf.ch[0] = asymmetricTwoTone (50.0, 100.0, fs, n, 0.35);
        buf.rebind();
        processInBlocks (bass, buf, 512);
        const double dc = dcOfSecondHalf (buf.ch[0]);
        measured ("bass harmonics DC (amount 1, even)", dc, "dBFS");
        CHECK_LE (dc, -100.0);
        CHECK_GE (bass.getDistortionDb(), -20.0); // the generator is working (harmonics re output)
    }
    {
        ClarityEnhancer clarity;
        clarity.prepare ({ fs, 512, 1 });
        ClarityParams p;
        p.air = 1.0f;
        clarity.setParams (p);
        clarity.reset();
        Planar buf (1, n);
        buf.ch[0] = asymmetricTwoTone (4000.0, 5000.0, fs, n, 0.25);
        buf.rebind();
        processInBlocks (clarity, buf, 512);
        const double dc = dcOfSecondHalf (buf.ch[0]);
        measured ("air exciter DC (air 1)", dc, "dBFS");
        CHECK_LE (dc, -100.0);
    }
}

// =============================================================================
// Capture FIFO sanitising (step 2)
// =============================================================================
TEST_CASE ("Signal hygiene: DriftCompensatedFifo::push mutes and counts NaN / Inf / beyond +24 dBFS samples; everything else is untouched")
{
    // Two FIFOs, same packets and pulls: one gets bad samples, the other the
    // same stream with exactly those samples zeroed. The outputs must be
    // identical (bit for bit) and finite, and only the bad samples counted.
    constexpr double fs = 48000.0;
    constexpr int packet = 480, block = 128, packets = 20;
    const auto tone = sine (997.0, fs, packet * packets, 0.5f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    const auto run = [&] (int srcChannels, bool corrupt, uint64_t& corruptCount) {
        app::DriftCompensatedFifo fifo;
        fifo.prepare (2, fs, fs, block);
        std::vector<float> out;
        std::vector<float> l (block), r (block);
        float* dest[2] = { l.data(), r.data() };
        int pulled = 0;
        for (int k = 0; k < packets; ++k)
        {
            std::vector<float> inter (static_cast<size_t> (packet * srcChannels));
            for (int i = 0; i < packet; ++i)
                for (int c = 0; c < srcChannels; ++c)
                    inter[static_cast<size_t> (i * srcChannels + c)] = tone[static_cast<size_t> (k * packet + i)] * (c == 3 && srcChannels == 8 ? 0.0f : 1.0f);
            if (k == 3)
            {
                // Same-layout pushes are scanned; a surround push is sanitised
                // per source sample before the downmix (the LFE, channel 3 of
                // 7.1, is dropped by the downmix and never read).
                const int bad[] = { 10 * srcChannels, 11 * srcChannels + 1, 200 * srcChannels };
                const float values[] = { nan, -inf, 1.0e30f };
                for (int j = 0; j < 3; ++j)
                    inter[static_cast<size_t> (bad[j])] = corrupt ? values[j] : 0.0f;
                if (srcChannels == 8)
                    inter[static_cast<size_t> (300 * srcChannels + 3)] = corrupt ? nan : 0.0f; // LFE: not read, not counted
            }
            fifo.push (inter.data(), packet, srcChannels);
            for (; pulled + block <= (k + 1) * packet; pulled += block)
            {
                fifo.pull (dest, 2, block, false);
                out.insert (out.end(), l.begin(), l.end());
                out.insert (out.end(), r.begin(), r.end());
            }
        }
        corruptCount = fifo.getStats().corruptSamples;
        return out;
    };

    for (const int srcChannels : { 2, 8 })
    {
        uint64_t bad = 0, none = 0;
        const auto corrupted = run (srcChannels, true, bad);
        const auto clean = run (srcChannels, false, none);
        CHECK (bad == 3u);
        CHECK (none == 0u);
        REQUIRE (corrupted.size() == clean.size());
        bool identical = true, finite = true;
        for (size_t i = 0; i < clean.size(); ++i)
        {
            identical = identical && (corrupted[i] == clean[i]);
            finite = finite && std::isfinite (corrupted[i]);
        }
        CHECK (identical);
        CHECK (finite);
        CHECK_GE (peakAbs (clean.data(), static_cast<int> (clean.size())), 0.1); // it streamed
    }
}
