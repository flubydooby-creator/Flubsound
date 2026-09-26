// Tests for the multi-band dynamic EQ: gain computer modes, range and noise
// floor, stereo linking, detector shapes, timing, click-free switching,
// real-time safety, robustness and block-size invariance.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/dsp/DynamicEq.h"

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

float dbfs (double db) { return static_cast<float> (std::pow (10.0, db / 20.0)); }

DynEqBandParams makeBand (DynEqMode mode, EqBandType shape, float freq, float q, float thresholdDb, float ratio, float rangeDb)
{
    DynEqBandParams p;
    p.enabled = true;
    p.mode = mode;
    p.shape = shape;
    p.frequency = freq;
    p.q = q;
    p.thresholdDb = thresholdDb;
    p.ratio = ratio;
    p.rangeDb = rangeDb;
    return p;
}

DynEqBandParams cutAbove1k (float rangeDb = 12.0f)
{
    return makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, -30.0f, 4.0f, rangeDb);
}

void prepareEq (DynamicEq& eq, double fs = kFs, int channels = 2, int maxBlock = 512)
{
    ProcessSpec spec;
    spec.sampleRate = fs;
    spec.maxBlockSize = maxBlock;
    spec.numChannels = channels;
    eq.prepare (spec);
}

struct ToneResult
{
    double outputGainDb = 0.0; // measured change of the tone (channel 0)
    double appliedDb = 0.0;    // getBandGainDb() at the end
};

/** Steady sine on all channels; settle 0.5 s, measure the tone over the next
    0.2 s (an integer number of periods for every frequency used here). */
ToneResult runTone (DynamicEq& eq, int band, double freq, float amplitude, int channels = 2, double fs = kFs)
{
    const int settle = static_cast<int> (fs * 0.5);
    const int measure = static_cast<int> (fs * 0.2);
    Planar buf (channels, settle + measure);
    const auto s = sine (freq, fs, settle + measure, amplitude);
    for (auto& c : buf.ch)
        c = s;
    eq.reset();
    processInBlocks (eq, buf, 256);
    ToneResult r;
    r.outputGainDb = toDb (toneAmplitude (buf.ch[0].data() + settle, measure, freq, fs) / amplitude);
    r.appliedDb = eq.getBandGainDb (band);
    return r;
}

double maxStep (const std::vector<float>& x, int from = 1)
{
    double m = 0.0;
    for (size_t i = static_cast<size_t> (std::max (1, from)); i < x.size(); ++i)
        m = std::max (m, static_cast<double> (std::abs (x[i] - x[i - 1])));
    return m;
}

/** Copies into an existing channel. Never move-assign into Planar::ch: that
    would replace the storage its channel pointers refer to. */
void setChannel (Planar& buf, int c, const std::vector<float>& v)
{
    auto& dst = buf.ch[static_cast<size_t> (c)];
    std::copy (v.begin(), v.begin() + static_cast<std::ptrdiff_t> (std::min (v.size(), dst.size())), dst.begin());
}

bool allFinite (const Planar& buf, double bound)
{
    for (const auto& c : buf.ch)
        for (float v : c)
            if (! std::isfinite (v) || std::abs (v) > bound)
                return false;
    return true;
}
} // namespace

//==============================================================================
TEST_CASE ("DynamicEq: CutAbove at the band centre is range capped (-12 dB) and reported")
{
    DynamicEq eq;
    prepareEq (eq);
    eq.setBand (0, cutAbove1k (12.0f));
    // overshoot 20 dB * (1 - 1/4) = 15 dB -> capped at the 12 dB range
    const auto r = runTone (eq, 0, 1000.0, dbfs (-10.0));
    CHECK_NEAR (r.appliedDb, -12.0, 0.1);
    CHECK_NEAR (r.outputGainDb, -12.0, 0.25);
    CHECK_NEAR (r.outputGainDb, r.appliedDb, 0.2);
}

TEST_CASE ("DynamicEq: CutAbove with a 24 dB range follows the ratio (-15 dB)")
{
    DynamicEq eq;
    prepareEq (eq);
    eq.setBand (0, cutAbove1k (24.0f));
    const auto r = runTone (eq, 0, 1000.0, dbfs (-10.0));
    CHECK_NEAR (r.appliedDb, -15.0, 0.1);
    CHECK_NEAR (r.outputGainDb, -15.0, 0.25);

    // Below threshold: no action.
    const auto quiet = runTone (eq, 0, 1000.0, dbfs (-40.0));
    CHECK_NEAR (quiet.appliedDb, 0.0, 0.01);
    CHECK_NEAR (quiet.outputGainDb, 0.0, 0.05);
}

TEST_CASE ("DynamicEq: BoostBelow lifts quiet content, but never the noise floor")
{
    DynamicEq eq;
    prepareEq (eq);
    auto p = makeBand (DynEqMode::BoostBelow, EqBandType::Bell, 1000.0f, 1.0f, -30.0f, 2.0f, 6.0f);
    p.noiseFloorDb = -70.0f;
    eq.setBand (0, p);

    // -50 dBFS: 20 dB under * (1 - 1/2) = 10 dB -> capped at 6 dB
    const auto lifted = runTone (eq, 0, 1000.0, dbfs (-50.0));
    CHECK_NEAR (lifted.appliedDb, 6.0, 0.1);
    CHECK_NEAR (lifted.outputGainDb, 6.0, 0.25);

    // In the taper: 5 dB above the floor -> half of the 6 dB lift
    const auto tapered = runTone (eq, 0, 1000.0, dbfs (-65.0));
    CHECK_NEAR (tapered.appliedDb, 3.0, 0.15);

    // Below the noise floor: no lift at all
    const auto floor = runTone (eq, 0, 1000.0, dbfs (-75.0));
    CHECK_NEAR (floor.appliedDb, 0.0, 0.02);
    CHECK_NEAR (floor.outputGainDb, 0.0, 0.05);

    // Above threshold: nothing to lift
    const auto loud = runTone (eq, 0, 1000.0, dbfs (-20.0));
    CHECK_NEAR (loud.appliedDb, 0.0, 0.02);

    // Silence: nothing is lifted and the output stays silent.
    Planar buf (2, 4800);
    eq.reset();
    eq.process (buf.block());
    CHECK (peakAbs (buf.ch[0].data(), 4800) == 0.0);
    CHECK_NEAR (eq.getBandGainDb (0), 0.0, 1e-6);
}

TEST_CASE ("DynamicEq: BoostAbove and CutBelow (expansion) have the expected sign and slope")
{
    DynamicEq eq;
    prepareEq (eq);
    eq.setBand (0, makeBand (DynEqMode::BoostAbove, EqBandType::Bell, 1000.0f, 1.0f, -30.0f, 2.0f, 6.0f));
    CHECK_NEAR (runTone (eq, 0, 1000.0, dbfs (-20.0)).appliedDb, 6.0, 0.1);  // 10 dB * (2 - 1), capped
    CHECK_NEAR (runTone (eq, 0, 1000.0, dbfs (-27.0)).appliedDb, 3.0, 0.1);  // 3 dB * (2 - 1)
    CHECK_NEAR (runTone (eq, 0, 1000.0, dbfs (-40.0)).appliedDb, 0.0, 0.02); // below: nothing
    CHECK_NEAR (runTone (eq, 0, 1000.0, dbfs (-20.0)).outputGainDb, 6.0, 0.25);

    eq.setBand (0, makeBand (DynEqMode::CutBelow, EqBandType::Bell, 1000.0f, 1.0f, -30.0f, 2.0f, 6.0f));
    CHECK_NEAR (runTone (eq, 0, 1000.0, dbfs (-40.0)).appliedDb, -6.0, 0.1); // 10 dB * (2 - 1), capped
    CHECK_NEAR (runTone (eq, 0, 1000.0, dbfs (-33.0)).appliedDb, -3.0, 0.1);
    CHECK_NEAR (runTone (eq, 0, 1000.0, dbfs (-20.0)).appliedDb, 0.0, 0.02);
    CHECK_NEAR (runTone (eq, 0, 1000.0, dbfs (-40.0)).outputGainDb, -6.0, 0.25);
}

TEST_CASE ("DynamicEq: static gain adds to the dynamic gain")
{
    DynamicEq eq;
    prepareEq (eq);
    auto p = cutAbove1k (12.0f);
    p.staticGainDb = 4.0f;
    eq.setBand (0, p);
    const auto loud = runTone (eq, 0, 1000.0, dbfs (-10.0));
    CHECK_NEAR (loud.appliedDb, 4.0 - 12.0, 0.1);
    CHECK_NEAR (loud.outputGainDb, -8.0, 0.25);
    const auto quiet = runTone (eq, 0, 1000.0, dbfs (-50.0));
    CHECK_NEAR (quiet.appliedDb, 4.0, 0.05);
    CHECK_NEAR (quiet.outputGainDb, 4.0, 0.2);
}

TEST_CASE ("DynamicEq: detection is stereo linked (a loud L drives the gain of a quiet R)")
{
    DynamicEq eq;
    prepareEq (eq);
    eq.setBand (0, cutAbove1k (12.0f));

    const int settle = 24000, measure = 9600, n = settle + measure;
    Planar buf (2, n);
    setChannel (buf, 0, sine (1000.0, kFs, n, dbfs (-10.0)));
    setChannel (buf, 1, sine (1000.0, kFs, n, dbfs (-40.0))); // alone, R would be 10 dB under threshold
    eq.reset();
    processInBlocks (eq, buf, 128);

    const double gl = toDb (toneAmplitude (buf.ch[0].data() + settle, measure, 1000.0, kFs) / dbfs (-10.0));
    const double gr = toDb (toneAmplitude (buf.ch[1].data() + settle, measure, 1000.0, kFs) / dbfs (-40.0));
    CHECK_NEAR (gl, -12.0, 0.25);
    CHECK_NEAR (gr, -12.0, 0.25);
    CHECK_NEAR (gl - gr, 0.0, 0.02); // the image (L/R ratio) is preserved

    // Same with 8 channels: only channel 5 is loud, all channels get the same gain.
    DynamicEq eq8;
    prepareEq (eq8, kFs, 8);
    eq8.setBand (0, cutAbove1k (12.0f));
    Planar b8 (8, n);
    for (int c = 0; c < 8; ++c)
        setChannel (b8, c, sine (1000.0, kFs, n, c == 5 ? dbfs (-10.0) : dbfs (-45.0)));
    eq8.reset();
    processInBlocks (eq8, b8, 100);
    for (int c = 0; c < 8; ++c)
    {
        const float a = c == 5 ? dbfs (-10.0) : dbfs (-45.0);
        CHECK_NEAR (toDb (toneAmplitude (b8.ch[static_cast<size_t> (c)].data() + settle, measure, 1000.0, kFs) / a), -12.0, 0.25);
    }
}

TEST_CASE ("DynamicEq: a Bell band barely touches content two octaves away")
{
    for (const auto& cfg : { std::pair<float, float> { 1.0f, 6.0f }, std::pair<float, float> { 2.0f, 12.0f } })
    {
        DynamicEq eq;
        prepareEq (eq);
        auto p = makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, cfg.first, -30.0f, 4.0f, cfg.second);
        eq.setBand (0, p);

        const int settle = 24000, measure = 9600, n = settle + measure;
        const auto inBand = sine (1000.0, kFs, n, dbfs (-10.0));
        const auto low = sine (250.0, kFs, n, dbfs (-30.0));
        const auto high = sine (4000.0, kFs, n, dbfs (-30.0));
        Planar buf (2, n);
        for (auto& c : buf.ch)
            for (int i = 0; i < n; ++i)
                c[static_cast<size_t> (i)] = inBand[static_cast<size_t> (i)] + low[static_cast<size_t> (i)] + high[static_cast<size_t> (i)];
        eq.reset();
        processInBlocks (eq, buf, 512);

        const float* y = buf.ch[0].data() + settle;
        CHECK_NEAR (eq.getBandGainDb (0), -cfg.second, 0.1);
        CHECK_NEAR (toDb (toneAmplitude (y, measure, 1000.0, kFs) / dbfs (-10.0)), -cfg.second, 0.25);
        CHECK_LE (std::abs (toDb (toneAmplitude (y, measure, 250.0, kFs) / dbfs (-30.0))), 1.0);
        CHECK_LE (std::abs (toDb (toneAmplitude (y, measure, 4000.0, kFs) / dbfs (-30.0))), 1.0);
    }
}

TEST_CASE ("DynamicEq: shelf detectors listen to their own side of the spectrum")
{
    DynamicEq eq;
    prepareEq (eq);
    // Low shelf de-boom at 200 Hz: low-pass detector.
    eq.setBand (0, makeBand (DynEqMode::CutAbove, EqBandType::LowShelf, 200.0f, 0.7071f, -30.0f, 4.0f, 12.0f));
    const auto bass = runTone (eq, 0, 50.0, dbfs (-6.0));
    CHECK_NEAR (bass.appliedDb, -12.0, 0.1);
    CHECK_NEAR (bass.outputGainDb, SvfCoeffs::make (FilterType::LowShelf, 200.0, 0.7071, -12.0, kFs).magnitudeDb (50.0, kFs), 0.25);
    CHECK_LE (bass.outputGainDb, -10.0);
    const auto treble = runTone (eq, 0, 5000.0, dbfs (-6.0));
    CHECK_NEAR (treble.appliedDb, 0.0, 0.05);
    CHECK_NEAR (treble.outputGainDb, 0.0, 0.05);

    // High shelf at 4 kHz: high-pass detector.
    eq.setBand (0, makeBand (DynEqMode::CutAbove, EqBandType::HighShelf, 4000.0f, 0.7071f, -30.0f, 4.0f, 12.0f));
    const auto hiss = runTone (eq, 0, 10000.0, dbfs (-6.0));
    CHECK_NEAR (hiss.appliedDb, -12.0, 0.1);
    CHECK_NEAR (hiss.outputGainDb, SvfCoeffs::make (FilterType::HighShelf, 4000.0, 0.7071, -12.0, kFs).magnitudeDb (10000.0, kFs), 0.25);
    CHECK_LE (hiss.outputGainDb, -9.0);
    const auto rumble = runTone (eq, 0, 100.0, dbfs (-6.0));
    CHECK_NEAR (rumble.appliedDb, 0.0, 0.05);
    CHECK_NEAR (rumble.outputGainDb, 0.0, 0.05);

    // A resonant shelf Q must not make the detector read hot (Q capped at
    // Butterworth): a tone at the corner, just under threshold, stays untouched.
    eq.setBand (0, makeBand (DynEqMode::CutAbove, EqBandType::LowShelf, 200.0f, 8.0f, -30.0f, 4.0f, 12.0f));
    CHECK_NEAR (runTone (eq, 0, 200.0, dbfs (-31.0)).appliedDb, 0.0, 0.02);
}

TEST_CASE ("DynamicEq: a steady tone is not modulated by detector ripple (fast release, low frequency)")
{
    // A naive peak follower with a 5 ms release ripples at twice the signal
    // frequency and turns the EQ into a distortion generator on bass. The
    // windowed peak hold must keep the gain constant on a steady tone. The
    // low-shelf cases matter most: its low-pass detector passes the deepest
    // bass at full level, far below the shelf corner (30 Hz under a 500 Hz
    // shelf), and the shelf applies its gain right there.
    struct Case
    {
        EqBandType shape;
        float bandHz, q;
        double toneHz;
    };
    for (const Case c : { Case { EqBandType::Bell, 100.0f, 1.0f, 100.0 }, Case { EqBandType::Bell, 1000.0f, 1.0f, 620.0 },
                          Case { EqBandType::LowShelf, 120.0f, 0.7071f, 40.0 }, Case { EqBandType::LowShelf, 500.0f, 0.7071f, 30.0 },
                          Case { EqBandType::HighShelf, 3000.0f, 0.7071f, 2000.0 } })
    {
        DynamicEq eq;
        prepareEq (eq, kFs, 1);
        auto p = makeBand (DynEqMode::CutAbove, c.shape, c.bandHz, c.q, -40.0f, 1.5f, 24.0f); // not range capped
        p.attackMs = 0.5f;
        p.releaseMs = 5.0f;
        eq.setBand (0, p);
        eq.reset();

        const int n = 48000;
        const float amp = dbfs (-10.0);
        Planar buf (1, n);
        setChannel (buf, 0, sine (c.toneHz, kFs, n, amp));
        double gMin = 100.0, gMax = -100.0;
        for (int pos = 0; pos < n; pos += 16)
        {
            eq.process (buf.block (pos, 16));
            if (pos >= n / 2)
            {
                gMin = std::min (gMin, static_cast<double> (eq.getBandGainDb (0)));
                gMax = std::max (gMax, static_cast<double> (eq.getBandGainDb (0)));
            }
        }
        CHECK_LE (gMax, -5.0); // the band is really acting
        CHECK_LE (gMax - gMin, 0.05);
        const float* y = buf.ch[0].data() + n / 2;
        const double fund = toneAmplitude (y, n / 2, c.toneHz, kFs);
        CHECK_LE (toDb (toneAmplitude (y, n / 2, 2.0 * c.toneHz, kFs) / fund), -70.0);
        CHECK_LE (toDb (toneAmplitude (y, n / 2, 3.0 * c.toneHz, kFs) / fund), -70.0);
    }
}

TEST_CASE ("DynamicEq: loud bass leaking through a detector skirt does not intermodulate")
{
    // A -3/-6 dBFS bass note reaches the band-pass (Bell) and high-pass
    // (HighShelf) detectors through their skirts, far below the band. If the
    // peak hold spanned only the band's own period, the level would ripple at
    // twice the bass frequency and amplitude-modulate the in-band content,
    // i.e. sidebands at fHi +- 2 fBass. Default attack / release (5 / 80 ms).
    struct Case
    {
        EqBandType shape;
        float bandHz, thresholdDb;
        double bassHz, bassDb, hiHz, hiDb;
    };
    for (const Case c : { Case { EqBandType::Bell, 1000.0f, -40.0f, 100.0, -6.0, 1000.0, -30.0 },
                          Case { EqBandType::HighShelf, 1000.0f, -45.0f, 100.0, -3.0, 4000.0, -30.0 } })
    {
        DynamicEq eq;
        prepareEq (eq, kFs, 1);
        eq.setBand (0, makeBand (DynEqMode::CutAbove, c.shape, c.bandHz, c.shape == EqBandType::Bell ? 1.0f : 0.7071f,
                                 c.thresholdDb, 3.0f, 12.0f));
        eq.reset();

        const int n = 96000, m = 48000; // measure the second second: integer cycles for every tone
        const auto bass = sine (c.bassHz, kFs, n, dbfs (c.bassDb));
        const auto hi = sine (c.hiHz, kFs, n, dbfs (c.hiDb));
        Planar buf (1, n);
        for (int i = 0; i < n; ++i)
            buf.ch[0][static_cast<size_t> (i)] = bass[static_cast<size_t> (i)] + hi[static_cast<size_t> (i)];
        double gMin = 100.0, gMax = -100.0;
        for (int pos = 0; pos < n; pos += 32)
        {
            eq.process (buf.block (pos, 32));
            if (pos >= m)
            {
                gMin = std::min (gMin, static_cast<double> (eq.getBandGainDb (0)));
                gMax = std::max (gMax, static_cast<double> (eq.getBandGainDb (0)));
            }
        }
        CHECK_LE (gMax, -9.0); // the band is really acting
        CHECK_LE (gMax - gMin, 0.02);

        const float* y = buf.ch[0].data() + m;
        const double carrier = toneAmplitude (y, m, c.hiHz, kFs);
        const double lower = toneAmplitude (y, m, c.hiHz - 2.0 * c.bassHz, kFs);
        const double upper = toneAmplitude (y, m, c.hiHz + 2.0 * c.bassHz, kFs);
        CHECK_LE (toDb (std::max (lower, upper) / carrier), -80.0);
    }
}

TEST_CASE ("DynamicEq: detector reads the dry input, bands do not chase each other")
{
    DynamicEq eq;
    prepareEq (eq);
    auto boost = makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    boost.staticGainDb = 12.0f; // band 0: static +12 dB at 1 kHz
    eq.setBand (0, boost);
    eq.setBand (1, makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, -20.0f, 20.0f, 24.0f));
    // Input -26 dBFS: under band 1's threshold even though band 0 lifts it to -14.
    const auto r = runTone (eq, 1, 1000.0, dbfs (-26.0));
    CHECK_NEAR (r.appliedDb, 0.0, 0.02);
    CHECK_NEAR (eq.getBandGainDb (0), 12.0, 0.05);
    CHECK_NEAR (r.outputGainDb, 12.0, 0.25);

    // Independent bands: only the band whose region is excited reacts.
    DynamicEq eq2;
    prepareEq (eq2);
    eq2.setBand (0, makeBand (DynEqMode::CutAbove, EqBandType::Bell, 150.0f, 1.5f, -30.0f, 4.0f, 9.0f));
    eq2.setBand (1, makeBand (DynEqMode::CutAbove, EqBandType::Bell, 3500.0f, 1.5f, -30.0f, 4.0f, 9.0f));
    const auto r2 = runTone (eq2, 1, 3500.0, dbfs (-10.0));
    CHECK_NEAR (r2.appliedDb, -9.0, 0.1);
    CHECK_NEAR (eq2.getBandGainDb (0), 0.0, 0.05);
}

TEST_CASE ("DynamicEq: attack and release timing")
{
    // CutAbove, attack 10 ms / release 100 ms (one-pole time constants).
    DynamicEq eq;
    prepareEq (eq, kFs, 1);
    auto p = cutAbove1k (12.0f);
    p.attackMs = 10.0f;
    p.releaseMs = 100.0f;
    eq.setBand (0, p);
    eq.reset();

    const int block = 16;
    const int onAt = 4800, offAt = 4800 + 24000, n = offAt + 24000;
    auto x = sine (1000.0, kFs, n, dbfs (-10.0));
    for (int i = 0; i < n; ++i)
        if (i < onAt || i >= offAt)
            x[static_cast<size_t> (i)] = 0.0f;
    Planar buf (1, n);
    setChannel (buf, 0, x);

    double attack90 = -1.0, release90 = -1.0;
    for (int pos = 0; pos < n; pos += block)
    {
        eq.process (buf.block (pos, block));
        const double g = eq.getBandGainDb (0);
        const double t = (pos + block) / kFs * 1000.0;
        if (pos + block <= onAt)
            CHECK_NEAR (g, 0.0, 1e-6);
        if (pos >= onAt && pos + block <= offAt && attack90 < 0.0 && g <= -10.8)
            attack90 = t - onAt / kFs * 1000.0;
        if (pos >= offAt && release90 < 0.0 && g >= -1.2)
            release90 = t - offAt / kFs * 1000.0;
    }
    // One pole: 90 % after 2.3 tau (+ detector and control-rate granularity).
    CHECK (attack90 > 0.0);
    CHECK_GE (attack90, 18.0);
    CHECK_LE (attack90, 30.0);
    CHECK (release90 > 0.0);
    CHECK_GE (release90, 200.0);
    CHECK_LE (release90, 265.0);
}

TEST_CASE ("DynamicEq: level accuracy and timing do not depend on the sample rate")
{
    // The control rate is fs / kControlInterval; every smoother, the hold
    // window and the gain smoother are derived from it, so the static curve
    // and the attack / release times must come out the same at every rate.
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        DynamicEq eq;
        prepareEq (eq, fs, 1, 64);
        auto p = cutAbove1k (12.0f);
        p.attackMs = 10.0f;
        p.releaseMs = 100.0f;
        eq.setBand (0, p);
        eq.reset();

        const int offAt = static_cast<int> (fs * 0.6), n = static_cast<int> (fs * 1.0), measure = static_cast<int> (fs * 0.2);
        auto x = sine (1000.0, fs, n, dbfs (-10.0));
        std::fill (x.begin() + offAt, x.end(), 0.0f);
        Planar buf (1, n);
        setChannel (buf, 0, x);

        double attack90 = -1.0, release90 = -1.0, gainAtOff = 0.0;
        for (int pos = 0; pos < n; pos += 64)
        {
            eq.process (buf.block (pos, std::min (64, n - pos)));
            const double g = eq.getBandGainDb (0);
            const double t = (pos + 64) / fs * 1000.0;
            if (attack90 < 0.0 && g <= -10.8)
                attack90 = t;
            if (pos + 64 <= offAt)
                gainAtOff = g; // last block that ends before the tone stops
            if (pos >= offAt && release90 < 0.0 && g >= -1.2)
                release90 = t - offAt / fs * 1000.0;
        }
        CHECK_NEAR (gainAtOff, -12.0, 0.05);
        CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + offAt - measure, measure, 1000.0, fs) / dbfs (-10.0)), -12.0, 0.25);
        CHECK_GE (attack90, 18.0);
        CHECK_LE (attack90, 30.0);
        CHECK_GE (release90, 200.0);
        CHECK_LE (release90, 265.0);
    }
}

TEST_CASE ("DynamicEq: expansion modes use attack for a RISING gain")
{
    // BoostAbove with a fast attack and a slow release: the boost must arrive
    // quickly when the level rises and decay slowly when it falls.
    DynamicEq eq;
    prepareEq (eq, kFs, 1);
    auto p = makeBand (DynEqMode::BoostAbove, EqBandType::Bell, 1000.0f, 1.0f, -30.0f, 2.0f, 6.0f);
    p.attackMs = 1.0f;
    p.releaseMs = 300.0f;
    eq.setBand (0, p);
    eq.reset();

    const int onAt = 4800, offAt = 4800 + 9600, n = offAt + 48000;
    auto x = sine (1000.0, kFs, n, dbfs (-10.0));
    for (int i = 0; i < n; ++i)
        if (i < onAt || i >= offAt)
            x[static_cast<size_t> (i)] = 0.0f;
    Planar buf (1, n);
    setChannel (buf, 0, x);
    double up90 = -1.0, down90 = -1.0;
    for (int pos = 0; pos < n; pos += 16)
    {
        eq.process (buf.block (pos, 16));
        const double g = eq.getBandGainDb (0);
        const double t = (pos + 16) / kFs * 1000.0;
        if (pos >= onAt && up90 < 0.0 && g >= 5.4)
            up90 = t - onAt / kFs * 1000.0;
        if (pos >= offAt && down90 < 0.0 && g <= 0.6)
            down90 = t - offAt / kFs * 1000.0;
    }
    CHECK (up90 > 0.0);
    CHECK_LE (up90, 6.0);
    CHECK_GE (down90, 600.0); // 2.3 * 300 ms
    CHECK_LE (down90, 760.0);
}

TEST_CASE ("DynamicEq: disabled bands are bit-transparent; enable / disable glide click-free")
{
    DynamicEq eq;
    prepareEq (eq);
    const int n = 48000;
    const auto tone = sine (1000.0, kFs, n, 0.3f);

    // Nothing enabled: exact pass-through.
    {
        Planar buf (2, n);
        setChannel (buf, 0, tone);
        setChannel (buf, 1, tone);
        processInBlocks (eq, buf, 333);
        CHECK (buf.ch[0] == tone);
        CHECK (buf.ch[1] == tone);
        CHECK_NEAR (eq.getBandGainDb (0), 0.0, 0.0);
    }

    // Enable a fast, deep cut mid-stream, then disable it again.
    auto p = makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, -60.0f, 20.0f, 12.0f);
    p.attackMs = 0.1f;
    p.enabled = false;
    eq.setBand (0, p);
    eq.reset();

    Planar buf (2, n);
    setChannel (buf, 0, tone);
    setChannel (buf, 1, tone);
    const int block = 64;
    const int enableAt = 9600, disableAt = 28800;
    std::vector<float> gains;
    for (int pos = 0; pos < n; pos += block)
    {
        if (pos == enableAt)
        {
            p.enabled = true;
            eq.setBand (0, p);
        }
        if (pos == disableAt)
        {
            p.enabled = false;
            eq.setBand (0, p);
        }
        eq.process (buf.block (pos, block));
        gains.push_back (eq.getBandGainDb (0));
    }

    // The gain glides (~20 ms) instead of jumping.
    const size_t enBlock = static_cast<size_t> (enableAt / block);
    CHECK_LE (gains[enBlock], -0.05);
    CHECK_GE (gains[enBlock], -1.5);
    CHECK_NEAR (gains[enBlock + 60], -12.0, 0.1); // 80 ms later
    const size_t disBlock = static_cast<size_t> (disableAt / block);
    CHECK_GE (gains[disBlock], -11.5);
    CHECK_LE (gains[disBlock], -10.0);
    CHECK_NEAR (gains.back(), 0.0, 0.0); // idle again

    // No click: the output never moves faster than the unprocessed sine does,
    // and after the band went idle the signal is passed through bit-exactly.
    CHECK_LE (maxStep (buf.ch[0]), maxStep (tone) * 1.03);
    CHECK_LE (maxStep (buf.ch[1]), maxStep (tone) * 1.03);
    int mismatches = 0;
    for (int i = disableAt + 9600; i < n; ++i)
        mismatches += buf.ch[0][static_cast<size_t> (i)] != tone[static_cast<size_t> (i)] ? 1 : 0;
    CHECK (mismatches == 0);
    CHECK_NEAR (toDb (toneAmplitude (buf.ch[0].data() + 19200, 9600, 1000.0, kFs) / 0.3), -12.0, 0.25);
}

TEST_CASE ("DynamicEq: mode and shape switches are click-free crossfades")
{
    DynamicEq eq;
    prepareEq (eq);
    const int n = 48000 * 2;
    const auto tone = sine (1000.0, kFs, n, 0.3f);
    auto p = makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, -40.0f, 4.0f, 9.0f);
    eq.setBand (0, p);
    eq.reset();

    Planar buf (2, n);
    setChannel (buf, 0, tone);
    setChannel (buf, 1, tone);
    const int block = 100;
    std::vector<float> gains;
    for (int pos = 0; pos < n; pos += block)
    {
        if (pos == 24000)
        {
            p.shape = EqBandType::HighShelf; // Bell -> HighShelf (tone at the corner)
            eq.setBand (0, p);
        }
        if (pos == 48000)
        {
            p.mode = DynEqMode::BoostAbove; // cut -> boost: glides through 0 dB
            eq.setBand (0, p);
        }
        if (pos == 72000)
        {
            p.shape = EqBandType::LowShelf;
            eq.setBand (0, p);
        }
        eq.process (buf.block (pos, block));
        gains.push_back (eq.getBandGainDb (0));
    }

    // Steady states: cut, cut (high-pass detector: -3 dB at its corner, still
    // range capped), boost, boost.
    CHECK_NEAR (gains[239], -9.0, 0.1);
    CHECK_NEAR (gains[479], -9.0, 0.1);
    CHECK_NEAR (gains[719], 9.0, 0.1);
    CHECK_NEAR (gains.back(), 9.0, 0.1);

    // Every switch glides through 0 dB over the 20 ms fade: per-block changes
    // stay small, the gain never overshoots its steady-state values, and the
    // output never jumps.
    double maxDelta = 0.0;
    for (size_t i = 40; i < gains.size(); ++i) // after the initial 5 ms attack
        maxDelta = std::max (maxDelta, static_cast<double> (std::abs (gains[i] - gains[i - 1])));
    CHECK_LE (maxDelta, 1.6);
    const auto [minIt, maxIt] = std::minmax_element (gains.begin(), gains.end());
    CHECK_GE (*minIt, -9.05f);
    CHECK_LE (*maxIt, 9.05f);
    for (size_t i : { size_t (240), size_t (480), size_t (720) })
    {
        // a swap goes through 0 dB within ~50 ms of the request (sampled
        // at block ends, so the exact 0 of the swap tick may fall between)
        float closest = 100.0f;
        for (size_t j = i; j < i + 25; ++j)
            closest = std::min (closest, std::abs (gains[j]));
        CHECK_LE (closest, 0.1f);
    }
    CHECK_LE (maxStep (buf.ch[0]), maxStep (tone) * std::pow (10.0, 9.2 / 20.0));
}

TEST_CASE ("DynamicEq: continuous parameter changes glide")
{
    DynamicEq eq;
    prepareEq (eq, kFs, 1);
    auto p = makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, -30.0f, 2.0f, 0.0f);
    eq.setBand (0, p);
    eq.reset();
    Planar buf (1, 16);
    eq.process (buf.block());
    CHECK_NEAR (eq.getBandGainDb (0), 0.0, 1e-6);

    p.staticGainDb = 12.0f;
    eq.setBand (0, p);
    eq.process (buf.block());
    CHECK_GE (eq.getBandGainDb (0), 0.01);
    CHECK_LE (eq.getBandGainDb (0), 1.0); // first control tick of a 20 ms glide
    for (int i = 0; i < 600; ++i)
        eq.process (buf.block());
    CHECK_NEAR (eq.getBandGainDb (0), 12.0, 0.01);

    // A frequency sweep keeps the output finite and smooth.
    const int n = 48000;
    auto x = sine (2000.0, kFs, n, 0.5f);
    Planar sweep (1, n);
    setChannel (sweep, 0, x);
    for (int pos = 0; pos < n; pos += 64)
    {
        p.frequency = 200.0f * std::pow (50.0f, static_cast<float> (pos) / static_cast<float> (n));
        eq.setBand (0, p);
        eq.process (sweep.block (pos, 64));
    }
    CHECK (allFinite (sweep, 10.0));
    CHECK_LE (maxStep (sweep.ch[0]), maxStep (x) * 4.5); // +12 dB max
}

TEST_CASE ("DynamicEq: setBand sanitises parameters; index checks")
{
    DynamicEq eq;
    prepareEq (eq);
    DynEqBandParams p;
    p.enabled = true;
    p.frequency = 5.0f;
    p.q = 100.0f;
    p.thresholdDb = -200.0f;
    p.ratio = 0.5f;
    p.rangeDb = 99.0f;
    p.staticGainDb = -40.0f;
    p.attackMs = 0.0f;
    p.releaseMs = 1.0e6f;
    p.shape = EqBandType::Notch;
    p.mode = static_cast<DynEqMode> (17);
    eq.setBand (2, p);
    const auto& g = eq.getBand (2);
    CHECK (g.frequency == 20.0f);
    CHECK (g.q == 10.0f);
    CHECK (g.thresholdDb == -80.0f);
    CHECK (g.ratio == 1.0f);
    CHECK (g.rangeDb == 24.0f);
    CHECK (g.staticGainDb == -12.0f);
    CHECK (g.attackMs == 0.1f);
    CHECK (g.releaseMs == 2000.0f);
    CHECK (g.shape == EqBandType::Bell);
    CHECK (g.mode == DynEqMode::CutAbove);

    // Non-finite values keep the previous setting.
    auto bad = eq.getBand (2);
    bad.frequency = std::numeric_limits<float>::quiet_NaN();
    bad.thresholdDb = std::numeric_limits<float>::infinity();
    bad.q = -std::numeric_limits<float>::infinity();
    eq.setBand (2, bad);
    CHECK (eq.getBand (2).frequency == 20.0f);
    CHECK (eq.getBand (2).thresholdDb == -80.0f);
    CHECK (eq.getBand (2).q == 10.0f);

    // Out-of-range indices are ignored / return neutral values.
    eq.setBand (-1, p);
    eq.setBand (DynamicEq::kMaxBands, p);
    CHECK (eq.getBandGainDb (-1) == 0.0f);
    CHECK (eq.getBandGainDb (DynamicEq::kMaxBands) == 0.0f);
    CHECK (! eq.getBand (99).enabled);
    CHECK (eq.getBand (0) == DynEqBandParams {});
    CHECK (std::string (eq.name()) == "Dynamic EQ");
}

//==============================================================================
TEST_CASE ("DynamicEq: process, reset and setters do not allocate")
{
    DynamicEq eq;
    prepareEq (eq, kFs, 2, 512);
    Planar buf (2, 512);
    setChannel (buf, 0, whiteNoise (512, 0.5f, 7));
    setChannel (buf, 1, whiteNoise (512, 0.5f, 8));

    flubtest::AllocationGuard guard;
    eq.reset();
    for (int b = 0; b < 4; ++b)
    {
        auto p = makeBand (static_cast<DynEqMode> (b), b == 1 ? EqBandType::HighShelf : EqBandType::Bell,
                           300.0f * static_cast<float> (b + 1), 1.5f, -30.0f, 3.0f, 9.0f);
        p.staticGainDb = 2.0f;
        eq.setBand (b, p);
    }
    for (int i = 0; i < 20; ++i)
    {
        auto p = eq.getBand (i % 4);
        p.frequency *= 1.05f;
        p.thresholdDb += 0.5f;
        if (i == 10)
            p.shape = EqBandType::LowShelf;
        if (i == 15)
            p.enabled = false;
        eq.setBand (i % 4, p);
        eq.process (buf.block (0, 1 + (i * 97) % 512));
        (void) eq.getBandGainDb (i % 4);
    }
    CHECK (guard.allocations() == 0);
}

TEST_CASE ("DynamicEq: robustness - silence, DC, full-scale noise, impulses, extreme settings, all rates")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const int n = static_cast<int> (fs * 0.25);

        for (int setting = 0; setting < 4; ++setting)
        {
            // Block sizes 480 / 4096 (the maximum) / 1 / 333, and one 8-channel run.
            const int bs = setting == 1 ? 4096 : setting == 2 ? 1 : setting == 3 ? 333 : 480;
            const int channels = setting == 3 ? 8 : 2;
            DynamicEq eq;
            prepareEq (eq, fs, channels, bs);
            for (int b = 0; b < DynamicEq::kMaxBands; ++b)
            {
                DynEqBandParams p;
                p.enabled = true;
                p.mode = static_cast<DynEqMode> (b % 4);
                p.shape = static_cast<EqBandType> (b % 3);
                if (setting == 0) // documented extremes
                {
                    p.frequency = (b & 1) ? 20000.0f : 20.0f;
                    p.q = (b & 2) ? 10.0f : 0.1f;
                    p.thresholdDb = (b & 4) ? 0.0f : -80.0f;
                    p.ratio = 20.0f;
                    p.rangeDb = 24.0f;
                    p.staticGainDb = (b & 1) ? 12.0f : -12.0f;
                    p.attackMs = 0.1f;
                    p.releaseMs = 5.0f;
                    p.noiseFloorDb = -120.0f;
                }
                else if (setting == 1) // out of range / non-finite
                {
                    p.frequency = (b & 1) ? 1.0e9f : -5.0f;
                    p.q = std::numeric_limits<float>::quiet_NaN();
                    p.thresholdDb = -std::numeric_limits<float>::infinity();
                    p.ratio = 1.0e9f;
                    p.rangeDb = 1.0e9f;
                    p.staticGainDb = 1.0e9f;
                    p.attackMs = -1.0f;
                    p.releaseMs = 0.0f;
                    p.noiseFloorDb = std::numeric_limits<float>::quiet_NaN();
                }
                else // everything boosting as hard as possible, resonant
                {
                    if (setting == 2)
                        p.shape = EqBandType::Bell;
                    p.mode = (b & 1) ? DynEqMode::BoostBelow : DynEqMode::BoostAbove;
                    p.frequency = 1000.0f * static_cast<float> (b + 1);
                    p.q = 10.0f;
                    p.thresholdDb = (b & 1) ? 0.0f : -80.0f;
                    p.ratio = 20.0f;
                    p.rangeDb = 24.0f;
                    p.staticGainDb = 12.0f;
                    p.noiseFloorDb = -120.0f;
                }
                eq.setBand (b, p);
            }

            for (int sig = 0; sig < 5; ++sig)
            {
                Planar buf (channels, n);
                for (int c = 0; c < channels; ++c)
                {
                    auto& x = buf.ch[static_cast<size_t> (c)];
                    switch (sig)
                    {
                        case 0: break; // silence
                        case 1: std::fill (x.begin(), x.end(), c % 2 == 0 ? 1.0f : -1.0f); break;
                        case 2: setChannel (buf, c, whiteNoise (n, 1.0f, static_cast<uint32_t> (11 + c))); break;
                        case 3:
                            for (int i = 0; i < n; i += 997)
                                x[static_cast<size_t> (i)] = 1.0f;
                            break;
                        default: setChannel (buf, c, sine (fs * 0.45, fs, n, 1.0f)); break;
                    }
                }
                eq.reset();
                processInBlocks (eq, buf, bs);
                // Worst case per band: +36 dB (static + range). Settings 1 and 3
                // stack up to three +36 dB shelves over the same region (e.g. a
                // low shelf at 20 kHz is a broadband boost): up to +108 dB is the
                // correct static response there. Otherwise at most one band acts.
                const bool stackedShelves = setting == 1 || setting == 3;
                CHECK (allFinite (buf, stackedShelves ? 1.0e6 : 1.0e3));
                if (sig == 0)
                    for (int c = 0; c < channels; ++c)
                        CHECK (peakAbs (buf.ch[static_cast<size_t> (c)].data(), n) == 0.0);
                for (int b = 0; b < DynamicEq::kMaxBands; ++b)
                    CHECK (std::isfinite (eq.getBandGainDb (b)) && std::abs (eq.getBandGainDb (b)) <= 36.001f);
            }
        }

        // Random parameter automation every block on full-scale noise, varying
        // channel counts and block sizes (including 1).
        DynamicEq eq;
        prepareEq (eq, fs, 2, 4096);
        FastRandom rng (99);
        Planar buf (2, n);
        setChannel (buf, 0, whiteNoise (n, 1.0f, 3));
        setChannel (buf, 1, whiteNoise (n, 1.0f, 4));
        auto rnd = [&rng] (float lo, float hi) { return lo + (hi - lo) * 0.5f * (rng.nextBipolar() + 1.0f); };
        for (int pos = 0; pos < n;)
        {
            const int len = std::min (n - pos, 1 + static_cast<int> (rng.nextU32() % 700u));
            for (int b = 0; b < 4; ++b)
            {
                DynEqBandParams p;
                p.enabled = rng.nextU32() % 5u != 0u;
                p.mode = static_cast<DynEqMode> (rng.nextU32() % 4u);
                p.shape = static_cast<EqBandType> (rng.nextU32() % 3u);
                p.frequency = rnd (20.0f, 20000.0f);
                p.q = rnd (0.1f, 10.0f);
                p.thresholdDb = rnd (-80.0f, 0.0f);
                p.ratio = rnd (1.0f, 20.0f);
                p.rangeDb = rnd (0.0f, 24.0f);
                p.staticGainDb = rnd (-12.0f, 12.0f);
                p.attackMs = rnd (0.1f, 200.0f);
                p.releaseMs = rnd (5.0f, 2000.0f);
                p.noiseFloorDb = rnd (-100.0f, -20.0f);
                eq.setBand (b, p);
            }
            const int channels = 1 + static_cast<int> (rng.nextU32() % 2u);
            eq.process (buf.block (pos, len).firstChannels (channels));
            pos += len;
        }
        CHECK (allFinite (buf, 1.0e4));

        // NaN input must not poison the band forever.
        Planar poison (2, 64);
        poison.ch[0][10] = std::numeric_limits<float>::quiet_NaN();
        poison.ch[1][20] = std::numeric_limits<float>::infinity();
        eq.process (poison.block());
        Planar after (2, n);
        setChannel (after, 0, sine (1000.0, fs, n, 0.5f));
        setChannel (after, 1, after.ch[0]);
        processInBlocks (eq, after, 256);
        Planar tail (2, 4800);
        for (int c = 0; c < 2; ++c)
            std::copy (after.ch[static_cast<size_t> (c)].end() - 4800, after.ch[static_cast<size_t> (c)].end(), tail.ch[static_cast<size_t> (c)].begin());
        CHECK (allFinite (tail, 1.0e4));
    }
}

TEST_CASE ("DynamicEq: output is independent of the host block size")
{
    const int n = 3584 * 4; // 3584 = 7 * 512: the parameter change lands on a block edge for every size
    Planar input (2, n);
    {
        auto noise = whiteNoise (n, 0.2f, 42);
        auto tone = sine (1000.0, kFs, n, 1.0f);
        auto tone2 = sine (7000.0, kFs, n, 1.0f);
        for (int i = 0; i < n; ++i)
        {
            const size_t k = static_cast<size_t> (i);
            const float env = (i / 1500) % 2 == 0 ? 0.5f : 0.01f; // bursts drive attack / release
            input.ch[0][k] = noise[k] + env * tone[k];
            input.ch[1][k] = 0.5f * noise[k] + 0.3f * env * tone2[k];
        }
    }

    auto run = [&] (int blockSize)
    {
        DynamicEq eq;
        prepareEq (eq, kFs, 2, 512);
        eq.setBand (0, cutAbove1k (12.0f));
        auto p1 = makeBand (DynEqMode::BoostBelow, EqBandType::HighShelf, 6000.0f, 0.7f, -25.0f, 3.0f, 6.0f);
        eq.setBand (1, p1);
        eq.setBand (2, makeBand (DynEqMode::CutBelow, EqBandType::LowShelf, 150.0f, 0.7f, -40.0f, 2.0f, 6.0f));
        eq.reset();
        Planar buf = input;
        buf.ptrs.clear();
        for (auto& c : buf.ch)
            buf.ptrs.push_back (c.data());
        for (int pos = 0; pos < n; pos += blockSize)
        {
            if (pos == 7168)
            {
                // continuous glide + a structural swap + a new band fading in
                auto p0 = cutAbove1k (18.0f);
                p0.frequency = 1500.0f;
                p0.staticGainDb = -3.0f;
                eq.setBand (0, p0);
                p1.shape = EqBandType::Bell;
                eq.setBand (1, p1);
                eq.setBand (3, makeBand (DynEqMode::BoostAbove, EqBandType::Bell, 400.0f, 2.0f, -30.0f, 2.0f, 4.0f));
            }
            eq.process (buf.block (pos, std::min (blockSize, n - pos)));
        }
        return buf;
    };

    const auto ref = run (512);
    for (int bs : { 1, 7, 64 })
    {
        const auto out = run (bs);
        double maxDiff = 0.0;
        for (size_t c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                maxDiff = std::max (maxDiff, static_cast<double> (std::abs (out.ch[c][static_cast<size_t> (i)] - ref.ch[c][static_cast<size_t> (i)])));
        CHECK_LE (maxDiff, 1.0e-5);
    }
}

TEST_CASE ("DynamicEq: zero latency - an impulse is not delayed")
{
    DynamicEq eq;
    prepareEq (eq, kFs, 2);
    CHECK (eq.latencySamples() == 0);

    auto p = makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, 0.0f, 2.0f, 6.0f);
    p.staticGainDb = 6.0f;
    eq.setBand (0, p);
    eq.reset();

    const int n = 2048;
    Planar buf (2, n);
    buf.ch[0][100] = 1.0e-3f; // low level: no dynamic action
    buf.ch[1][100] = 1.0e-3f;
    processInBlocks (eq, buf, 64);
    CHECK (buf.ch[0][99] == 0.0f);
    CHECK (buf.ch[0][100] != 0.0f);
    int peakAt = 0;
    for (int i = 1; i < n; ++i)
        if (std::abs (buf.ch[0][static_cast<size_t> (i)]) > std::abs (buf.ch[0][static_cast<size_t> (peakAt)]))
            peakAt = i;
    CHECK (peakAt == 100 + eq.latencySamples());

    // A neutral band (0 dB, no dynamics) is an exact identity with no delay.
    DynamicEq neutral;
    prepareEq (neutral, kFs, 2);
    neutral.setBand (0, makeBand (DynEqMode::CutAbove, EqBandType::Bell, 1000.0f, 1.0f, 0.0f, 2.0f, 6.0f));
    neutral.reset();
    Planar imp (2, n);
    imp.ch[0][100] = 1.0e-3f;
    processInBlocks (neutral, imp, 64);
    int mismatches = 0;
    for (int i = 0; i < n; ++i)
        mismatches += imp.ch[0][static_cast<size_t> (i)] != (i == 100 ? 1.0e-3f : 0.0f) ? 1 : 0;
    CHECK (mismatches == 0);
}
