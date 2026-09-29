// High-frequency harshness control (docs/11 E07): the Smoothness stage
// (SmoothnessGuard.h), the chain's tonal-balance meter (TonalBalanceMeter.h)
// and the SafetyGovernor's tonal-balance rule (Protection.h):
//   * the stage passes its input bit for bit at amount 0 and when the input
//     keeps its reference's balance; it takes an "s" the enhancement made
//     hotter back to the reference's balance at any level, leaves vowels and
//     dense programme alone, caps Gaming's light guard, and does not depend
//     on the host's block size; handed the chain's output it also takes
//     back what a limiter after it adds to an "s" against the vowels
//     (batch 2);
//   * the meter reads no lift for a copy, a broadband gain or a pause, and
//     a presence bell's lift where an FFT of the same signals puts it;
//   * the rule takes its scale down while a band's lift is over its budget,
//     reports it, lets it recover, and leaves the drive scale and Off alone;
//   * through the chain: Smoothness 0 and protection strength Off are
//     bit-exact against a chain that never had them, the slot adds no
//     latency, its cut is on the meter bus, and at Normal the scale
//     reaches the presence and air macros and the Gaming voice band, but not
//     base values.
// The Done-when metrics through the CLI renderer are KnownGap cases in
// tests/test_known_gaps.cpp.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/dsp/Fft.h"
#include "flub/dsp/SmoothnessGuard.h"
#include "flub/dsp/Svf.h"
#include "flub/dsp/TonalBalanceMeter.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/engine/Protection.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iterator>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

int samplesOf (double seconds) { return static_cast<int> (std::lround (seconds * kFs)); }

/** Runs `x` through an SVF of the given design (double-free, the chain's filter). */
std::vector<float> filtered (const std::vector<float>& x, FilterType type, double f, double q, double gainDb)
{
    const SvfCoeffs c = SvfCoeffs::make (type, f, q, gainDb, kFs);
    SvfState s;
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i)
        y[i] = svfTick (c, s, x[i]);
    return y;
}

std::vector<float> scaled (std::vector<float> x, float g)
{
    for (auto& v : x)
        v *= g;
    return x;
}

/** Band power (Hann-windowed FFT, bins in [lo, hi)) of x[begin, begin + n), n <= 4096. */
double bandPower (const std::vector<float>& x, int begin, int n, double lo, double hi)
{
    constexpr int kN = 4096;
    static Fft fft;
    if (fft.getSize() != kN)
        fft.prepare (kN);
    std::vector<float> frame (kN, 0.0f);
    for (int i = 0; i < n; ++i)
        frame[static_cast<size_t> (i)] = x[static_cast<size_t> (begin + i)] * static_cast<float> (0.5 - 0.5 * std::cos (kTwoPi * i / n));
    std::vector<Fft::Complex> bins (kN / 2 + 1);
    fft.forwardReal (frame.data(), bins.data());
    double sum = 0.0;
    for (int k = 0; k <= kN / 2; ++k)
    {
        const double f = k * kFs / kN;
        if (f >= lo && f < hi)
            sum += std::norm (bins[static_cast<size_t> (k)]);
    }
    return sum;
}

/** Mean band power over consecutive 4096-sample frames of [begin, end). */
double bandPowerOver (const std::vector<float>& x, int begin, int end, double lo, double hi)
{
    double sum = 0.0;
    int frames = 0;
    for (int p = begin; p + 4096 <= end; p += 4096, ++frames)
        sum += bandPower (x, p, 4096, lo, hi);
    return frames > 0 ? sum / frames : 0.0;
}

double db (double p) { return 10.0 * std::log10 (std::max (p, 1.0e-30)); }

// ---- the sung-vocal stand-in (the report's stimulus, docs/11 E07) ----------
/** A 180 Hz "vowel" (22 harmonics, 1/k, 5 Hz vibrato) at -18 dBFS RMS, and
    a 120 ms Hann-gated "s" (white noise, 5 - 10 kHz, -24 dBFS RMS) every
    500 ms from 250 ms on, the vowel ducked by 80 % under it. */
struct Vocal
{
    std::vector<float> x;
    std::vector<int> onsets;
};

Vocal vocal (double seconds, float gainDb = 0.0f)
{
    const int n = samplesOf (seconds);
    Vocal v;
    v.x.assign (static_cast<size_t> (n), 0.0f);
    std::vector<double> voice (static_cast<size_t> (n));
    double phase = 0.0, power = 0.0;
    for (int i = 0; i < n; ++i)
    {
        phase += kTwoPi * 180.0 * (1.0 + 0.01 * std::sin (kTwoPi * 5.0 * i / kFs)) / kFs;
        double s = 0.0;
        for (int k = 1; k <= 22; ++k)
            s += std::sin (k * phase) / k;
        voice[static_cast<size_t> (i)] = s;
        power += s * s;
    }
    const double voiceGain = std::pow (10.0, -18.0 / 20.0) / std::sqrt (power / n);
    // 5 - 10 kHz: white noise through 4th-order Butterworth high- and low-passes.
    auto s = whiteNoise (n, 1.0f, 99);
    for (int stage = 0; stage < 2; ++stage)
    {
        s = filtered (s, FilterType::HighPass, 5000.0, butterworthQ (2, stage), 0.0);
        s = filtered (s, FilterType::LowPass, 10000.0, butterworthQ (2, stage), 0.0);
    }
    const double sGain = std::pow (10.0, -24.0 / 20.0) / rms (s.data(), n);
    std::vector<double> gate (static_cast<size_t> (n), 0.0);
    const int len = samplesOf (0.12);
    for (double t = 0.25; t + 0.3 < seconds; t += 0.5) // each "s" and the vowel window after it inside the signal
    {
        const int a = samplesOf (t);
        v.onsets.push_back (a);
        for (int i = 0; i < len; ++i)
            gate[static_cast<size_t> (a + i)] = 0.5 - 0.5 * std::cos (kTwoPi * i / len);
    }
    const double g = std::pow (10.0, gainDb / 20.0);
    for (size_t i = 0; i < v.x.size(); ++i)
        v.x[i] = static_cast<float> (g * (voiceGain * voice[i] * (1.0 - 0.8 * gate[i]) + sGain * s[i] * gate[i]));
    return v;
}

/** Sibilance (5 - 10 kHz, 30..90 ms into each "s") over voice (150 Hz - 2 kHz,
    200..260 ms after each onset), dB; the first burst is skipped. */
double sibilanceOverVoiceDb (const std::vector<float>& y, const std::vector<int>& onsets)
{
    double s = 0.0, v = 0.0;
    for (size_t k = 1; k < onsets.size(); ++k)
    {
        s += bandPower (y, onsets[k] + samplesOf (0.03), samplesOf (0.06), 5000.0, 10000.0);
        v += bandPower (y, onsets[k] + samplesOf (0.2), samplesOf (0.06), 150.0, 2000.0);
    }
    return db (s) - db (v);
}

/** The stage on `input` against `reference` in blocks of `block` (0 = ragged). */
std::vector<float> smooth (const std::vector<float>& input, const std::vector<float>& reference, SmoothnessParams params,
                           int block = 512)
{
    SmoothnessGuard g;
    g.setParams (params);
    g.prepare ({ kFs, 4096, 2 });
    std::vector<float> l (input), r (input), rl (reference), rr (reference);
    static constexpr int kRagged[] = { 1, 17, 480, 4096, 64, 333, 2048, 7 };
    size_t k = 0;
    for (size_t p = 0; p < input.size();)
    {
        const int n = static_cast<int> (std::min (static_cast<size_t> (block > 0 ? block : kRagged[k++ % std::size (kRagged)]), input.size() - p));
        float* io[2] = { l.data() + p, r.data() + p };
        float* ref[2] = { rl.data() + p, rr.data() + p };
        g.setReference (AudioBlock (ref, 2, n));
        g.process (AudioBlock (io, 2, n));
        p += static_cast<size_t> (n);
    }
    return l;
}
} // namespace

// =============================================================================
TEST_CASE ("Smoothness: at amount 0, or with an input that keeps its reference's balance, the stage passes the input bit for bit")
{
    const auto v = vocal (3.0);
    const auto brighter = filtered (v.x, FilterType::HighShelf, 4000.0, 0.7, 6.0);
    // Amount 0: nothing, whatever the reference says.
    CHECK (smooth (brighter, v.x, { 0.0f, false }) == brighter);
    // Full amount, input == reference: every ratio is the reference's.
    CHECK (smooth (v.x, v.x, { 1.0f, false }) == v.x);
    // A broadband gain keeps the balance (both ratios are level free).
    const auto louder = scaled (v.x, 2.0f);
    CHECK (smooth (louder, v.x, { 1.0f, false }) == louder);
}

TEST_CASE ("Smoothness: an \"s\" the enhancement made 6 dB hotter goes back to its reference's balance at any level; the vowels keep their lift")
{
    for (const float levelDb : { -20.0f, 0.0f, 10.0f })
    {
        const auto v = vocal (4.0, levelDb);
        // The enhancement: +6 dB on 4 - 16 kHz (a high shelf), which lifts the
        // vowel's top harmonics too.
        const auto lifted = filtered (v.x, FilterType::HighShelf, 4000.0, 0.7, 6.0);
        const auto out = smooth (lifted, v.x, { 1.0f, false });
        const double before = sibilanceOverVoiceDb (lifted, v.onsets) - sibilanceOverVoiceDb (v.x, v.onsets);
        const double after = sibilanceOverVoiceDb (out, v.onsets) - sibilanceOverVoiceDb (v.x, v.onsets);
        std::printf ("    measured level %+.0f dB: sibilance / voice +%.2f dB lifted, %+.2f dB after the stage\n", levelDb, before, after);
        CHECK (before > 5.0);
        CHECK (std::abs (after) <= 0.5);
        // The vowels (their 2 - 4 kHz top, 200..260 ms after each onset) keep the shelf's lift.
        double lv = 0.0, ov = 0.0;
        for (size_t k = 1; k < v.onsets.size(); ++k)
        {
            lv += bandPower (lifted, v.onsets[k] + samplesOf (0.2), samplesOf (0.06), 2000.0, 4000.0);
            ov += bandPower (out, v.onsets[k] + samplesOf (0.2), samplesOf (0.06), 2000.0, 4000.0);
        }
        std::printf ("    measured level %+.0f dB: vowels' 2 - 4 kHz %+.2f dB against the lifted input\n", levelDb, db (ov) - db (lv));
        // (The cut's 60 ms release is still -0.15 dB at 7 kHz 80 ms after the "s".)
        CHECK (std::abs (db (ov) - db (lv)) <= 0.25);
    }
}

TEST_CASE ("Smoothness: dense programme keeps a 2 dB brightening; Gaming's light guard takes back at most 6 dB; the output does not depend on the block size")
{
    // Pink noise brightened by 2 dB above 4 kHz: its sibilant band stays
    // under its body (pink reads -2.8 dB), so nothing is taken back.
    const auto pink = pinkNoise (samplesOf (3.0), 0.1f, 5);
    const auto bright = filtered (pink, FilterType::HighShelf, 4000.0, 0.7, 2.0);
    const auto out = smooth (bright, pink, { 1.0f, false });
    const double kept = db (bandPowerOver (out, samplesOf (0.5), samplesOf (3.0), 5000.0, 10000.0))
                        - db (bandPowerOver (pink, samplesOf (0.5), samplesOf (3.0), 5000.0, 10000.0));
    const double given = db (bandPowerOver (bright, samplesOf (0.5), samplesOf (3.0), 5000.0, 10000.0))
                         - db (bandPowerOver (pink, samplesOf (0.5), samplesOf (3.0), 5000.0, 10000.0));
    std::printf ("    measured pink +2 dB above 4 kHz: 5 - 10 kHz lift %.2f dB in, %.2f dB out\n", given, kept);
    // Pink sits 2.8 dB under the threshold on average; its 5 ms ratios
    // cross it now and then, where the lift is taken back: most of it stays.
    CHECK (kept >= given - 0.4);

    // An "s" made 12 dB hotter: Music takes back up to its 9 dB cap, Gaming 6 dB.
    const auto v = vocal (4.0);
    const auto hot = filtered (v.x, FilterType::HighShelf, 4000.0, 0.7, 12.0);
    const double ref = sibilanceOverVoiceDb (v.x, v.onsets);
    const double music = sibilanceOverVoiceDb (smooth (hot, v.x, { 1.0f, false }), v.onsets) - ref;
    const double gaming = sibilanceOverVoiceDb (smooth (hot, v.x, { 1.0f, true }), v.onsets) - ref;
    const double lifted = sibilanceOverVoiceDb (hot, v.onsets) - ref;
    std::printf ("    measured +12 dB shelf: sibilance / voice +%.2f dB lifted, Music %+.2f dB, Gaming %+.2f dB\n", lifted, music, gaming);
    CHECK (music <= lifted - 8.5);
    CHECK (gaming >= lifted - 6.5);
    CHECK (gaming <= lifted - 4.0);
    // Half the amount takes back about half.
    const double half = sibilanceOverVoiceDb (smooth (hot, v.x, { 0.5f, false }), v.onsets) - ref;
    CHECK (half > music + 3.0);
    CHECK (half < lifted - 3.0);

    // Per-sample processing, control ticks on absolute stream time.
    const auto a = smooth (hot, v.x, { 1.0f, false }, 64);
    CHECK (a == smooth (hot, v.x, { 1.0f, false }, 4096));
    CHECK (a == smooth (hot, v.x, { 1.0f, false }, 0));
}

TEST_CASE ("Smoothness: handed the chain's output, it holds an \"s\" to the voice's lift there - a limiter after it that ducks the loud vowels 1.5 dB more than the \"s\" is taken back too")
{
    // The chain after the stage, modelled: a 48-sample delay (the slots'
    // latency) and a gain that ducks the vowels 2 dB and each "s" 0.5 dB
    // (a limiter on the loud vowels, released under the quieter "s",
    // following the vowel's own envelope).
    constexpr int kDelay = 48;
    const auto v = vocal (4.0);
    const auto lifted = filtered (v.x, FilterType::HighShelf, 4000.0, 0.7, 6.0);
    const int n = static_cast<int> (v.x.size());
    std::vector<float> duck (v.x.size(), dbToGain (-2.0f));
    for (const int a : v.onsets)
        for (int i = 0, len = samplesOf (0.12); i < len && a + i < n; ++i)
            duck[static_cast<size_t> (a + i)] = dbToGain (-2.0f + 1.5f * static_cast<float> (0.5 - 0.5 * std::cos (kTwoPi * i / len)));
    const auto downstream = [&] (const std::vector<float>& stageOut, int i) {
        return i >= kDelay ? duck[static_cast<size_t> (i)] * stageOut[static_cast<size_t> (i - kDelay)] : 0.0f;
    };
    const auto run = [&] (bool handOutput, int block) {
        SmoothnessGuard g;
        g.setParams ({ 1.0f, false });
        g.setDownstreamDelay (kDelay);
        g.prepare ({ kFs, 4096, 2 });
        std::vector<float> l (lifted), r (lifted), rl (v.x), rr (v.x), ol (v.x.size()), orr (v.x.size());
        for (int p = 0; p < n; p += block)
        {
            const int m = std::min (block, n - p);
            float* io[2] = { l.data() + p, r.data() + p };
            float* ref[2] = { rl.data() + p, rr.data() + p };
            g.setReference (AudioBlock (ref, 2, m));
            g.process (AudioBlock (io, 2, m));
            for (int i = p; i < p + m; ++i)
            {
                ol[static_cast<size_t> (i)] = downstream (l, i);
                orr[static_cast<size_t> (i)] = downstream (r, i);
            }
            float* out[2] = { ol.data() + p, orr.data() + p };
            if (handOutput)
                g.processDownstream (AudioBlock (out, 2, m));
        }
        return ol;
    };
    // The input to compare with: the bypass through the same delay (the
    // metric's windows are 60 ms long; the 1 ms offset does not matter).
    const double ref = sibilanceOverVoiceDb (v.x, v.onsets);
    const double duckOnly = sibilanceOverVoiceDb (run (false, 480), v.onsets) - ref;
    const double coupled = sibilanceOverVoiceDb (run (true, 480), v.onsets) - ref;
    std::printf ("    measured 6 dB shelf, vowels ducked 1.5 dB more than the \"s\" after the stage: sibilance / voice %+.2f dB without the output, %+.2f dB with it\n",
                 duckOnly, coupled);
    // The stage alone cannot see the ducking; handed the output it takes
    // back all but what its detectors lag behind the duck's Hann ramps (the
    // stage without any duck leaves +0.15 dB, its bell against the flat band).
    CHECK (duckOnly >= 1.5);
    CHECK (coupled <= duckOnly - 1.0);
    CHECK (std::abs (coupled) <= 0.6);
    // The segment length only moves when g_down is read (the chain's 10 ms grid).
    CHECK (std::abs (sibilanceOverVoiceDb (run (true, 64), v.onsets) - ref - coupled) <= 0.15);
}

TEST_CASE ("Smoothness: the cut is sized for the 5 - 10 kHz band (G per band cut is monotonic, 1 at 0 dB), and below 20 kHz sample rate the stage does nothing")
{
    SmoothnessGuard g;
    g.prepare ({ kFs, 512, 2 });
    CHECK (g.gainForBandCut (0.0f) == 1.0f);
    float last = 1.0f;
    for (float c = 0.5f; c <= SmoothnessGuard::kMaxCutDb; c += 0.5f)
    {
        const float gain = g.gainForBandCut (c);
        CHECK (gain < last);
        last = gain;
    }
    // The bell is narrower than the band: G at its centre goes deeper than the band cut.
    CHECK (gainToDb (g.gainForBandCut (6.0f)) < -6.0f);

    SmoothnessGuard narrow;
    narrow.setParams ({ 1.0f, false });
    narrow.prepare ({ 16000.0, 512, 2 });
    const auto x = whiteNoise (16000, 0.3f, 3), quiet = scaled (x, 0.01f);
    std::vector<float> l (x), r (x), rl (quiet), rr (quiet);
    float* io[2] = { l.data(), r.data() };
    float* ref[2] = { rl.data(), rr.data() };
    narrow.setReference (AudioBlock (ref, 2, 16000));
    narrow.process (AudioBlock (io, 2, 16000));
    CHECK (l == x);
}

// =============================================================================
TEST_CASE ("TonalBalanceMeter: a copy, a broadband gain and a pause read no lift; a 3.2 kHz presence bell reads its lift in the presence band")
{
    const int n = samplesOf (4.0);
    const auto pink = pinkNoise (n, 0.1f, 8);
    const auto measure = [n] (const std::vector<float>& ref, const std::vector<float>& out, TonalBalanceMeter& m) {
        m.prepare (kFs);
        std::vector<float> rl (ref), rr (ref), ol (out), orr (out);
        for (int p = 0; p < n; p += 480)
        {
            float* r[2] = { rl.data() + p, rr.data() + p };
            float* o[2] = { ol.data() + p, orr.data() + p };
            m.processReference (AudioBlock (r, 2, 480));
            m.processOutput (AudioBlock (o, 2, 480));
            m.tick();
        }
    };
    TonalBalanceMeter m;
    measure (pink, pink, m);
    REQUIRE (m.hasReading());
    for (int b = TonalBalanceMeter::Presence; b < TonalBalanceMeter::kNumBands; ++b)
        CHECK (m.getLiftDb (b) == 0.0f);
    CHECK (m.getLiftDb (TonalBalanceMeter::Mids) == TonalBalanceMeter::kNoReading);
    measure (pink, scaled (pink, 3.0f), m);
    for (int b = TonalBalanceMeter::Presence; b < TonalBalanceMeter::kNumBands; ++b)
        CHECK (std::abs (m.getLiftDb (b)) <= 0.01f);

    // +6 dB bell at 3.2 kHz, Q 0.8 (the Clarity presence bell at full lift).
    const auto bell = filtered (pink, FilterType::Bell, 3200.0, 0.8, 6.0);
    measure (pink, bell, m);
    const auto fftLift = [&] (double lo, double hi) {
        return (db (bandPowerOver (bell, samplesOf (1.0), n, lo, hi)) - db (bandPowerOver (pink, samplesOf (1.0), n, lo, hi)))
               - (db (bandPowerOver (bell, samplesOf (1.0), n, 200.0, 1000.0)) - db (bandPowerOver (pink, samplesOf (1.0), n, 200.0, 1000.0)));
    };
    std::printf ("    measured presence bell: meter %.2f / %.2f / %.2f dB, FFT %.2f / %.2f / %.2f dB (presence / harsh / air)\n",
                 m.getLiftDb (TonalBalanceMeter::Presence), m.getLiftDb (TonalBalanceMeter::Harsh), m.getLiftDb (TonalBalanceMeter::Air),
                 fftLift (2000.0, 5000.0), fftLift (5000.0, 10000.0), fftLift (10000.0, 16000.0));
    CHECK (std::abs (m.getLiftDb (TonalBalanceMeter::Presence) - fftLift (2000.0, 5000.0)) <= 0.5);
    CHECK (std::abs (m.getLiftDb (TonalBalanceMeter::Harsh) - fftLift (5000.0, 10000.0)) <= 0.5);
    CHECK (m.getLiftDb (TonalBalanceMeter::Presence) > 3.0f);

    // A pause holds the readings.
    const float held = m.getLiftDb (TonalBalanceMeter::Presence);
    std::vector<float> silence (4800, 0.0f), s2 (silence);
    for (int k = 0; k < 20; ++k)
    {
        float* z[2] = { silence.data(), s2.data() };
        m.processReference (AudioBlock (z, 2, 480));
        m.processOutput (AudioBlock (z, 2, 480));
        m.tick();
    }
    // (Only the filters' ringing in the first silent window is averaged.)
    CHECK (std::abs (m.getLiftDb (TonalBalanceMeter::Presence) - held) <= 0.05f);
}

// =============================================================================
TEST_CASE ("SafetyGovernor tonal rule: a presence lift over the budget takes the tonal scale down and says so; under it the scale comes back; the drive scale and Off are untouched")
{
    SafetyGovernor g;
    g.prepare (kFs);
    g.setStrength (ProtectionStrength::Normal);
    g.setMusicMode (false); // Gaming: presence budget +2 dB
    const int tick = samplesOf (0.01);
    SafetyGovernor::Readings r; // nothing else over any budget
    r.presenceLiftDb = 5.0f;
    r.harshLiftDb = r.airLiftDb = 0.0f;
    for (int k = 0; k < 300; ++k)
        g.updateMeasured (r, tick);
    const float down = g.getTonalScale();
    CHECK (down < 0.8f);
    CHECK ((g.getReason() & SafetyGovernor::kReasonTonal) != 0);
    CHECK ((g.getReason() & (SafetyGovernor::kReasonLimiter | SafetyGovernor::kReasonDistortion)) == 0);
    CHECK (g.getScale() == 1.0f);
    CHECK (g.getHarmonicsScale() == 1.0f);
    // At the set point (budget - kTonalMarginDb) it holds ...
    r.presenceLiftDb = 2.0f - SafetyGovernor::kTonalMarginDb - 0.5f;
    for (int k = 0; k < 300; ++k)
        g.updateMeasured (r, tick);
    CHECK (std::abs (g.getTonalScale() - down) < 0.05f);
    // ... and a darker programme gets its lifts back (after the probe hold).
    r.presenceLiftDb = -1.0f;
    for (int k = 0; k < 9000; ++k)
        g.updateMeasured (r, tick);
    CHECK (g.getTonalScale() == 1.0f);
    CHECK (g.getReason() == 0);
    CHECK (g.getState() == SafetyGovernor::State::Idle);

    // The budgets: Music is allowed more, Strict less.
    const auto music = SafetyGovernor::budgetsFor (ProtectionStrength::Normal, true);
    const auto gaming = SafetyGovernor::budgetsFor (ProtectionStrength::Normal, false);
    const auto strict = SafetyGovernor::budgetsFor (ProtectionStrength::Strict, false);
    CHECK (gaming.presenceDb == 2.0f); // docs/11 E07 Done-when
    CHECK (music.presenceDb > gaming.presenceDb);
    CHECK (strict.presenceDb < gaming.presenceDb);

    // Off: the stepwise loop, no tonal rule.
    SafetyGovernor off;
    off.prepare (kFs);
    r.presenceLiftDb = 12.0f;
    for (int k = 0; k < 300; ++k)
        off.updateMeasured (r, tick);
    CHECK (off.getTonalScale() == 1.0f);
    // Back to Off from Normal: the scale is 1 at once.
    g.updateMeasured (r, 300 * tick);
    REQUIRE (g.getTonalScale() < 1.0f);
    g.setStrength (ProtectionStrength::Off);
    CHECK (g.getTonalScale() == 1.0f);
}

// =============================================================================
namespace
{
/** Runs a chain over `x` (both channels) in 512-sample blocks. */
std::vector<float> runChain (ProcessingChain& chain, const std::vector<float>& x, std::vector<float>* right = nullptr)
{
    ScopedNoDenormals noDenormals;
    std::vector<float> l (x), r (x);
    for (size_t p = 0; p < x.size(); p += 512)
    {
        const int n = static_cast<int> (std::min<size_t> (512, x.size() - p));
        float* ch[2] = { l.data() + p, r.data() + p };
        chain.process (AudioBlock (ch, 2, n));
    }
    if (right != nullptr)
        *right = r;
    return l;
}
} // namespace

TEST_CASE ("Chain: Smoothness 0 is bit-exact, the slot adds no latency, and switching Smoothness on and off fades without a step")
{
    // Two chains with the same history, one of them with smooth.amount at 1
    // for 3 s; both back at 0 and reset: from then on they must agree bit
    // for bit (the stage at 0 is out of the signal path).
    const auto v = vocal (3.0);
    ParameterStore plain, withSlot;
    for (auto* s : { &plain, &withSlot })
    {
        s->set (Mode, 0.0f);
        s->set (BoostIntensity, 1.0f);
        s->set (Macro3, 1.0f);
    }
    ProcessingChain a (plain), b (withSlot);
    a.prepare ({ kFs, 512, 2 });
    withSlot.set (SmoothAmount, 1.0f);
    b.prepare ({ kFs, 512, 2 });
    CHECK (b.getLatencySamples() == a.getLatencySamples());
    const auto ya0 = runChain (a, v.x), yb0 = runChain (b, v.x);
    CHECK (ya0 != yb0); // the stage acted
    withSlot.set (SmoothAmount, 0.0f);
    const std::vector<float> silence (4800, 0.0f);
    runChain (a, silence);
    runChain (b, silence); // the slot fades out and stops
    a.reset();
    b.reset();
    const auto ya = runChain (a, v.x), yb = runChain (b, v.x);
    CHECK (ya == yb);
    CHECK (b.getSmoothnessCutDb() == 0.0f);
    CHECK (b.meters().smoothnessCutDb.load() == 0.0f);

    // On and off mid-programme: the slot's 20 ms crossfade and the stage's
    // smoothed amount; no sample-to-sample step beyond the programme's own.
    // The cut is on the meter bus while the stage runs (MeterBus::smoothnessCutDb).
    std::vector<float> l (v.x), r (v.x);
    float deepestOnBus = 0.0f;
    for (size_t p = 0; p < v.x.size(); p += 512)
    {
        if (p == 48 * 512 || p == 150 * 512)
            withSlot.set (SmoothAmount, p == 48 * 512 ? 1.0f : 0.0f);
        const int n = static_cast<int> (std::min<size_t> (512, v.x.size() - p));
        float* ch[2] = { l.data() + p, r.data() + p };
        b.process (AudioBlock (ch, 2, n));
        deepestOnBus = std::min (deepestOnBus, b.meters().smoothnessCutDb.load());
    }
    CHECK (deepestOnBus < -0.5f);
    float maxStep = 0.0f, maxStepRef = 0.0f;
    for (size_t i = 1; i < l.size(); ++i)
    {
        maxStep = std::max (maxStep, std::abs (l[i] - l[i - 1]));
        maxStepRef = std::max (maxStepRef, std::abs (ya[i] - ya[i - 1]));
    }
    CHECK (maxStep <= 1.05f * maxStepRef);
}

TEST_CASE ("Chain: at protection strength Normal the tonal scale reaches the macros' presence and air and the Gaming voice band, never a base value or Off")
{
    // Gaming, Boost 100 and every macro 100 on -30 dBFS pink (the E07
    // Done-when scene): the net 2-5 kHz lift is over the +2 dB budget.
    const auto x = pinkNoise (samplesOf (8.0), std::pow (10.0f, -30.0f / 20.0f), 21);
    const auto setup = [] (ParameterStore& s, float basePresence) {
        s.set (Mode, 1.0f);
        s.set (BoostIntensity, 1.0f);
        for (int m = Macro1; m <= Macro5; ++m)
            s.set (m, 1.0f);
        s.set (ClarityPresence, basePresence);
    };
    ParameterStore store;
    setup (store, 0.0f);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, 512, 2 });
    chain.setProtectionStrength (ProtectionStrength::Normal);
    runChain (chain, x);
    const float scale = chain.getGovernorTonalScale();
    std::printf ("    measured Gaming full stack at Normal after 8 s: tonal scale %.3f, presence lift %.2f dB, reason %u\n", scale,
                 chain.getTonalLiftDb (TonalBalanceMeter::Presence), chain.meters().governorReason.load());
    CHECK (scale < 0.7f);
    CHECK ((chain.meters().governorReason.load() & SafetyGovernor::kReasonTonal) != 0);
    CHECK (chain.getTonalLiftDb (TonalBalanceMeter::Presence) <= 2.0f);
    // The macros' presence (Boost 0.3 + Voice & Score 0.7 = 1) is scaled; air too (Detail 0.4).
    CHECK (std::abs (chain.effectiveValue (ClarityPresence) - scale) <= 0.02f);
    CHECK (chain.effectiveValue (ClarityAir) < 0.4f * scale + 0.02f);
    // The automatic preamp's prediction still sees the unscaled lifts.
    // (Its key would otherwise follow the loop; checked through the prediction.)
    const float predicted = chain.getPredictedBoostDb();

    // A base presence of 0.5 stays: only what the macros add is scaled.
    ParameterStore based;
    setup (based, 0.5f);
    ProcessingChain withBase (based);
    withBase.prepare ({ kFs, 512, 2 });
    withBase.setProtectionStrength (ProtectionStrength::Normal);
    runChain (withBase, x);
    CHECK (withBase.effectiveValue (ClarityPresence) >= 0.5f);

    // Off: no rule, no meter.
    ParameterStore offStore;
    setup (offStore, 0.0f);
    ProcessingChain off (offStore);
    off.prepare ({ kFs, 512, 2 });
    runChain (off, x);
    CHECK (off.getGovernorTonalScale() == 1.0f);
    CHECK (off.getTonalLiftDb (TonalBalanceMeter::Presence) == TonalBalanceMeter::kNoReading);
    CHECK (off.effectiveValue (ClarityPresence) == 1.0f);
    CHECK (std::abs (off.getPredictedBoostDb() - predicted) <= 0.01f);
}

// =============================================================================
// docs/11 E07: the rule counts the upward lifts. The meter divides every
// window by the programme's level, so a quiet passage weighs as much as a
// loud one: what lifts the quiet passages - the inverse-level presence, the
// dynamic EQ's boost-below bands, the upward compressor - is no longer
// hidden under the loud passages' power.
namespace
{
/** 0.5 s passages, alternately `loud` and `quiet` (the first is loud). */
std::vector<float> alternate (const std::vector<float>& loud, const std::vector<float>& quiet)
{
    std::vector<float> x (loud.size());
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (static_cast<int> (i) / samplesOf (0.5)) % 2 == 0 ? loud[i] : quiet[i];
    return x;
}

bool quietPassage (int i) { return (i / samplesOf (0.5)) % 2 == 1; }

/** The meter over `ref` / `out` (stereo copies), 10 ms ticks. */
void meterOver (TonalBalanceMeter& m, const std::vector<float>& ref, const std::vector<float>& out)
{
    m.prepare (kFs);
    std::vector<float> rl (ref), rr (ref), ol (out), orr (out);
    for (size_t p = 0; p + 480 <= ref.size(); p += 480)
    {
        float* r[2] = { rl.data() + p, rr.data() + p };
        float* o[2] = { ol.data() + p, orr.data() + p };
        m.processReference (AudioBlock (r, 2, 480));
        m.processOutput (AudioBlock (o, 2, 480));
        m.tick();
    }
}

/** What a plain power average over [from, end) reads: presence (2 - 5 kHz) lift over the 200 Hz - 1 kHz lift, FFT. */
double powerAverageLiftDb (const std::vector<float>& ref, const std::vector<float>& out, int from)
{
    const int end = static_cast<int> (ref.size());
    return (db (bandPowerOver (out, from, end, 2000.0, 5000.0)) - db (bandPowerOver (ref, from, end, 2000.0, 5000.0)))
           - (db (bandPowerOver (out, from, end, 200.0, 1000.0)) - db (bandPowerOver (ref, from, end, 200.0, 1000.0)));
}
} // namespace

TEST_CASE ("TonalBalanceMeter (E07): every passage weighs the same - the lifts of quiet passages (inverse-level presence, the upward compressor) count; a plain power average hid them")
{
    const int n = samplesOf (6.0);
    const auto pink = pinkNoise (n, 1.0f, 31);
    const float loud = std::pow (10.0f, -12.0f / 20.0f), quiet = std::pow (10.0f, -45.0f / 20.0f);
    TonalBalanceMeter m;

    // (a) The inverse-level presence: the +6 dB bell on the quiet passages only.
    const auto ref = alternate (scaled (pink, loud), scaled (pink, quiet));
    const auto belled = filtered (ref, FilterType::Bell, 3200.0, 0.8, 6.0);
    std::vector<float> out (ref);
    for (int i = 0; i < n; ++i)
        if (quietPassage (i))
            out[static_cast<size_t> (i)] = belled[static_cast<size_t> (i)];
    meterOver (m, ref, out);
    const float presence = m.getLiftDb (TonalBalanceMeter::Presence);
    const double plain = powerAverageLiftDb (ref, out, samplesOf (1.0));
    std::printf ("    measured +6 dB bell on the quiet passages (-45 against -12 dBFS pink): meter %.2f dB, plain power average %.2f dB\n", presence, plain);
    CHECK (presence >= 1.5f); // half the time lifted by the bell's ~4.4 dB band lift
    CHECK (plain <= 0.1);

    // (b) The upward compressor: the quiet passages lifted 8 dB as a whole,
    // where they are brighter than the loud ones (dark explosions, quiet
    // footsteps and foliage). Each passage keeps its own balance; the
    // programme brightens.
    const auto dark = filtered (filtered (pink, FilterType::LowPass, 2000.0, 0.7071, 0.0), FilterType::LowPass, 2000.0, 0.7071, 0.0);
    const auto ref2 = alternate (scaled (dark, loud), scaled (pink, quiet));
    auto out2 = ref2;
    for (int i = 0; i < n; ++i)
        if (quietPassage (i))
            out2[static_cast<size_t> (i)] *= std::pow (10.0f, 8.0f / 20.0f);
    meterOver (m, ref2, out2);
    const float upward = m.getLiftDb (TonalBalanceMeter::Presence);
    const double plain2 = powerAverageLiftDb (ref2, out2, samplesOf (1.0));
    std::printf ("    measured 8 dB upward lift of bright quiet passages between dark loud ones: meter %.2f dB, plain power average %.2f dB\n", upward,
                 plain2);
    CHECK (upward >= 1.5f);
    CHECK (plain2 <= 0.3);

    // (c) The same lift on every passage (a broadband gain on the whole
    // programme) still reads nothing.
    meterOver (m, ref2, scaled (ref2, 2.5f));
    for (int b = TonalBalanceMeter::Presence; b < TonalBalanceMeter::kNumBands; ++b)
        CHECK (std::abs (m.getLiftDb (b)) <= 0.01f);
}

TEST_CASE ("Chain (E07): at protection strength Normal the tonal rule sees the presence the quiet passages get - Music Boost 100 + Clarity 100 on -12 / -45 dBFS pink passages")
{
    // The absolute presence law gives the -45 dBFS passages its full lift
    // and the -12 dBFS ones almost none (docs/11 E07's presence row).
    const int n = samplesOf (8.0);
    const auto pink = pinkNoise (n, 1.0f, 44);
    const auto x = alternate (scaled (pink, std::pow (10.0f, -12.0f / 20.0f)), scaled (pink, std::pow (10.0f, -45.0f / 20.0f)));
    const auto quietLift = [&] (const std::vector<float>& y) {
        // Presence over mids lift in the quiet passages of the last 4 s (their middle 400 ms).
        double po = 0.0, pi = 0.0, mo = 0.0, mi = 0.0;
        for (int a = samplesOf (4.5); a + samplesOf (0.5) <= n; a += samplesOf (1.0))
        {
            const int b = a + samplesOf (0.05), len = 4096;
            po += bandPower (y, b, len, 2000.0, 5000.0) + bandPower (y, b + len, len, 2000.0, 5000.0);
            pi += bandPower (x, b, len, 2000.0, 5000.0) + bandPower (x, b + len, len, 2000.0, 5000.0);
            mo += bandPower (y, b, len, 200.0, 1000.0) + bandPower (y, b + len, len, 200.0, 1000.0);
            mi += bandPower (x, b, len, 200.0, 1000.0) + bandPower (x, b + len, len, 200.0, 1000.0);
        }
        return (db (po) - db (pi)) - (db (mo) - db (mi));
    };
    double lift[3] = {};
    float scale[3] = {}, reading[3] = {};
    for (const auto s : { ProtectionStrength::Off, ProtectionStrength::Normal, ProtectionStrength::Strict })
    {
        ParameterStore store;
        store.set (BoostIntensity, 1.0f);
        store.set (Macro3, 1.0f);
        ProcessingChain chain (store);
        chain.prepare ({ kFs, 512, 2 });
        chain.setProtectionStrength (s);
        const auto y = runChain (chain, x);
        // The chain's latency is under 5 ms: inside the 50 ms margin of each window.
        const auto k = static_cast<size_t> (s);
        lift[k] = quietLift (y);
        scale[k] = chain.getGovernorTonalScale();
        reading[k] = chain.getTonalLiftDb (TonalBalanceMeter::Presence);
        std::printf ("    measured strength %d: quiet passages' presence over mids lift %.2f dB, tonal scale %.3f, meter %.2f dB\n", static_cast<int> (s),
                     lift[k], scale[k], reading[k]);
    }
    // The rule sees the quiet passages' lift and holds the programme to the
    // Music budget (+3 dB at Normal, +1.5 dB at Strict); a plain power
    // average read the loud passages only and left the scale at 1.
    CHECK (reading[1] >= 1.5f);
    CHECK (scale[1] < 1.0f);
    CHECK (scale[2] < 0.6f);
    CHECK (lift[2] <= lift[0] - 1.5);
    CHECK (reading[2] <= 1.5f);
}
