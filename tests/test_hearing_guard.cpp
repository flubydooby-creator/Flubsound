// HearingGuard (docs/11 E32 (c)): the listening-level estimate, the dose on a
// synthetic clock and the listening-level cap on MixEngine's output. The
// levels are estimates by design (no coupler here); these tests pin the
// arithmetic and the cap's bound, not the acoustic accuracy.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/engine/DeviceProfiles.h"
#include "flub/engine/HearingGuard.h"
#include "flub/engine/MixEngine.h"
#include "flub/io/Json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace flub;
using namespace flubtest;

namespace
{
/** Runs `guard` over a stereo programme in host blocks of `block` samples. */
void runGuard (HearingGuard& guard, Planar& buf, int block)
{
    for (int pos = 0; pos < buf.numSamples(); pos += block)
        guard.process (buf.block (pos, std::min (block, buf.numSamples() - pos)));
}

/** The largest A-weighted Leq over any sample-aligned window of `window`
    samples, per ear, in dB(A) re `splAtFullScaleDb` (a fresh A filter, the
    guard's design; windows before the start see silence). */
double maxWindowLeqDbA (const Planar& out, double rate, double window, double splAtFullScaleDb)
{
    const auto aw = HearingGuard::aWeighting (rate);
    const int n = out.numSamples();
    const int w = static_cast<int> (std::lround (window));
    double worst = -1000.0;
    for (int c = 0; c < 2; ++c)
    {
        std::array<BiquadState, 3> st {};
        std::vector<double> prefix (static_cast<size_t> (n) + 1, 0.0);
        for (int k = 0; k < n; ++k)
        {
            double a = out.ch[static_cast<size_t> (c)][static_cast<size_t> (k)];
            for (size_t s = 0; s < 3; ++s)
                a = biquadTick (aw[s], st[s], a);
            prefix[static_cast<size_t> (k) + 1] = prefix[static_cast<size_t> (k)] + 2.0 * a * a;
        }
        double best = 0.0;
        for (int k = 1; k <= n; ++k)
            best = std::max (best, prefix[static_cast<size_t> (k)] - prefix[static_cast<size_t> (std::max (0, k - w))]);
        worst = std::max (worst, splAtFullScaleDb + 10.0 * std::log10 (std::max (1.0e-30, best / w)));
    }
    return worst;
}

/** Appends `seconds` of white noise (amplitude a, both ears) or a sine. */
void append (Planar& p, double rate, double seconds, float amplitude, double sineHz, uint32_t seed)
{
    const int n = static_cast<int> (std::lround (seconds * rate));
    const auto v = sineHz > 0.0 ? sine (sineHz, rate, n, amplitude) : whiteNoise (n, amplitude, seed);
    for (int c = 0; c < 2; ++c)
        p.ch[static_cast<size_t> (c)].insert (p.ch[static_cast<size_t> (c)].end(), v.begin(), v.end());
    p.rebind();
}
} // namespace

TEST_CASE ("HearingGuard: A-weighting follows IEC 61672-1")
{
    // The analog curve against the standard's table (0.1 dB rounding) at the
    // exact base-10 frequencies behind the nominal ones (31.5 Hz = 10^1.5 ...).
    const double table[][2] = { { 1.5, -39.4 }, { 1.8, -26.2 }, { 2.1, -16.1 }, { 2.4, -8.6 }, { 2.7, -3.2 },
                                { 3.0, 0.0 },   { 3.3, 1.2 },   { 3.6, 1.0 },   { 3.9, -1.1 }, { 4.2, -6.6 } };
    for (const auto& row : table)
        CHECK_NEAR (HearingGuard::aWeightingCurveDb (std::pow (10.0, row[0])), row[1], 0.05);

    // The digital sections against the curve.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto aw = HearingGuard::aWeighting (rate);
        double worstLow = 0.0, worstHigh = 0.0;
        for (double f = 20.0; f <= 12500.0; f *= 1.05)
        {
            const double dig = 20.0 * std::log10 (std::abs (aw[0].response (f, rate) * aw[1].response (f, rate) * aw[2].response (f, rate)));
            const double err = std::abs (dig - HearingGuard::aWeightingCurveDb (f));
            (f <= 4000.0 ? worstLow : worstHigh) = std::max (f <= 4000.0 ? worstLow : worstHigh, err);
        }
        std::cout << "    A-weighting at " << rate << " Hz: worst error " << worstLow << " dB to 4 kHz, " << worstHigh << " dB to 12.5 kHz\n";
        CHECK_LE (worstLow, 0.1);
        CHECK_LE (worstHigh, 0.1);
    }
}

TEST_CASE ("HearingGuard: synthetic-clock dose matches a hand computation")
{
    // 2 h at 86 dB(A) = 2 / 40 x 10^((86 - 80) / 10) = 0.19905 of the week.
    CHECK_NEAR (Dosimeter::doseFor (86.0, 2.0 * 3600.0), 0.05 * std::pow (10.0, 0.6), 1e-12);
    CHECK_NEAR (Dosimeter::doseFor (86.0, 2.0 * 3600.0), 0.19905, 1e-5);
    // 80 dB(A) for 40 h is the whole allowance; +3 dB halves the time.
    CHECK_NEAR (Dosimeter::doseFor (80.0, 40.0 * 3600.0), 1.0, 1e-12);
    CHECK_NEAR (Dosimeter::secondsFor (83.0, 1.0) / 3600.0, 40.0 / std::pow (10.0, 0.3), 1e-9);
    CHECK_NEAR (Dosimeter::secondsFor (100.0, 1.0) / 60.0, 24.0, 1e-9); // 40 h / 100

    // A day on a synthetic clock, one second at a time: 1 h at 80, 30 min at
    // 95, 2 h at 70 dB(A) = 1 / 40 + 0.5 / 40 x 10^1.5 + 2 / 40 x 10^-1.
    Dosimeter d;
    for (int s = 0; s < 3600; ++s)
        d.add (80.0, 1.0);
    for (int s = 0; s < 1800; ++s)
        d.add (95.0, 1.0);
    for (int s = 0; s < 7200; ++s)
        d.add (70.0, 1.0);
    const double hand = 1.0 / 40.0 + 0.5 / 40.0 * std::pow (10.0, 1.5) + 2.0 / 40.0 * 0.1;
    CHECK_NEAR (d.fraction(), hand, 1e-9);
    CHECK_NEAR (d.seconds(), 12600.0, 1e-9);
    CHECK_NEAR (d.equivalentLevelDbA(), 80.0 + 10.0 * std::log10 (hand * 40.0 / 3.5), 1e-9);
    d.add (90.0, 0.0); // no time, no dose
    CHECK_NEAR (d.fraction(), hand, 1e-9);

    // The same arithmetic on the engine's clock (the sample count): 1 kHz at
    // -20 dBFS, 106 dB SPL at 0 dBFS, 5 minutes at full volume (86 dB(A)),
    // then 5 minutes 6 dB down (80 dB(A)), at 8 kHz to keep the test fast.
    const double rate = 8000.0;
    HearingGuard g;
    g.prepare (rate);
    g.setSensitivityDbSpl (106.0f);
    g.setEndpointVolumeDb (0.0f);
    g.setDoseBaseline (0.25); // the day's persisted dose before this session
    const int half = static_cast<int> (300.0 * rate), block = 400;
    const float amp = static_cast<float> (std::pow (10.0, -20.0 / 20.0));
    Planar buf (2, block);
    for (int pos = 0, k = 0; pos < 2 * half; pos += block)
    {
        if (pos == half)
            g.setEndpointVolumeDb (-6.0f);
        for (int i = 0; i < block; ++i, ++k)
            buf.ch[0][static_cast<size_t> (i)] = buf.ch[1][static_cast<size_t> (i)]
                = amp * static_cast<float> (std::sin (kTwoPi * 1000.0 * k / rate));
        g.process (buf.block());
    }
    const double expected = Dosimeter::doseFor (86.0, 300.0) + Dosimeter::doseFor (80.0, 300.0);
    const auto& m = g.meters();
    std::cout << "    engine clock: session dose " << m.sessionDose.load() << " (hand " << expected << "), session Leq "
              << m.sessionLeqDbA.load() << " dB(A)\n";
    CHECK_NEAR (m.sessionDose.load() / expected, 1.0, 0.002);
    CHECK_NEAR (m.doseToday.load(), 0.25 + m.sessionDose.load(), 1e-12);
    CHECK_NEAR (m.sessionLeqDbA.load(), 10.0 * std::log10 ((std::pow (10.0, 0.6) + 1.0) / 2.0) + 80.0, 0.01);
}

TEST_CASE ("HearingGuard: level estimate = programme + endpoint volume + sensitivity")
{
    const double rate = 48000.0;
    const int n = static_cast<int> (6.0 * rate);
    const auto measure = [&] (double freq, float dbfs, float sens, float vol, bool leftOnly) {
        HearingGuard g;
        g.prepare (rate);
        g.setSensitivityDbSpl (sens);
        g.setEndpointVolumeDb (vol);
        Planar buf (2, n);
        buf.ch[0] = sine (freq, rate, n, static_cast<float> (std::pow (10.0, dbfs / 20.0)));
        if (! leftOnly)
            buf.ch[1] = buf.ch[0];
        buf.rebind();
        runGuard (g, buf, 256);
        return g.meters().leq5sDbA.load();
    };
    CHECK_NEAR (measure (1000.0, -20.0f, 100.0f, 0.0f, false), 80.0, 0.02);
    CHECK_NEAR (measure (1000.0, -20.0f, 100.0f, -15.0f, false), 65.0, 0.02);
    CHECK_NEAR (measure (1000.0, -20.0f, 100.0f, -15.0f, true), 65.0, 0.02); // the louder ear counts
    const auto aw = HearingGuard::aWeighting (rate);
    const double a100 = 20.0 * std::log10 (std::abs (aw[0].response (100.0, rate) * aw[1].response (100.0, rate) * aw[2].response (100.0, rate)));
    CHECK_NEAR (measure (100.0, -10.0f, 110.0f, 0.0f, false), 100.0 + a100, 0.05);
    CHECK_NEAR (a100, HearingGuard::aWeightingCurveDb (100.0), 0.05);
    CHECK (measure (1000.0, -20.0f, 100.0f, -std::numeric_limits<float>::infinity(), false) <= -99.0f); // muted

    // A new estimate every block: the Fast level follows, the session Leq holds.
    HearingGuard g;
    g.prepare (rate);
    CHECK (! g.meters().known.load());
    CHECK (g.meters().levelDbA.load() == HearingMeters::kUnknown);
    g.setSensitivityDbSpl (100.0f);
    Planar buf (2, n);
    buf.ch[0] = buf.ch[1] = sine (1000.0, rate, n, 0.1f);
    buf.rebind();
    runGuard (g, buf, 480);
    CHECK (g.meters().known.load());
    CHECK_NEAR (g.meters().levelDbA.load(), 80.0, 0.05);
    CHECK_NEAR (g.meters().sessionLeqDbA.load(), 80.0, 0.05);
    CHECK (! g.meters().capActive.load());

    // The sensitivity: the user's figure wins; a dB / mW figure needs the
    // source voltage (104 dB / mW, 32 ohm, 1 Vrms: 104 + 10 log10 (1000 / 32)).
    float chosen = 0.0f;
    CHECK (HearingGuard::chooseSensitivity (101.0f, 95.0f, chosen) == HearingGuard::SensitivitySource::User);
    CHECK_NEAR (chosen, 101.0, 1e-6);
    CHECK (HearingGuard::chooseSensitivity (std::numeric_limits<float>::quiet_NaN(), 95.0f, chosen) == HearingGuard::SensitivitySource::Profile);
    CHECK_NEAR (chosen, 95.0, 1e-6);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK (HearingGuard::chooseSensitivity (nan, nan, chosen) == HearingGuard::SensitivitySource::Unknown);
    CHECK (std::isnan (chosen));
    device::Sensitivity s;
    CHECK (std::isnan (device::splAtFullScale (s)));
    s.dbSplPerMw = 104.0f;
    s.impedanceOhm = 32.0f;
    CHECK (std::isnan (device::splAtFullScale (s)));
    CHECK_NEAR (device::splAtFullScale (s, 1.0f), 104.0 + 10.0 * std::log10 (1000.0 / 32.0), 1e-4);
    s.dbSplAtFullScale = 112.0f;
    CHECK_NEAR (device::splAtFullScale (s, 1.0f), 112.0, 1e-6);
}

TEST_CASE ("HearingGuard: device profiles carry a sensitivity, unknown by default")
{
    const char* text = R"({"format": "flubsound-device-profiles", "version": 1, "profiles": [
        {"id": "a", "matchAny": ["alpha"], "sensitivity": {"dbSplAtFullScale": 108, "source": "manufacturer"}},
        {"id": "b", "matchAny": ["beta"], "sensitivity": {"dbSplPerMw": 104, "impedanceOhm": 32}},
        {"id": "c", "matchAny": ["gamma"], "sensitivity": {"dbSplAtFullScale": "loud", "dbSplPerMw": 400}},
        {"id": "d", "matchAny": ["delta"]}]})";
    json::Value v;
    std::string error;
    REQUIRE (json::parse (text, v, error));
    device::Database db;
    REQUIRE (db.load (v, error));
    const auto& p = db.profiles();
    REQUIRE (p.size() == 4);
    CHECK_NEAR (device::splAtFullScale (p[0].sensitivity), 108.0, 1e-6);
    CHECK (p[0].sensitivity.source == "manufacturer");
    CHECK (std::isnan (device::splAtFullScale (p[1].sensitivity)));
    CHECK_NEAR (device::splAtFullScale (p[1].sensitivity, 0.5f), 104.0 + 10.0 * std::log10 (250.0 / 32.0), 1e-4);
    CHECK (std::isnan (p[2].sensitivity.dbSplAtFullScale)); // not a number / out of range: unknown
    CHECK (std::isnan (p[2].sensitivity.dbSplPerMw));
    CHECK (std::isnan (device::splAtFullScale (p[3].sensitivity)));

    // No shipped entry claims a figure yet (none published in a usable form
    // for these headsets; the device lab measures them).
    device::Database shipped;
    REQUIRE (shipped.loadBuiltIn (error));
    for (const auto& profile : shipped.profiles())
        CHECK (std::isnan (device::splAtFullScale (profile.sensitivity, 1.0f)));
}

TEST_CASE ("HearingGuard: the cap never exceeds its A-weighted Leq over 5 s")
{
    const double rate = 48000.0;
    const float sens = 110.0f, capDb = 85.0f;
    // Silence; under the cap; far over it (white noise at 0.9: about +24 dB);
    // silence; bursts (150 ms loud / 350 ms quiet); a steady level just
    // under the cap, then a jump to full scale; a full-scale 60 Hz tone.
    Planar in (2, 0);
    append (in, rate, 2.0, 0.0f, 0.0, 1);
    append (in, rate, 4.0, 0.006f, 0.0, 2);
    append (in, rate, 8.0, 0.9f, 0.0, 3);
    append (in, rate, 1.0, 0.0f, 0.0, 4);
    for (int b = 0; b < 16; ++b)
    {
        append (in, rate, 0.15, 0.9f, 0.0, 10u + static_cast<uint32_t> (b));
        append (in, rate, 0.35, 0.003f, 0.0, 40u + static_cast<uint32_t> (b));
    }
    append (in, rate, 10.0, 0.057f, 0.0, 5); // about 84.5 dB(A)
    append (in, rate, 4.0, 0.9f, 0.0, 6);
    append (in, rate, 4.0, 0.9f, 60.0, 0);
    Planar out = in;

    HearingGuard g;
    g.prepare (rate);
    g.setSensitivityDbSpl (sens);
    g.setCap (true, capDb);
    runGuard (g, out, 173); // host blocks that never line up with the chunks or segments
    const double worst = maxWindowLeqDbA (out, rate, HearingGuard::kCapWindowSeconds * rate, sens);
    const double before = maxWindowLeqDbA (in, rate, HearingGuard::kCapWindowSeconds * rate, sens);
    const uint64_t steps = g.meters().capSteps.load();

    // The slow gain in steady loud programme: the last 2 s of the 8 s block
    // settle kCapTargetUnderDb under the cap.
    Planar tail (2, 0);
    const int from = static_cast<int> (12.0 * rate), len = static_cast<int> (2.0 * rate);
    for (int c = 0; c < 2; ++c)
        tail.ch[static_cast<size_t> (c)].assign (out.ch[static_cast<size_t> (c)].begin() + from,
                                                 out.ch[static_cast<size_t> (c)].begin() + from + len);
    tail.rebind();
    const double settled = maxWindowLeqDbA (tail, rate, len, sens);

    // The gain's largest move per sample where the input is not tiny.
    double largestMove = 0.0, previous = -1.0;
    for (size_t k = 0; k < out.ch[0].size(); ++k)
    {
        const float x = in.ch[0][k];
        if (std::abs (x) < 0.02f)
        {
            previous = -1.0;
            continue;
        }
        const double gk = out.ch[0][k] / x;
        if (previous >= 0.0)
            largestMove = std::max (largestMove, std::abs (gk - previous));
        previous = gk;
    }
    std::cout << "    cap " << capDb << " dB(A): worst 5 s Leq " << worst << " dB(A) (uncapped " << before << "), steady "
              << settled << " dB(A), steps " << steps << ", largest gain move per sample " << largestMove << "\n";
    CHECK_LE (worst, capDb);
    CHECK (before > capDb + 15.0); // the programme really is far over it
    CHECK_NEAR (settled, capDb - HearingGuard::kCapTargetUnderDb, 0.3);
    CHECK (steps == 0u);
    CHECK_LE (largestMove, 0.05);

    // The same with a lower cap (the 75 dB(A) sensitive mode) and a 2 dB
    // quieter endpoint volume set mid-way.
    Planar out2 = in;
    HearingGuard g2;
    g2.prepare (rate);
    g2.setSensitivityDbSpl (sens);
    g2.setEndpointVolumeDb (-2.0f);
    g2.setCap (true, 75.0f);
    runGuard (g2, out2, 512);
    const double worst2 = maxWindowLeqDbA (out2, rate, HearingGuard::kCapWindowSeconds * rate, sens - 2.0);
    std::cout << "    cap 75 dB(A): worst 5 s Leq " << worst2 << " dB(A), steps " << g2.meters().capSteps.load() << "\n";
    CHECK_LE (worst2, 75.0);
}

TEST_CASE ("HearingGuard: off or unknown is bit-identical; switching off glides")
{
    const double rate = 48000.0;
    const int n = 256 * 560; // whole host blocks (about 3 s)
    Planar in (2, n);
    in.ch[0] = whiteNoise (n, 0.9f, 7);
    in.ch[1] = whiteNoise (n, 0.9f, 8);
    in.rebind();
    const auto same = [] (const Planar& a, const Planar& b) {
        return std::memcmp (a.ch[0].data(), b.ch[0].data(), a.ch[0].size() * sizeof (float)) == 0
               && std::memcmp (a.ch[1].data(), b.ch[1].data(), a.ch[1].size() * sizeof (float)) == 0;
    };
    {
        HearingGuard g; // unknown sensitivity, cap on: still nothing
        g.prepare (rate);
        g.setCap (true, 60.0f);
        Planar out = in;
        runGuard (g, out, 256);
        CHECK (same (out, in));
        CHECK (g.meters().leq5sDbA.load() == HearingMeters::kUnknown);
        CHECK_NEAR (g.meters().sessionDose.load(), 0.0, 0.0);
    }
    {
        HearingGuard g; // known, cap off: measured, untouched
        g.prepare (rate);
        g.setSensitivityDbSpl (120.0f);
        Planar out = in;
        runGuard (g, out, 256);
        CHECK (same (out, in));
        CHECK (g.meters().leq5sDbA.load() > 100.0f);
    }
    {
        HearingGuard g; // cap on, the level under it
        g.prepare (rate);
        g.setSensitivityDbSpl (80.0f);
        g.setCap (true, 100.0f);
        Planar out = in;
        runGuard (g, out, 256);
        CHECK (same (out, in));
    }

    // Through MixEngine: the default engine, one with a sensitivity and the
    // cap off, and one capped far above the programme give the same bits.
    const std::vector<StripConfig> layout { { "Music", 2, 0.0f, false } };
    std::vector<Planar> outs;
    for (int variant = 0; variant < 3; ++variant)
    {
        MixEngine e;
        e.configure (layout, rate, 256);
        if (variant >= 1)
            e.getHearingGuard().setSensitivityDbSpl (70.0f);
        if (variant == 2)
            e.getHearingGuard().setCap (true, 100.0f);
        Planar src = in, out (2, n);
        for (int pos = 0; pos < n; pos += 256)
        {
            AudioBlock inBlock = src.block (pos, 256);
            const AudioBlock* inputs[] = { &inBlock };
            e.process (inputs, out.block (pos, 256));
        }
        outs.push_back (out);
    }
    CHECK (same (outs[0], outs[1]));
    CHECK (same (outs[0], outs[2]));

    // Switched off while it holds the level down, the cap glides back up
    // (never a jump) and then leaves the signal alone again.
    HearingGuard g;
    g.prepare (rate);
    g.setSensitivityDbSpl (110.0f);
    g.setCap (true, 85.0f);
    Planar hold = in;
    runGuard (g, hold, 256);
    const float held = g.meters().capGainDb.load();
    g.setCap (false);
    Planar ramp (2, 0);
    append (ramp, rate, 45.0, 0.25f, 1000.0, 0);
    Planar rampOut = ramp;
    runGuard (g, rampOut, 256);
    double largestMove = 0.0, previous = -1.0;
    size_t firstUntouched = rampOut.ch[0].size();
    for (size_t k = 0; k < rampOut.ch[0].size(); ++k)
    {
        const float x = ramp.ch[0][k];
        if (std::abs (x) < 0.05f)
            continue;
        const double gk = rampOut.ch[0][k] / x;
        if (previous >= 0.0)
            largestMove = std::max (largestMove, std::abs (gk - previous));
        previous = gk;
        if (rampOut.ch[0][k] != x)
            firstUntouched = rampOut.ch[0].size();
        else if (firstUntouched == rampOut.ch[0].size())
            firstUntouched = k;
    }
    std::cout << "    cap off from " << held << " dB: back to untouched after " << static_cast<double> (firstUntouched) / rate
              << " s, largest gain move per sample " << largestMove << "\n";
    CHECK (held < -10.0f);
    CHECK_LE (largestMove, 1.0e-4);
    CHECK (firstUntouched < rampOut.ch[0].size());
    CHECK (! g.meters().capActive.load());
}

TEST_CASE ("HearingGuard: MixEngine keeps the settings, the dose and the cap across a swap")
{
    const double rate = 48000.0;
    const std::vector<StripConfig> layout { { "Game", 2, 0.0f, false } };
    MixEngine a;
    a.configure (layout, rate, 480);
    auto& ga = a.getHearingGuard();
    ga.setSensitivityDbSpl (112.0f);
    ga.setEndpointVolumeDb (-3.0f);
    ga.setCap (true, 80.0f);
    ga.setDoseBaseline (0.4);
    const int n = static_cast<int> (4.0 * rate);
    Planar src (2, n), out (2, n);
    src.ch[0] = whiteNoise (n, 0.5f, 3);
    src.ch[1] = whiteNoise (n, 0.5f, 4);
    src.rebind();
    for (int pos = 0; pos < n; pos += 480)
    {
        AudioBlock inBlock = src.block (pos, 480);
        const AudioBlock* inputs[] = { &inBlock };
        a.process (inputs, out.block (pos, 480));
    }
    const double doseA = ga.meters().sessionDose.load();
    const float gainA = ga.meters().capGainDb.load();
    CHECK (doseA > 0.0);
    CHECK (gainA < -3.0f);
    CHECK_NEAR (ga.meters().doseToday.load(), 0.4 + doseA, 1e-12);

    MixEngine b;
    b.configureFrom (a, layout, rate, 480);
    const auto& gb = b.getHearingGuard();
    CHECK_NEAR (gb.getSensitivityDbSpl(), 112.0, 0.0);
    CHECK_NEAR (gb.getEndpointVolumeDb(), -3.0, 0.0);
    CHECK (gb.getCapEnabled());
    CHECK_NEAR (gb.getCapDbA(), 80.0, 0.0);
    CHECK_NEAR (gb.meters().sessionDose.load(), doseA, 1e-15);
    CHECK_NEAR (gb.meters().doseToday.load(), 0.4 + doseA, 1e-12);
    CHECK_NEAR (gb.meters().capGainDb.load(), gainA, 1e-4);

    // A re-configure (a new rate) keeps the dose and the cap's gain too.
    a.configure (layout, 44100.0, 441);
    CHECK_NEAR (ga.meters().sessionDose.load(), doseA, 1e-15);
    CHECK_NEAR (ga.meters().capGainDb.load(), gainA, 1e-4);
    CHECK (ga.getCapEnabled());
}
