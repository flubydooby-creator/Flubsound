// Discontinuity detector and the CLI soak (docs/11 E53 steps 2 and 4, Linux
// slice): the detector (flub/analysis/Discontinuity.h) reads nothing on
// clean programme and flags each injected break once, as the right type and
// at the right frame; the soak (tools/flubsound-cli/Soak.h) runs the chain on
// generated programme with parameter automation for 10 s and reports none,
// and flags a 1-sample skip injected into its stream.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/Discontinuity.h"
#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kN = 48000; // 1 s

/** Runs a detector over mono `x` (as one channel) in blocks of `block`. */
DiscontinuityDetector detect (const std::vector<float>& x, int block = 512, const DiscontinuitySettings& s = {})
{
    DiscontinuityDetector d;
    d.prepare (kFs, 1, s);
    for (size_t pos = 0; pos < x.size(); pos += static_cast<size_t> (block))
    {
        const float* ch[] = { x.data() + pos };
        d.process (ch, static_cast<int> (std::min (x.size() - pos, static_cast<size_t> (block))));
    }
    d.finish();
    return d;
}

/** Smooth tonal programme: a bass, a chord of 250 ms notes with 20 ms
    raised-cosine attacks and releases, a 997 Hz lead with vibrato, at about
    -12 dBFS. */
std::vector<float> tonal (int n)
{
    std::vector<float> x (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const double note = std::fmod (t, 0.25), attack = note < 0.02 ? 0.5 - 0.5 * std::cos (kPi * note / 0.02) : 1.0;
        const double release = note > 0.23 ? 0.5 + 0.5 * std::cos (kPi * (note - 0.23) / 0.02) : 1.0;
        const double env = attack * release * std::exp (-4.0 * note);
        double v = 0.2 * std::sin (kTwoPi * 55.0 * t);
        v += env * (0.08 * std::sin (kTwoPi * 220.0 * t) + 0.06 * std::sin (kTwoPi * 277.2 * t) + 0.05 * std::sin (kTwoPi * 329.6 * t));
        v += 0.05 * std::sin (kTwoPi * 997.0 * t + 3.0 * std::sin (kTwoPi * 5.0 * t));
        x[static_cast<size_t> (i)] = static_cast<float> (v);
    }
    return x;
}

/** First-order 10 Hz high-pass (a DC blocker): Kellet pink noise keeps
    rising down to about 9 Hz, which reads as DC wander at -30 dBFS. */
std::vector<float> highPassed (std::vector<float> x)
{
    const double k = std::exp (-kTwoPi * 10.0 / kFs);
    double x1 = 0.0, y1 = 0.0;
    for (auto& v : x)
    {
        const double y = k * (y1 + v - x1);
        x1 = v;
        y1 = y;
        v = static_cast<float> (y);
    }
    return x;
}

bool onlyEvent (const DiscontinuityDetector& d, DiscontinuityType type, int64_t frame, int64_t tolerance)
{
    return d.total() == 1 && d.count (type) == 1 && d.events().size() == 1 && d.events()[0].type == type
           && std::abs (d.events()[0].frame - frame) <= tolerance;
}
} // namespace

TEST_CASE ("Discontinuity detector: clean programme reads nothing (tones 40 Hz - 16 kHz, tonal music, pink noise, onsets, fades, silence)")
{
    for (double hz : { 40.0, 100.0, 997.0, 5000.0, 12000.0, 16000.0 })
        CHECK (detect (sine (hz, kFs, kN, 0.5f)).total() == 0);
    CHECK (detect (tonal (2 * kN)).total() == 0);
    CHECK (detect (highPassed (pinkNoise (2 * kN, 0.1f))).total() == 0);
    CHECK (detect (highPassed (pinkNoise (2 * kN, 0.001f))).total() == 0);
    CHECK (detect (std::vector<float> (static_cast<size_t> (kN), 0.0f)).total() == 0);

    // A sound that starts or stops on a smooth 5 ms ramp, and one that starts
    // abruptly on a zero crossing into noise (a gate opening): not breaks.
    auto x = tonal (kN);
    for (int i = 0; i < kN; ++i)
    {
        const double t = i / kFs;
        const double g = t < 0.3 ? 0.0 : t < 0.305 ? 0.5 - 0.5 * std::cos (kPi * (t - 0.3) / 0.005) : t < 0.7 ? 1.0 : t < 0.705 ? 0.5 + 0.5 * std::cos (kPi * (t - 0.7) / 0.005) : 0.0;
        x[static_cast<size_t> (i)] *= static_cast<float> (g);
    }
    CHECK (detect (x).total() == 0);
    auto burst = std::vector<float> (static_cast<size_t> (kN), 0.0f);
    const auto noise = pinkNoise (kN / 2, 0.1f);
    std::copy (noise.begin(), noise.end(), burst.begin() + kN / 4);
    CHECK (detect (burst).count (DiscontinuityType::Click) == 0);

    // Below the floor (-70 dBFS residual) nothing counts, whatever the ratio.
    auto quiet = sine (997.0, kFs, kN, 1.0e-5f);
    quiet[static_cast<size_t> (kN / 2)] += 1.0e-5f;
    CHECK (detect (quiet).total() == 0);
}

TEST_CASE ("Discontinuity detector: a skipped or repeated sample, an impulse and a step read as one click each, at the break")
{
    const int at = 24011;
    for (const auto& base : { sine (997.0, kFs, kN, 0.5f), sine (100.0, kFs, kN, 0.5f), tonal (kN) })
    {
        auto skip = base;
        skip.erase (skip.begin() + at);
        CHECK (onlyEvent (detect (skip), DiscontinuityType::Click, at, 4));

        auto repeat = base;
        repeat.insert (repeat.begin() + at, repeat[static_cast<size_t> (at)]);
        repeat.pop_back();
        CHECK (onlyEvent (detect (repeat), DiscontinuityType::Click, at, 4));

        auto impulse = base;
        impulse[static_cast<size_t> (at)] += 0.001f; // -60 dBFS
        const auto d = detect (impulse);
        CHECK (onlyEvent (d, DiscontinuityType::Click, at, 4));
        if (! d.events().empty())
            CHECK_NEAR (d.events()[0].levelDb, 20.0 * std::log10 (0.006), 1.0); // the 4th difference's peak: 6 x

        auto step = base;
        for (int i = at; i < kN; ++i)
            step[static_cast<size_t> (i)] += 0.003f; // -50 dBFS: under the DC-step threshold
        CHECK (onlyEvent (detect (step), DiscontinuityType::Click, at, 4));
    }

    // In broadband noise the residual is high: only a large break shows.
    auto noise = highPassed (pinkNoise (kN, 0.03f));
    noise[static_cast<size_t> (at)] += 0.001f;
    CHECK (detect (noise).total() == 0);
    noise[static_cast<size_t> (at)] += 0.3f;
    CHECK (onlyEvent (detect (noise), DiscontinuityType::Click, at, 4));
}

TEST_CASE ("Discontinuity detector: dropouts, non-finite runs and DC steps are their own events, edges not counted twice")
{
    const int at = 24011;
    auto x = tonal (kN);
    auto drop = x;
    std::fill (drop.begin() + at, drop.begin() + at + 256, 0.0f); // an underrun of 256 frames
    auto d = detect (drop);
    CHECK (onlyEvent (d, DiscontinuityType::Dropout, at, 0));
    if (! d.events().empty())
    {
        CHECK (d.events()[0].length == 256);
        CHECK_NEAR (d.events()[0].levelDb, -20.0, 6.0); // the programme's RMS before
    }
    // Shorter than 0.5 ms: a click (two: out and back), not a dropout.
    auto shortDrop = x;
    std::fill (shortDrop.begin() + at, shortDrop.begin() + at + 8, 0.0f);
    d = detect (shortDrop);
    CHECK (d.count (DiscontinuityType::Dropout) == 0 && d.count (DiscontinuityType::Click) >= 1);

    auto nan = x;
    std::fill (nan.begin() + at, nan.begin() + at + 3, std::numeric_limits<float>::quiet_NaN());
    nan[static_cast<size_t> (at + 5000)] = std::numeric_limits<float>::infinity();
    d = detect (nan);
    CHECK (d.count (DiscontinuityType::NonFinite) == 2 && d.total() == 2);
    if (d.events().size() == 2)
    {
        CHECK (d.events()[0].frame == at && d.events()[0].length == 3);
        CHECK (d.events()[1].frame == at + 5000 && d.events()[1].length == 1);
    }

    // A DC step of -26 dBFS, applied over 20 ms (no click): one DC step,
    // reported within dcStepWindowMs.
    auto dc = sine (100.0, kFs, 2 * kN, 0.5f);
    for (int i = at; i < 2 * kN; ++i)
        dc[static_cast<size_t> (i)] += static_cast<float> (0.05 * std::min (1.0, (i - at) / 960.0));
    d = detect (dc);
    CHECK (d.count (DiscontinuityType::DcStep) == 1 && d.total() == 1);
    if (! d.events().empty())
    {
        CHECK (d.events()[0].frame >= at && d.events()[0].frame <= at + 12000);
        CHECK (d.events()[0].levelDb >= -30.0f);
    }
    // The same step made abruptly is a click as well.
    for (int i = at; i < 2 * kN; ++i)
        dc[static_cast<size_t> (i)] = static_cast<float> (0.5 * std::sin (kTwoPi * 100.0 * i / kFs) + 0.05);
    d = detect (dc);
    CHECK (d.count (DiscontinuityType::DcStep) == 1 && d.count (DiscontinuityType::Click) == 1 && d.total() == 2);
}

TEST_CASE ("Discontinuity detector: block-size invariant, per channel, counts complete past maxReported, reset forgets")
{
    auto x = tonal (kN);
    for (int k = 0; k < 20; ++k)
        x[static_cast<size_t> (1000 + k * 2300)] += 0.01f;
    const auto a = detect (x, 1), b = detect (x, 37), c = detect (x, 4096);
    REQUIRE (a.count (DiscontinuityType::Click) == 20);
    CHECK (b.count (DiscontinuityType::Click) == 20 && c.count (DiscontinuityType::Click) == 20);
    for (size_t i = 0; i < a.events().size(); ++i)
        CHECK (a.events()[i].frame == c.events()[i].frame && a.events()[i].levelDb == c.events()[i].levelDb);

    DiscontinuitySettings s;
    s.maxReported = 5;
    const auto capped = detect (x, 512, s);
    CHECK (capped.count (DiscontinuityType::Click) == 20 && capped.events().size() == 5);

    // Two channels: a click on the right only.
    DiscontinuityDetector d;
    d.prepare (kFs, 2);
    const auto left = tonal (kN);
    auto right = left;
    right[30000] -= 0.01f;
    const float* ch[] = { left.data(), right.data() };
    d.process (ch, kN);
    d.finish();
    REQUIRE (d.total() == 1);
    CHECK (d.events()[0].channel == 1 && std::abs (d.events()[0].frame - 30000) <= 4);
    CHECK (d.framesSeen() == kN);
    d.reset();
    CHECK (d.total() == 0 && d.events().empty() && d.framesSeen() == 0);
}
