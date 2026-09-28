// Discontinuity detector and the CLI soak (docs/11 E53 steps 2 and 4, Linux
// slice): the detector (flub/analysis/Discontinuity.h) reads nothing on
// clean programme and flags each injected break once, as the right type and
// at the right frame; the soak (tools/flubsound-cli/Soak.h) runs the chain on
// generated programme with parameter automation for 10 s and reports none,
// and flags a 1-sample skip injected into its stream.
#include "TestFramework.h"
#include "TestSignals.h"

#include "CliOptions.h"
#include "Soak.h"

#include "flub/analysis/Discontinuity.h"
#include "flub/common/AudioBlock.h"
#include "flub/common/Math.h"
#include "flub/engine/Parameters.h"
#include "flub/engine/ProcessingChain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::cli;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kN = 48000; // 1 s

/** Runs a detector over mono `x` (as one channel) in blocks of `block`. */
DiscontinuityDetector detect (const std::vector<float>& x, int block = 512, const DiscontinuitySettings& s = DiscontinuitySettings {})
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

    // Below the floor (-50 dBFS residual) nothing counts, whatever the ratio.
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

TEST_CASE ("Discontinuity detector: waveform structure is not a click - a clipped buzzy voice recurs every period, a kink breaks the slope only; zipper noise at the block rate is")
{
    // A 150 Hz voice with 20 harmonics (1/k), hard-clipped at 0.2: a spike
    // at every clipped corner, once per period.
    std::vector<float> voice (static_cast<size_t> (kN));
    for (int i = 0; i < kN; ++i)
    {
        double v = 0.0;
        for (int k = 1; k <= 20; ++k)
            v += 0.3 / k * std::sin (kTwoPi * 150.0 * k * i / kFs);
        voice[static_cast<size_t> (i)] = static_cast<float> (std::clamp (v, -0.2, 0.2));
    }
    auto d = detect (voice);
    CHECK (d.total() == 0);
    CHECK (d.recurring() + d.kinks() > 0);

    // One change of slope (K = 0.002 per sample, decaying) on a 60 Hz tone:
    // its 4th difference peaks at 2 K = -48 dBFS, over the floor - a kink.
    // A step of the same spike (3 J = 2 K) is a click.
    const int at = 24011;
    auto kink = sine (60.0, kFs, kN, 0.3f), step = kink;
    for (int i = at; i < kN; ++i)
    {
        const double t = i - at;
        kink[static_cast<size_t> (i)] += static_cast<float> (0.002 * t * std::exp (-t / 50.0));
        step[static_cast<size_t> (i)] += static_cast<float> (0.004 / 3.0);
    }
    d = detect (kink);
    CHECK (d.total() == 0 && d.kinks() == 1);
    CHECK (onlyEvent (detect (step), DiscontinuityType::Click, at, 4));

    // Zipper: a 220 Hz tone whose gain steps +0.1 dB at every 512th sample
    // for 20 blocks. Told the block size, each step is a click; not told,
    // the steps recur and read as structure.
    auto zipper = sine (220.0, kFs, kN, 0.3f);
    for (int i = 0; i < kN; ++i)
    {
        const int steps = std::clamp ((i - 10240) / 512 + 1, 0, 20);
        zipper[static_cast<size_t> (i)] *= static_cast<float> (std::pow (10.0, 0.1 * steps / 20.0));
    }
    DiscontinuitySettings s;
    s.blockSize = 512;
    CHECK (detect (zipper, 512, s).count (DiscontinuityType::Click) >= 15);
    CHECK (detect (zipper).count (DiscontinuityType::Click) == 0);
}

// ===========================================================================
// Soak (tools/flubsound-cli/Soak.h)
// ===========================================================================
namespace
{
std::vector<float> defaults()
{
    std::vector<float> v (static_cast<size_t> (param::kNumParams));
    for (int id = 0; id < param::kNumParams; ++id)
        v[static_cast<size_t> (id)] = param::layout()[static_cast<size_t> (id)].defaultValue;
    return v;
}

/** The factory presets, as `flubsound-cli soak` loads them. */
void addFactoryPresets (SoakSettings& s)
{
    for (const char* name : { "Flubsound Signature", "Competitive FPS", "Punchy Pop", "Late Night", "Lo-Fi Chill" })
    {
        CliOptions o;
        std::string error;
        REQUIRE (parseCommandLine ({ "soak", "--preset", name }, o, error));
        o.render.presetDir = FLUB_PRESET_DIR;
        ResolvedParameters p;
        REQUIRE (buildParameters (o.render, p, error));
        s.presets.push_back (p.values);
        s.presetNames.push_back (name);
    }
}
} // namespace

TEST_CASE ("Soak: the programme is deterministic, stays under full scale, and reads clean to the detector (its own self-check)")
{
    SoakProgramme a (kFs, 7), b (kFs, 7), c (kFs, 8);
    const int n = static_cast<int> (SoakProgramme::kSceneSeconds * kFs * 5); // one cycle of the five scenes
    std::vector<float> al (static_cast<size_t> (n)), ar (al), bl (al), br (al), cl (al), cr (al);
    a.render (al.data(), ar.data(), n);
    for (int pos = 0; pos < n; pos += 480) // another block size: the same stream
        b.render (bl.data() + pos, br.data() + pos, std::min (480, n - pos));
    c.render (cl.data(), cr.data(), n);
    CHECK (al == bl && ar == br);
    CHECK (al != cl);
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
        peak = std::max ({ peak, std::abs (al[static_cast<size_t> (i)]), std::abs (ar[static_cast<size_t> (i)]) });
    CHECK (peak < 0.891f); // -1 dBFS
    CHECK (peak > 0.3f);
    // The fade scene (24 .. 30 s) fades out over 26.9 .. 27.0 s and holds
    // digital silence until 28.0 s.
    const auto silentFrom = static_cast<size_t> (27.01 * kFs), silentTo = static_cast<size_t> (27.99 * kFs);
    CHECK (std::all_of (al.begin() + static_cast<std::ptrdiff_t> (silentFrom), al.begin() + static_cast<std::ptrdiff_t> (silentTo), [] (float v) { return v == 0.0f; }));
    DiscontinuityDetector d;
    d.prepare (kFs, 2);
    const float* ch[] = { al.data(), ar.data() };
    d.process (ch, n);
    d.finish();
    CHECK (d.total() == 0);
}

TEST_CASE ("Soak: 10 s of the chain under user automation (E53 CI soak): no click, dropout, NaN or DC step")
{
    SoakSettings s;
    s.seconds = 10.0;
    s.seed = 1;
    addFactoryPresets (s);
    SoakReport r;
    std::string error;
    REQUIRE (soakChain (defaults(), s, r, error));
    CHECK (r.frames == 480000);
    CHECK (r.actions >= 25 && r.actions <= 60); // one per 250 ms +- 50 %
    CHECK (r.inputTotal() == 0);
    CHECK (r.output[static_cast<size_t> (DiscontinuityType::Dropout)] == 0);
    CHECK (r.output[static_cast<size_t> (DiscontinuityType::NonFinite)] == 0);
    CHECK (r.output[static_cast<size_t> (DiscontinuityType::DcStep)] == 0);
    CHECK (r.outputPeakDbfs <= -0.9f); // the ceiling (-1 dBTP) holds through every action
    // docs/11 E53 Done-when: 0 discontinuities. Every click this soak found
    // was the global bypass engaging (about 1.4 ms after "bypass -> on"),
    // fixed in ProcessingChain step 7 (see the KnownGap closed case below).
    CHECK (r.output[static_cast<size_t> (DiscontinuityType::Click)] == 0);
    CHECK (r.detections.empty());
}

TEST_CASE ("Soak: a 1-sample skip, a 256-frame dropout and a NaN injected into the output stream are each flagged once, where they are")
{
    SoakSettings s;
    s.seconds = 3.0;
    s.automation = SoakAutomation::Off;
    const int64_t skipAt = 53768, dropAt = 90007, nanAt = 120003; // 1.12 / 1.88 / 2.5 s: the music scene
    std::array<float, 2> previous {};
    s.inject = [&] (int64_t start, float* const* out, int n) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                const int64_t f = start + i;
                // Skip: everything before skipAt is one sample late, so sample
                // skipAt - 1 never appears.
                const float v = out[c][i];
                if (f < skipAt)
                {
                    out[c][i] = previous[static_cast<size_t> (c)];
                    previous[static_cast<size_t> (c)] = v;
                }
                if (f >= dropAt && f < dropAt + 256)
                    out[c][i] = 0.0f;
                if (f == nanAt && c == 0)
                    out[c][i] = std::numeric_limits<float>::quiet_NaN();
            }
    };
    SoakReport r;
    std::string error;
    REQUIRE (soakChain (defaults(), s, r, error));
    CHECK (r.inputTotal() == 0);
    CHECK (r.output[static_cast<size_t> (DiscontinuityType::Click)] == 2); // the skip, on both channels
    CHECK (r.output[static_cast<size_t> (DiscontinuityType::Dropout)] == 2);
    CHECK (r.output[static_cast<size_t> (DiscontinuityType::NonFinite)] == 1);
    CHECK (r.output[static_cast<size_t> (DiscontinuityType::DcStep)] == 0);
    for (const auto& d : r.detections)
    {
        CHECK (! d.bypassed);
        if (d.event.type == DiscontinuityType::Click)
            CHECK (std::abs (d.event.frame - skipAt) <= 4);
        if (d.event.type == DiscontinuityType::Dropout)
            CHECK (d.event.frame == dropAt && d.event.length == 256);
        if (d.event.type == DiscontinuityType::NonFinite)
            CHECK (d.event.frame == nanAt && d.event.channel == 0);
    }
    const auto injected = soakToJson (r);
    REQUIRE (! injected["detections"].asArray().empty());
    CHECK (injected["detections"].asArray().front()["bypassed"].isBool());
    CHECK (! injected["detections"].asArray().front()["bypassed"].asBool (true));

    // The same run without the injections is clean, and its JSON says so.
    s.inject = {};
    REQUIRE (soakChain (defaults(), s, r, error));
    CHECK (r.outputTotal() == 0);
    const auto j = soakToJson (r);
    CHECK (j["output"]["total"].asNumber() == 0.0);
    CHECK (j["automation"].asString() == "off");
    CHECK (j["detections"].asArray().empty());
    CHECK (formatSoak (r).find ("no discontinuities") != std::string::npos);
}

TEST_CASE ("KnownGap: a 1-sample skip in the chain's music output is flagged at 11 of 32 places (the others sit where the waveform barely moves)")
{
    // 6 s of the soak's music scene through the defaults, captured, then 32
    // skips 7919 frames apart. A skip is a break of one sample's worth of
    // slope: where the programme barely moves (a bass wave near its peak)
    // that is under the -50 dBFS floor, or a change of slope only.
    SoakSettings s;
    s.seconds = 6.0;
    s.automation = SoakAutomation::Off;
    std::vector<float> left;
    s.inject = [&left] (int64_t, float* const* out, int n) { left.insert (left.end(), out[0], out[0] + n); };
    SoakReport r;
    std::string error;
    REQUIRE (soakChain (defaults(), s, r, error));
    REQUIRE (left.size() == 288000);
    CHECK (detect (left).total() == 0);
    int flagged = 0, injected = 0;
    for (int64_t at = 275500; at >= 30011; at -= 7919, ++injected) // from the end, so earlier positions stay put
        left.erase (left.begin() + static_cast<std::ptrdiff_t> (at));
    const auto d = detect (left);
    for (int k = 0; k < injected; ++k)
    {
        const int64_t at = 30011 + 7919 * k - k; // where skip k sits once the k before it are gone
        flagged += std::any_of (d.events().begin(), d.events().end(), [at] (const Discontinuity& e) { return std::abs (e.frame - at) <= 4; }) ? 1 : 0;
    }
    CHECK (injected == 32);
    CHECK (d.total() == flagged); // nothing else
    // KNOWN_GAP: target every skip per docs/11 E53 Done-when ("an injected
    // 1-sample skip is flagged"); a phase-locked tone predictor (E53's
    // real-time soak) sees the skip wherever it falls.
    CHECK (flagged >= 11); // 11 today
}

TEST_CASE ("KnownGap closed: engaging the global bypass is click-free - the crossfade waits for the dry path's true-peak limiter (E53 soak finding)")
{
    // A 110 Hz tone at -10 dBFS through the defaults; bypass on (or off) at
    // block 100. The dry reference passes a true-peak limiter that runs only
    // while bypass is engaged (ProcessingChain step 7): started cold it
    // outputs silence for its latency (67 samples). The crossfade used to
    // start at once, so the dry path entered it as a step of ~4.7 % of the
    // dry signal (one click, -37.8 dB, 68 samples in); it now waits for the
    // limiter's latency.
    const auto run = [] (bool engage) {
        auto store = std::make_unique<param::ParameterStore>();
        store->setActiveBank (param::Bank::A);
        if (! engage)
            store->set (param::BypassAll, 1.0f);
        auto chain = std::make_unique<ProcessingChain> (*store);
        chain->prepare ({ kFs, 512, 2 });
        AudioBuffer io (2, 512);
        io.clear();
        chain->process (io.block (2, 512));
        chain->reset();
        std::vector<float> out;
        for (int blk = 0; blk < 200; ++blk)
        {
            if (blk == 100)
                store->set (param::BypassAll, engage ? 1.0f : 0.0f);
            for (int i = 0; i < 512; ++i)
                io.channel (0)[i] = io.channel (1)[i] = static_cast<float> (0.3 * std::sin (kTwoPi * 110.0 * (blk * 512 + i) / kFs));
            chain->process (io.block (2, 512));
            out.insert (out.end(), io.channel (0), io.channel (0) + 512);
        }
        return detect (out);
    };
    CHECK (run (true).total() == 0);  // engaging (was one click)
    CHECK (run (false).total() == 0); // disengaging
}
