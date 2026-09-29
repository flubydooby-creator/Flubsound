// MixEngine idle freeze (docs/11 E45): a fed strip whose input and output
// stay below -120 dBFS for the idle hold stops running its chain, adds zeros
// and wakes with a pre-roll and a 5 ms fade.
//
// The null tests run two engines on the same input: `frozen` (freeze on) and
// `continuous` (freeze off), and compare their outputs sample by sample. The
// chain's control grids (the governor's 10 ms ticks, 25 / 50 / 100 ms meter
// and loudness steps) count processed samples, so the wake is placed where
// the frozen engine has skipped a whole number of seconds (the frozen blocks
// minus the pre-roll): at any other point those grids tick at another phase
// against the new signal, which is no fault of the freeze and is measured
// separately. Blocks are 10 ms, so the 10 ms pre-roll is one block at every
// rate.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/analysis/CallbackTiming.h"
#include "flub/engine/MixEngine.h"
#include "flub/io/PresetIO.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>

using namespace flub;
using namespace flubtest;

namespace
{
enum class Signal
{
    Impulse,
    Footstep, // a 70 Hz thump with a noise crunch, 120 ms
    Music     // kick, a 220 Hz line and a little noise
};

float signalSample (Signal kind, int k, double rate, int channel, FastRandom& rng)
{
    const double t = k / rate;
    switch (kind)
    {
        case Signal::Impulse:
            return k == 0 ? 0.8f : 0.0f;
        case Signal::Footstep:
            return t < 0.12 ? static_cast<float> (std::exp (-t * 40.0) * (0.5 * std::sin (kTwoPi * 70.0 * t) + 0.2 * rng.nextBipolar())) : 0.0f;
        case Signal::Music:
        default:
        {
            const double beat = std::fmod (t, 0.5);
            const double kick = std::exp (-beat * 18.0) * std::sin (kTwoPi * (50.0 + 80.0 * std::exp (-beat * 30.0)) * beat);
            return static_cast<float> (0.4 * kick + 0.15 * std::sin (kTwoPi * 220.0 * t + channel) + 0.05 * rng.nextBipolar());
        }
    }
}

struct NullResult
{
    double diffDb = 0.0;        // max |frozen - continuous| over the whole run, dBFS
    uint64_t frozenBlocks = 0;  // blocks the frozen engine skipped
    bool woke = false;          // the frozen strip ran again after the onset
    int64_t allocations = 0;    // heap allocations inside process() (both engines)
    double signalPeakDb = -300; // the continuous engine's output peak after the onset
};

/** Programme (0.5 s pink noise), silence until `frozen` freezes, `wakeAfterSeconds`
    of frozen silence, then `kind` for 1.5 s. wakeAfterSeconds whole: grid aligned. */
NullResult runNull (double rate, int channels, Signal kind, double holdSeconds, const preset::Preset* preset = nullptr,
                    double wakeAfterSeconds = 1.0)
{
    const int block = static_cast<int> (std::lround (rate / 100.0));
    const std::vector<StripConfig> layout { { "Strip", channels, 0.0f, false } };
    MixEngine frozen, continuous;
    for (MixEngine* m : { &frozen, &continuous })
    {
        m->configure (layout, rate, block);
        if (preset != nullptr)
        {
            preset::applyPresetToStore (*preset, m->params (0), param::Bank::A);
            if (m->needsReprepare())
                m->configure (layout, rate, block);
        }
        m->setIdleHoldSeconds (holdSeconds);
    }
    continuous.setIdleFreeze (false);

    const int leadIn = static_cast<int> (0.5 * rate);
    const auto programme = pinkNoise (leadIn, 0.1f, 99);
    Planar in (channels, block), inCopy (channels, block), outFrozen (2, block), outContinuous (2, block);
    FastRandom rng (5);
    const int wakeBlocks = static_cast<int> (std::lround (wakeAfterSeconds * 100.0));

    NullResult r;
    double maxDiff = 0.0, peak = 0.0;
    int onsetBlock = -1, signalPos = 0;
    for (int b = 0, pos = 0;; ++b)
    {
        for (int i = 0; i < block; ++i, ++pos)
        {
            FastRandom shared = rng; // one noise value for every channel of this sample
            for (int c = 0; c < channels; ++c)
            {
                float v = 0.0f;
                if (pos < leadIn)
                    v = programme[static_cast<size_t> (pos)];
                else if (onsetBlock >= 0 && b >= onsetBlock)
                {
                    FastRandom local = shared;
                    v = signalSample (kind, signalPos, rate, c, local);
                    if (c == channels - 1)
                        rng = local;
                }
                in.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] = inCopy.ch[static_cast<size_t> (c)][static_cast<size_t> (i)] = v;
            }
            if (onsetBlock >= 0 && b >= onsetBlock)
                ++signalPos;
        }
        const AudioBlock x = in.block(), y = inCopy.block();
        const AudioBlock* xs[] = { &x };
        const AudioBlock* ys[] = { &y };
        {
            AllocationGuard guard;
            frozen.process (xs, outFrozen.block());
            continuous.process (ys, outContinuous.block());
            r.allocations += guard.allocations();
        }
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
            {
                const float a = outFrozen.ch[static_cast<size_t> (c)][static_cast<size_t> (i)];
                const float e = outContinuous.ch[static_cast<size_t> (c)][static_cast<size_t> (i)];
                maxDiff = std::max (maxDiff, static_cast<double> (std::abs (a - e)));
                if (onsetBlock >= 0 && b >= onsetBlock)
                    peak = std::max (peak, static_cast<double> (std::abs (e)));
            }

        // Frozen from the next block on: skip wakeBlocks + 1 blocks, the
        // pre-roll gives one back.
        if (onsetBlock < 0 && frozen.isStripFrozen (0))
            onsetBlock = b + 2 + wakeBlocks;
        if (onsetBlock >= 0 && b == onsetBlock)
            r.woke = ! frozen.isStripFrozen (0);
        if (onsetBlock >= 0 && b >= onsetBlock + 150)
            break;
        if (b > 100 * 60) // never froze
            break;
    }
    r.diffDb = toDb (maxDiff);
    r.frozenBlocks = frozen.getStripFrozenBlocks (0);
    r.signalPeakDb = toDb (peak);
    return r;
}

void report (const char* what, double value, const char* unit)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.1f", value);
    std::cout << "    measured " << what << " = " << buf << " " << unit << "\n";
}

void checkNullAtRate (double rate)
{
    for (const Signal kind : { Signal::Impulse, Signal::Footstep, Signal::Music })
    {
        const auto r = runNull (rate, 2, kind, 0.5);
        CHECK (r.frozenBlocks >= 100); // it did freeze, for the whole second
        CHECK (r.woke);
        CHECK (r.allocations == 0);
        CHECK_GE (r.signalPeakDb, -20.0); // the signal did come out
        CHECK_LE (r.diffDb, -90.0);
    }
}

std::optional<preset::Preset> factoryPreset (const std::string& file)
{
    preset::Preset p;
    std::string error;
    if (! preset::load (std::string (FLUB_PRESET_DIR) + "/" + file, p, error))
        return std::nullopt;
    return p;
}
} // namespace

TEST_CASE ("Idle freeze (E45): frozen-then-resumed strip nulls against continuous processing at 44.1 kHz (impulse, footstep, music)")
{
    checkNullAtRate (44100.0);
}

TEST_CASE ("Idle freeze (E45): frozen-then-resumed strip nulls against continuous processing at 48 kHz (impulse, footstep, music)")
{
    checkNullAtRate (48000.0);
}

TEST_CASE ("Idle freeze (E45): frozen-then-resumed strip nulls against continuous processing at 96 kHz (impulse, footstep, music)")
{
    checkNullAtRate (96000.0);
}

TEST_CASE ("Idle freeze (E45): a preset with control loops (MOBA & Strategy: compressor, dynamic EQ, Gaming macros) nulls on a footstep")
{
    // Its control state has settled 2 s into the silence (the default hold
    // is 10 s; a shorter one keeps the test fast), and the wake lands on its
    // grids, so it continues from the state continuous processing has.
    const auto p = factoryPreset ("gaming-moba-strategy.json");
    REQUIRE (p.has_value());
    const auto r = runNull (48000.0, 2, Signal::Footstep, 2.0, &*p);
    CHECK (r.frozenBlocks >= 100);
    CHECK (r.woke);
    CHECK (r.allocations == 0);
    CHECK_GE (r.signalPeakDb, -20.0);
    CHECK_LE (r.diffDb, -90.0);
    report ("MOBA footstep, wake 1 s after the freeze: |frozen - continuous|", r.diffDb, "dBFS");
}

TEST_CASE ("Idle freeze (E45): a strip with no silence long enough is bit-identical with the freeze on and off")
{
    // Programme with quiet stretches just above the floor (-110 dBFS) and
    // digital silence shorter than the hold: the freeze never engages and
    // the output is the same bit for bit.
    constexpr double rate = 48000.0;
    constexpr int block = 256;
    const std::vector<StripConfig> layout { { "Strip", 2, 0.0f, false } };
    MixEngine on, off;
    on.configure (layout, rate, block);
    off.configure (layout, rate, block);
    on.setIdleHoldSeconds (0.5);
    off.setIdleFreeze (false);

    const int total = static_cast<int> (4.0 * rate);
    const auto noise = pinkNoise (total, 0.1f, 17);
    Planar a (2, total), b (2, total), outOn (2, total), outOff (2, total);
    for (int i = 0; i < total; ++i)
    {
        const double t = i / rate;
        float v = noise[static_cast<size_t> (i)];
        if (t >= 1.0 && t < 1.4)
            v = 0.0f; // 0.4 s of digital silence (< the 0.5 s hold)
        else if (t >= 2.0 && t < 3.0)
            v = static_cast<float> (3.2e-6 * std::sin (kTwoPi * 440.0 * t)); // -110 dBFS: not silence
        a.ch[0][static_cast<size_t> (i)] = a.ch[1][static_cast<size_t> (i)] = v;
        b.ch[0][static_cast<size_t> (i)] = b.ch[1][static_cast<size_t> (i)] = v;
    }
    for (int pos = 0; pos < total; pos += block)
    {
        const int len = std::min (block, total - pos);
        const AudioBlock x = a.block (pos, len), y = b.block (pos, len);
        const AudioBlock* xs[] = { &x };
        const AudioBlock* ys[] = { &y };
        on.process (xs, outOn.block (pos, len));
        off.process (ys, outOff.block (pos, len));
    }
    CHECK (on.getStripFrozenBlocks (0) == 0);
    CHECK (outOn.ch[0] == outOff.ch[0]);
    CHECK (outOn.ch[1] == outOff.ch[1]);
}

TEST_CASE ("Idle freeze (E45): a fed-silence strip's callback time drops by at least 90 % (CallbackTiming)")
{
    // Three engines on the same 48 kHz / 256 blocks: a strip fed zeros with
    // the freeze on (frozen after 0.1 s), the same with it off, and no input
    // at all (the master limiter and device correction only). The strip's
    // own cost is the difference to the unfed engine. Blocks are timed
    // round robin, so a clock or cache change hits all three alike.
    constexpr double rate = 48000.0;
    constexpr int block = 256;
    const std::vector<StripConfig> layout { { "Game", 8, 0.0f, false } };
    MixEngine frozen, running, unfed;
    for (MixEngine* m : { &frozen, &running, &unfed })
        m->configure (layout, rate, block);
    frozen.setIdleHoldSeconds (0.1);
    running.setIdleFreeze (false);

    Planar zeros (8, block), out (2, block);
    const AudioBlock z = zeros.block();
    const AudioBlock* fed[] = { &z };
    const AudioBlock* none[] = { nullptr };
    for (int b = 0; b < 40; ++b) // warm up: 0.2 s, frozen after 0.1 s + its latency
    {
        frozen.process (fed, out.block());
        running.process (fed, out.block());
        unfed.process (none, out.block());
    }
    REQUIRE (frozen.isStripFrozen (0));

    const auto periodNs = static_cast<uint64_t> (block * 1.0e9 / rate);
    CallbackTiming timing[3];
    const auto now = [] { return static_cast<uint64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count()); };
    for (int b = 0; b < 1500; ++b)
        for (int e = 0; e < 3; ++e)
        {
            MixEngine& m = e == 0 ? frozen : (e == 1 ? running : unfed);
            const uint64_t t0 = now();
            m.process (e == 2 ? none : fed, out.block());
            const uint64_t t1 = now();
            timing[e].record (t0, t1 - t0, periodNs);
        }
    CHECK (frozen.isStripFrozen (0));
    // The median callback of each (robust against a preempted block).
    const double frozenNs = timing[0].snapshot().duration.percentileNs (0.5);
    const double runningNs = timing[1].snapshot().duration.percentileNs (0.5);
    const double unfedNs = timing[2].snapshot().duration.percentileNs (0.5);
    const double stripRunning = runningNs - unfedNs, stripFrozen = std::max (0.0, frozenNs - unfedNs);
    REQUIRE (stripRunning > 0.0);
    report ("7.1 strip fed silence, running", stripRunning / 1000.0, "us per 256-sample block");
    report ("7.1 strip fed silence, frozen", stripFrozen / 1000.0, "us per 256-sample block");
    report ("fed-silence strip CPU saved", 100.0 * (1.0 - stripFrozen / stripRunning), "%");
    CHECK_LE (stripFrozen, 0.1 * stripRunning);
}

TEST_CASE ("Idle freeze (E45): input under the floor keeps a strip frozen, the first sample above it wakes it, and the strip gain glides while frozen")
{
    constexpr double rate = 48000.0;
    constexpr int block = 480;
    const std::vector<StripConfig> layout { { "Strip", 2, 0.0f, false } };
    MixEngine m;
    m.configure (layout, rate, block);
    m.setIdleHoldSeconds (0.05);

    // The strip processes its input in place: every block is filled anew.
    Planar in (2, block), out (2, block);
    const auto run = [&] (int channel = -1, int index = 0, float value = 0.0f) {
        for (auto& c : in.ch)
            std::fill (c.begin(), c.end(), 0.0f);
        if (channel >= 0)
            in.ch[static_cast<size_t> (channel)][static_cast<size_t> (index)] = value;
        const AudioBlock x = in.block();
        const AudioBlock* xs[] = { &x };
        m.process (xs, out.block());
    };
    const auto runUntilFrozen = [&] {
        for (int b = 0; b < 40 && ! m.isStripFrozen (0); ++b)
            run();
    };
    runUntilFrozen();
    REQUIRE (m.isStripFrozen (0));

    // -126 dBFS keeps it frozen; -100 dBFS wakes it in that block.
    run (0, 10, 5.0e-7f);
    CHECK (m.isStripFrozen (0));
    run (1, 300, 1.0e-5f);
    CHECK (! m.isStripFrozen (0));

    // Frozen again, then muted: the 20 ms glide to 0 runs while frozen, so
    // an impulse that wakes the strip 100 ms later is silent. Had the gain
    // stood still, it would glide down only after the wake and let the
    // impulse through at the strip's latency (5.4 ms into a 20 ms glide).
    runUntilFrozen();
    REQUIRE (m.isStripFrozen (0));
    m.setStripMuted (0, true);
    for (int b = 0; b < 10; ++b)
        run();
    run (0, 0, 0.5f);
    CHECK (! m.isStripFrozen (0));
    CHECK (peakAbs (out.ch[0].data(), block) == 0.0);
    run();
    CHECK (peakAbs (out.ch[0].data(), block) == 0.0);

    // Unmuted and awake, the same impulse does come out.
    m.setStripMuted (0, false);
    for (int b = 0; b < 5; ++b)
        run();
    run (0, 0, 0.5f);
    CHECK_GE (peakAbs (out.ch[0].data(), block), 0.1);
}
