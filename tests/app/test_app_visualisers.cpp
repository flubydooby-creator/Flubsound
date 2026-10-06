// App-level tests: the analyser's visualiser framework (docs/06 §6.4.2) and
// its views: the registry, the host, switching and persistence in the
// analyser panel, the feed's aligned mid / side pairs, no allocation per
// frame, and per view a deterministic check of its data mapping (goniometer,
// stereo field, correlation meter, loudness history, waveform, gain reduction).
#include "AppTestSupport.h"

#include "ui/AnalyzerFeed.h"
#include "ui/AnalyzerPanel.h"
#include "ui/MeterSnapshot.h"
#include "ui/vis/CorrelationMeter.h"
#include "ui/vis/GainReductionTrace.h"
#include "ui/vis/Goniometer.h"
#include "ui/vis/LoudnessHistory.h"
#include "ui/vis/StereoField.h"
#include "ui/vis/VisualiserHost.h"
#include "ui/vis/VisualiserRegistry.h"
#include "ui/vis/WaveformView.h"

#include "flub/engine/MeterBus.h"
#include "flub/engine/Parameters.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace flub::app;
namespace vis = flub::app::ui::vis;

namespace
{
constexpr double kPi = juce::MathConstants<double>::pi;
constexpr double kRate = 48000.0;
constexpr int kFrame = 800; // one 60 Hz display frame at 48 kHz
constexpr double kDt = kFrame / kRate;

/** Feeds `frames` frames of L / R (from left (t), right (t)) as mid / side pairs, advancing once per frame. */
template <typename Left, typename Right>
void runStereo (vis::Visualiser& v, int frames, Left left, Right right, const ui::MeterSnapshot& meters = {}, int64_t start = 0)
{
    std::vector<float> mid (kFrame), side (kFrame);
    for (int f = 0; f < frames; ++f)
    {
        for (int i = 0; i < kFrame; ++i)
        {
            const int64_t n = start + static_cast<int64_t> (f) * kFrame + i;
            const double l = left (n), r = right (n);
            mid[static_cast<size_t> (i)] = static_cast<float> (0.5 * (l + r));
            side[static_cast<size_t> (i)] = static_cast<float> (0.5 * (l - r));
        }
        v.pushPost (mid.data(), side.data(), kFrame);
        v.advance (vis::FrameContext { meters, kDt, kRate });
    }
}

double sine (double hz, int64_t n, double amplitude = 0.5)
{
    return amplitude * std::sin (2.0 * kPi * hz * static_cast<double> (n) / kRate);
}

/** Deterministic white noise in -a..a (two independent generators for L and R). */
struct Noise
{
    uint32_t state;
    double operator() (int64_t)
    {
        state = state * 1664525u + 1013904223u;
        return 0.5 * (static_cast<double> (state >> 8) / 8388608.0 - 1.0);
    }
};

/** Intensity-weighted mean distance of the phosphor's light from the centre, across (x) and up/down (y). */
juce::Point<double> spread (const vis::Goniometer& g)
{
    const auto& p = g.getPhosphor();
    const double c = 0.5 * (p.getWidth() - 1);
    double sum = 0.0, sx = 0.0, sy = 0.0;
    for (int y = 0; y < p.getHeight(); ++y)
        for (int x = 0; x < p.getWidth(); ++x)
        {
            const double v = p.getCell (x, y);
            sum += v;
            sx += v * std::abs (x - c);
            sy += v * std::abs (y - c);
        }
    return sum > 0.0 ? juce::Point<double> (sx / sum, sy / sum) : juce::Point<double>();
}
} // namespace

// =============================================================================
// Framework: registry, host, options, panel
// =============================================================================
TEST_CASE ("App: visualiser registry entries are complete, unique and creatable")
{
    const auto& list = vis::registry();
    REQUIRE (list.size() >= 6);
    std::set<std::string> ids;
    for (const auto& d : list)
    {
        CHECK (vis::isValidId (d.id));
        CHECK (juce::String (d.id) != ui::AnalyzerPanel::Options::kSpectrum);
        CHECK (juce::String (d.id) != ui::AnalyzerPanel::Options::kNone);
        CHECK (ids.insert (d.id).second);
        CHECK (juce::String (d.menuName).isNotEmpty());
        CHECK (juce::String (d.caption).isNotEmpty());
        CHECK (juce::String (d.description).length() > 20);
        CHECK (d.canBeMain || d.canBeStrip);
        REQUIRE (d.create != nullptr);
        auto view = d.create();
        REQUIRE (view != nullptr);
        CHECK (view->getStripHeight() > 10);
        CHECK (vis::findDescriptor (d.id) == &d);
    }
    for (const char* id : { "goniometer", "stereo-field", "loudness-history", "waveform", "gain-reduction", "correlation" })
        CHECK (vis::findDescriptor (id) != nullptr);
    CHECK (vis::findDescriptor ("no-such-view") == nullptr);
    CHECK (! vis::isValidId ("Bad Id"));
    CHECK (! vis::isValidId (""));
    CHECK (! vis::isValidId ("a,b"));
}

TEST_CASE ("App: visualiser host creates views on first use and feeds the selected and history views")
{
    vis::VisualiserHost host;
    CHECK (host.getNumCreated() == 0);
    CHECK (host.findCreated ("goniometer") == nullptr);
    CHECK (host.get ("no-such-view") == nullptr);

    auto* gonio = host.get ("goniometer");
    REQUIRE (gonio != nullptr);
    CHECK (host.get ("goniometer") == gonio); // kept
    CHECK (gonio->getTitle() == "Goniometer (vectorscope)");
    CHECK (gonio->getDescription().isNotEmpty());
    auto* loud = host.get ("loudness-history");
    REQUIRE (loud != nullptr);
    CHECK (host.getNumCreated() == 2);

    // Nothing selected: only the view with a history is fed.
    host.setSelected ({}, {});
    CHECK (! host.isFed ("goniometer"));
    CHECK (host.isFed ("loudness-history"));
    CHECK (! host.isFed ("stereo-field")); // not created
    host.setSelected ("goniometer", "correlation");
    CHECK (host.isFed ("goniometer"));
    CHECK (! host.isFed ("correlation")); // selected but not created yet

    // Data reaches the fed views only.
    ui::MeterSnapshot m;
    m.active = true;
    m.momentaryLufs = -18.0f;
    m.shortTermLufs = -19.0f;
    for (int f = 0; f < 30; ++f)
        host.advance (vis::FrameContext { m, 0.02, kRate });
    auto& history = static_cast<vis::LoudnessHistory&> (*loud).getHistory();
    CHECK (history.getFilled() >= 5);
    CHECK_NEAR (history.get (0, vis::LoudnessHistory::Momentary), -18.0, 1.0e-4);
}

TEST_CASE ("App: analyser options persist the visualisers and read the two older formats")
{
    using O = ui::AnalyzerPanel::Options;
    O o;
    CHECK (o.toString() == "1,1,1,1,12,0,0,0,0,0,spectrum,none,0"); // the default: the spectrum, no strip

    o.visualiser = "goniometer";
    o.strip = "correlation";
    o.beside = true;
    o.pianoKeys = true;
    O back;
    REQUIRE (O::fromString (o.toString(), back));
    CHECK (back.visualiser == "goniometer");
    CHECK (back.strip == "correlation");
    CHECK (back.beside);
    CHECK (back.pianoKeys);
    CHECK (back.toString() == o.toString());

    // The 10-field format of the previous version, and the 5-field one before it.
    REQUIRE (O::fromString ("1,1,1,1,12,1,0,0,0,1", back));
    CHECK (back.difference);
    CHECK (back.spectrogram);
    CHECK (back.visualiser == O::kSpectrum);
    CHECK (back.strip == O::kNone);
    CHECK (! back.beside);
    REQUIRE (O::fromString ("1,0,1,1,6", back));
    CHECK (back.toString() == "1,0,1,1,6,0,0,0,0,0,spectrum,none,0");

    // Ids this build does not have (or for the other place) read as the defaults.
    REQUIRE (O::fromString ("1,1,1,1,12,0,0,0,0,0,future-view,also-future,1", back));
    CHECK (back.visualiser == O::kSpectrum);
    CHECK (back.strip == O::kNone);
    REQUIRE (O::fromString ("1,1,1,1,12,0,0,0,0,0,correlation,goniometer,0", back));
    CHECK (back.visualiser == O::kSpectrum);
    CHECK (back.strip == O::kNone);
    CHECK (! O::fromString ("1,1,1,1,12,0,0,0,0,0,goniometer", back));
}

TEST_CASE ("App: analyser panel switches between the spectrum, a visualiser beside or instead of it, and a strip")
{
    ui::AnalyzerPanel panel ([] { return nullptr; });
    panel.setBounds (0, 0, 1000, 420);
    auto& spectrum = panel.getAnalyzer();
    CHECK (panel.getMainVisualiser() == nullptr);
    CHECK (panel.getStripVisualiser() == nullptr);
    CHECK (spectrum.isVisible());
    const auto fullHeight = spectrum.getHeight();
    CHECK (panel.getVisualisers().getNumCreated() == 0); // nothing exists until chosen

    int saved = 0;
    panel.onOptionsChanged = [&saved] (const ui::AnalyzerPanel::Options&) { ++saved; };

    auto o = panel.getOptions();
    o.visualiser = "goniometer";
    panel.setOptions (o);
    auto* gonio = panel.getMainVisualiser();
    REQUIRE (gonio != nullptr);
    CHECK (gonio->isVisible());
    CHECK (gonio->getParentComponent() == &panel);
    CHECK (panel.isSpectrumReplaced());
    CHECK (! spectrum.isVisible());
    CHECK (! panel.getEqEditor().isVisible());
    CHECK (panel.getViewButton().isVisible());
    CHECK (gonio->getHeight() == fullHeight);

    // Beside: both visible, the visualiser at the right.
    o.beside = true;
    panel.setOptions (o);
    CHECK (spectrum.isVisible());
    CHECK (gonio->isVisible());
    CHECK (! panel.isSpectrumReplaced());
    CHECK (gonio->getX() > spectrum.getRight());
    CHECK (gonio->getWidth() >= 220);
    // Too narrow for both: it replaces the spectrum again.
    panel.setBounds (0, 0, 560, 420);
    CHECK (panel.isSpectrumReplaced());
    CHECK (! spectrum.isVisible());
    panel.setBounds (0, 0, 1000, 420);

    // A strip under the spectrum.
    o.visualiser = ui::AnalyzerPanel::Options::kSpectrum;
    o.strip = "correlation";
    panel.setOptions (o);
    CHECK (panel.getMainVisualiser() == nullptr);
    CHECK (! gonio->isVisible()); // kept, hidden
    CHECK (spectrum.isVisible());
    auto* strip = panel.getStripVisualiser();
    REQUIRE (strip != nullptr);
    CHECK (strip->isVisible());
    CHECK (strip->getY() > spectrum.getBottom());
    CHECK (spectrum.getHeight() < fullHeight);
    CHECK (panel.getVisualisers().isFed ("correlation"));
    CHECK (! panel.getVisualisers().isFed ("goniometer"));

    // Back to the default: exactly the original layout.
    o.strip = ui::AnalyzerPanel::Options::kNone;
    panel.setOptions (o);
    CHECK (panel.getStripVisualiser() == nullptr);
    CHECK (! strip->isVisible());
    CHECK (spectrum.getHeight() == fullHeight);
    CHECK (saved == 0); // setOptions never reports a user change

    // An id for the wrong place is ignored.
    o.visualiser = "correlation";
    o.strip = "goniometer";
    panel.setOptions (o);
    CHECK (panel.getMainVisualiser() == nullptr);
    CHECK (panel.getStripVisualiser() == nullptr);
}

TEST_CASE ("App: the loudness target is the maximizer's while automatic drive is on, else auto level's")
{
    flub::param::ParameterStore store;
    juce::String name;
    store.set (flub::param::MaxAutoDrive, 0.0f);
    store.set (flub::param::AutoLevelOn, 0.0f);
    CHECK (std::isnan (ui::AnalyzerPanel::loudnessTarget (store, name)));
    store.set (flub::param::AutoLevelOn, 1.0f);
    store.set (flub::param::AutoLevelTargetLufs, -16.0f);
    CHECK_NEAR (ui::AnalyzerPanel::loudnessTarget (store, name), -16.0, 1.0e-4);
    CHECK (name.startsWith ("Auto level"));
    store.set (flub::param::MaxAutoDrive, 1.0f);
    store.set (flub::param::MaximizerOn, 0.0f);
    CHECK_NEAR (ui::AnalyzerPanel::loudnessTarget (store, name), -16.0, 1.0e-4); // the maximizer is off
    store.set (flub::param::MaximizerOn, 1.0f);
    const float maxTarget = store.get (flub::param::MaxTargetLufs);
    CHECK_NEAR (ui::AnalyzerPanel::loudnessTarget (store, name), maxTarget, 1.0e-4);
    CHECK (name.startsWith ("Maximizer"));
}

TEST_CASE ("App: the feed hands the visualisers the post tap as aligned mid / side pairs")
{
    flub::AnalyzerTaps taps;
    ui::AnalyzerFeed feed;
    std::vector<float> mids, sides;
    feed.setStereoSink ([&] (const float* m, const float* s, int n)
                        {
                            mids.insert (mids.end(), m, m + n);
                            sides.insert (sides.end(), s, s + n);
                        });
    std::vector<flub::StereoTapFrame> pairs (3000);
    for (size_t i = 0; i < pairs.size(); ++i)
        pairs[i] = { static_cast<float> (i), -static_cast<float> (i) };
    taps.postStereo.push (pairs.data(), pairs.size());
    feed.pull (taps);
    REQUIRE (mids.size() == 3000);
    REQUIRE (sides.size() == 3000);
    for (size_t i = 0; i < mids.size(); i += 97)
        CHECK (sides[i] == -mids[i]);

    // A stale backlog is trimmed as one stream: the pairs stay aligned.
    mids.clear();
    sides.clear();
    std::vector<flub::StereoTapFrame> backlog (20000);
    for (size_t i = 0; i < backlog.size(); ++i)
        backlog[i] = { static_cast<float> (i), -static_cast<float> (i) };
    taps.postStereo.push (backlog.data(), backlog.size());
    feed.pull (taps);
    REQUIRE (! mids.empty());
    CHECK (mids.back() == 19999.0f);
    CHECK (mids.size() == sides.size());
    CHECK (sides.front() == -mids.front());
}

TEST_CASE ("App: visualisers do not allocate per frame once running")
{
    ui::MeterSnapshot m;
    m.active = true;
    m.momentaryLufs = -14.0f;
    m.shortTermLufs = -15.0f;
    m.maxGainReductionDb = -3.0f;
    m.compGainReductionDb = -1.0f;
    std::vector<float> mid (kFrame), side (kFrame);
    for (size_t i = 0; i < mid.size(); ++i)
    {
        mid[i] = static_cast<float> (sine (440.0, static_cast<int64_t> (i)));
        side[i] = static_cast<float> (sine (660.0, static_cast<int64_t> (i), 0.2));
    }
    for (const auto& d : vis::registry())
    {
        auto view = d.create();
        view->setSampleRate (kRate);
        view->setBounds (0, 0, 640, 300);
        view->setVisible (true);
        const vis::FrameContext frame { m, kDt, kRate };
        for (int f = 0; f < 20; ++f) // warm up: the FFT's first pass, slots filling
        {
            view->pushPre (mid.data(), kFrame);
            view->pushPost (mid.data(), side.data(), kFrame);
            view->advance (frame);
        }
        flubapptest::RealtimeProbe probe;
        for (int f = 0; f < 10; ++f)
        {
            view->pushPre (mid.data(), kFrame);
            view->pushPost (mid.data(), side.data(), kFrame);
            view->advance (frame);
        }
        if (probe.allocations() != 0)
            std::printf ("  %s allocated %lld times in 10 frames\n", d.id, static_cast<long long> (probe.allocations()));
        CHECK (probe.allocations() == 0);
    }
}

// =============================================================================
// 1. Goniometer
// =============================================================================
TEST_CASE ("App: goniometer draws mono on the vertical axis and out-of-phase on the horizontal")
{
    // The mapping itself: mid up, side across with the left channel to the left.
    const auto p = vis::Goniometer::scopePoint (0.5f, 0.0f, 1.0f);
    CHECK_NEAR (p.x, 0.0, 1.0e-6);
    CHECK_NEAR (p.y, 0.5, 1.0e-6);
    const auto left = vis::Goniometer::scopePoint (0.25f, 0.25f, 2.0f); // L = 0.5, R = 0
    CHECK (left.x < 0.0f);
    CHECK_NEAR (left.x, -0.5, 1.0e-6);
    CHECK_NEAR (left.y, 0.5, 1.0e-6);

    {
        vis::Goniometer g;
        runStereo (g, 60, [] (int64_t n) { return sine (220.0, n); }, [] (int64_t n) { return sine (220.0, n); });
        const auto s = spread (g);
        CHECK (s.x < 1.0);    // a line on the vertical axis
        CHECK (s.y > 40.0);   // spread up and down
        CHECK_NEAR (20.0 * std::log10 (g.getGain()), 20.0 * std::log10 (vis::Goniometer::kTargetPeak / 0.5), 0.5);
    }
    {
        vis::Goniometer g;
        runStereo (g, 12, [] (int64_t n) { return sine (220.0, n); }, [] (int64_t n) { return -sine (220.0, n); });
        const auto s = spread (g);
        CHECK (s.y < 1.0);  // on the horizontal axis
        CHECK (s.x > 40.0);
    }
    {
        // Quiet programme (-40 dBFS) still fills the scope: the automatic gain rises to its +30 dB limit.
        vis::Goniometer g;
        runStereo (g, 120, [] (int64_t n) { return sine (220.0, n, 0.01); }, [] (int64_t n) { return sine (220.0, n, 0.01); });
        CHECK_NEAR (20.0 * std::log10 (g.getGain()), vis::Goniometer::kMaxGainDb, 0.5);
        g.reset();
        CHECK (spread (g).y == 0.0);
    }
}

// =============================================================================
// 2. Stereo field
// =============================================================================
TEST_CASE ("App: stereo field pan and width from mid / side powers")
{
    using SF = vis::StereoField;
    // Mono: M^2 = 1, S = 0.
    auto b = SF::analyse (1.0, 0.0, 0.0);
    CHECK_NEAR (b.pan, 0.0, 1.0e-6);
    CHECK_NEAR (b.correlation, 1.0, 1.0e-6);
    // Left only: M = S, C = M^2.
    b = SF::analyse (1.0, 1.0, 1.0);
    CHECK_NEAR (b.pan, -1.0, 1.0e-6);
    CHECK_NEAR (b.correlation, 1.0, 1.0e-6);
    // Right only: S = -M.
    b = SF::analyse (1.0, 1.0, -1.0);
    CHECK_NEAR (b.pan, 1.0, 1.0e-6);
    // Out of phase: side only.
    b = SF::analyse (0.0, 1.0, 0.0);
    CHECK_NEAR (b.pan, 0.0, 1.0e-6);
    CHECK_NEAR (b.correlation, -1.0, 1.0e-6);
    // Uncorrelated equal channels: M^2 = S^2, C = 0.
    b = SF::analyse (0.5, 0.5, 0.0);
    CHECK_NEAR (b.correlation, 0.0, 1.0e-6);
    CHECK (SF::analyse (0.0, 0.0, 0.0).levelDb < -150.0f);

    CHECK (SF::bandIndexFor (1000.0) == 16);
    CHECK_NEAR (SF::bandCentreHz (16), 1000.0, 1.0e-9);
    CHECK (SF::bandIndexFor (25.0) == 0);
    CHECK (SF::bandIndexFor (20000.0) == SF::kNumBands - 1);
}

TEST_CASE ("App: stereo field places a left-only tone left, a mono tone centre and noise wide")
{
    using SF = vis::StereoField;
    vis::StereoField field;
    field.setBounds (0, 0, 600, 360);
    // 1 kHz left only, 250 Hz in the centre, 4 kHz right only (at -12 dB).
    runStereo (field, 40, [] (int64_t n) { return sine (1000.0, n, 0.4) + sine (250.0, n, 0.3); },
               [] (int64_t n) { return sine (250.0, n, 0.3) + sine (4000.0, n, 0.1); });
    REQUIRE (field.getAnalyses() > 5);
    const auto& left = field.getBand (SF::bandIndexFor (1000.0));
    CHECK (left.pan < -0.95f);
    CHECK (left.correlation > 0.95f);
    const auto& centre = field.getBand (SF::bandIndexFor (250.0));
    CHECK_NEAR (centre.pan, 0.0, 0.05);
    CHECK (centre.correlation > 0.95f);
    const auto& right = field.getBand (SF::bandIndexFor (4000.0));
    CHECK (right.pan > 0.95f);
    CHECK (left.levelDb > right.levelDb + 8.0f);
    CHECK (field.xForPan (left.pan) < field.xForPan (0.0f));
    CHECK (field.yForBand (16) < field.yForBand (10)); // higher frequencies further up

    // Independent noise on each side: correlation near 0 across the mid bands.
    vis::StereoField wide;
    runStereo (wide, 60, Noise { 1u }, Noise { 987654321u });
    for (const double hz : { 200.0, 1000.0, 5000.0 })
    {
        const auto& band = wide.getBand (SF::bandIndexFor (hz));
        CHECK_NEAR (band.correlation, 0.0, 0.3);
        CHECK_NEAR (band.pan, 0.0, 0.3);
    }
}

// =============================================================================
// 3. Correlation meter
// =============================================================================
TEST_CASE ("App: correlation meter reads +1 for mono, -1 inverted, about 0 for unrelated channels")
{
    vis::CorrelationMeter meter;
    meter.setSampleRate (kRate);
    CHECK (! meter.hasSignal());
    runStereo (meter, 30, [] (int64_t n) { return sine (300.0, n); }, [] (int64_t n) { return sine (300.0, n); });
    CHECK (meter.hasSignal());
    CHECK_NEAR (meter.getCorrelation(), 1.0, 0.001);

    runStereo (meter, 60, [] (int64_t n) { return sine (300.0, n); }, [] (int64_t n) { return -sine (300.0, n); });
    CHECK_NEAR (meter.getCorrelation(), -1.0, 0.001);
    CHECK_NEAR (meter.getHold(), -1.0, 0.001);

    runStereo (meter, 60, Noise { 7u }, Noise { 123457u });
    CHECK_NEAR (meter.getCorrelation(), 0.0, 0.12);
    // The hold marker keeps the -1 for 3 s, then rises.
    CHECK_NEAR (meter.getHold(), -1.0, 0.001);
    runStereo (meter, 240, Noise { 9u }, Noise { 99u }); // 4 s more
    CHECK (meter.getHold() > -0.8f);

    // Silence: no reading.
    runStereo (meter, 120, [] (int64_t) { return 0.0; }, [] (int64_t) { return 0.0; });
    CHECK (! meter.hasSignal());

    meter.setBounds (0, 0, 600, 30);
    CHECK (meter.xForCorrelation (-1.0f) < meter.xForCorrelation (0.0f));
    CHECK (meter.xForCorrelation (0.0f) < meter.xForCorrelation (1.0f));
    CHECK_NEAR (meter.xForCorrelation (0.0f), meter.getBarArea().getCentreX(), 0.01);
}

// =============================================================================
// 4. Loudness history
// =============================================================================
TEST_CASE ("App: loudness history records momentary and short-term loudness and the target")
{
    vis::LoudnessHistory history;
    history.setBounds (0, 0, 700, 300);
    history.setVisible (true);
    ui::MeterSnapshot m;
    m.active = true;
    m.momentaryLufs = -20.0f;
    m.shortTermLufs = -23.0f;
    vis::FrameContext frame { m, 1.0 / 60.0, kRate };
    frame.targetLufs = -14.0f;
    frame.targetName = "Auto level target (input)";
    for (int f = 0; f < 120; ++f) // 2 s
        history.advance (frame);
    const auto& h = history.getHistory();
    CHECK (h.getFilled() >= 19);
    CHECK (h.getFilled() <= 21);
    CHECK_NEAR (h.get (0, vis::LoudnessHistory::Momentary), -20.0, 1.0e-4);
    CHECK_NEAR (h.get (0, vis::LoudnessHistory::ShortTerm), -23.0, 1.0e-4);
    CHECK_NEAR (history.getTargetLufs(), -14.0, 1.0e-4);
    CHECK (history.yForLufs (-14.0f) < history.yForLufs (-23.0f));

    // A momentary peak within a slot is kept (the slot's maximum).
    m.momentaryLufs = -10.0f;
    history.advance (frame);
    m.momentaryLufs = -20.0f;
    for (int f = 0; f < 6; ++f)
        history.advance (frame);
    bool peakKept = false;
    for (int age = 0; age < 3; ++age)
        peakKept = peakKept || std::abs (h.get (age, vis::LoudnessHistory::Momentary) + 10.0f) < 1.0e-4f;
    CHECK (peakKept);

    // An inactive strip records silence (a gap).
    m.active = false;
    for (int f = 0; f < 12; ++f)
        history.advance (frame);
    CHECK (h.get (0, vis::LoudnessHistory::ShortTerm) < vis::LoudnessHistory::kFloorLufs);
    history.reset();
    CHECK (h.getFilled() == 0);
}

// =============================================================================
// 5. Waveform before / after
// =============================================================================
TEST_CASE ("App: waveform envelope of a known burst, input and output separately")
{
    vis::WaveformView wave;
    wave.setSampleRate (kRate);
    const int spc = wave.getSamplesPerColumn();
    CHECK (spc == 240); // 4 s over 800 columns at 48 kHz

    // Input: 0.1 s of a +-0.5 square wave, then 0.2 s of silence. Output: the same at +-0.8.
    std::vector<float> in (14400, 0.0f), out (14400, 0.0f), side (14400, 0.0f);
    for (int i = 0; i < 4800; ++i)
    {
        in[static_cast<size_t> (i)] = (i / 24) % 2 == 0 ? 0.5f : -0.5f;
        out[static_cast<size_t> (i)] = 1.6f * in[static_cast<size_t> (i)];
    }
    ui::MeterSnapshot m;
    for (int f = 0; f < 18; ++f)
    {
        wave.pushPre (in.data() + f * kFrame, kFrame);
        wave.pushPost (out.data() + f * kFrame, side.data(), kFrame);
        wave.advance (vis::FrameContext { m, kDt, kRate });
    }
    REQUIRE (wave.getFilled (false) == 14400 / spc);
    REQUIRE (wave.getFilled (true) == 14400 / spc);
    int loudIn = 0, loudOut = 0;
    for (int age = 0; age < wave.getFilled (false); ++age)
    {
        const auto& c = wave.getColumn (false, age);
        if (c.hi > 0.25f)
        {
            ++loudIn;
            CHECK_NEAR (c.hi, 0.5, 1.0e-6);
            CHECK_NEAR (c.lo, -0.5, 1.0e-6);
        }
        const auto& o = wave.getColumn (true, age);
        if (o.hi > 0.25f)
        {
            ++loudOut;
            CHECK_NEAR (o.hi, 0.8, 1.0e-6);
        }
    }
    CHECK (loudIn == 4800 / spc); // 20 columns = 0.1 s
    CHECK (loudOut == 4800 / spc);
    // The newest columns are the silence after the burst.
    CHECK (wave.getColumn (false, 0).hi == 0.0f);
    CHECK (wave.getColumn (false, 39).hi == 0.0f);
    CHECK (wave.getColumn (false, 40).hi == 0.5f);
}

// =============================================================================
// 6. Gain-reduction trace
// =============================================================================
TEST_CASE ("App: gain-reduction trace follows the meter values and deepens its scale")
{
    using GR = vis::GainReductionTrace;
    GR trace;
    trace.setBounds (0, 0, 700, 300);
    trace.setVisible (true);
    ui::MeterSnapshot m;
    m.active = true;
    m.maxGainReductionDb = -4.0f;
    m.glueGainReductionDb = -1.5f;
    for (int f = 0; f < 60; ++f)
        trace.advance (vis::FrameContext { m, 1.0 / 60.0, kRate });
    const auto& h = trace.getHistory();
    CHECK (h.getFilled() >= 38);
    CHECK_NEAR (h.get (0, GR::Limiter), -4.0, 1.0e-4);
    CHECK_NEAR (h.get (0, GR::Glue), -1.5, 1.0e-4);
    CHECK_NEAR (h.get (0, GR::Compressor), 0.0, 1.0e-6);
    CHECK_NEAR (h.get (0, GR::Master), 0.0, 1.0e-6);
    CHECK (trace.getRangeDb() == 12.0f);

    // A slot keeps its deepest reading; a deep reduction widens the scale.
    m.maxGainReductionDb = -15.0f;
    trace.advance (vis::FrameContext { m, 0.01, kRate });
    m.maxGainReductionDb = -2.0f;
    trace.advance (vis::FrameContext { m, 0.02, kRate });
    CHECK_NEAR (h.get (0, GR::Limiter), -15.0, 1.0e-4);
    CHECK (trace.getRangeDb() == 24.0f);

    // An inactive strip reads 0 (no reduction).
    m.active = false;
    for (int f = 0; f < 3; ++f)
        trace.advance (vis::FrameContext { m, 1.0 / 60.0, kRate });
    CHECK_NEAR (h.get (0, GR::Limiter), 0.0, 1.0e-6);
    CHECK (juce::String (GR::stageName (GR::Limiter)) == "Limiter");
}
