// App-level tests: the analyser's optional views (docs/06 §6.4): the hover
// readout's note naming, the difference trace, sharper lows (the decimated
// long analysis), the spectrogram image, stereo width (and the feed's side
// stream), freeze, the piano keys and the persisted options.
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "ui/AnalyzerFeed.h"
#include "ui/AnalyzerPanel.h"
#include "ui/EqCurveEditor.h"
#include "ui/Spectrogram.h"
#include "ui/SpectrumAnalyzer.h"

#include "flub/engine/MeterBus.h"
#include "flub/engine/Parameters.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace flub::app;

namespace
{
constexpr double kPi = juce::MathConstants<double>::pi;

std::vector<float> tones (std::initializer_list<double> hz, float amplitude, int numSamples, double sampleRate, int64_t start = 0)
{
    std::vector<float> v (static_cast<size_t> (numSamples), 0.0f);
    int k = 0;
    for (const double f : hz)
    {
        const double phase = 0.7 * k++;
        for (int i = 0; i < numSamples; ++i)
            v[static_cast<size_t> (i)] += amplitude * static_cast<float> (std::sin (2.0 * kPi * f * static_cast<double> (start + i) / sampleRate + phase));
    }
    return v;
}

/** Feeds `seconds` of `signal (start, n)` to the post stream (and pre if asked) in 1024-sample frames. */
template <typename Signal>
void run (ui::SpectrumAnalyzer& a, double seconds, Signal signal, bool pre = false)
{
    const int frames = static_cast<int> (seconds * 48000.0 / 1024.0);
    for (int f = 0; f < frames; ++f)
    {
        const auto x = signal (static_cast<int64_t> (f) * 1024, 1024);
        a.push (true, x.data(), 1024);
        if (pre)
            a.push (false, x.data(), 1024);
        a.advance (1024.0 / 48000.0);
    }
}

/** Highest band level within +-3 % of hz (the display point nearest the tone). */
float peakNear (const ui::SpectrumAnalyzer& a, double hz)
{
    float best = -200.0f;
    for (double f = hz * 0.97; f <= hz * 1.03; f *= 1.002)
        best = juce::jmax (best, a.getBandLevelDb (true, f));
    return best;
}

float lowestBetween (const ui::SpectrumAnalyzer& a, double lo, double hi)
{
    float worst = 200.0f;
    for (double f = lo; f <= hi; f *= 1.002)
        worst = juce::jmin (worst, a.getBandLevelDb (true, f));
    return worst;
}
} // namespace

// =============================================================================
// 1. Hover readout: frequency -> note + cents
// =============================================================================
TEST_CASE ("App: analyser readout names the nearest note with cents (A4 = 440 Hz)")
{
    using A = ui::SpectrumAnalyzer;
    CHECK (A::noteName (440.0) == "A4 +0c");
    CHECK (A::noteName (55.0) == "A1 +0c");
    CHECK (A::noteName (55.4) == "A1 +13c"); // 1200 log2 (55.4 / 55) = 12.5 -> 13
    CHECK (A::noteName (261.6256) == "C4 +0c");
    CHECK (A::noteName (277.18) == "C#4 +0c");
    CHECK (A::noteName (30.0) == "B0 -49c");
    CHECK (A::noteName (450.0) == "A4 +39c");
    CHECK (A::noteName (16744.0) == "C10 +0c");
    CHECK (A::noteName (4.0).isEmpty());

    CHECK (A::frequencyText (55.4) == "55.4 Hz");
    CHECK (A::frequencyText (440.0) == "440 Hz");
    CHECK (A::frequencyText (1250.0) == "1.25 kHz");
    CHECK (A::frequencyText (12500.0) == "12.5 kHz");
    CHECK (A::describeFrequency (55.4) == juce::String (juce::CharPointer_UTF8 ("A1 +13c \xc2\xb7 55.4 Hz")));
}

TEST_CASE ("App: EQ editor shows the hover readout over the plot only and hides it on exit")
{
    ui::SpectrumAnalyzer analyzer;
    ui::EqCurveEditor editor (analyzer, [] { return static_cast<flub::param::ParameterStore*> (nullptr); });
    analyzer.setBounds (0, 0, 900, 300);
    editor.setBounds (0, 0, 900, 300);
    const auto plot = analyzer.getPlotArea();

    const float x = analyzer.xForFrequency (110.0);
    editor.showReadoutAt ({ x, plot.getCentreY() });
    REQUIRE (editor.isReadoutVisible());
    const auto lines = editor.getReadoutLines();
    REQUIRE (lines.size() == 1); // no data yet: no level line
    CHECK (lines[0].startsWith ("A2 "));
    CHECK (lines[0].endsWith ("Hz"));

    editor.showReadoutAt ({ plot.getX() - 10.0f, plot.getCentreY() }); // over the dB labels
    CHECK (! editor.isReadoutVisible());
    editor.showReadoutAt ({ x, plot.getCentreY() });
    editor.hideReadout();
    CHECK (! editor.isReadoutVisible());
    CHECK (editor.getReadoutLines().isEmpty());
}

// =============================================================================
// 2. Difference trace
// =============================================================================
TEST_CASE ("App: the difference trace is post minus pre, smoothed and gated")
{
    using A = ui::SpectrumAnalyzer;
    constexpr int n = 40;
    std::vector<float> pre (n), post (n), out (n);

    for (int i = 0; i < n; ++i)
    {
        pre[static_cast<size_t> (i)] = -30.0f - static_cast<float> (i);
        post[static_cast<size_t> (i)] = pre[static_cast<size_t> (i)] + 6.0f;
    }
    A::computeDifference (pre.data(), post.data(), out.data(), n);
    for (const float d : out)
        CHECK_NEAR (d, 6.0, 1.0e-4);

    // Unsmoothed: exactly post - pre; smoothed: the mean over +-halfWidth.
    for (int i = 0; i < n; ++i)
        post[static_cast<size_t> (i)] = pre[static_cast<size_t> (i)] + static_cast<float> (i);
    A::computeDifference (pre.data(), post.data(), out.data(), n, 0);
    for (int i = 0; i < n; ++i)
        CHECK_NEAR (out[static_cast<size_t> (i)], i, 1.0e-4);
    A::computeDifference (pre.data(), post.data(), out.data(), n, 3);
    CHECK_NEAR (out[20], 20.0, 1.0e-4);
    CHECK_NEAR (out[0], 1.5, 1.0e-4); // edge: points 0..3

    // Below the gate on both sides: no change is shown.
    std::fill (pre.begin(), pre.end(), -140.0f);
    std::fill (post.begin(), post.end(), -130.0f);
    A::computeDifference (pre.data(), post.data(), out.data(), n);
    for (const float d : out)
        CHECK_NEAR (d, 0.0, 1.0e-6);
}

TEST_CASE ("App: the analyser's difference view reads +6 dB for a 1 kHz sine 6 dB louder at the output")
{
    ui::SpectrumAnalyzer a;
    a.setSampleRate (48000.0);
    a.setDifferenceEnabled (true);
    for (int f = 0; f < 30; ++f)
    {
        const auto in = tones ({ 1000.0 }, 0.05f, 1024, 48000.0, static_cast<int64_t> (f) * 1024);
        auto out = in;
        for (auto& s : out)
            s *= 2.0f; // +6.02 dB
        a.push (false, in.data(), 1024);
        a.push (true, out.data(), 1024);
        a.advance (1024.0 / 48000.0);
    }
    CHECK_NEAR (a.getDifferenceDb (1000.0), 6.02, 0.3);
}

// =============================================================================
// 3. Sharper lows
// =============================================================================
TEST_CASE ("App: Sharper lows reads 55 and 80 Hz sines at their level on the analyser's scale")
{
    // The scale is the one of the whole display (docs/06 §6.4): a band
    // density referred to the 1/6-octave bandwidth at 1 kHz, so pink noise
    // reads flat and a 1 kHz sine reads its dBFS level. Below the crossover
    // the long analysis resolves a tone in 1.5 of its bins: the tone reads
    // its level + 10 log10 (B1k / that bandwidth) (+16.0 dB at 48 kHz).
    ui::SpectrumAnalyzer a;
    a.setSampleRate (48000.0);
    a.setSharpLowsEnabled (true);
    const double b1k = 1000.0 * (std::pow (2.0, 1.0 / 12.0) - std::pow (2.0, -1.0 / 12.0));
    const double offset = 10.0 * std::log10 (b1k / a.getLowResolutionBandwidthHz());
    CHECK_NEAR (offset, 16.0, 0.1);

    for (const double hz : { 55.0, 80.0 })
    {
        for (const float levelDb : { -20.0f, -40.0f })
        {
            a.reset();
            const float amp = std::pow (10.0f, levelDb / 20.0f);
            run (a, 0.7, [&] (int64_t start, int n) { return tones ({ hz }, amp, n, 48000.0, start); });
            std::printf ("    %.0f Hz sine at %.0f dBFS reads %.2f dB (expected %.2f)\n", hz, static_cast<double> (levelDb), static_cast<double> (peakNear (a, hz)), levelDb + offset);
            CHECK_NEAR (peakNear (a, hz), levelDb + offset, 0.5);
            // An octave away nothing but leakage.
            CHECK_LE (a.getBandLevelDb (true, hz * 2.0), levelDb + offset - 50.0f);
        }
    }

    // The calibration above the crossover is untouched: 1 kHz still reads its level.
    a.reset();
    run (a, 0.7, [] (int64_t start, int n) { return tones ({ 1000.0 }, 0.1f, n, 48000.0, start); });
    CHECK_NEAR (a.getBandLevelDb (true, 1000.0), -20.0, 0.5);
}

TEST_CASE ("App: Sharper lows resolves two sines 1/6 octave apart near 60 Hz; the main analysis does not")
{
    const double f1 = 60.0 / std::pow (2.0, 1.0 / 12.0), f2 = 60.0 * std::pow (2.0, 1.0 / 12.0); // 56.6 / 63.6 Hz
    auto signal = [&] (int64_t start, int n) { return tones ({ f1, f2 }, 0.1f, n, 48000.0, start); };

    for (const bool sharp : { true, false })
    {
        ui::SpectrumAnalyzer a;
        a.setSampleRate (48000.0);
        a.setSharpLowsEnabled (sharp);
        run (a, 0.7, signal);
        const float p1 = peakNear (a, f1), p2 = peakNear (a, f2);
        const float dip = lowestBetween (a, f1 * 1.01, f2 / 1.01);
        const float depth = juce::jmin (p1, p2) - dip;
        std::printf ("    two tones near 60 Hz, %s: peaks %.2f / %.2f dB, dip %.2f dB deep\n", sharp ? "sharper lows" : "main analysis", p1, p2, depth);
        if (sharp)
        {
            CHECK_GE (depth, 6.0f); // two peaks
            CHECK_NEAR (p1, p2, 1.0);
        }
        else
        {
            CHECK_LE (depth, 1.5f); // one lump (11.7 Hz bins)
        }
    }
}

TEST_CASE ("App: Sharper lows keeps noise on the same scale across the crossover")
{
    // White noise: the long and main analyses read the same density, so the
    // traces meet at the crossover (averaged over 60..200 Hz and many hops).
    auto mean = [] (bool sharp)
    {
        ui::SpectrumAnalyzer a;
        a.setSampleRate (48000.0);
        a.setSharpLowsEnabled (sharp);
        uint32_t seed = 0x2468aceu;
        double sum = 0.0;
        int count = 0;
        for (int f = 0; f < 80; ++f)
        {
            std::vector<float> x (1024);
            for (auto& s : x)
            {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                s = 0.2f * (static_cast<float> (seed >> 8) / 8388608.0f - 1.0f);
            }
            a.push (true, x.data(), 1024);
            a.advance (1024.0 / 48000.0);
            if (f >= 30)
                for (double hz = 60.0; hz <= 200.0; hz *= 1.02, ++count)
                    sum += a.getBandLevelDb (true, hz);
        }
        return sum / count;
    };
    // dB averaging of a chi-square spread reads ~2.5 dB low for single bins
    // (the long analysis) and less for averaged bands: allow for that bias.
    const double longMean = mean (true), shortMean = mean (false);
    std::printf ("    white noise 60..200 Hz, mean reading: sharper lows %.2f dB, main analysis %.2f dB\n", longMean, shortMean);
    CHECK_NEAR (longMean, shortMean, 2.5);
}

// =============================================================================
// 4. Spectrogram
// =============================================================================
TEST_CASE ("App: the spectrogram writes rows into one fixed image and scrolls as a ring")
{
    ui::Spectrogram s (420, 64);
    s.setColours (juce::Colours::black, juce::Colours::cyan, juce::Colours::white);
    s.clear();
    const auto* pixels = s.getImage().getPixelData().get();
    CHECK (s.getNewestRow() == -1);

    std::vector<float> row (420, -84.0f);
    row[100] = 0.0f; // top of the scale
    for (int i = 0; i < 100; ++i)
    {
        s.writeRow (row.data(), 420, -84.0f, 0.0f);
        CHECK (s.getImage().getPixelData().get() == pixels); // no reallocation
    }
    CHECK (s.getRowsWritten() == 100);
    CHECK (s.getNewestRow() == (64 - 100 % 64) % 64); // the ring index moves down by one per row
    CHECK (s.getImage().getPixelAt (100, s.getNewestRow()) == s.colourFor (1.0f));
    CHECK (s.getImage().getPixelAt (5, s.getNewestRow()) == s.colourFor (0.0f));

    // The analyser writes one row per post hop when the view is on, none when off.
    ui::SpectrumAnalyzer a;
    a.setSampleRate (48000.0);
    run (a, 0.2, [] (int64_t start, int n) { return tones ({ 1000.0 }, 0.1f, n, 48000.0, start); });
    CHECK (a.getSpectrogram().getRowsWritten() == 0);
    a.setSpectrogramEnabled (true);
    const auto* analyserPixels = a.getSpectrogram().getImage().getPixelData().get();
    run (a, 0.2, [] (int64_t start, int n) { return tones ({ 1000.0 }, 0.1f, n, 48000.0, start); });
    CHECK (a.getSpectrogram().getRowsWritten() == static_cast<int64_t> (0.2 * 48000.0 / 1024.0));
    CHECK (a.getSpectrogram().getImage().getPixelData().get() == analyserPixels);
}

// =============================================================================
// 5. Stereo width
// =============================================================================
TEST_CASE ("App: stereo width is S / (M + S) per band; the feed delivers the side stream")
{
    using A = ui::SpectrumAnalyzer;
    CHECK_NEAR (A::widthFromLevels (-20.0f, -20.0f), 0.5, 1.0e-5);
    CHECK_NEAR (A::widthFromLevels (-20.0f, -140.0f), 0.0, 1.0e-6);
    CHECK_NEAR (A::widthFromLevels (-140.0f, -20.0f), 1.0, 1.0e-6);
    CHECK_NEAR (A::widthFromLevels (-130.0f, -130.0f), 0.0, 1.0e-6); // gated

    // Through the analyser: mid only, equal mid and side, side only (at 1 kHz).
    for (const float sideGain : { 0.0f, 1.0f, -1.0f })
    {
        ui::SpectrumAnalyzer a;
        a.setSampleRate (48000.0);
        a.setWidthEnabled (true);
        for (int f = 0; f < 8; ++f)
        {
            const auto x = tones ({ 1000.0 }, 0.1f, 1024, 48000.0, static_cast<int64_t> (f) * 1024);
            std::vector<float> mid = x, side = x;
            for (auto& s : side)
                s *= std::abs (sideGain);
            if (sideGain < 0.0f)
                std::fill (mid.begin(), mid.end(), 0.0f);
            a.push (true, mid.data(), 1024);
            a.pushSide (side.data(), 1024);
            a.advance (1024.0 / 48000.0);
        }
        const double expected = sideGain == 0.0f ? 0.0 : (sideGain > 0.0f ? 0.5 : 1.0);
        CHECK_NEAR (a.getStereoWidth (1000.0), expected, 0.02);
    }

    // AnalyzerFeed: the side ring goes to the side sink only; mid sinks unchanged.
    flub::AnalyzerTaps taps;
    ui::AnalyzerFeed feed;
    int midSamples = 0, sideSamples = 0;
    feed.addSink ([&] (ui::AnalyzerFeed::Stream, const float*, int n) { midSamples += n; });
    std::vector<float> block (500, 0.25f);
    std::vector<flub::StereoTapFrame> pairs (500, flub::StereoTapFrame { 0.5f, 0.25f });
    taps.post.push (block.data(), block.size());
    taps.postStereo.push (pairs.data(), pairs.size());
    feed.pull (taps);
    CHECK (midSamples == 500);
    CHECK (taps.postStereo.available() == 0); // no side sink: drained and dropped
    feed.setSideSink ([&] (const float* x, int n)
                      {
                          sideSamples += n;
                          CHECK (x[0] == 0.25f);
                      });
    taps.postStereo.push (pairs.data(), pairs.size());
    feed.pull (taps);
    CHECK (sideSamples == 500);
    CHECK (midSamples == 500);
}

TEST_CASE ("App: the chain's post side tap carries (L - R) / 2")
{
    AudioEngineHost host;
    host.prepareOffline (48000.0, 512);
    auto& store = host.getMixEngine().params (1);
    store.set (flub::param::Bank::A, flub::param::BypassAll, 1.0f);
    store.set (flub::param::Bank::B, flub::param::BypassAll, 1.0f);

    struct LeftOnly final : StripSignalSource
    {
        bool renderStrip (int strip, const flub::AudioBlock& block) override
        {
            if (strip != 1)
                return false;
            for (int i = 0; i < block.numSamples; ++i)
            {
                block.channel (0)[i] = 0.5f;
                block.channel (1)[i] = 0.0f;
            }
            return true;
        }
    } source;
    for (int b = 0; b < 40; ++b)
        host.renderOffline (source, 512);

    auto& taps = host.getMixEngine().chain (1).taps();
    const size_t n = taps.postStereo.available();
    REQUIRE (n > 512);
    CHECK (taps.post.available() == n);
    std::vector<float> mid (n);
    std::vector<flub::StereoTapFrame> pairs (n);
    taps.post.pop (mid.data(), n);
    taps.postStereo.pop (pairs.data(), n);
    // Bypassed strip: after the warm-up L = 0.5, R = 0: mid = side = 0.25,
    // and the stereo ring's mid is the post ring's sample for sample.
    CHECK_NEAR (pairs.back().side, mid.back(), 1.0e-4);
    CHECK_NEAR (pairs.back().side, 0.25, 0.02);
    CHECK (pairs.back().mid == mid.back());
    CHECK (pairs[n / 2].mid == mid[n / 2]);
}

// =============================================================================
// 6. Freeze, 7. piano keys, options
// =============================================================================
TEST_CASE ("App: freeze keeps a reference across a reset until cleared")
{
    ui::SpectrumAnalyzer a;
    a.setBounds (0, 0, 800, 300);
    CHECK (! a.isFrozen());
    a.freeze();
    CHECK (a.isFrozen());
    a.reset();
    CHECK (a.isFrozen()); // a strip switch keeps it
    a.freeze();           // re-capture
    CHECK (a.isFrozen());
    a.clearFreeze();
    CHECK (! a.isFrozen());
}

TEST_CASE ("App: piano keys line up with the frequency axis and leave the plot alone")
{
    ui::SpectrumAnalyzer a;
    a.setBounds (0, 0, 1000, 320);
    const auto plot = a.getPlotArea();
    a.setPianoKeysEnabled (true);
    CHECK (a.getPlotArea() == plot);

    // A (white, black keys on both sides) is centred on its frequency; so is every black key.
    const auto keyA4 = a.getKeyBounds (69);
    CHECK_NEAR (keyA4.getCentreX(), a.xForFrequency (440.0), 0.01);
    const auto keyCs4 = a.getKeyBounds (61);
    CHECK_NEAR (keyCs4.getCentreX(), a.xForFrequency (277.1826), 0.01);
    CHECK (keyCs4.getHeight() < keyA4.getHeight());
    CHECK_NEAR (keyA4.getBottom(), plot.getBottom(), 1.0e-3);

    // White keys tile the axis from C1 to C8 without gaps or overlaps.
    float right = -1.0f;
    for (int m = ui::SpectrumAnalyzer::kKeysLowMidi; m <= ui::SpectrumAnalyzer::kKeysHighMidi; ++m)
    {
        const int n = m % 12;
        if (n == 1 || n == 3 || n == 6 || n == 8 || n == 10)
            continue;
        const auto k = a.getKeyBounds (m);
        if (right >= 0.0f)
            CHECK_NEAR (k.getX(), right, 0.01);
        right = k.getRight();
    }
    CHECK (a.getKeyBounds (23).isEmpty());
    CHECK (a.getKeyBounds (109).isEmpty());
}

TEST_CASE ("App: piano keys light the notes that are playing and fade when they stop")
{
    // The pure rule: a note 12 dB over a flat bed lights fully, its
    // neighbours and the bed do not; nothing under the floor lights.
    {
        std::array<float, ui::SpectrumAnalyzer::kNumKeys> db {}, act {};
        db.fill (-40.0f);
        db[21] = -28.0f; // A2
        ui::SpectrumAnalyzer::keyActivity (db.data(), ui::SpectrumAnalyzer::kNumKeys, act.data());
        CHECK_NEAR (act[21], 1.0f, 1.0e-6f);
        CHECK (act[19] == 0.0f);
        CHECK (act[23] == 0.0f);
        CHECK (act[60] == 0.0f);
        db.fill (-95.0f);
        db[21] = -83.0f;
        ui::SpectrumAnalyzer::keyActivity (db.data(), ui::SpectrumAnalyzer::kNumKeys, act.data());
        CHECK (act[21] == 0.0f);
    }
    // Through the analyser: A4 and E5 played together light their keys, C5
    // between them stays dark; 1 s of silence lets the glow fade out.
    ui::SpectrumAnalyzer a;
    a.setSampleRate (48000.0);
    a.setPianoKeysEnabled (true);
    for (int f = 0; f < 30; ++f)
    {
        const auto out = tones ({ 440.0, 659.255 }, 0.05f, 1024, 48000.0, static_cast<int64_t> (f) * 1024);
        a.push (true, out.data(), 1024);
        a.advance (1024.0 / 48000.0);
    }
    CHECK_GE (a.getKeyGlow (69), 0.5f); // A4
    CHECK_GE (a.getKeyGlow (76), 0.5f); // E5
    CHECK_LE (a.getKeyGlow (72), 0.05f); // C5
    CHECK_LE (a.getKeyGlow (45), 0.05f); // A2: nothing there
    const std::vector<float> silence (1024, 0.0f);
    for (int f = 0; f < 47; ++f)
    {
        a.push (true, silence.data(), 1024);
        a.advance (1024.0 / 48000.0);
    }
    CHECK (a.getKeyGlow (69) == 0.0f);
    // Off: nothing glows.
    a.setPianoKeysEnabled (false);
    CHECK (a.getKeyGlow (76) == 0.0f);
}

TEST_CASE ("App: analyser options persist the new views and read the older format")
{
    ui::AnalyzerPanel::Options o;
    CHECK (o.toString() == "1,1,1,1,12,0,0,0,0,0,spectrum,none,0"); // new views off by default

    o.tilt = false;
    o.eqRangeDb = 24.0f;
    o.difference = o.sharpLows = o.width = o.pianoKeys = o.spectrogram = true;
    ui::AnalyzerPanel::Options back;
    REQUIRE (ui::AnalyzerPanel::Options::fromString (o.toString(), back));
    CHECK (back.toString() == o.toString());

    // A preference written before the new views: they stay off.
    REQUIRE (ui::AnalyzerPanel::Options::fromString ("1,0,1,1,6", back));
    CHECK (back.toString() == "1,0,1,1,6,0,0,0,0,0,spectrum,none,0");
    CHECK (! ui::AnalyzerPanel::Options::fromString ("1,1,1", back));
}
