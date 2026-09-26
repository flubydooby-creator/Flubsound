// App-level tests: analyser and meter data paths (R4.2).
//
// The views are used without a window: their data side (push / update /
// advance) is plain message-thread code, and the values they display are
// read back through small read-only accessors. The engine-driven cases run
// the real path: AudioEngineHost::renderOffline -> ProcessingChain (MeterBus
// atomics, AnalyzerTaps rings) -> MeterSnapshot / AnalyzerFeed -> view.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "ui/AnalyzerFeed.h"
#include "ui/LevelMeters.h"
#include "ui/LoudnessPanel.h"
#include "ui/MeterSnapshot.h"
#include "ui/SpectrumAnalyzer.h"
#include "ui/WaveformHistory.h"

#include "flub/engine/Parameters.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace flub::app;

namespace
{
constexpr double kPi = juce::MathConstants<double>::pi;

std::vector<float> sine (double hz, float amplitude, int numSamples, double sampleRate)
{
    std::vector<float> v (static_cast<size_t> (numSamples));
    for (int i = 0; i < numSamples; ++i)
        v[static_cast<size_t> (i)] = amplitude * static_cast<float> (std::sin (2.0 * kPi * hz * i / sampleRate));
    return v;
}

/** Stereo test programme for strip 1 (Music); the other strips are unfed. */
class StereoSource final : public StripSignalSource
{
public:
    enum class Kind
    {
        Mono,      // L = R: sine
        AntiPhase, // R = -L: sine
        Noise      // independent white noise per channel
    };

    StereoSource (Kind k, float amp) : kind (k), amplitude (amp) {}

    bool renderStrip (int strip, const flub::AudioBlock& block) override
    {
        if (strip != 1)
            return false;
        for (int i = 0; i < block.numSamples; ++i, ++n)
        {
            float l = 0.0f, r = 0.0f;
            if (kind == Kind::Noise)
            {
                l = amplitude * (2.0f * uniform (seedL) - 1.0f);
                r = amplitude * (2.0f * uniform (seedR) - 1.0f);
            }
            else
            {
                l = amplitude * static_cast<float> (std::sin (2.0 * kPi * 1000.0 * static_cast<double> (n) / 48000.0));
                r = kind == Kind::Mono ? l : -l;
            }
            block.channel (0)[i] = l;
            block.channel (1)[i] = r;
        }
        return true;
    }

private:
    static float uniform (uint32_t& state) noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float> (state >> 8) / 16777216.0f;
    }

    Kind kind;
    float amplitude;
    int64_t n = 0;
    uint32_t seedL = 0x12345678u, seedR = 0x9e3779b9u;
};

/** Host at 48 kHz / 512 with strip 1 bypassed (BypassAll on both banks), so
    its output is its input and the output meters see the test signal. */
void prepareBypassedHost (AudioEngineHost& host)
{
    host.prepareOffline (48000.0, 512);
    auto& store = host.getMixEngine().params (1);
    store.set (flub::param::Bank::A, flub::param::BypassAll, 1.0f);
    store.set (flub::param::Bank::B, flub::param::BypassAll, 1.0f);
}

/** Renders `seconds` in 512-sample blocks; after each block one display
    frame: MeterSnapshot::read -> LevelMeters / LoudnessPanel::update. */
ui::MeterSnapshot renderFrames (AudioEngineHost& host, StripSignalSource& source, double seconds, ui::LevelMeters& meters,
                                ui::LoudnessPanel& loudness)
{
    ui::MeterSnapshot snapshot;
    const int blocks = static_cast<int> (seconds * 48000.0 / 512.0);
    for (int b = 0; b < blocks; ++b)
    {
        host.renderOffline (source, 512);
        snapshot.read (host.getMixEngine().chain (1).meters());
        snapshot.active = host.isStripActive (1);
        meters.update (snapshot, 512.0 / 48000.0);
        loudness.update (snapshot, 512.0 / 48000.0);
    }
    return snapshot;
}
} // namespace

// =============================================================================
// SpectrumAnalyzer calibration
// =============================================================================
TEST_CASE ("App: SpectrumAnalyzer reads a 1 kHz sine at its dBFS level in the 1/6-octave band (0 and -20 dBFS)")
{
    for (const double rate : { 48000.0, 44100.0 })
    {
        for (const float levelDb : { 0.0f, -20.0f })
        {
            ui::SpectrumAnalyzer analyzer;
            analyzer.setSampleRate (rate);
            const auto x = sine (1000.0, std::pow (10.0f, levelDb / 20.0f), ui::SpectrumAnalyzer::kFftSize, rate);
            analyzer.push (true, x.data(), static_cast<int> (x.size()));
            analyzer.advance (1.0 / 60.0);

            CHECK_NEAR (analyzer.getBandLevelDb (true, 1000.0), levelDb, 0.5);
            // One octave away the band is empty (Hann side lobes only).
            CHECK_LE (analyzer.getBandLevelDb (true, 2000.0), levelDb - 60.0f);
            CHECK_LE (analyzer.getBandLevelDb (true, 500.0), levelDb - 60.0f);
            // The pre stream received nothing.
            CHECK_LE (analyzer.getBandLevelDb (false, 1000.0), -100.0f);
        }
    }
}

TEST_CASE ("App: SpectrumAnalyzer is calibrated end to end through the chain's analyser taps and AnalyzerFeed")
{
    AudioEngineHost host;
    host.prepareOffline (48000.0, 512);

    ui::SpectrumAnalyzer analyzer;
    analyzer.setSampleRate (host.getSampleRate());
    ui::AnalyzerFeed feed;
    feed.addSink ([&analyzer] (ui::AnalyzerFeed::Stream stream, const float* samples, int n)
                  { analyzer.push (stream == ui::AnalyzerFeed::Stream::Post, samples, n); });

    // The pre tap is the strip input's mid, (L + R) / 2: a -20 dBFS sine on
    // both channels reads -20 in its band. One display frame per 1024 samples.
    StereoSource source (StereoSource::Kind::Mono, 0.1f);
    for (int frame = 0; frame < 12; ++frame)
    {
        host.renderOffline (source, 1024);
        feed.pull (host.getMixEngine().chain (1).taps());
        analyzer.advance (1024.0 / 48000.0);
    }
    CHECK_NEAR (analyzer.getBandLevelDb (false, 1000.0), -20.0, 0.5);
    CHECK (analyzer.getBandLevelDb (true, 1000.0) > -60.0f); // the processed stream arrives too
}

TEST_CASE ("App: AnalyzerFeed hands every sink the same blocks and trims a stale backlog")
{
    flub::AnalyzerTaps taps;
    ui::AnalyzerFeed feed;
    std::vector<float> a, b;
    int preSamples = 0;
    feed.addSink ([&] (ui::AnalyzerFeed::Stream s, const float* x, int n)
                  {
                      if (s == ui::AnalyzerFeed::Stream::Post)
                          a.insert (a.end(), x, x + n);
                      else
                          preSamples += n;
                  });
    feed.addSink ([&] (ui::AnalyzerFeed::Stream s, const float* x, int n)
                  {
                      if (s == ui::AnalyzerFeed::Stream::Post)
                          b.insert (b.end(), x, x + n);
                  });

    std::vector<float> ramp (3000);
    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float> (i);
    taps.post.push (ramp.data(), ramp.size());
    taps.pre.push (ramp.data(), 100);
    feed.pull (taps);
    CHECK (a == ramp);
    CHECK (b == ramp);
    CHECK (preSamples == 100);

    // A long stall: 20000 queued samples. The view jumps to "now": only the
    // newest 8192 are delivered, ending with the newest sample.
    a.clear();
    std::vector<float> backlog (20000);
    for (size_t i = 0; i < backlog.size(); ++i)
        backlog[i] = static_cast<float> (i);
    taps.post.push (backlog.data(), backlog.size());
    feed.pull (taps);
    CHECK (a.size() == 8192);
    CHECK (! a.empty() && a.back() == 19999.0f);
    CHECK (taps.post.available() == 0);

    taps.post.push (ramp.data(), 10);
    ui::AnalyzerFeed::discard (taps);
    CHECK (taps.post.available() == 0);
}

// =============================================================================
// Level meters: RMS and peak
// =============================================================================
TEST_CASE ("App: LevelMeters show a sine's RMS 3.01 dB below its peak on the input and output bars")
{
    AudioEngineHost host;
    prepareBypassedHost (host);
    ui::LevelMeters meters;
    ui::LoudnessPanel loudness;

    StereoSource source (StereoSource::Kind::Mono, 0.5f); // -6.02 dBFS peak
    const auto snapshot = renderFrames (host, source, 2.0, meters, loudness);
    CHECK (snapshot.active);

    for (const bool output : { false, true })
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const float peak = meters.getDisplayedPeakDb (output, ch);
            const float rms = meters.getDisplayedRmsDb (output, ch);
            CHECK_NEAR (peak, -6.02, 0.05);
            CHECK_NEAR (rms, peak - 3.01, 0.05);
        }
    }

    // Silence: the bars fall away (peak at 12 dB/s, RMS behind it).
    StereoSource quiet (StereoSource::Kind::Mono, 0.0f);
    renderFrames (host, quiet, 1.0, meters, loudness);
    CHECK_LE (meters.getDisplayedPeakDb (true, 0), -17.0f);
    CHECK_LE (meters.getDisplayedRmsDb (true, 0), meters.getDisplayedPeakDb (true, 0));
}

// =============================================================================
// Correlation (LoudnessPanel's stereo meter)
// =============================================================================
TEST_CASE ("App: correlation meter reads +1 for mono, -1 for anti-phase and about 0 for uncorrelated noise")
{
    struct Case
    {
        StereoSource::Kind kind;
        double expected, tolerance;
    };
    const Case cases[] = {
        { StereoSource::Kind::Mono, 1.0, 0.01 },
        { StereoSource::Kind::AntiPhase, -1.0, 0.01 },
        { StereoSource::Kind::Noise, 0.0, 0.1 },
    };

    for (const auto& c : cases)
    {
        AudioEngineHost host;
        prepareBypassedHost (host);
        ui::LevelMeters meters;
        ui::LoudnessPanel loudness;

        StereoSource source (c.kind, 0.3f);
        const auto snapshot = renderFrames (host, source, 2.0, meters, loudness);
        CHECK_NEAR (snapshot.correlation, c.expected, c.tolerance);
        CHECK_NEAR (loudness.getDisplayedCorrelation(), c.expected, c.tolerance);
    }
}

// =============================================================================
// Waveform history
// =============================================================================
TEST_CASE ("App: WaveformHistory decimates the output tap into min / max columns tagged with the short-term loudness")
{
    ui::WaveformHistory history;
    history.setSampleRate (48000.0);
    const int perColumn = history.getSamplesPerColumn();
    CHECK (perColumn == 480); // 12 s over 1200 columns

    // 10 columns of a 0.5 sine (1 kHz: every column holds whole periods,
    // including the exact peaks) at about -14 LUFS, then 3 silent columns at -30.
    history.setLoudness (-14.0f);
    history.setLoudness (-14.03f); // recorded as is (only the header readout has a 0.05 LU dead band)
    const auto x = sine (1000.0, 0.5f, 10 * perColumn, 48000.0);
    history.push (x.data(), static_cast<int> (x.size()));
    history.setLoudness (-30.0f);
    const std::vector<float> silence (static_cast<size_t> (3 * perColumn), 0.0f);
    history.push (silence.data(), static_cast<int> (silence.size()));

    for (int age = 0; age < 3; ++age)
    {
        CHECK (history.getColumn (age).lo == 0.0f);
        CHECK (history.getColumn (age).hi == 0.0f);
        CHECK (history.getColumn (age).lufs == -30.0f);
    }
    for (int age = 3; age < 13; ++age)
    {
        CHECK_NEAR (history.getColumn (age).lo, -0.5, 1.0e-6);
        CHECK_NEAR (history.getColumn (age).hi, 0.5, 1.0e-6);
        CHECK (history.getColumn (age).lufs == -14.03f);
    }
    CHECK (history.getColumn (13).lufs == -160.0f); // never written

    // A partial column is not committed; a non-finite loudness reads as silence.
    history.setLoudness (std::nanf (""));
    history.push (x.data(), perColumn - 1);
    CHECK (history.getColumn (0).hi == 0.0f);
    history.push (x.data(), 1);
    CHECK_NEAR (history.getColumn (0).hi, 0.5, 1.0e-6);
    CHECK (history.getColumn (0).lufs == -160.0f);

    history.reset();
    CHECK (history.getColumn (0).hi == 0.0f);
    CHECK (history.getColumn (5).lufs == -160.0f);
}

TEST_CASE ("App: LoudnessPanel shows the chain's short-term loudness for a -20 dBFS stereo sine")
{
    AudioEngineHost host;
    prepareBypassedHost (host);
    ui::LevelMeters meters;
    ui::LoudnessPanel loudness;

    // BS.1770: a 1 kHz sine of -20 dBFS peak on one channel reads -23.01 LUFS
    // (mean square -3.01 dB; the K-weighting's ~+0.69 dB at 1 kHz cancels the
    // -0.691 offset). The same sine on both channels sums to -20.0 LUFS.
    StereoSource source (StereoSource::Kind::Mono, 0.1f);
    renderFrames (host, source, 3.5, meters, loudness); // the 3 s window is full
    CHECK_NEAR (loudness.getDisplayedShortTermLufs(), -20.0, 0.2);
}
