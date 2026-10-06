// App-level tests: the eye-candy visualisers and the visualiser window
// (docs/06 §6.4.2): the 3D waterfall's row ring and projection, the radial
// spectrum's mapping and bass pulse, the window's spectrum / spectrogram
// mirrors, no allocation per frame with a live analyser, and the window's
// lifecycle (views, the host's extra feed, full screen, persisted state).
#include "AppTestSupport.h"

#include "ui/MeterSnapshot.h"
#include "ui/SpectrumAnalyzer.h"
#include "ui/vis/RadialSpectrum.h"
#include "ui/vis/SpectrumMirror.h"
#include "ui/vis/VisualiserHost.h"
#include "ui/vis/VisualiserRegistry.h"
#include "ui/vis/VisualiserWindow.h"
#include "ui/vis/Waterfall3D.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace flub::app;
namespace vis = flub::app::ui::vis;

namespace
{
constexpr double kPi = juce::MathConstants<double>::pi;
constexpr double kRate = 48000.0;
constexpr int kFrame = 800; // one 60 Hz display frame at 48 kHz
constexpr double kDt = kFrame / kRate;

/** Runs `frames` display frames of the mono signal `signal (n)` through a
    spectrum analyser (pre = post) and advances `view` with it after each. */
struct Feed
{
    ui::SpectrumAnalyzer analyser;
    ui::MeterSnapshot meters;
    std::vector<float> buffer = std::vector<float> (kFrame), side = std::vector<float> (kFrame);
    int64_t position = 0;

    Feed()
    {
        analyser.setSampleRate (kRate);
        meters.active = true;
        meters.momentaryLufs = -14.0f;
    }

    void run (vis::Visualiser& view, int frames, const std::function<double (int64_t)>& signal)
    {
        for (int f = 0; f < frames; ++f)
        {
            for (int i = 0; i < kFrame; ++i)
                buffer[static_cast<size_t> (i)] = static_cast<float> (signal (position++));
            analyser.push (false, buffer.data(), kFrame);
            analyser.push (true, buffer.data(), kFrame);
            analyser.advance (kDt);
            view.pushPost (buffer.data(), side.data(), kFrame);
            view.advance (vis::FrameContext { meters, kDt, kRate, &analyser });
        }
    }
};

double sine (double hz, int64_t n, double amplitude = 0.5)
{
    return amplitude * std::sin (2.0 * kPi * hz * static_cast<double> (n) / kRate);
}

template <typename Get>
int argMax (int count, Get get)
{
    int best = 0;
    for (int i = 1; i < count; ++i)
        if (get (i) > get (best))
            best = i;
    return best;
}
} // namespace

// =============================================================================
// 3D waterfall
// =============================================================================
TEST_CASE ("App: 3D waterfall columns, heights and perspective projection")
{
    using W = vis::Waterfall3D;
    CHECK_NEAR (W::columnHz (0), 20.0, 1.0e-9);
    CHECK_NEAR (W::columnHz (W::kColumns - 1), 20000.0, 1.0e-6);
    for (const int c : { 0, 17, 120, W::kColumns - 1 })
        CHECK (W::columnFor (W::columnHz (c)) == c);
    CHECK (std::abs (W::columnHz (W::columnFor (1000.0)) / 1000.0 - 1.0) < 0.02);

    CHECK (W::heightFor (-30.0f, -10.0f) > W::heightFor (-40.0f, -10.0f));
    CHECK (W::heightFor (-10.0f, -10.0f) == 1.0f);
    CHECK (W::heightFor (-10.0f - W::kRangeDb, -10.0f) == 0.0f);
    CHECK (W::heightFor (-200.0f, -10.0f) == 0.0f);

    const juce::Rectangle<float> area (0.0f, 0.0f, 800.0f, 400.0f);
    const auto front = W::project (area, 0.5f, 0.0f, 0.0f);
    const auto back = W::project (area, 0.5f, 1.0f, 0.0f);
    CHECK_NEAR (front.y, area.getBottom(), 1.0e-3); // the newest row's floor is the bottom of the plot
    CHECK (back.y < front.y);             // older rows recede upwards, towards the horizon
    CHECK_NEAR (front.x, 400.0f, 1.0e-3); // centred
    const float frontWidth = W::project (area, 1.0f, 0.0f, 0.0f).x - W::project (area, 0.0f, 0.0f, 0.0f).x;
    const float backWidth = W::project (area, 1.0f, 1.0f, 0.0f).x - W::project (area, 0.0f, 1.0f, 0.0f).x;
    CHECK_NEAR (frontWidth, 800.0f, 1.0e-3);
    CHECK (backWidth < 0.7f * frontWidth);
    CHECK (backWidth > 0.3f * frontWidth);
    // A level raises the point; the same level stands taller in front than at the back.
    const float frontRise = front.y - W::project (area, 0.5f, 0.0f, 1.0f).y;
    const float backRise = back.y - W::project (area, 0.5f, 1.0f, 1.0f).y;
    CHECK (frontRise > 0.0f);
    CHECK (backRise > 0.0f);
    CHECK (frontRise > 2.5f * backRise);
    CHECK (W::project (area, 0.5f, 0.0f, 1.0f).y > area.getY()); // stays in the plot
}

TEST_CASE ("App: 3D waterfall shows a sine as a ridge at its column, newest row in front")
{
    using W = vis::Waterfall3D;
    Feed feed;
    W view;
    view.setBounds (0, 0, 800, 400);
    view.setVisible (true);

    feed.run (view, 36, [] (int64_t) { return 0.0; });                // 0.6 s of silence
    feed.run (view, 30, [] (int64_t n) { return sine (1000.0, n); }); // then 0.5 s of 1 kHz
    const int expected = W::columnFor (1000.0);

    // The live (front) ridge and the newest completed row peak at 1 kHz.
    const int live = argMax (W::kColumns, [&view] (int c) { return view.getLiveLevel (c); });
    CHECK (std::abs (live - expected) <= 2);
    const int newest = argMax (W::kColumns, [&view] (int c) { return view.getLevel (0, c); });
    CHECK (std::abs (newest - expected) <= 2);
    CHECK (view.getLevel (0, expected) > view.getLevel (0, W::columnFor (100.0)) + 30.0f);

    // 1.1 s at 12 rows/s: 13 rows; the oldest ones are the silence, behind the ridge.
    CHECK (view.getFilled() >= 12);
    CHECK (view.getFilled() <= 14);
    const int oldest = view.getFilled() - 1;
    CHECK (view.getLevel (oldest, expected) < view.getLevel (0, expected) - 40.0f);
    // The automatic range put the ridge near the top.
    CHECK (W::heightFor (view.getLevel (0, expected), view.getTopDb()) > 0.8f);

    view.reset();
    CHECK (view.getFilled() == 0);
    CHECK (view.getLevel (0, expected) < -150.0f);
}

// =============================================================================
// Radial spectrum
// =============================================================================
TEST_CASE ("App: radial spectrum maps frequency to angle and level to radius, mirrored")
{
    using R = vis::RadialSpectrum;
    const float low = R::angleFor (R::kLowHz), high = R::angleFor (R::kHighHz);
    CHECK (low > 0.0f);
    CHECK (low < 0.2f); // near the top
    CHECK (high > static_cast<float> (kPi) - 0.2f);
    CHECK (high < static_cast<float> (kPi)); // near the bottom
    float previous = -1.0f;
    for (int b = 0; b < R::kBins; ++b)
    {
        const float a = R::angleFor (R::binHz (b));
        CHECK (a > previous); // clockwise with frequency
        previous = a;
    }
    // Log frequency: equal ratios, equal angles.
    CHECK_NEAR (R::angleFor (200.0) - R::angleFor (100.0), R::angleFor (2000.0) - R::angleFor (1000.0), 1.0e-4);

    const juce::Point<float> c (300.0f, 200.0f);
    const auto top = R::polar (c, 0.0f, 50.0f);
    CHECK_NEAR (top.x, 300.0f, 1.0e-4);
    CHECK_NEAR (top.y, 150.0f, 1.0e-4); // 0 = straight up
    const auto right = R::polar (c, static_cast<float> (kPi) * 0.5f, 50.0f);
    CHECK_NEAR (right.x, 350.0f, 1.0e-3);
    CHECK_NEAR (right.y, 200.0f, 1.0e-3);
    for (const double f : { 40.0, 440.0, 5000.0 })
    {
        const float a = R::angleFor (f);
        const auto p = R::polar (c, a, 80.0f), m = R::polar (c, -a, 80.0f);
        CHECK_NEAR (p.x + m.x, 2.0f * c.x, 1.0e-3); // the left half mirrors the right
        CHECK_NEAR (p.y, m.y, 1.0e-3);
        CHECK (p.x > c.x);
        CHECK_NEAR (p.getDistanceFrom (c), 80.0f, 1.0e-3);
    }
    CHECK (R::radiusFor (0.0f, 40.0f, 200.0f) == 40.0f);
    CHECK (R::radiusFor (1.0f, 40.0f, 200.0f) == 200.0f);
    CHECK (R::radiusFor (0.5f, 40.0f, 200.0f) == 120.0f);
    CHECK (R::radiusFor (2.0f, 40.0f, 200.0f) == 200.0f);
    CHECK (R::levelFor (-10.0f, -10.0f) == 1.0f);
    CHECK (R::levelFor (-10.0f - R::kRangeDb, -10.0f) == 0.0f);
}

TEST_CASE ("App: radial spectrum lights the ray of a sine and pulses on bass hits")
{
    using R = vis::RadialSpectrum;
    {
        Feed feed;
        R view;
        view.setBounds (0, 0, 600, 600);
        view.setVisible (true);
        feed.run (view, 40, [] (int64_t n) { return sine (2000.0, n); });
        int expected = 0;
        for (int b = 1; b < R::kBins; ++b)
            if (std::abs (std::log (R::binHz (b) / 2000.0)) < std::abs (std::log (R::binHz (expected) / 2000.0)))
                expected = b;
        const int lit = argMax (R::kBins, [&view] (int b) { return view.getLevel (b); });
        CHECK (std::abs (lit - expected) <= 1);
        CHECK (view.getLevel (lit) > 0.8f);
        CHECK (view.getLevel (0) < 0.3f);
        CHECK (view.getPulse() < 0.05f); // no bass: no pulse
        CHECK (view.getInnerRadius() > 0.0f);
        CHECK (view.getOuterRadius() > 3.0f * view.getInnerRadius());
    }
    {
        // 60 Hz hits, 120 ms on every 0.5 s: the centre swells on each one.
        Feed feed;
        R view;
        view.setBounds (0, 0, 600, 600);
        view.setVisible (true);
        const auto kicks = [] (int64_t n) { return (n % 24000) < 5760 ? sine (60.0, n, 0.7) : 0.0; };
        feed.run (view, 50, kicks);
        float maxPulse = 0.0f, minPulse = 1.0f;
        for (int f = 0; f < 60; ++f)
        {
            feed.run (view, 1, kicks);
            maxPulse = juce::jmax (maxPulse, view.getPulse());
            minPulse = juce::jmin (minPulse, view.getPulse());
        }
        CHECK (maxPulse > 0.5f);
        CHECK (minPulse < 0.3f);
    }
}

// =============================================================================
// Mirrors and allocation
// =============================================================================
TEST_CASE ("App: the window's spectrum and spectrogram mirror the analyser")
{
    using M = vis::SpectrumMirror;
    Feed feed;
    M spectrum (M::Mode::Spectrum), spectrogram (M::Mode::Spectrogram);
    for (auto* v : { &spectrum, &spectrogram })
    {
        v->setBounds (0, 0, 640, 300);
        v->setVisible (true);
    }
    feed.run (spectrum, 30, [] (int64_t n) { return sine (500.0, n); });
    const int peak = argMax (M::kPoints, [&spectrum] (int i) { return spectrum.getPostDb (i); });
    CHECK (std::abs (M::pointHz (peak) / 500.0 - 1.0) < 0.05); // the band is flat-topped; the tilt favours its top end
    // The mirror shows what the analyser displays (the panel's tilt included).
    const float tilt = 4.5f * std::log2 (static_cast<float> (M::pointHz (peak)) / 1000.0f);
    CHECK_NEAR (spectrum.getPostDb (peak), feed.analyser.getDisplayLevelDb (true, M::pointHz (peak)) + tilt, 1.0e-3);

    Feed second;
    second.run (spectrogram, 30, [] (int64_t n) { return sine (500.0, n); });
    // One row per 1/60 s: 30 frames of 1/60 s.
    CHECK (spectrogram.getSpectrogram().getRowsWritten() >= 29);
    CHECK (spectrogram.getSpectrogram().getRowsWritten() <= 31);
}

TEST_CASE ("App: eye-candy views and the mirrors do not allocate per frame with a live analyser")
{
    std::vector<std::unique_ptr<vis::Visualiser>> views;
    views.push_back (std::make_unique<vis::Waterfall3D>());
    views.push_back (std::make_unique<vis::RadialSpectrum>());
    views.push_back (std::make_unique<vis::SpectrumMirror> (vis::SpectrumMirror::Mode::Spectrum));
    views.push_back (std::make_unique<vis::SpectrumMirror> (vis::SpectrumMirror::Mode::Spectrogram));
    for (auto& view : views)
    {
        Feed feed;
        view->setBounds (0, 0, 1280, 720);
        view->setVisible (true);
        const auto music = [] (int64_t n) { return sine (55.0, n, 0.4) + sine (880.0, n, 0.1) + sine (6000.0, n, 0.02); };
        feed.run (*view, 20, music);
        // Only the view's own work is measured: the analyser runs outside the probe.
        int64_t allocations = 0;
        for (int f = 0; f < 10; ++f)
        {
            for (int i = 0; i < kFrame; ++i)
                feed.buffer[static_cast<size_t> (i)] = static_cast<float> (music (feed.position++));
            feed.analyser.push (true, feed.buffer.data(), kFrame);
            feed.analyser.advance (kDt);
            flubapptest::RealtimeProbe probe;
            view->pushPost (feed.buffer.data(), feed.side.data(), kFrame);
            view->advance (vis::FrameContext { feed.meters, kDt, kRate, &feed.analyser });
            allocations += probe.allocations();
        }
        if (allocations != 0)
            std::printf ("  %s allocated %lld times in 10 frames\n", view->getTitle().toRawUTF8(), static_cast<long long> (allocations));
        CHECK (allocations == 0);
    }
}

// =============================================================================
// Visualiser window
// =============================================================================
TEST_CASE ("App: visualiser window state round-trips and its view list")
{
    using Window = vis::VisualiserWindow;
    const auto& choices = Window::choices();
    REQUIRE (choices.size() >= 4);
    CHECK (choices[0].id == "spectrum");
    CHECK (choices[1].id == "spectrogram");
    for (const char* id : { "waterfall-3d", "radial-spectrum", "goniometer", "loudness-history" })
        CHECK (Window::findChoice (id) != nullptr);
    CHECK (Window::findChoice ("correlation") == nullptr); // a strip, not a main view
    for (const auto& c : choices)
    {
        auto v = Window::createView (c.id);
        REQUIRE (v != nullptr);
        CHECK (v->getTitle().isNotEmpty());
    }

    Window::State s;
    s.view = "radial-spectrum";
    s.bounds = { -1800, 120, 900, 560 }; // a monitor left of the main one
    s.maximised = false;
    s.fullScreen = true;
    Window::State r;
    REQUIRE (Window::State::fromString (s.toString(), r));
    CHECK (r.view == "radial-spectrum");
    CHECK (r.bounds == s.bounds);
    CHECK (! r.maximised);
    CHECK (r.fullScreen);

    Window::State unknown;
    REQUIRE (Window::State::fromString ("no-such-view,10,20,800,500,1,0", unknown));
    CHECK (unknown.view == Window::kDefaultView);
    CHECK (unknown.maximised);
    Window::State tiny;
    REQUIRE (Window::State::fromString ("goniometer,10,20,50,40,0,0", tiny));
    CHECK (tiny.bounds.isEmpty()); // below the minimum size: the default size
    Window::State kept = s;
    CHECK (! Window::State::fromString ("", kept));
    CHECK (! Window::State::fromString ("spectrum,1,2,3", kept));
    CHECK (! Window::State::fromString ("spectrum,a,2,800,500,0,0", kept));
    CHECK (kept.view == "radial-spectrum"); // unchanged on failure
}

TEST_CASE ("App: visualiser window lifecycle: views fed by the host, full screen, reopen with the saved state")
{
    using Window = vis::VisualiserWindow;
    vis::VisualiserHost host;
    ui::MeterSnapshot meters;
    std::vector<float> mid (kFrame, 0.1f), side (kFrame, 0.0f);
    const auto frame = [&]
    {
        host.pushPost (mid.data(), side.data(), kFrame);
        host.advance (vis::FrameContext { meters, kDt, kRate });
    };

    Window::State saved;
    {
        Window window ("waterfall-3d", false);
        int stateChanges = 0;
        window.onStateChanged = [&stateChanges] { ++stateChanges; };
        window.onViewChanged = [&host] (vis::Visualiser* v) { host.setExtra (v); };
        host.setExtra (window.getView());
        CHECK (window.getViewId() == "waterfall-3d");
        REQUIRE (dynamic_cast<vis::Waterfall3D*> (window.getView()) != nullptr);
        CHECK (window.getView()->getParentComponent() == window.getContentComponent());
        CHECK (host.getNumCreated() == 0); // the window's view is its own, not the panel's
        frame();

        // Another view: the host feeds the new one; the old one is gone.
        window.setView ("radial-spectrum");
        CHECK (window.getViewId() == "radial-spectrum");
        CHECK (host.getExtra() == window.getView());
        REQUIRE (dynamic_cast<vis::RadialSpectrum*> (window.getView()) != nullptr);
        CHECK (stateChanges >= 1);
        frame();
        window.setView ("no-such-view"); // unknown: the default
        CHECK (window.getViewId() == Window::kDefaultView);
        window.stepView (1);
        CHECK (window.getViewId() == "spectrogram");
        window.stepView (-2); // wraps around to the last view
        CHECK (window.getViewId() == Window::choices().back().id);
        window.setView ("goniometer");

        // Full screen: borderless, header hidden after the idle time, back to the same bounds.
        window.setBounds (60, 80, 900, 560);
        CHECK (! window.isFullScreenMode());
        CHECK (window.isHeaderShown());
        const int changesBefore = stateChanges;
        auto* content = window.getContentComponent();
        REQUIRE (content != nullptr);
        CHECK (content->keyPressed (juce::KeyPress (juce::KeyPress::F11Key)));
        CHECK (window.isFullScreenMode());
        CHECK (window.getState().fullScreen);
        CHECK (window.getState().bounds == juce::Rectangle<int> (60, 80, 900, 560)); // the normal bounds are kept
        CHECK (window.getBorderThickness().getTop() == 0);
        CHECK (stateChanges > changesBefore);
        const auto viewBounds = window.getView()->getBounds();
        CHECK (viewBounds.getY() < 20); // the view fills the screen (the header floats over it)
        CHECK (content->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
        CHECK (! window.isFullScreenMode());
        CHECK (window.getBounds() == juce::Rectangle<int> (60, 80, 900, 560));
        CHECK (window.getView()->getY() > 30); // under the header again
        CHECK (! content->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey))); // Esc does nothing outside full screen
        window.setFullScreenMode (true);
        saved = window.getState();
        host.setExtra (nullptr); // the owner clears the feed before the window goes
    }
    CHECK (host.getExtra() == nullptr);

    // Reopen: the same view, normal bounds and full screen come back.
    Window::State reread;
    REQUIRE (Window::State::fromString (saved.toString(), reread));
    Window again (Window::kDefaultView, false);
    again.applyState (reread);
    CHECK (again.getViewId() == "goniometer");
    CHECK (again.isFullScreenMode());
    CHECK (again.getState().bounds == juce::Rectangle<int> (60, 80, 900, 560));
    again.setFullScreenMode (false);
    CHECK (again.getBounds() == juce::Rectangle<int> (60, 80, 900, 560));
}
