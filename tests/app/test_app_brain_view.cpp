// App-level tests: the brain visualiser ("brain", docs/06 §6.4.2): the anatomy
// and its tract graph, the tonotopic maps, the crossed ascending pathway (a
// tone in one ear lights the opposite auditory cortex more) and the superior
// olive's wiring, the beat network, the repeat rule for snares and kicks
// (gunshots, bursts, backbeats, four on the floor, synthwave, footsteps,
// explosions, a rock beat, half-time grooves), chord surprise and its timing, build-up
// and drop, the heuristics' false triggers on the app's own test music,
// synthetic loops, pauses, a re-struck chord and noise, NaN / inf input,
// intensity, hover names, the picture's handedness (not a mirror image), no
// render while still and dark, no allocation per frame and the frame time at
// 1280 x 720 and 1920 x 1080.
// Set FLUB_BRAIN_SHOTS=<folder> to also write rendered PNGs of the view there.
#include "AppTestSupport.h"

#include "engine/TestSignalGenerator.h"
#include "ui/MeterSnapshot.h"
#include "ui/vis/BrainAnatomy.h"
#include "ui/vis/BrainView.h"
#include "ui/vis/PitchEstimator.h"

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace flub::app;
namespace vis = flub::app::ui::vis;
namespace brain = flub::app::ui::vis::brain;

namespace
{
constexpr double kPi = juce::MathConstants<double>::pi;
constexpr double kRate = 48000.0;
constexpr int kFrame = 800; // one 60 Hz display frame
constexpr double kDt = kFrame / kRate;

using brain::Station;

int nodeOf (Station s, int side)
{
    return brain::node (s, side);
}

/** Feeds a BrainView frame by frame from a stereo generator gen (n, l, r). */
struct Runner
{
    vis::BrainView view;
    ui::MeterSnapshot meters;
    std::vector<float> mid = std::vector<float> (kFrame), side = std::vector<float> (kFrame);
    int64_t n = 0;

    Runner() { view.setSampleRate (kRate); }

    template <typename Gen>
    void run (int frames, Gen&& gen, const std::function<void()>& each = {})
    {
        for (int f = 0; f < frames; ++f)
        {
            for (size_t i = 0; i < static_cast<size_t> (kFrame); ++i)
            {
                float l = 0.0f, r = 0.0f;
                gen (n++, l, r);
                mid[i] = 0.5f * (l + r);
                side[i] = 0.5f * (l - r);
            }
            view.pushPost (mid.data(), side.data(), kFrame);
            view.advance (vis::FrameContext { meters, kDt, kRate });
            if (each)
                each();
        }
    }
    float glow (Station s, int sideIndex) const { return view.getActivity().glow (nodeOf (s, sideIndex)).level; }
};

double seconds (int64_t n)
{
    return static_cast<double> (n) / kRate;
}

/** Deterministic noise in -1..1. */
struct Noise
{
    uint32_t state = 12345u;
    float operator()() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float> (static_cast<double> (state >> 8) / 8388608.0 - 1.0);
    }
};

struct HighPass
{
    float a = 0.0f, x1 = 0.0f, y1 = 0.0f;
    explicit HighPass (double hz) : a (static_cast<float> (std::exp (-2.0 * kPi * hz / kRate))) {}
    float operator() (float x) noexcept
    {
        y1 = a * (y1 + x - x1);
        x1 = x;
        return y1;
    }
};

struct LowPass
{
    float a = 0.0f, y1 = 0.0f;
    explicit LowPass (double hz) : a (static_cast<float> (1.0 - std::exp (-2.0 * kPi * hz / kRate))) {}
    float operator() (float x) noexcept
    {
        y1 += a * (x - y1);
        return y1;
    }
};

float decay (double tau, double timeConstant) noexcept
{
    return tau < 0.0 ? 0.0f : static_cast<float> (std::exp (-tau / timeConstant));
}

/** The test generator's kick: a pitch-swept sine and a short click (tau seconds after the hit). */
float kick (double tau, float click) noexcept
{
    if (tau < 0.0)
        return 0.0f;
    const double phase = 2.0 * kPi * (48.0 * tau + 110.0 * 0.035 * (1.0 - std::exp (-tau / 0.035)));
    return 0.85f * static_cast<float> (std::sin (phase)) * decay (tau, 0.28) + 0.25f * click * decay (tau, 0.002);
}

/** The test music's snare (tau seconds after the hit): band-passed noise (1.2 - 7 kHz) and a 185 Hz body. */
struct Snare
{
    HighPass hp { 1200.0 };
    LowPass lp { 7000.0 };
    float operator() (double tau, float noise) noexcept
    {
        const float wires = lp (hp (noise));
        if (tau < 0.0)
            return 0.0f;
        return 0.55f * wires * decay (tau, 0.14) + 0.3f * static_cast<float> (std::sin (2.0 * kPi * 185.0 * tau)) * decay (tau, 0.07);
    }
};

/** Gunshots at `times` (seconds, ascending): a noise crack (1 ms rise, 25 ms decay) over a body of noise under
    1 kHz (90 ms decay), `level` at the peak. */
struct Gunshots
{
    std::vector<double> times;
    float level = 0.5f;
    Noise noise { 987654321u }; // not the music's noise
    LowPass body { 1000.0 };
    size_t next = 0;
    double last = -1.0;
    float operator() (int64_t n) noexcept
    {
        const double t = seconds (n);
        const float x = noise();
        const float low = body (x);
        while (next < times.size() && times[next] <= t)
            last = times[next++];
        if (last < 0.0)
            return 0.0f;
        const double tau = t - last;
        return level * std::min (1.0f, static_cast<float> (tau / 0.001)) * (x * decay (tau, 0.025) + 2.5f * low * decay (tau, 0.09));
    }
};

/** Single shots at irregular gaps (0.9, 0.45, 1.3, 0.6, 1.7, 0.75, 0.35, 1.1 s: no gap within 15 % of the one before). */
const std::vector<double> kIrregularShots { 0.3, 1.2, 1.65, 2.95, 3.55, 5.25, 6.0, 6.35, 7.45 };

/** Whether a beat log holds a beat within `tolerance` seconds of t. */
bool beatNear (const brain::BrainListener& listener, double t, double tolerance)
{
    for (int i = 0; i < listener.getNumBeatTimes(); ++i)
        if (std::abs (listener.getBeatTime (i) - t) < tolerance)
            return true;
    return false;
}

/** A sum of harmonic notes (MIDI), 4 harmonics each at level / h. */
float chordSample (const std::vector<int>& midis, int64_t n, double level = 0.06)
{
    double v = 0.0;
    for (const int m : midis)
    {
        const double f0 = vis::PitchEstimator::midiToHz (m);
        for (int h = 1; h <= 4; ++h)
            v += level / h * std::sin (2.0 * kPi * f0 * h * static_cast<double> (n) / kRate + 0.7 * h + 0.3 * m);
    }
    return static_cast<float> (v);
}

/** The app's own test music (the screenshot scene), rendered once. */
struct TestMusic
{
    std::vector<float> l, r;
    explicit TestMusic (double secondsLong)
    {
        const int n = static_cast<int> (secondsLong * kRate);
        l.resize (static_cast<size_t> (n));
        r.resize (static_cast<size_t> (n));
        float* chans[2] = { l.data(), r.data() };
        TestSignalGenerator gen (kRate);
        gen.setProgramme (0, TestSignalGenerator::Programme::Music);
        gen.renderStrip (0, flub::AudioBlock (chans, 2, n));
    }
};
} // namespace

// =============================================================================
// Anatomy
// =============================================================================
TEST_CASE ("App: brain anatomy: every tract runs from one station to another, from each ear to both auditory cortices")
{
    const auto& a = brain::BrainAnatomy::get();
    const auto& tracts = a.getTracts();
    const auto& points = a.getTractPoints();
    REQUIRE (! tracts.empty());
    std::set<std::string> names;
    std::string crossers;
    int auditory = 0, crossing = 0;
    for (const auto& t : tracts)
    {
        CHECK (names.insert (t.name).second);
        REQUIRE (t.from >= 0);
        REQUIRE (t.from < brain::kNumNodes);
        REQUIRE (t.to >= 0);
        REQUIRE (t.to < brain::kNumNodes);
        CHECK (t.from != t.to);
        const auto& from = a.getNode (t.from);
        const auto& to = a.getNode (t.to);
        const auto first = points[static_cast<size_t> (t.firstPoint)];
        const auto last = points[static_cast<size_t> (t.firstPoint + brain::BrainAnatomy::kSegments)];
        // The drawn line starts and ends on its stations (within their drawn size).
        if (brain::length (first - from.position) > from.extent + 1.0e-4f || brain::length (last - to.position) > to.extent + 1.0e-4f)
            std::printf ("  %s: %.3f from %s, %.3f from %s\n", t.name, static_cast<double> (brain::length (first - from.position)), from.name,
                         static_cast<double> (brain::length (last - to.position)), to.name);
        CHECK (brain::length (first - from.position) <= from.extent + 1.0e-4f);
        CHECK (brain::length (last - to.position) <= to.extent + 1.0e-4f);
        if (t.kind == brain::TractKind::Auditory)
        {
            ++auditory;
            CHECK (t.delayEnd > t.delayStart);
            CHECK (t.channel != brain::Channel::None);
        }
        if (first.x * last.x < 0.0f)
        {
            ++crossing;
            crossers += std::string (crossers.empty() ? "" : ", ") + t.name;
        }
    }
    std::printf ("  tracts crossing the midline: %s\n", crossers.c_str());
    CHECK (tracts.size() == 44);
    CHECK (auditory == 24);
    // Per side: the trapezoid body to the other MSO and (via the MNTB) the other LSO, the LSO's output and the cochlear
    // nucleus's direct route to the other lateral lemniscus, the pons to the other cerebellum, the cerebellum to the
    // other thalamus.
    CHECK (crossing == 12);

    // Reachability along the tracts: each ear reaches both auditory cortices; every station is reached
    // from the ears or the VTA (the dopamine source).
    const auto reach = [&tracts] (std::vector<int> start)
    {
        std::vector<bool> seen (static_cast<size_t> (brain::kNumNodes), false);
        for (const int s : start)
            seen[static_cast<size_t> (s)] = true;
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const auto& t : tracts)
                if (seen[static_cast<size_t> (t.from)] && ! seen[static_cast<size_t> (t.to)])
                    grew = seen[static_cast<size_t> (t.to)] = true;
        }
        return seen;
    };
    for (int ear = brain::kLeft; ear <= brain::kRight; ++ear)
    {
        const auto seen = reach ({ nodeOf (Station::Ear, ear) });
        for (int side = brain::kLeft; side <= brain::kRight; ++side)
            for (const Station s : { Station::Mso, Station::Lso, Station::InferiorColliculus, Station::Mgn, Station::Heschl, Station::Premotor,
                                     Station::Sma, Station::Putamen, Station::Ifg, Station::Accumbens, Station::Cerebellum, Station::Ventrolateral })
                CHECK (seen[static_cast<size_t> (nodeOf (s, side))]);
        CHECK (! seen[static_cast<size_t> (nodeOf (Station::Ear, 1 - ear))]);
    }
    const auto all = reach ({ nodeOf (Station::Ear, brain::kLeft), nodeOf (Station::Ear, brain::kRight), nodeOf (Station::Vta, brain::kLeft) });
    for (int n = 0; n < brain::kNumNodes; ++n)
        CHECK (all[static_cast<size_t> (n)]);

    // The table: every station has approximate MNI coordinates and a source; the mapping to the view's axes.
    CHECK (brain::BrainAnatomy::mniTable().size() == static_cast<size_t> (brain::kNumStations));
    for (const auto& e : brain::BrainAnatomy::mniTable())
        CHECK (std::string (e.source).size() > 8);
    const auto v = brain::BrainAnatomy::fromMni ({ 90.0f, -18.0f, 10.0f });
    CHECK_NEAR (v.x, 1.0, 1.0e-6);
    CHECK_NEAR (v.y, 0.0, 1.0e-6);
    CHECK_NEAR (v.z, 0.0, 1.0e-6);
    const auto up = brain::BrainAnatomy::fromMni ({ 0.0f, 72.0f, 100.0f });
    CHECK_NEAR (up.y, 1.0, 1.0e-6); // MNI z (superior) is up
    CHECK_NEAR (up.z, 1.0, 1.0e-6); // MNI y (anterior) is the front

    // Right is +x, left -x; the cloud holds both hemispheres, the cerebellum, the brainstem and the landing spots.
    for (int n = 0; n < brain::kNumNodes; ++n)
    {
        const auto& node = a.getNode (n);
        if (node.station != Station::Vta)
            CHECK ((node.side == brain::kRight) == (node.position.x > 0.0f));
    }
    int cortex = 0, heschl = 0, members = 0;
    for (const auto& p : a.getPoints())
    {
        cortex += p.kind == brain::PointKind::Cortex ? 1 : 0;
        heschl += p.kind == brain::PointKind::Heschl ? 1 : 0;
        members += p.node >= 0 ? 1 : 0;
    }
    std::printf ("  points: %d (cortex %d, Heschl %d, landing-spot members %d); tracts %d\n", static_cast<int> (a.getPoints().size()), cortex,
                 heschl, members, static_cast<int> (tracts.size()));
    CHECK (cortex > 5000);
    CHECK (heschl == 1120);
    for (const Station s : { Station::Premotor, Station::Sma, Station::Ifg })
        for (int side = brain::kLeft; side <= brain::kRight; ++side)
        {
            int count = 0;
            for (const auto& p : a.getPoints())
                count += p.node == nodeOf (s, side) ? 1 : 0;
            CHECK (count >= 20); // a visible patch of the cortex
        }
}

TEST_CASE ("App: brain tonotopic maps: cochlea base high, apex low; Heschl's gyrus low anterolateral, high posteromedial")
{
    using A = brain::BrainAnatomy;
    // Greenwood (1990): place from the apex x = log10 (f / 165.4 + 0.88) / 2.1; the view's place is from the base.
    CHECK_NEAR (A::cochleaPlace (1000.0), 1.0 - std::log10 (1000.0 / 165.4 + 0.88) / 2.1, 1.0e-6);
    float previous = 2.0f;
    for (const double hz : { 30.0, 100.0, 500.0, 1000.0, 4000.0, 16000.0 })
    {
        const float place = A::cochleaPlace (hz);
        CHECK (place < previous);
        previous = place;
    }
    CHECK (A::cochleaPlace (30.0) > 0.95f);
    CHECK (A::cochleaPlace (16000.0) < 0.1f);
    const auto& a = A::get();
    for (int side = brain::kLeft; side <= brain::kRight; ++side)
    {
        const auto centre = a.getNode (nodeOf (Station::Cochlea, side)).position;
        // The base (high notes) is the outer turn, where the ear canal arrives; the apex (low) the centre.
        CHECK (brain::length (a.cochleaPoint (side, A::cochleaPlace (8000.0)) - centre) > brain::length (a.cochleaPoint (side, A::cochleaPlace (100.0)) - centre));
        const auto canalEnd = a.getTractPoints()[static_cast<size_t> (a.getTracts()[static_cast<size_t> (a.findTract (side == brain::kLeft ? "canal-L" : "canal-R"))].firstPoint
                                                                       + A::kSegments)];
        CHECK (brain::length (canalEnd - a.cochleaPoint (side, 0.0f)) < 1.0e-4f);

        // Heschl's gyrus: a low note lands further out (lateral) and further forward than a high one.
        const auto low = a.heschlPoint (side, A::heschlCoordinate (150.0));
        const auto high = a.heschlPoint (side, A::heschlCoordinate (6000.0));
        CHECK (std::abs (low.x) > std::abs (high.x) + 0.05f);
        CHECK (low.z > high.z + 0.05f);
    }
    CHECK (A::heschlCoordinate (30.0) == 0.0f);
    CHECK (A::heschlCoordinate (16000.0) == 1.0f);

    // In the model: a 4 kHz tone lights the cochlea and Heschl's gyrus at 4 kHz, a 150 Hz tone at 150 Hz.
    for (const double hz : { 150.0, 4000.0 })
    {
        Runner run;
        run.run (60, [hz] (int64_t n, float& l, float& r) { l = r = static_cast<float> (0.3 * std::sin (2.0 * kPi * hz * static_cast<double> (n) / kRate)); });
        const auto& act = run.view.getActivity();
        int cochleaBest = 0, heschlBest = 0;
        for (int b = 1; b < brain::kBands; ++b)
        {
            if (act.cochlea (brain::kLeft, b) > act.cochlea (brain::kLeft, cochleaBest))
                cochleaBest = b;
            if (act.heschl (brain::kRight, b) > act.heschl (brain::kRight, heschlBest))
                heschlBest = b;
        }
        std::printf ("  %.0f Hz tone: cochlea band %.0f Hz (place %.2f), Heschl band %.0f Hz\n", hz, brain::bandHz (cochleaBest),
                     static_cast<double> (A::cochleaPlace (brain::bandHz (cochleaBest))), brain::bandHz (heschlBest));
        CHECK (std::abs (std::log2 (brain::bandHz (cochleaBest) / hz)) < 0.35);
        CHECK (std::abs (std::log2 (brain::bandHz (heschlBest) / hz)) < 0.35);
    }
}

// =============================================================================
// The ascending pathway
// =============================================================================
TEST_CASE ("App: brain: a tone in one ear lights the opposite Heschl's gyrus more")
{
    for (int ear = brain::kLeft; ear <= brain::kRight; ++ear)
    {
        Runner run;
        double same = 0.0, opposite = 0.0, icSame = 0.0, icOpposite = 0.0;
        int frames = 0;
        run.run (90,
                 [ear] (int64_t n, float& l, float& r)
                 {
                     const auto v = static_cast<float> (0.3 * std::sin (2.0 * kPi * 440.0 * static_cast<double> (n) / kRate));
                     l = ear == brain::kLeft ? v : 0.0f;
                     r = ear == brain::kRight ? v : 0.0f;
                 },
                 [&]
                 {
                     if (run.view.getClock() < 0.6)
                         return;
                     const auto& act = run.view.getActivity();
                     same += act.heschlPeak (ear);
                     opposite += act.heschlPeak (1 - ear);
                     icSame += run.glow (Station::InferiorColliculus, ear);
                     icOpposite += run.glow (Station::InferiorColliculus, 1 - ear);
                     ++frames;
                 });
        REQUIRE (frames > 20);
        same /= frames;
        opposite /= frames;
        std::printf ("  440 Hz in the %s ear: Heschl's gyrus same side %.3f, opposite %.3f; IC %.3f / %.3f\n", ear == 0 ? "left" : "right", same,
                     opposite, icSame / frames, icOpposite / frames);
        // 65 % / 35 % from the lateral lemniscus up; the cortex adds +15 % for tonal sound on the right (Zatorre and Belin), so a
        // tone in the right ear lights the left gyrus about 0.65 x 0.85 / (0.35 x 1.15) = 1.37 times the right one.
        CHECK (opposite > (ear == brain::kLeft ? 2.0 : 1.25) * same);
        CHECK (icOpposite > 1.5 * icSame);
        const auto& act = run.view.getActivity();
        // Only the ear that hears it lights its cochlea.
        CHECK (act.glow (nodeOf (Station::Cochlea, ear)).level > 0.3f);
        CHECK (act.glow (nodeOf (Station::Cochlea, 1 - ear)).level == 0.0f);
    }
}

TEST_CASE ("App: brain superior olive: the LSO answers its own ear (the other ear inhibits it), the MSO both ears; the pathway crosses above "
           "the olive")
{
    // Grothe, Pecka and McAlpine 2010: the LSO is excited by the same-side cochlear nucleus and inhibited by the other ear
    // (through the MNTB); the MSO is excited by both; the crossed excitation to the midbrain leaves from the LSO (and
    // directly from the cochlear nucleus), the MSO's output stays on its own side.
    const auto& a = brain::BrainAnatomy::get();
    const int ipsi = a.findTract ("cn-lso-ipsi-L"), contra = a.findTract ("cn-lso-contra-L"), lsoOut = a.findTract ("lso-nll-L"),
              direct = a.findTract ("cn-nll-contra-L"), msoOut = a.findTract ("mso-nll-L");
    REQUIRE (ipsi >= 0);
    REQUIRE (contra >= 0);
    REQUIRE (lsoOut >= 0);
    REQUIRE (direct >= 0);
    REQUIRE (msoOut >= 0);
    const auto& tracts = a.getTracts();
    CHECK (tracts[static_cast<size_t> (lsoOut)].to == nodeOf (Station::LateralLemniscus, brain::kRight));
    CHECK (tracts[static_cast<size_t> (direct)].to == nodeOf (Station::LateralLemniscus, brain::kRight));
    CHECK (tracts[static_cast<size_t> (msoOut)].to == nodeOf (Station::LateralLemniscus, brain::kLeft));

    // A 300 Hz (MSO) and a 4 kHz (LSO) tone in the left ear only.
    Runner run;
    double lso[2] = {}, mso[2] = {}, nll[2] = {}, ipsiLevel = 0.0, contraLevel = 0.0;
    int frames = 0;
    const auto tractMean = [&run] (int tract)
    {
        double sum = 0.0;
        for (int j = 0; j <= brain::BrainAnatomy::kSegments; ++j)
            sum += run.view.getActivity().segment (tract, j).level;
        return sum / (brain::BrainAnatomy::kSegments + 1);
    };
    run.run (90,
             [] (int64_t n, float& l, float& r)
             {
                 const double t = seconds (n);
                 l = static_cast<float> (0.2 * std::sin (2.0 * kPi * 300.0 * t) + 0.2 * std::sin (2.0 * kPi * 4000.0 * t));
                 r = 0.0f;
             },
             [&]
             {
                 if (run.view.getClock() < 0.6)
                     return;
                 for (int s = brain::kLeft; s <= brain::kRight; ++s)
                 {
                     lso[s] += run.glow (Station::Lso, s);
                     mso[s] += run.glow (Station::Mso, s);
                     nll[s] += run.glow (Station::LateralLemniscus, s);
                 }
                 ipsiLevel += tractMean (ipsi);
                 contraLevel += tractMean (contra);
                 ++frames;
             });
    REQUIRE (frames > 20);
    std::printf ("  left-ear tones: LSO left %.3f right %.3f; MSO left %.3f right %.3f; lateral lemniscus left %.3f right %.3f; "
                 "cochlear nucleus -> LSO same side %.3f, other side %.3f\n",
                 lso[0] / frames, lso[1] / frames, mso[0] / frames, mso[1] / frames, nll[0] / frames, nll[1] / frames, ipsiLevel / frames,
                 contraLevel / frames);
    CHECK (lso[brain::kLeft] > 0.2 * frames);
    CHECK (lso[brain::kLeft] > 2.0 * lso[brain::kRight]);
    CHECK (mso[brain::kLeft] > 0.1 * frames);
    CHECK (std::abs (mso[brain::kLeft] - mso[brain::kRight]) < 0.2 * std::max (mso[brain::kLeft], mso[brain::kRight]));
    CHECK (nll[brain::kRight] > 1.5 * nll[brain::kLeft]);
    CHECK (ipsiLevel > contraLevel);
}

TEST_CASE ("App: brain: landing spots follow the level (quiet < loud < very loud, never white), silence is dark")
{
    float levels[3] = {};
    const float amplitudes[3] = { 0.01f, 0.1f, 0.5f }; // about -40, -20 and -6 dBFS
    for (int k = 0; k < 3; ++k)
    {
        Runner run;
        Noise noise;
        const float a = amplitudes[k];
        run.run (45, [&noise, a] (int64_t, float& l, float& r) { l = r = a * noise(); });
        levels[k] = run.glow (Station::CochlearNucleus, brain::kLeft);
        const auto& g = run.view.getActivity().glow (nodeOf (Station::CochlearNucleus, brain::kLeft));
        CHECK (std::max ({ g.r, g.g, g.b }) <= 1.0f + 1.0e-5f);
    }
    std::printf ("  cochlear nucleus: -40 dBFS noise %.2f, -20 dBFS %.2f, -6 dBFS %.2f\n", static_cast<double> (levels[0]), static_cast<double> (levels[1]),
                 static_cast<double> (levels[2]));
    CHECK (levels[0] > 0.05f);
    CHECK (levels[1] > levels[0] + 0.15f);
    CHECK (levels[2] > levels[1] + 0.1f);
    CHECK (levels[2] <= 1.37f);

    Runner silent;
    silent.run (180, [] (int64_t, float& l, float& r) { l = r = 0.0f; });
    const auto& act = silent.view.getActivity();
    float brightest = 0.0f;
    for (int n = 0; n < brain::kNumNodes; ++n)
        brightest = std::max (brightest, act.glow (n).level);
    for (size_t t = 0; t < brain::BrainAnatomy::get().getTracts().size(); ++t)
        for (int j = 0; j <= brain::BrainAnatomy::kSegments; ++j)
            brightest = std::max (brightest, act.segment (static_cast<int> (t), j).level);
    CHECK (brightest == 0.0f);
    CHECK (act.getBeats() == 0);
    CHECK (act.getChordChanges() == 0);
    CHECK (act.getAnticipations() == 0);
    CHECK (act.getDrops() == 0);
    CHECK (act.getActiveWaves() == 0);
}

// =============================================================================
// Beyond hearing
// =============================================================================
TEST_CASE ("App: brain: a kick pattern lights the beat network (cerebellum, thalamus, premotor, SMA, putamen)")
{
    Runner run;
    Noise noise;
    float peak[5] = {};
    const Station beatSpots[5] = { Station::Cerebellum, Station::Ventrolateral, Station::Premotor, Station::Sma, Station::Putamen };
    run.run (180,
             [&noise] (int64_t n, float& l, float& r)
             {
                 const double t = seconds (n);
                 l = r = 0.6f * kick (std::fmod (t, 0.5), noise());
             },
             [&]
             {
                 for (int k = 0; k < 5; ++k)
                     for (int side = brain::kLeft; side <= brain::kRight; ++side)
                         peak[k] = std::max (peak[k], run.glow (beatSpots[k], side));
             });
    const auto& listener = run.view.getListener();
    std::printf ("  6 kicks in 3 s: %d kick-like hits, %d counted as kick beats, %d snares; peaks cerebellum %.2f, VL thalamus %.2f, premotor %.2f, "
                 "SMA %.2f, putamen %.2f\n",
                 listener.getKickHits(), listener.getKicks(), listener.getSnares(), static_cast<double> (peak[0]), static_cast<double> (peak[1]),
                 static_cast<double> (peak[2]), static_cast<double> (peak[3]), static_cast<double> (peak[4]));
    // All 6 read as kicks; a kick is a beat from the third on, when it repeats like a drum beat (before 2026-10-13: 6).
    CHECK (listener.getKickHits() == 6);
    CHECK (listener.getKicks() == 4);
    CHECK (listener.getSnares() == 0);
    // Each beat within 40 ms of its kick (display time is the stream time of the analysis window's centre), the first on
    // the third kick.
    for (int i = 0; i < listener.getNumBeatTimes(); ++i)
    {
        const double t = listener.getBeatTime (i);
        CHECK (std::abs (t - 0.5 * std::round (t / 0.5)) < 0.04);
        CHECK (t > 0.9);
    }
    for (const float p : peak)
        CHECK (p > 0.2f);
}

// A snare-like hit (the noisy 1 - 4 kHz candidate) is a beat only when it repeats like a drum beat (owner decision
// 2026-10-10): single shots, automatic fire and irregular bursts on the Game strip must not pulse the beat network.
// One case per signal (each under 2 s).
namespace
{
struct BeatCount
{
    int kicks = 0, snares = 0, snareHits = 0, beats = 0, networkBeats = 0, kickHits = 0;
};

BeatCount countBeats (const char* name, int frames, const std::function<void (int64_t, float&, float&)>& gen, Runner& run)
{
    run.run (frames, gen);
    const auto& listener = run.view.getListener();
    BeatCount c { listener.getKicks(), listener.getSnares(), listener.getSnareHits(), listener.getNumBeatTimes(), run.view.getActivity().getBeats(),
                  listener.getKickHits() };
    juce::String times;
    for (int i = 0; i < std::min (32, listener.getNumBeatTimes()); ++i)
        times << juce::String (listener.getBeatTime (i), 2) << " ";
    std::printf ("  %s: %d snare-like hits, %d counted as snare beats; %d kick-like hits, %d counted as kick beats; %d beats in all (the beat "
                 "network pulsed %d times)%s%s\n",
                 name, c.snareHits, c.snares, c.kickHits, c.kicks, c.beats, c.networkBeats, times.isEmpty() ? "" : "; at ", times.trimEnd().toRawUTF8());
    return c;
}
} // namespace

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - single gunshots at irregular intervals")
{
    Runner run;
    Gunshots shots;
    shots.times = kIrregularShots;
    const auto c = countBeats ("9 single gunshots in 8 s at irregular gaps", 480, [&shots] (int64_t n, float& l, float& r) { l = r = shots (n); }, run);
    CHECK (c.snareHits >= 7); // they read as snares (before the rule each was a beat)
    CHECK (c.snares == 0);
    CHECK (c.kicks == 0);
    CHECK (c.networkBeats == 0);
}

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - automatic fire (10 shots a second for 2 s)")
{
    Runner run;
    Gunshots shots;
    for (int k = 0; k < 20; ++k)
        shots.times.push_back (0.5 + 0.1 * k);
    const auto c = countBeats ("automatic fire, 20 shots 0.1 s apart", 180, [&shots] (int64_t n, float& l, float& r) { l = r = shots (n); }, run);
    CHECK (c.snareHits >= 10);
    CHECK (c.snares == 0);
    CHECK (c.kicks == 0);
    CHECK (c.networkBeats == 0);
}

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - a backbeat (kick on 1 and 3, snare on 2 and 4) "
           "counts from the third snare on, on time")
{
    // 120 BPM, 6 s: kicks at 0, 1, .. 5 s, the test music's snare at 0.5, 1.5, .. 5.5 s, hats on the eighths.
    Runner run;
    Noise noise;
    Snare snare;
    HighPass h1 (6500.0), h2 (6500.0);
    const auto c = countBeats ("backbeat, 6 s (6 kicks, 6 snares)", 360,
                               [&] (int64_t n, float& l, float& r)
                               {
                                   const double t = seconds (n);
                                   const float x = noise();
                                   const float hat = 0.15f * h2 (h1 (x)) * decay (std::fmod (t, 0.25), 0.03);
                                   const float s = snare (t < 0.5 ? -1.0 : std::fmod (t - 0.5, 1.0), x);
                                   l = r = 0.55f * (kick (std::fmod (t, 1.0), x) + s + hat);
                               },
                               run);
    const auto& listener = run.view.getListener();
    CHECK (c.snareHits == 6);
    CHECK (c.snares == 4);
    // The first two snares only start the pattern; from the third on each is a beat within 40 ms of its hit.
    CHECK (! beatNear (listener, 0.5, 0.1));
    CHECK (! beatNear (listener, 1.5, 0.1));
    for (const double t : { 2.5, 3.5, 4.5, 5.5 })
        CHECK (beatNear (listener, t, 0.04));
    // The kicks the same way (2026-10-13): all 6 read as kicks (before the late-onset fix 5: the one at 2 s was missed),
    // the first two only start their pattern.
    CHECK (c.kickHits == 6);
    CHECK (c.kicks == 4);
    CHECK (! beatNear (listener, 0.0, 0.1));
    CHECK (! beatNear (listener, 1.0, 0.1));
    for (const double t : { 2.0, 3.0, 4.0, 5.0 })
        CHECK (beatNear (listener, t, 0.04));
    CHECK (c.networkBeats == c.beats);
}

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - gunshots over a four-on-the-floor groove")
{
    // The steady groove of the synthetic loops (kick every 0.5 s, hats, bass, pad) with the 9 irregular gunshots on top.
    Runner run;
    Noise noise;
    HighPass h1 (6500.0), h2 (6500.0);
    Gunshots shots;
    shots.times = kIrregularShots;
    const auto c = countBeats ("steady groove and 9 irregular gunshots, 8 s (16 kicks)", 480,
                               [&] (int64_t n, float& l, float& r)
                               {
                                   const double t = seconds (n);
                                   const float x = noise();
                                   double bass = 0.0;
                                   for (int h = 1; h <= 5; ++h)
                                       bass += std::sin (2.0 * kPi * 55.0 * h * t) / h;
                                   l = r = 0.5f * kick (std::fmod (t, 0.5), x) + 0.15f * h2 (h1 (x)) * decay (std::fmod (t, 0.25), 0.03)
                                           + 0.12f * static_cast<float> (bass) * decay (std::fmod (t, 0.25), 0.18) + chordSample ({ 57, 60, 64 }, n, 0.03)
                                           + shots (n);
                               },
                               run);
    const auto& listener = run.view.getListener();
    // A beat off the kicks' grid would be a shot.
    int offGrid = 0;
    for (int i = 0; i < listener.getNumBeatTimes(); ++i)
    {
        const double t = listener.getBeatTime (i);
        offGrid += std::abs (t - 0.5 * std::round (t / 0.5)) < 0.06 ? 0 : 1;
    }
    // How many shots fall within 15 % of a beat (75 ms) of the kicks' grid: what a rule "on the beat grid" could take.
    int nearGrid = 0;
    for (const double t : kIrregularShots)
        nearGrid += std::abs (t - 0.5 * std::round (t / 0.5)) < 0.075 ? 1 : 0;
    std::printf ("  beats off the kicks' grid %d; shots within 75 ms of the grid %d of %zu\n", offGrid, nearGrid, kIrregularShots.size());
    // A shot just before a kick hides the kick's onset (one candidate for both); on the steady grid it takes the kick's
    // place as a beat. No shot adds a beat of its own.
    CHECK (offGrid == 0);
    CHECK (c.snares <= nearGrid);
    CHECK (c.beats <= 16);
}

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - random gunfire (30 s, a shot every 0.7 s on average)")
{
    // Gaps drawn from an exponential distribution (mean 0.7 s, at least 0.12 s): a match within 8 % of the gap before
    // happens by chance (about 4 % of the pairs for exponential gaps).
    Runner run;
    Gunshots shots;
    uint32_t state = 2024u;
    for (double t = 0.3; t < 29.5;)
    {
        shots.times.push_back (t);
        state = state * 1664525u + 1013904223u;
        const double u = (static_cast<double> (state >> 8) + 0.5) / 16777216.0;
        t += std::max (0.12, -0.7 * std::log (u));
    }
    const auto c = countBeats ("random gunfire, 30 s", 1800, [&shots] (int64_t n, float& l, float& r) { l = r = shots (n); }, run);
    std::printf ("  %zu shots: %d snare-like hits, %d beats (%.1f %%)\n", shots.times.size(), c.snareHits, c.snares,
                 100.0 * c.snares / std::max (1, c.snareHits));
    CHECK (c.snareHits >= static_cast<int> (shots.times.size()) * 3 / 4);
    CHECK (c.snares * 10 <= c.snareHits); // at most 10 %
    CHECK (c.kicks == 0);
}

namespace
{
/** A backbeat (kick on 1 and 3, the test music's snare on 2 and 4, hats on the eighths) at `bpm`, from 0 s; the snare
    at `leftOut` s (if any) is not played. */
struct Backbeat
{
    double beat = 0.5, leftOut = -1.0;
    Noise noise;
    Snare snare;
    HighPass h1 { 6500.0 }, h2 { 6500.0 };
    explicit Backbeat (double bpm, double leftOutAt = -1.0) : beat (60.0 / bpm), leftOut (leftOutAt) {}
    void operator() (int64_t n, float& l, float& r)
    {
        const double t = seconds (n);
        const float x = noise();
        const float hat = 0.15f * h2 (h1 (x)) * decay (std::fmod (t, 0.5 * beat), 0.03);
        double tau = t < beat ? -1.0 : std::fmod (t - beat, 2.0 * beat); // since the last snare (beats 2 and 4)
        if (tau >= 0.0 && std::abs (t - tau - leftOut) < 0.01)
            tau += 2.0 * beat; // not played: the snare before it rings on (long decayed)
        l = r = 0.55f * (kick (std::fmod (t, 2.0 * beat), x) + snare (tau, x) + hat);
    }
};

/** Shots from a seeded LCG: steps of `gap` (1 +- jitter, uniform); each u drawn on its own line (a draw inside an
    argument list would run in another order on another compiler). */
std::vector<double> jitteredShots (double from, double to, double gap, double jitter, uint32_t seed)
{
    std::vector<double> times;
    uint32_t state = seed;
    for (double t = from; t < to;)
    {
        times.push_back (t);
        state = state * 1664525u + 1013904223u;
        const double u = (static_cast<double> (state >> 8) + 0.5) / 16777216.0;
        t += gap * (1.0 + jitter * (2.0 * u - 1.0));
    }
    return times;
}
} // namespace

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - a 60 BPM backbeat (a snare every 2.0 s) counts from "
           "the third snare")
{
    // 16 s: kicks at 0, 2, .. 14 s, snares at 1, 3, .. 15 s. The hop grid (10.7 ms) measures the 2.0 s gaps as 1.995 /
    // 2.005 s, so the gaps' upper bound must not sit at 2.0 s (with it there: 0 of 8 counted).
    Runner run;
    Backbeat groove (60.0);
    const auto c = countBeats ("60 BPM backbeat, 16 s (8 kicks, 8 snares)", 960, std::ref (groove), run);
    const auto& listener = run.view.getListener();
    CHECK (c.snareHits == 8);
    CHECK (c.snares == 6);
    CHECK (! beatNear (listener, 1.0, 0.1));
    CHECK (! beatNear (listener, 3.0, 0.1));
    for (int k = 2; k < 8; ++k)
        CHECK (beatNear (listener, 1.0 + 2.0 * k, 0.04));
    // The kicks (2026-10-13): all 8 read as kicks (before the late-onset fix 4: the hop grid's phase hid every other
    // one), from the third on each is a beat.
    CHECK (c.kickHits == 8);
    CHECK (c.kicks == 6);
    CHECK (! beatNear (listener, 0.0, 0.1));
    CHECK (! beatNear (listener, 2.0, 0.1));
    for (int k = 2; k < 8; ++k)
        CHECK (beatNear (listener, 2.0 * k, 0.04));
}

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - a backbeat with one snare left out carries on "
           "over the gap")
{
    // 120 BPM, 10 s, the snare at 4.5 s not played (as a snare the detector misses): with nothing snare-like in its
    // place, the snares at 5.5 and 6.5 s continue the counted pattern (without the carry neither counts: the gaps
    // 2.0 / 1.0 s do not match the ones before).
    Runner run;
    Backbeat groove (120.0, 4.5);
    const auto c = countBeats ("backbeat 120 BPM without the snare at 4.5 s, 10 s (9 snares)", 600, std::ref (groove), run);
    const auto& listener = run.view.getListener();
    CHECK (c.snareHits == 9);
    CHECK (c.snares == 7);
    CHECK (! beatNear (listener, 0.5, 0.1));
    CHECK (! beatNear (listener, 1.5, 0.1));
    for (const double t : { 2.5, 3.5, 5.5, 6.5, 7.5, 8.5, 9.5 })
        CHECK (beatNear (listener, t, 0.04));
}

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - 3-round bursts at human gaps never count")
{
    // Bursts of 3 shots 0.075 s apart (under the 0.2 s gap), a burst every 0.45 - 0.9 s, 10 s.
    Runner run;
    Gunshots shots;
    uint32_t state = 77u;
    for (double b = 0.3; b < 10.0;)
    {
        for (int k = 0; k < 3; ++k)
            shots.times.push_back (b + 0.075 * k);
        state = state * 1664525u + 1013904223u;
        const double u = (static_cast<double> (state >> 8) + 0.5) / 16777216.0;
        b += 0.45 + 0.45 * u;
    }
    const auto c = countBeats ("3-round bursts at 0.45 - 0.9 s, 10 s", 630, [&shots] (int64_t n, float& l, float& r) { l = r = shots (n); }, run);
    std::printf ("  %zu shots\n", shots.times.size());
    CHECK (c.snareHits >= 20);
    CHECK (c.snares == 0);
    CHECK (c.networkBeats == 0);
}

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - a human tapping 4 shots a second (+-10 %) counts "
           "at most half (a documented limit)")
{
    // Steady game fire repeats: each gap 0.25 s +- 10 % (uniform). A gap within 8 % of the one before counts, so some
    // shots light the beat network; with 15 % most did (25 of 31).
    Runner run;
    Gunshots shots;
    shots.times = jitteredShots (0.3, 8.0, 0.25, 0.10, 11u);
    const auto c = countBeats ("taps 4 a second +- 10 %, 8 s", 510, [&shots] (int64_t n, float& l, float& r) { l = r = shots (n); }, run);
    std::printf ("  %zu shots: %d snare-like hits, %d beats (%.0f %%)\n", shots.times.size(), c.snareHits, c.snares,
                 100.0 * c.snares / std::max (1, c.snareHits));
    CHECK (c.snareHits >= 28);
    CHECK (c.snares * 2 <= c.snareHits);
    CHECK (c.kicks <= 1);
}

// A kick-like hit (a low-band rise whose pitch then falls) is a beat only when it belongs to a drum pattern: the same
// repeat test as a snare's. Footsteps with a low thud and explosion booms read as kicks; music keeps its kicks from the
// third on. One case per signal (each under 2 s).
namespace
{
/** A kick drum, tau seconds after the hit: a sine whose pitch falls from `fromHz` to `toHz` (time constant `sweep`),
    decaying with `ring`, and a 2 ms click (`click`: a noise sample). kick() is (158, 48, 0.035, 0.28, 0.85). */
float kickDrum (double tau, float click, double fromHz, double toHz, double sweep, double ring, float level) noexcept
{
    if (tau < 0.0)
        return 0.0f;
    const double phase = 2.0 * kPi * (toHz * tau + (fromHz - toHz) * sweep * (1.0 - std::exp (-tau / sweep)));
    return level * static_cast<float> (std::sin (phase)) * decay (tau, ring) + 0.25f * click * decay (tau, 0.002);
}

/** The times k x gap from `from` while under `to`. */
std::vector<double> grid (double from, double to, double gap)
{
    std::vector<double> times;
    for (int k = 0; from + k * gap < to - 1.0e-9; ++k)
        times.push_back (from + k * gap);
    return times;
}

/** How the counted beats fall on the kicks played: on one (within 40 ms; each kick once), the first such, and beats on
    no played drum hit (`drums`: every kick and snare played). */
struct OnKicks
{
    int on = 0, off = 0;
    double first = -1.0;
};

OnKicks beatsOnKicks (const brain::BrainListener& listener, const std::vector<double>& kicksPlayed, const std::vector<double>& drums)
{
    OnKicks o;
    std::set<size_t> used;
    for (int i = 0; i < listener.getNumBeatTimes(); ++i)
    {
        const double t = listener.getBeatTime (i);
        bool onKick = false;
        for (size_t k = 0; k < kicksPlayed.size() && ! onKick; ++k)
            if (std::abs (t - kicksPlayed[k]) < 0.04 && used.insert (k).second)
                onKick = true;
        if (onKick)
        {
            ++o.on;
            if (o.first < 0.0)
                o.first = t;
            continue;
        }
        bool onDrum = false;
        for (const double d : drums)
            onDrum = onDrum || std::abs (t - d) < 0.04;
        o.off += onDrum ? 0 : 1;
    }
    std::printf ("  counted beats on a played kick: %d of %zu (the first at %.2f s); beats on no played drum hit: %d\n", o.on, kicksPlayed.size(),
                 o.first, o.off);
    return o;
}

/** Four on the floor at `bpm` from 0 s: a kick on every beat (`sub`: a deeper, longer one, 150 -> 45 Hz in about 30 ms,
    0.35 s), a clap (the test music's snare) on 2 and 4, an open hat and a 55 Hz bass note on each off-beat. */
struct FourOnTheFloor
{
    double beat = 0.5;
    bool sub = false;
    Noise noise;
    Snare clap;
    HighPass h1 { 6500.0 }, h2 { 6500.0 };
    LowPass bassLp { 500.0 };
    FourOnTheFloor (double bpm, bool subKick) : beat (60.0 / bpm), sub (subKick) {}
    void operator() (int64_t n, float& l, float& r)
    {
        const double t = seconds (n);
        const float x = noise();
        const double tauBeat = std::fmod (t, beat);
        const double tauClap = t < beat ? -1.0 : std::fmod (t - beat, 2.0 * beat);
        const double tauOff = t < 0.5 * beat ? -1.0 : std::fmod (t - 0.5 * beat, beat);
        const float k = sub ? kickDrum (tauBeat, x, 150.0, 45.0, 0.03, 0.35, 0.9f) : kick (tauBeat, x);
        const float hat = 0.12f * h2 (h1 (x)) * decay (tauOff, 0.07);
        const float c = 0.8f * clap (tauClap, x);
        double saw = 0.0;
        for (int h = 1; h <= 5; ++h)
            saw += std::sin (2.0 * kPi * 55.0 * h * t) / h;
        const float bass = bassLp (static_cast<float> (saw));
        const float b = tauOff < 0.0 ? 0.0f : 0.2f * bass * std::min (1.0f, static_cast<float> (tauOff / 0.004)) * decay (tauOff, 0.12);
        l = r = 0.5f * (k + c + hat + b);
    }
};

/** Synthwave at 110 BPM from 0 s: a punchy drum-machine kick (130 -> 52 Hz in about 20 ms, 0.16 s) on 1 and 3, a gated
    snare (noise held for 0.22 s, then cut, and a 200 Hz body) on 2 and 4, an octave bass on the eighths (A1 / A2, a
    five-harmonic saw, ducked by each kick) and an A minor pad. */
struct Synthwave
{
    static constexpr double kBeat = 60.0 / 110.0;
    Noise noise;
    HighPass snareHp { 900.0 };
    LowPass snareLp { 8000.0 }, bassLp { 900.0 };
    double bassPhase = 0.0;
    void operator() (int64_t n, float& l, float& r)
    {
        const double t = seconds (n);
        const float x = noise();
        const double tauKick = std::fmod (t, 2.0 * kBeat);
        const double tauSnare = t < kBeat ? -1.0 : std::fmod (t - kBeat, 2.0 * kBeat);
        const double eighth = 0.5 * kBeat;
        const auto eighthIndex = static_cast<int64_t> (t / eighth);
        const double tauEighth = t - static_cast<double> (eighthIndex) * eighth;
        const float k = kickDrum (tauKick, x, 130.0, 52.0, 0.02, 0.16, 0.9f);
        const float wires = snareLp (snareHp (x));
        float s = 0.0f;
        if (tauSnare >= 0.0)
        {
            const float gate = tauSnare < 0.22 ? 1.0f : decay (tauSnare - 0.22, 0.008);
            s = gate * (0.35f * wires * (0.5f + 0.5f * decay (tauSnare, 0.05)) + 0.3f * static_cast<float> (std::sin (2.0 * kPi * 200.0 * tauSnare)) * decay (tauSnare, 0.05));
        }
        bassPhase += (eighthIndex % 2 == 0 ? 55.0 : 110.0) / kRate;
        if (bassPhase >= 1.0)
            bassPhase -= 1.0;
        double saw = 0.0;
        for (int h = 1; h <= 5; ++h)
            saw += std::sin (2.0 * kPi * h * bassPhase) / h;
        const float duck = 1.0f - 0.7f * decay (tauKick, 0.08);
        const float b = 0.25f * bassLp (static_cast<float> (saw)) * std::min (1.0f, static_cast<float> (tauEighth / 0.003)) * (0.4f + 0.6f * decay (tauEighth, 0.1)) * duck;
        l = r = 0.5f * (k + s + b) + duck * chordSample ({ 57, 60, 64 }, n, 0.02);
    }
};

/** Footsteps at `times` (ascending): a thud whose pitch falls from 120 to 70 Hz (45 ms decay) and a 1.5 - 6 kHz scuff
    (30 ms), every other step 2 dB softer. A kick drum's falling pitch is what the kick rule looks for. */
struct Footsteps
{
    std::vector<double> times;
    Noise noise;
    HighPass scuffHp { 1500.0 };
    LowPass scuffLp { 6000.0 };
    size_t next = 0;
    double last = -1.0;
    float gain = 1.0f;
    explicit Footsteps (std::vector<double> at) : times (std::move (at)) {}
    float operator() (int64_t n) noexcept
    {
        const double t = seconds (n);
        const float scuff = scuffLp (scuffHp (noise()));
        while (next < times.size() && times[next] <= t)
        {
            last = times[next];
            gain = next % 2 == 0 ? 1.0f : 0.8f;
            ++next;
        }
        if (last < 0.0)
            return 0.0f;
        const double tau = t - last;
        const double phase = 2.0 * kPi * (70.0 * tau + 50.0 * 0.02 * (1.0 - std::exp (-tau / 0.02)));
        return gain * 0.3f
               * (0.8f * static_cast<float> (std::sin (phase)) * decay (tau, 0.045)
                  + 0.6f * scuff * decay (tau, 0.03) * std::min (1.0f, static_cast<float> (tau / 0.002)));
    }
};

/** Explosions at `times` (ascending): a crack (noise, 10 ms), a boom whose pitch falls as a kick drum's does (110 ->
    40 Hz in about 40 ms, 0.6 s decay), a rumble (noise under 150 Hz, 1.2 s) and debris (grains of noise over 2 kHz,
    3 - 15 ms each, for 1.6 s). */
struct Explosions
{
    std::vector<double> times;
    Noise noise { 55555u };
    LowPass rumble1 { 150.0 }, rumble2 { 150.0 };
    HighPass debris1 { 2000.0 }, debris2 { 2000.0 };
    uint32_t state = 99u;
    size_t next = 0;
    double last = -1.0, grainStart = -1.0, grainLength = 0.0, nextGrain = 0.0;
    float grainLevel = 0.0f;
    explicit Explosions (std::vector<double> at) : times (std::move (at)) {}
    double draw() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (static_cast<double> (state >> 8) + 0.5) / 16777216.0;
    }
    float operator() (int64_t n) noexcept
    {
        const double t = seconds (n);
        const float x = noise();
        const float low = rumble2 (rumble1 (x));
        const float high = debris2 (debris1 (x));
        while (next < times.size() && times[next] <= t)
        {
            last = times[next++];
            nextGrain = last + 0.04;
        }
        if (last < 0.0)
            return 0.0f;
        const double tau = t - last;
        if (tau < 1.6 && t >= nextGrain)
        {
            grainStart = t;
            const double u1 = draw();
            grainLength = 0.003 + 0.012 * u1;
            const double u2 = draw();
            grainLevel = static_cast<float> (0.1 + 0.3 * u2) * decay (tau, 0.7);
            const double u3 = draw();
            nextGrain = t + 0.02 + 0.1 * u3;
        }
        const float grain = t - grainStart < grainLength ? grainLevel * high : 0.0f;
        const double phase = 2.0 * kPi * (40.0 * tau + 70.0 * 0.04 * (1.0 - std::exp (-tau / 0.04)));
        const float rise = std::min (1.0f, static_cast<float> (tau / 0.001));
        return rise * (0.6f * x * decay (tau, 0.01) + 0.55f * static_cast<float> (std::sin (phase)) * decay (tau, 0.6) + 4.0f * low * decay (tau, 1.2))
               + grain;
    }
};
} // namespace

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - four on the floor at 120 BPM counts from the third kick, "
           "on time")
{
    // Before 2026-10-13 all 16 were beats from the first (14 read as kicks, 2 claps on a kick as snares).
    Runner run;
    FourOnTheFloor groove (120.0, false);
    const auto c = countBeats ("four on the floor, 120 BPM, 8 s (16 kicks, a clap on 2 and 4)", 480, std::ref (groove), run);
    const auto kicksPlayed = grid (0.0, 8.0, 0.5);
    const auto o = beatsOnKicks (run.view.getListener(), kicksPlayed, kicksPlayed);
    CHECK (c.kickHits == 16);
    CHECK (o.on == 14);
    CHECK (std::abs (o.first - 1.0) < 0.04);
    CHECK (o.off == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - four on the floor at 128 BPM with a long sub kick")
{
    // Before: 16 of the 18 kicks were beats from the first (12 read as kicks, 4 claps on a kick as snares).
    Runner run;
    FourOnTheFloor groove (128.0, true);
    const auto c = countBeats ("four on the floor, 128 BPM, sub kick, 8 s (18 kicks)", 480, std::ref (groove), run);
    const auto kicksPlayed = grid (0.0, 8.0, 60.0 / 128.0);
    const auto o = beatsOnKicks (run.view.getListener(), kicksPlayed, kicksPlayed);
    CHECK (c.kickHits + c.snareHits >= 15);
    // The late-onset fix reads the claps on a kick as the kick (measured 15 kick-like hits and 1 snare-like hit; without
    // the fix 12 and 4, which these two checks catch).
    CHECK (c.kickHits >= 15);
    CHECK (c.snareHits <= 1);
    CHECK (o.on >= 13); // measured 14
    CHECK (std::abs (o.first - 2.0 * 60.0 / 128.0) < 0.04); // the third kick
    CHECK (o.off == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - a synthwave groove (110 BPM, kick on 1 and 3, gated snare, "
           "octave bass)")
{
    // Before: all 10 kicks were beats from the first.
    Runner run;
    Synthwave groove;
    const auto c = countBeats ("synthwave, 110 BPM, 10 s (10 kicks, 9 snares)", 600, std::ref (groove), run);
    const auto kicksPlayed = grid (0.0, 10.0, 2.0 * Synthwave::kBeat);
    const auto o = beatsOnKicks (run.view.getListener(), kicksPlayed, grid (0.0, 10.0, Synthwave::kBeat));
    CHECK (c.kickHits == 10);
    CHECK (o.on == 8);
    CHECK (std::abs (o.first - 2.0 * 2.0 * Synthwave::kBeat) < 0.04); // the third kick
    CHECK (c.snares == 7);                                            // from the third snare, as before
    CHECK (o.off == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - footsteps with a falling thud at a steady walk still count "
           "from the third step (a documented limit)")
{
    // Walking, a step every 0.52 s +- 3 %: as regular as a drum beat (about 2 steps a second, a dance tempo), so from the
    // third step each one repeats the gap before it within 8 % (before 2026-10-13: 19 of 19 steps were beats).
    Runner run;
    Footsteps steps (jitteredShots (0.3, 10.0, 0.52, 0.03, 31u));
    const auto c = countBeats ("footsteps, 0.52 s +- 3 %, 10 s", 630, [&steps] (int64_t n, float& l, float& r) { l = r = steps (n); }, run);
    std::printf ("  %zu steps\n", steps.times.size());
    CHECK (c.kickHits >= static_cast<int> (steps.times.size()) * 3 / 4);
    CHECK (c.kicks <= c.kickHits - 2); // measured 17 of 19
    CHECK (! beatNear (run.view.getListener(), steps.times[0], 0.1));
    CHECK (! beatNear (run.view.getListener(), steps.times[1], 0.1));
    CHECK (c.snares == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - footsteps with a falling thud at an irregular pace")
{
    // Moving about: each gap 0.3 - 0.9 s (0.6 s +- 50 %, uniform), 12 s. Before 2026-10-13: 20 of 20 steps were beats.
    Runner run;
    Footsteps steps (jitteredShots (0.3, 12.0, 0.6, 0.5, 47u));
    const auto c = countBeats ("footsteps, gaps 0.3 - 0.9 s, 12 s", 750, [&steps] (int64_t n, float& l, float& r) { l = r = steps (n); }, run);
    std::printf ("  %zu steps\n", steps.times.size());
    CHECK (c.kickHits >= static_cast<int> (steps.times.size()) * 3 / 4); // they read as kicks
    // At most 20 % line up by chance: measured 1 of 20 on this seed, the low end; over 60 seeds of the same generator
    // (1000 - 1059) 190 of 1203 (15.8 %; 167, 13.9 %, before the kicks' carry over hits between their beats).
    CHECK (c.kicks * 5 <= c.kickHits);
    CHECK (c.snares == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - explosion booms with debris")
{
    // Three explosions at 0.8, 3.9 and 6.4 s, 9 s: each boom reads as a kick (before 2026-10-13: 3 beats). Their gaps
    // (3.1, 2.5 s) are over the 2.3 s bound; the case below keeps the gaps in range.
    Runner run;
    Explosions booms ({ 0.8, 3.9, 6.4 });
    const auto c = countBeats ("3 explosions with debris, 9 s", 540, [&booms] (int64_t n, float& l, float& r) { l = r = booms (n); }, run);
    CHECK (c.kickHits == 3);
    CHECK (c.kicks == 0);
    CHECK (c.snares == 0);
    CHECK (c.networkBeats == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - a rock beat's syncopated kick does not count, its snares do "
           "(a documented limit)")
{
    // 110 BPM, 12 s: a damped kick (95 -> 58 Hz, 60 ms) on 1, 3 and the and of 3 (gaps of 2, 0.5 and 1.5 beats: no gap
    // repeats the one before), the test music's snare on 2 and 4, hats on the eighths. Before 2026-10-13: all 16 kicks
    // were beats.
    const double beat = 60.0 / 110.0, bar = 4.0 * beat;
    std::vector<double> kicksPlayed, snaresPlayed;
    for (double b = 0.0; b < 12.0; b += bar)
    {
        for (const double p : { 0.0, 2.0, 2.5 })
            if (b + p * beat < 12.0)
                kicksPlayed.push_back (b + p * beat);
        for (const double p : { 1.0, 3.0 })
            if (b + p * beat < 12.0)
                snaresPlayed.push_back (b + p * beat);
    }
    Runner run;
    Noise noise;
    Snare snare;
    HighPass h1 (6500.0), h2 (6500.0);
    const auto c = countBeats ("rock beat 110 BPM, damped kick on 1, 3 and the and of 3, 12 s (16 kicks, 11 snares)", 720,
                               [&] (int64_t n, float& l, float& r)
                               {
                                   const double t = seconds (n);
                                   const float x = noise();
                                   double tk = -1.0;
                                   for (const double k : kicksPlayed)
                                       if (k <= t)
                                           tk = t - k;
                                   const double tb = std::fmod (t, bar);
                                   const double ts = tb >= 3.0 * beat ? tb - 3.0 * beat : (tb >= beat ? tb - beat : -1.0);
                                   const float hat = 0.12f * h2 (h1 (x)) * decay (std::fmod (t, 0.5 * beat), 0.03);
                                   l = r = 0.55f * (kickDrum (tk, x, 95.0, 58.0, 0.015, 0.06, 0.9f) + snare (ts, x) + hat);
                               },
                               run);
    auto drums = kicksPlayed;
    drums.insert (drums.end(), snaresPlayed.begin(), snaresPlayed.end());
    const auto o = beatsOnKicks (run.view.getListener(), kicksPlayed, drums);
    CHECK (c.kickHits >= 14); // they read as kicks (measured 16)
    CHECK (c.kicks <= 2);     // measured 0
    CHECK (c.snares >= 8);    // the beat network still pulses on 2 and 4 (measured 9, from the third snare)
    CHECK (o.off == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - explosion booms at irregular gaps inside the beat range "
           "(near misses of 13 - 14 %)")
{
    // Seven explosions in 9 s, gaps 1.2, 1.36, 0.9, 1.03, 1.5 and 1.3 s: every gap is inside the 0.2 - 2.3 s range, and
    // three of them miss the one before by only 13 - 14 % (the hop grid moves a gap by at most 10.7 ms, about 1 %). So
    // this case fails if the repeat test's 8 % tolerance is loosened (measured with 15 %: the explosions at 3.06 and
    // 7.79 s count), which the three widely spaced explosions above cannot show (their gaps of 3.1 and 2.5 s are out
    // of range, so they pass with any tolerance). The carry-over and the counted beats' grid need a counted beat
    // first, so this tests the matching gaps alone.
    Runner run;
    Explosions booms ({ 0.5, 1.7, 3.06, 3.96, 4.99, 6.49, 7.79 });
    const auto c = countBeats ("7 explosions at irregular gaps of 0.9 - 1.5 s, 9 s", 540, [&booms] (int64_t n, float& l, float& r) { l = r = booms (n); }, run);
    CHECK (c.kickHits >= 6); // the booms read as kicks
    CHECK (c.kicks == 0);
    CHECK (c.snares == 0);
    CHECK (c.networkBeats == 0);
}

namespace
{
/** The test music's kick at `kicksAt` and its snare at `snaresAt` (ascending), hats on the eighths of `beat`. */
struct DrumTrack
{
    std::vector<double> kicksAt, snaresAt;
    double beat = 0.5;
    Noise noise;
    Snare snare;
    HighPass h1 { 6500.0 }, h2 { 6500.0 };
    size_t nextKick = 0, nextSnare = 0;
    double lastKick = -1.0, lastSnare = -1.0;
    void operator() (int64_t n, float& l, float& r)
    {
        const double t = seconds (n);
        const float x = noise();
        while (nextKick < kicksAt.size() && kicksAt[nextKick] <= t)
            lastKick = kicksAt[nextKick++];
        while (nextSnare < snaresAt.size() && snaresAt[nextSnare] <= t)
            lastSnare = snaresAt[nextSnare++];
        const float hat = 0.12f * h2 (h1 (x)) * decay (std::fmod (t, 0.5 * beat), 0.03);
        l = r = 0.55f * (kick (lastKick < 0.0 ? -1.0 : t - lastKick, x) + snare (lastSnare < 0.0 ? -1.0 : t - lastSnare, x) + hat);
    }
};

/** A half-time groove at `bpm` for `bars` bars from 0 s: a kick on 1 and a snare on 3 of each bar (one of each a bar),
    hats on the eighths. Counts its kicks and snares. */
BeatCount halfTimeGroove (double bpm, int bars, Runner& run)
{
    DrumTrack drums;
    drums.beat = 60.0 / bpm;
    const double bar = 4.0 * drums.beat;
    for (int b = 0; b < bars; ++b)
    {
        drums.kicksAt.push_back (b * bar);
        drums.snaresAt.push_back (b * bar + 2.0 * drums.beat);
    }
    const auto name = juce::String (bpm, 0) + " BPM half-time groove (a kick on 1, a snare on 3; a gap of " + juce::String (bar, 2) + " s), "
                      + juce::String (bars) + " bars";
    return countBeats (name.toRawUTF8(), static_cast<int> (std::ceil (bars * bar / kDt)), std::ref (drums), run);
}
} // namespace

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - one kick a bar counts only above 104 BPM (a documented "
           "limit) - 110 BPM")
{
    // A bar of 2.18 s is inside the 0.2 - 2.3 s range: the kicks and the snares count from their third.
    Runner run;
    const auto c = halfTimeGroove (110.0, 6, run);
    CHECK (c.kickHits == 6);
    CHECK (c.kicks == 4);
    CHECK (c.snares == 4);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - one kick a bar counts only above 104 BPM (a documented "
           "limit) - 100 BPM")
{
    // A bar of 2.4 s is longer than the 2.3 s bound (60 / 104 x 4 = 2.31 s): neither the kicks nor the snares ever
    // count, so the beat network stays dark (before 2026-10-13 every kick was a beat).
    Runner run;
    const auto c = halfTimeGroove (100.0, 6, run);
    CHECK (c.kickHits == 6);
    CHECK (c.kicks == 0);
    CHECK (c.snares == 0);
    CHECK (c.networkBeats == 0);
}

namespace
{
/** A plucked bass note, tau seconds after it: a 12-harmonic saw at `hz` through a 4-pole low-pass whose cutoff falls
    from `fromHz` to `toHz` (time constant `envTau`), a 2 ms attack and a 0.12 s decay. Its spectrum sinks as a kick
    drum's pitch does, so some notes read as kick-like hits. */
struct Pluck
{
    double phase = 0.0;
    std::array<float, 4> y {};
    float operator() (double tau, double hz, double fromHz, double toHz, double envTau) noexcept
    {
        if (tau < 0.0)
            return 0.0f;
        phase += hz / kRate;
        if (phase >= 1.0)
            phase -= 1.0;
        double saw = 0.0;
        for (int h = 1; h <= 12; ++h)
            saw += std::sin (2.0 * kPi * h * phase) / h;
        const double fc = toHz + (fromHz - toHz) * std::exp (-tau / envTau);
        const float a = static_cast<float> (1.0 - std::exp (-2.0 * kPi * fc / kRate));
        auto x = static_cast<float> (saw);
        for (auto& s : y)
        {
            s += a * (x - s);
            x = s;
        }
        return x * std::min (1.0f, static_cast<float> (tau / 0.002)) * decay (tau, 0.12);
    }
};

/** Four on the floor at 128 BPM for 10 s (the test music's kick, 22 kicks) with a rolling bass on the three 16ths after
    each kick (A1 / A2 in turn, a pluck whose filter falls from 800 to 80 Hz in about 10 ms, ducked by the kick), the
    bass at `level` against the kick's 0.5: a progressive-house bassline. Returns the counts; `o` gets the beats on the
    kicks played (a beat on no kick would be a bass note's). */
BeatCount rollingBass (const char* name, float level, Runner& run, OnKicks& o)
{
    const double beat = 60.0 / 128.0;
    Noise noise;
    Pluck pluck;
    const auto c = countBeats (name, 600,
                               [&] (int64_t n, float& l, float& r)
                               {
                                   const double t = seconds (n);
                                   const float x = noise();
                                   const double tauBeat = std::fmod (t, beat);
                                   const double sixteenth = 0.25 * beat;
                                   const auto i = static_cast<int64_t> (t / sixteenth);
                                   const double tau = i % 4 == 0 ? -1.0 : t - static_cast<double> (i) * sixteenth;
                                   const float duck = 1.0f - 0.7f * decay (tauBeat, 0.08);
                                   l = r = 0.5f * kick (tauBeat, x) + level * duck * pluck (tau, i % 2 == 0 ? 55.0 : 110.0, 800.0, 80.0, 0.01);
                               },
                               run);
    const auto kicksPlayed = grid (0.0, 10.0, beat);
    o = beatsOnKicks (run.view.getListener(), kicksPlayed, kicksPlayed);
    return c;
}
} // namespace

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - four on the floor at 128 BPM with a rolling 16th bass keeps "
           "its kick beats")
{
    // Some bass notes read as kick-like hits between the kicks, and a kick right after a loud note can be missed (its
    // low band rises less). A counted kick pattern carries over the hits between its beats, so neither breaks it.
    // Before (each hit in between broke the pattern until three clean kicks came in a row): 7 of the 22 kicks.
    Runner run;
    OnKicks o;
    const auto c = rollingBass ("four on the floor 128 BPM, rolling 16th bass at 0.35, 10 s (22 kicks)", 0.35f, run, o);
    CHECK (c.kickHits >= 24); // bass notes among them (measured 28)
    CHECK (o.on >= 15);       // measured 16
    CHECK (o.off == 0);       // no bass note is a beat
    CHECK (c.snares == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - four on the floor at 128 BPM with a loud rolling 16th bass "
           "keeps its kick beats")
{
    // The bass at 0.7 (the kick 0.5): more of its notes read as kicks. Before: 4 of the 22 kicks.
    Runner run;
    OnKicks o;
    const auto c = rollingBass ("four on the floor 128 BPM, rolling 16th bass at 0.7, 10 s (22 kicks)", 0.7f, run, o);
    CHECK (c.kickHits >= 30); // measured 36
    CHECK (o.on >= 17);       // measured 20
    CHECK (o.off == 0);
    CHECK (c.snares == 0);
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - four on the floor with one kick left out carries on over "
           "the gap")
{
    // 120 BPM, 8 s, kicks only (and hats), the kick at 4.0 s not played: the kick at 4.5 s lands two of the counted
    // pattern's gaps after the last counted one (one missed kick), the one at 5.0 s one gap after it. Without the
    // carry neither counts (their gaps 1.0 / 0.5 s do not match the ones before, nor the counted beats' next slot).
    DrumTrack drums;
    for (const double t : grid (0.0, 8.0, 0.5))
        if (std::abs (t - 4.0) > 0.01)
            drums.kicksAt.push_back (t);
    const auto kicksPlayed = drums.kicksAt;
    Runner run;
    const auto c = countBeats ("four on the floor 120 BPM without the kick at 4.0 s, 8 s (15 kicks)", 480, std::ref (drums), run);
    const auto o = beatsOnKicks (run.view.getListener(), kicksPlayed, kicksPlayed);
    CHECK (c.kickHits == 15);
    CHECK (o.on == 13); // all but the first two
    CHECK (o.off == 0);
    CHECK (beatNear (run.view.getListener(), 4.5, 0.04));
    CHECK (beatNear (run.view.getListener(), 5.0, 0.04));
}

TEST_CASE ("App: brain: a kick is a beat only when it belongs to a drum pattern - four on the floor with one kick left out and a ghost kick "
           "in the gap carries on (a kick pattern bridges kick-like hits between its beats)")
{
    // As above, with a ghost kick (a short, damped one: 95 -> 58 Hz, 60 ms) at 4.2 s, in the gap: a counted kick
    // pattern carries over the kick-like hits between its beats, so the kicks at 4.5 and 5.0 s still count and the
    // ghost does not. Before (a hit of its kind in between ended the carry, as it still does for snares, below):
    // neither counted, 11 of the 15 kicks on the beat.
    DrumTrack drums;
    for (const double t : grid (0.0, 8.0, 0.5))
        if (std::abs (t - 4.0) > 0.01)
            drums.kicksAt.push_back (t);
    const auto onTheBeat = drums.kicksAt;
    auto played = onTheBeat;
    played.push_back (4.2);
    Runner run;
    const auto c = countBeats ("four on the floor 120 BPM without the kick at 4.0 s, a ghost kick at 4.2 s, 8 s (15 kicks on the beat)", 480,
                               [&] (int64_t n, float& l, float& r)
                               {
                                   drums (n, l, r);
                                   const float ghost = 0.55f * kickDrum (seconds (n) - 4.2, 0.0f, 95.0, 58.0, 0.015, 0.06, 0.9f);
                                   l += ghost;
                                   r += ghost;
                               },
                               run);
    const auto o = beatsOnKicks (run.view.getListener(), onTheBeat, played);
    CHECK (c.kickHits == 16); // the ghost reads as a kick
    CHECK (o.on == 13);       // the kicks on the beat but the first two
    CHECK (! beatNear (run.view.getListener(), 4.2, 0.04));
    CHECK (o.off == 0);
    CHECK (beatNear (run.view.getListener(), 4.5, 0.04));
    CHECK (beatNear (run.view.getListener(), 5.0, 0.04));
}

TEST_CASE ("App: brain: a snare-like hit is a beat only when it repeats like a drum beat - a clap on every beat with one left out and a "
           "gunshot in the gap does not carry on (the snare rule unchanged)")
{
    // The owner's snare rule (2026-10-10), kept as decided when kicks got their carry over hits in between: claps (the
    // test music's snare) on every beat at 120 BPM from 0.5 s, 8 s, the one at 4.0 s not played and a gunshot at
    // 4.15 s: the shot ends the carry, so the claps at 4.5 and 5.0 s do not count (gaps 0.35 / 0.65 and 0.5 / 0.35 s,
    // and they miss the counted beats' next slot); from 5.5 s they count again. Without the shot they would (the
    // backbeat with one snare left out, above).
    DrumTrack drums;
    for (const double t : grid (0.5, 8.0, 0.5))
        if (std::abs (t - 4.0) > 0.01)
            drums.snaresAt.push_back (t);
    Gunshots shot;
    shot.times = { 4.15 };
    Runner run;
    const auto c = countBeats ("claps on every beat 120 BPM without the one at 4.0 s, a gunshot at 4.15 s, 8 s (14 claps)", 480,
                               [&] (int64_t n, float& l, float& r)
                               {
                                   drums (n, l, r);
                                   const float s = shot (n);
                                   l += s;
                                   r += s;
                               },
                               run);
    const auto& listener = run.view.getListener();
    CHECK (c.snareHits == 15); // the 14 claps and the shot
    CHECK (c.snares == 10);    // from the third clap, less the two after the gap
    CHECK (! beatNear (listener, 4.15, 0.04));
    CHECK (! beatNear (listener, 4.5, 0.04));
    CHECK (! beatNear (listener, 5.0, 0.04));
    for (const double t : { 3.5, 5.5, 6.0, 7.5 })
        CHECK (beatNear (listener, t, 0.04));
}

TEST_CASE ("App: brain: a chord outside the key lights the right inferior frontal gyrus more than an in-key change")
{
    // C major (I - IV - V - I, twice) to set the key, then an in-key change (C -> Am) or one outside it (C -> Ab).
    const std::vector<std::vector<int>> progression { { 48, 60, 64, 67 }, { 41, 60, 65, 69 }, { 43, 59, 62, 67 }, { 48, 60, 64, 67 } };
    const auto runWith = [&progression] (const std::vector<int>& last, float& right, float& left, int& surprise, std::string& key, double& arrival)
    {
        Runner run;
        right = left = 0.0f;
        int changes = 0;
        double namedAt = -1.0;
        float previousRight = 0.0f;
        arrival = -1.0;
        run.run (
            570,
            [&] (int64_t n, float& l, float& r)
            {
                const double t = seconds (n);
                const auto& chord = t < 8.0 ? progression[static_cast<size_t> (static_cast<int> (t) % 4)] : last;
                l = r = chordSample (chord, n);
            },
            [&]
            {
                const auto& act = run.view.getActivity();
                if (act.getChordChanges() != changes)
                {
                    changes = act.getChordChanges();
                    if (run.view.getClock() >= 8.0 && namedAt < 0.0)
                        namedAt = run.view.getClock(); // the frame the tracker names the change to the last chord
                }
                const float nowRight = run.glow (Station::Ifg, brain::kRight);
                if (namedAt >= 0.0 && arrival < 0.0 && nowRight > 0.05f && nowRight > previousRight + 0.02f)
                    arrival = run.view.getClock() - namedAt; // the wave reaches the gyrus
                previousRight = nowRight;
                if (run.view.getClock() < 8.0)
                    return;
                right = std::max (right, nowRight);
                left = std::max (left, run.glow (Station::Ifg, brain::kLeft));
            });
        surprise = run.view.getListener().getLastChordSurprise();
        key = flub::app::ui::vis::music::keyName (run.view.getListener().getMusic().key.getKey()).toStdString();
    };
    float inRight = 0.0f, inLeft = 0.0f, outRight = 0.0f, outLeft = 0.0f;
    int inSurprise = -1, outSurprise = -1;
    std::string inKey, outKey;
    double inArrival = -1.0, outArrival = -1.0;
    runWith ({ 45, 57, 60, 64 }, inRight, inLeft, inSurprise, inKey, inArrival);     // A minor: in C major
    runWith ({ 44, 56, 60, 63 }, outRight, outLeft, outSurprise, outKey, outArrival); // A-flat major: A-flat and E-flat are outside
    std::printf ("  key %s / %s; C -> Am: IFG right %.2f left %.2f (surprise %d); C -> Ab: right %.2f left %.2f (surprise %d); the IFG lights "
                 "%.3f / %.3f s after the change is named\n",
                 inKey.c_str(), outKey.c_str(), static_cast<double> (inRight), static_cast<double> (inLeft), inSurprise, static_cast<double> (outRight),
                 static_cast<double> (outLeft), outSurprise, inArrival, outArrival);
    CHECK (inKey == "C major");
    CHECK (inSurprise == 0);
    CHECK (outSurprise == 1);
    CHECK (outRight > 2.0f * inRight);
    CHECK (outRight > outLeft);
    CHECK (outRight > 0.5f);
    // The ventral wave starts 0.1 s after the change is named and runs 0.12 s (docs/06: arrives 0.22 s after), within a frame.
    for (const double arrival : { inArrival, outArrival })
    {
        CHECK (arrival > 0.2);
        CHECK (arrival < 0.22 + 1.5 * kDt);
    }
}

namespace
{
/** A breakdown (pad and hats, 3 s), a 4 s build-up (a rising noise riser and an accelerating snare roll, no bass)
    and a drop (kick and bass) at 7 s. */
struct BuildUpAndDrop
{
    Noise noise;
    HighPass riser1 { 3000.0 }, riser2 { 3000.0 }, snareHp { 1200.0 }, hat1 { 6500.0 }, hat2 { 6500.0 };
    std::vector<double> rollHits;

    BuildUpAndDrop()
    {
        for (double t = 3.0; t < 7.0;)
        {
            rollHits.push_back (t);
            t += 0.25 * (1.0 - 0.75 * (t - 3.0) / 4.0); // 4 -> 16 hits per second
        }
    }

    void operator() (int64_t n, float& l, float& r)
    {
        const double t = seconds (n);
        const float x = noise();
        float v = 0.012f * (static_cast<float> (std::sin (2.0 * kPi * 261.63 * t)) + static_cast<float> (std::sin (2.0 * kPi * 329.63 * t))
                            + static_cast<float> (std::sin (2.0 * kPi * 392.0 * t)));
        if (t < 3.0 || t >= 7.0)
            v += 0.12f * hat2 (hat1 (x)) * decay (std::fmod (t, 0.25), 0.03);
        if (t >= 3.0 && t < 7.0)
        {
            const double progress = (t - 3.0) / 4.0;
            v += static_cast<float> (std::pow (10.0, (-40.0 + 26.0 * progress) / 20.0)) * 3.0f * riser2 (riser1 (x));
            double last = -1.0;
            for (const double h : rollHits)
                if (h <= t)
                    last = h;
            if (last >= 0.0)
            {
                const auto level = static_cast<float> (std::pow (10.0, (-30.0 + 18.0 * progress) / 20.0));
                v += level * 3.0f * (snareHp (x) * decay (t - last, 0.06) + 0.5f * static_cast<float> (std::sin (2.0 * kPi * 185.0 * (t - last))) * decay (t - last, 0.05));
            }
        }
        if (t >= 7.0)
        {
            v += 0.6f * kick (std::fmod (t - 7.0, 0.5), x);
            double bass = 0.0;
            for (int h = 1; h <= 5; ++h)
                bass += std::sin (2.0 * kPi * 55.0 * h * t) / h;
            v += 0.15f * static_cast<float> (bass) * decay (std::fmod (t - 7.0, 0.25), 0.2);
        }
        l = r = v;
    }
};
} // namespace

TEST_CASE ("App: brain: a build-up then a drop lights the caudate, then the nucleus accumbens")
{
    Runner run;
    BuildUpAndDrop music;
    float caudate = 0.0f, caudateBefore = 0.0f, caudateEarly = 0.0f, accumbensBefore = 0.0f, accumbensAfter = 0.0f;
    double caudatePeakAt = 0.0, accumbensPeakAt = 0.0;
    run.run (570, std::ref (music),
             [&]
             {
                 const double t = run.view.getActivity().getNow(); // display time (the clock minus the analysis latency)
                 const float c = std::max (run.glow (Station::Caudate, brain::kLeft), run.glow (Station::Caudate, brain::kRight));
                 const float a = std::max (run.glow (Station::Accumbens, brain::kLeft), run.glow (Station::Accumbens, brain::kRight));
                 if (c > caudate)
                 {
                     caudate = c;
                     caudatePeakAt = t;
                 }
                 if (t < 3.0)
                     caudateEarly = std::max (caudateEarly, c); // the breakdown before the build-up
                 if (t < 7.0)
                 {
                     caudateBefore = std::max (caudateBefore, c);
                     accumbensBefore = std::max (accumbensBefore, a);
                 }
                 else if (a > accumbensAfter)
                 {
                     accumbensAfter = a;
                     accumbensPeakAt = t;
                 }
             });
    const auto& act = run.view.getActivity();
    const auto& listener = run.view.getListener();
    std::printf ("  build-ups %d (anticipation pulses %d, the last at %.2f s), drops %d at %.2f s; caudate %.2f in the breakdown, %.2f before the drop, "
                 "peak %.2f at %.2f s; accumbens %.2f before the drop, %.2f at %.2f s; beats %d (%d kicks of %d kick-like hits, %d snares of %d "
                 "snare-like hits)\n",
                 listener.getBuildUps(), act.getAnticipations(), act.getLastAnticipationTime(), act.getDrops(), act.getLastDropTime(),
                 static_cast<double> (caudateEarly), static_cast<double> (caudateBefore), static_cast<double> (caudate), caudatePeakAt,
                 static_cast<double> (accumbensBefore), static_cast<double> (accumbensAfter), accumbensPeakAt, listener.getNumBeatTimes(),
                 listener.getKicks(), listener.getKickHits(), listener.getSnares(), listener.getSnareHits());
    juce::String beforeDrop;
    for (int i = 0; i < listener.getNumBeatTimes(); ++i)
        if (listener.getBeatTime (i) < 7.0)
            beforeDrop << juce::String (listener.getBeatTime (i), 2) << " ";
    std::printf ("  beats before the drop (the snare roll speeds up from 4 to 16 hits a second from 3 s): %s\n", beforeDrop.trimEnd().toRawUTF8());
    CHECK (listener.getBuildUps() == 1);
    CHECK (act.getAnticipations() >= 2);
    CHECK (act.getDrops() == 1);
    CHECK (act.getLastDropTime() > 6.95);
    CHECK (act.getLastDropTime() < 7.5);
    CHECK (caudateEarly == 0.0f);
    CHECK (caudateBefore > 0.4f);
    CHECK (caudatePeakAt < act.getLastDropTime() + 0.3); // the last anticipation pulse lands within its 0.25 s run
    CHECK (accumbensBefore < 0.05f);
    CHECK (accumbensAfter > 0.6f);
    CHECK (accumbensPeakAt > caudatePeakAt);
}

TEST_CASE ("App: brain heuristics on the app's own test music: beats on the kicks, in-key chords, no build-up or drop")
{
    // The screenshot scene's programme: kick on every beat (120 BPM), snare on 2 and 4, hats on the eighths,
    // a bass line on the eighths and a pad, Am - F - C - G (A minor), 12 s.
    const TestMusic music (12.0);
    Runner run;
    run.run (720, [&music] (int64_t n, float& l, float& r) { l = music.l[static_cast<size_t> (n)]; r = music.r[static_cast<size_t> (n)]; });
    const auto& listener = run.view.getListener();
    const auto& act = run.view.getActivity();
    int hits = 0, falseTriggers = 0;
    std::set<long long> matched;
    for (int i = 0; i < listener.getNumBeatTimes(); ++i)
    {
        const double t = listener.getBeatTime (i);
        const double nearest = 0.5 * std::round (t / 0.5);
        if (std::abs (t - nearest) < 0.06 && matched.insert (std::llround (nearest * 2.0)).second)
            ++hits;
        else
            ++falseTriggers;
    }
    const int kicksPlayed = 24;
    std::printf ("  test music 12 s: %d beats (%d kicks of %d kick-like hits + %d snares of %d snare-like hits) for %d kicks played: %d on a kick, "
                 "%d false, the first at %.2f s; chord changes %d (%d outside the key %s); build-ups %d, drops %d\n",
                 listener.getNumBeatTimes(), listener.getKicks(), listener.getKickHits(), listener.getSnares(), listener.getSnareHits(), kicksPlayed, hits,
                 falseTriggers, listener.getNumBeatTimes() > 0 ? listener.getBeatTime (0) : -1.0,
                 act.getChordChanges(),
                 act.getSurprises(), flub::app::ui::vis::music::keyName (listener.getMusic().key.getKey()).toStdString().c_str(), listener.getBuildUps(),
                 listener.getDrops());
    // Measured on MSVC: 21 of 24. The first two kicks only start the pattern (a kick is a beat when it repeats like a
    // drum beat, 2026-10-13; before: 23 of 24 from the first kick); the one at 10.0 s, under a new bass note, does not
    // read as a kick (before and after).
    CHECK (hits >= kicksPlayed - 3);
    CHECK (listener.getNumBeatTimes() > 0);
    if (listener.getNumBeatTimes() > 0)
        CHECK (std::abs (listener.getBeatTime (0) - 1.0) < 0.04); // the third kick
    CHECK (falseTriggers == 0);
    CHECK (act.getChordChanges() >= 4);
    CHECK (act.getSurprises() == 0);
    CHECK (listener.getBuildUps() == 0);
    CHECK (listener.getDrops() == 0);
}

// One loop per case (each under 2 s on a busy PC); the cases are built together so the shared noise and filters
// keep their order, and only the loop asked for is run.
static void checkSyntheticLoop (int which)
{
    struct Case
    {
        const char* name;
        std::function<void (int64_t, float&, float&)> gen;
        int maxBeats;
    };
    Noise noise;
    HighPass h1 (6500.0), h2 (6500.0);
    std::vector<Case> cases;
    cases.push_back ({ "sustained pad (C major)", [] (int64_t n, float& l, float& r) { l = r = chordSample ({ 48, 60, 64, 67 }, n); }, 0 });
    cases.push_back ({ "bass line, gated eighths without a click",
                       [] (int64_t n, float& l, float& r)
                       {
                           const double t = seconds (n), tau = std::fmod (t, 0.25);
                           double bass = 0.0;
                           for (int h = 1; h <= 5; ++h)
                               bass += std::sin (2.0 * kPi * 55.0 * h * t) / h;
                           l = r = 0.3f * static_cast<float> (bass) * std::min (1.0f, static_cast<float> (tau / 0.004)) * decay (tau, 0.18);
                       },
                       0 }); // measured on MSVC: 1 of the 24 notes reads as a kick (its centroid happened to fall smoothly); alone it
                             // does not repeat, so it is no beat (before 2026-10-13 it was)
    cases.push_back ({ "hi-hats on the eighths", [&noise, &h1, &h2] (int64_t n, float& l, float& r)
                       { l = r = 0.3f * h2 (h1 (noise())) * decay (std::fmod (seconds (n), 0.25), 0.03); },
                       0 });
    // A steady groove: kick, hats, bass and pad, 6 s (looping a 2 s bar would read the same).
    cases.push_back ({ "steady groove (kick, hats, bass, pad)",
                       [&noise, &h1, &h2] (int64_t n, float& l, float& r)
                       {
                           const double t = seconds (n);
                           const float x = noise();
                           double bass = 0.0;
                           for (int h = 1; h <= 5; ++h)
                               bass += std::sin (2.0 * kPi * 55.0 * h * t) / h;
                           l = r = 0.5f * kick (std::fmod (t, 0.5), x) + 0.15f * h2 (h1 (x)) * decay (std::fmod (t, 0.25), 0.03)
                                   + 0.12f * static_cast<float> (bass) * decay (std::fmod (t, 0.25), 0.18) + chordSample ({ 57, 60, 64 }, n, 0.03);
                       },
                       12 });
    {
        const auto& c = cases[static_cast<size_t> (which)];
        Runner run;
        run.run (360, c.gen);
        const auto& listener = run.view.getListener();
        juce::String times;
        for (int i = 0; i < std::min (6, listener.getNumBeatTimes()); ++i)
            times << juce::String (listener.getBeatTime (i), 2) << " ";
        std::printf ("  %s, 6 s: %d beats (%d kicks of %d kick-like hits, %d snares of %d snare-like hits%s%s), build-ups %d, drops %d\n", c.name,
                     listener.getNumBeatTimes(), listener.getKicks(), listener.getKickHits(), listener.getSnares(), listener.getSnareHits(),
                     times.isEmpty() ? "" : "; at ", times.trimEnd().toRawUTF8(), listener.getBuildUps(), listener.getDrops());
        CHECK (listener.getNumBeatTimes() <= c.maxBeats);
        CHECK (listener.getBuildUps() == 0);
        CHECK (listener.getDrops() == 0);
    }
}

TEST_CASE ("App: brain heuristics on synthetic loops: no beats from bass notes, hats or a pad; no build-up or drop in a steady groove - sustained pad")
{
    checkSyntheticLoop (0);
}

TEST_CASE ("App: brain heuristics on synthetic loops: no beats from bass notes, hats or a pad; no build-up or drop in a steady groove - bass line")
{
    checkSyntheticLoop (1);
}

TEST_CASE ("App: brain heuristics on synthetic loops: no beats from bass notes, hats or a pad; no build-up or drop in a steady groove - hi-hats")
{
    checkSyntheticLoop (2);
}

TEST_CASE ("App: brain heuristics on synthetic loops: no beats from bass notes, hats or a pad; no build-up or drop in a steady groove - steady groove")
{
    checkSyntheticLoop (3);
}

TEST_CASE ("App: brain heuristics: a pause in the music (pause and play, a gap between tracks) is not a drop")
{
    const TestMusic music (12.0);
    for (const double gap : { 0.5, 2.0 })
    {
        // The test music to 6 s, `gap` seconds of silence, then the music goes on where it stopped.
        Runner run;
        const auto pausedAt = static_cast<int64_t> (6.0 * kRate), resumeAt = pausedAt + static_cast<int64_t> (gap * kRate);
        run.run (static_cast<int> ((11.0 + gap) * kRate / kFrame),
                 [&] (int64_t n, float& l, float& r)
                 {
                     if (n >= pausedAt && n < resumeAt)
                     {
                         l = r = 0.0f;
                         return;
                     }
                     const auto i = static_cast<size_t> (n < pausedAt ? n : n - (resumeAt - pausedAt));
                     l = music.l[i];
                     r = music.r[i];
                 });
        const auto& listener = run.view.getListener();
        std::printf ("  test music paused for %.1f s at 6 s: drops %d, build-ups %d (anticipation pulses %d)\n", gap, listener.getDrops(),
                     listener.getBuildUps(), run.view.getActivity().getAnticipations());
        CHECK (listener.getDrops() == 0);
        CHECK (listener.getBuildUps() == 0);
    }
}

TEST_CASE ("App: brain heuristics: chords after silence or after a gap are not a build-up")
{
    // Chords without bass (C - F - G - C, a second each) with a 2 s gap at 5 s; then 6 s of silence before a held chord.
    const std::vector<std::vector<int>> chords { { 60, 64, 67 }, { 60, 65, 69 }, { 59, 62, 67 }, { 60, 64, 67 } };
    {
        Runner run;
        run.run (720,
                 [&chords] (int64_t n, float& l, float& r)
                 {
                     const double t = seconds (n);
                     l = r = t >= 5.0 && t < 7.0 ? 0.0f : chordSample (chords[static_cast<size_t> (static_cast<int> (t) % 4)], n);
                 });
        const auto& listener = run.view.getListener();
        std::printf ("  chords without bass, a 2 s gap at 5 s: build-ups %d (anticipation pulses %d), drops %d\n", listener.getBuildUps(),
                     run.view.getActivity().getAnticipations(), listener.getDrops());
        CHECK (listener.getBuildUps() == 0);
        CHECK (listener.getDrops() == 0);
    }
    {
        Runner run;
        run.run (720, [&chords] (int64_t n, float& l, float& r) { l = r = seconds (n) < 6.0 ? 0.0f : chordSample (chords[0], n); });
        const auto& listener = run.view.getListener();
        std::printf ("  6 s of silence, then a held chord: build-ups %d (anticipation pulses %d), drops %d\n", listener.getBuildUps(),
                     run.view.getActivity().getAnticipations(), listener.getDrops());
        CHECK (listener.getBuildUps() == 0);
        CHECK (listener.getDrops() == 0);
    }
}

TEST_CASE ("App: brain heuristics: a chord re-struck on every beat (a pumping synth, no hats) is not a build-up")
{
    // C major at -26 dBFS a note, struck every 0.5 s (a step to 1.0, decaying to 0.6), 17 s.
    Runner run;
    run.run (1020,
             [] (int64_t n, float& l, float& r)
             {
                 const double t = seconds (n), tau = std::fmod (t, 0.5);
                 double v = 0.0;
                 for (const int m : { 60, 64, 67 })
                     v += 0.05 * std::sin (2.0 * kPi * vis::PitchEstimator::midiToHz (m) * t + 0.3 * m);
                 l = r = static_cast<float> ((0.6 + 0.4 * std::exp (-tau / 0.15)) * v);
             });
    const auto& listener = run.view.getListener();
    std::printf ("  re-struck triad, 17 s: build-ups %d (anticipation pulses %d), drops %d, beats %d\n", listener.getBuildUps(),
                 run.view.getActivity().getAnticipations(), listener.getDrops(), listener.getNumBeatTimes());
    CHECK (listener.getBuildUps() == 0);
    CHECK (run.view.getActivity().getAnticipations() == 0);
    CHECK (listener.getDrops() == 0);
}

// One signal per case (each under 2 s).
static void checkNoise (int which)
{
    Noise noise;
    Runner run;
    std::function<void (int64_t, float&, float&)> gen;
    const char* name = "";
    int frames = 360;
    if (which == 0)
    {
        // White noise at -20 dBFS RMS (uniform noise: RMS = peak / sqrt 3).
        name = "white noise at -20 dBFS, 12 s";
        frames = 720;
        gen = [&noise] (int64_t, float& l, float& r) { l = r = 0.1f * 1.7320508f * noise(); };
    }
    else if (which == 1)
    {
        // 40 ms noise bursts at the irregular gunshots' times, 8 s.
        name = "9 noise bursts of 40 ms at irregular gaps, 8 s";
        frames = 480;
        gen = [&noise] (int64_t n, float& l, float& r)
        {
            const double t = seconds (n);
            const float x = noise();
            bool on = false;
            for (const double h : kIrregularShots)
                on = on || (t >= h && t < h + 0.04);
            l = r = on ? 0.5f * x : 0.0f;
        };
    }
    else
    {
        // 40 ms noise bursts every 0.7 s, 6 s: a perfectly regular gun, the limit of a rule on timing alone.
        name = "40 ms noise bursts every 0.7 s, 6 s";
        gen = [&noise] (int64_t n, float& l, float& r)
        {
            const double tau = std::fmod (seconds (n), 0.7);
            l = r = tau < 0.04 ? 0.5f * noise() : 0.0f;
        };
    }
    const auto c = countBeats (name, frames, gen, run);
    const auto& listener = run.view.getListener();
    const auto& act = run.view.getActivity();
    std::printf ("  %s: chord changes %d (%d outside the key), build-ups %d, drops %d\n", name, act.getChordChanges(), act.getSurprises(),
                 listener.getBuildUps(), listener.getDrops());
    CHECK (act.getSurprises() == 0);
    CHECK (listener.getBuildUps() == 0);
    CHECK (listener.getDrops() == 0);
    CHECK (c.kicks == 0);
    if (which < 2)
    {
        CHECK (c.snares == 0);
        CHECK (c.networkBeats == 0);
    }
    else
    {
        // Repeating steadily, the bursts count from the third on (each on its burst): the documented limit.
        CHECK (c.snareHits == 9);
        CHECK (c.snares == 7);
        CHECK (! beatNear (listener, 0.0, 0.1));
        CHECK (! beatNear (listener, 0.7, 0.1));
        for (int k = 2; k < 9; ++k)
            CHECK (beatNear (listener, 0.7 * k, 0.04));
    }
}

TEST_CASE ("App: brain heuristics on noise and noise bursts: a snare beat only when the bursts repeat steadily; no chord surprise, build-up or drop "
           "- white noise")
{
    checkNoise (0);
}

TEST_CASE ("App: brain heuristics on noise and noise bursts: a snare beat only when the bursts repeat steadily; no chord surprise, build-up or drop "
           "- irregular bursts")
{
    checkNoise (1);
}

TEST_CASE ("App: brain heuristics on noise and noise bursts: a snare beat only when the bursts repeat steadily; no chord surprise, build-up or drop "
           "- bursts every 0.7 s")
{
    checkNoise (2);
}

TEST_CASE ("App: brain: a NaN or infinite block does not latch the view (the level and the drop come back)")
{
    const auto tone = [] (int64_t n, float& l, float& r) { l = r = static_cast<float> (0.3 * std::sin (2.0 * kPi * 440.0 * seconds (n))); };
    const auto heschlAfter = [&tone] (float bad)
    {
        // The tone, one bad display frame at 1 s, then 2 s more of the tone.
        Runner run;
        run.run (60, tone);
        if (! std::isfinite (bad))
            run.run (1, [bad] (int64_t, float& l, float& r) { l = r = bad; });
        else
            run.run (1, tone);
        run.run (120, tone);
        return run.view.getActivity().heschlPeak (brain::kRight);
    };
    const float control = heschlAfter (0.0f);
    const float afterNan = heschlAfter (std::numeric_limits<float>::quiet_NaN());
    const float afterInf = heschlAfter (std::numeric_limits<float>::infinity());
    std::printf ("  Heschl's gyrus 2 s after one bad frame: control %.3f, NaN %.3f, inf %.3f\n", static_cast<double> (control),
                 static_cast<double> (afterNan), static_cast<double> (afterInf));
    CHECK (control > 0.3f);
    CHECK (afterNan > 0.9f * control);
    CHECK (afterInf > 0.9f * control);

    // The build-up and drop with a NaN frame at 1 s: the drop is still found.
    Runner run;
    BuildUpAndDrop music;
    run.run (60, std::ref (music));
    run.run (1, [] (int64_t, float& l, float& r) { l = r = std::numeric_limits<float>::quiet_NaN(); });
    run.run (509, std::ref (music));
    std::printf ("  build-up and drop with a NaN frame at 1 s: drops %d at %.2f s\n", run.view.getActivity().getDrops(),
                 run.view.getActivity().getLastDropTime());
    CHECK (run.view.getActivity().getDrops() == 1);
}

// =============================================================================
// The view
// =============================================================================
TEST_CASE ("App: brain view: hover names a landing spot, the menu stops the turning, the view resets")
{
    Runner run;
    run.view.setBounds (0, 0, 1000, 600);
    run.view.setVisible (true);
    run.run (30, [] (int64_t n, float& l, float& r) { l = r = static_cast<float> (0.2 * std::sin (2.0 * kPi * 300.0 * static_cast<double> (n) / kRate)); });
    run.view.renderScene();
    for (const Station s : { Station::CochlearNucleus, Station::Mgn, Station::Premotor, Station::Ifg })
    {
        const int n = nodeOf (s, brain::kRight);
        const auto at = run.view.getNodeScreenPosition (n);
        CHECK (run.view.getPlotArea().expanded (40.0f).contains (at));
        const int found = run.view.findNodeAt (at);
        CHECK (found >= 0);
        if (found >= 0)
            CHECK (brain::BrainAnatomy::get().getNode (found).position.x > 0.0f); // the right one, or a close neighbour on the right
    }
    CHECK (run.view.findNodeAt ({ 2.0f, 2.0f }) == -1);
    CHECK (vis::BrainView::describeNode (nodeOf (Station::Ifg, brain::kRight)).contains ("heuristic"));
    CHECK (vis::BrainView::describeNode (nodeOf (Station::Heschl, brain::kLeft)).contains ("left"));

    // Turning: on by default; the menu's first item stops it; the angle then stays.
    CHECK (run.view.isTurning());
    juce::PopupMenu menu;
    run.view.addMenuItems (menu);
    CHECK (menu.getNumItems() == 4); // Stop turning, Zoom in, Zoom out, Reset view
    run.view.setTurning (false);
    const float angle = run.view.getAngle();
    run.run (10, [] (int64_t, float& l, float& r) { l = r = 0.0f; });
    CHECK (run.view.getAngle() == angle);
    run.view.setTurning (true);
    run.run (10, [] (int64_t, float& l, float& r) { l = r = 0.0f; });
    CHECK (run.view.getAngle() != angle);
    run.view.resetView();
    CHECK (run.view.getAngle() == vis::BrainView::kStartAngle);
    CHECK (run.view.getTilt() == vis::BrainView::kStartTilt);
}

TEST_CASE ("App: brain view zooms: the wheel, a pinch and the menu bring the brain closer or further (it starts at 1.2 x), Reset view restores it")
{
    Runner run;
    run.view.setBounds (0, 0, 1000, 600);
    run.view.setVisible (true);
    run.run (5, [] (int64_t, float& l, float& r) { l = r = 0.0f; });
    run.view.setTurning (false);
    CHECK (run.view.getZoom() == vis::BrainView::kDefaultZoom);

    // The zoom scales every distance on screen about the centre: the two Heschl's gyri at 1.2 x are 1.2 times as far
    // apart as at 1 x (the framing that fits the ear rings).
    const int left = nodeOf (Station::Heschl, brain::kLeft), right = nodeOf (Station::Heschl, brain::kRight);
    const auto spread = [&run, left, right]
    {
        run.view.renderScene();
        return run.view.getNodeScreenPosition (left).getDistanceFrom (run.view.getNodeScreenPosition (right));
    };
    const float atDefault = spread();
    run.view.setZoom (1.0f);
    const float atOne = spread();
    std::printf ("  Heschl's gyri apart on screen: %.1f px at 1 x, %.1f px at the default %.1f x\n", static_cast<double> (atOne),
                 static_cast<double> (atDefault), static_cast<double> (vis::BrainView::kDefaultZoom));
    CHECK (atOne > 10.0f);
    CHECK_NEAR (atDefault / atOne, vis::BrainView::kDefaultZoom, 0.01);

    // The wheel: one notch (deltaY about 0.23 on Windows) is about 15 %, and back.
    const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), { 500.0f, 300.0f }, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &run.view,
                              &run.view, juce::Time::getCurrentTime(), { 500.0f, 300.0f }, juce::Time::getCurrentTime(), 1, false);
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = 0.23f;
    run.view.mouseWheelMove (e, wheel);
    CHECK_NEAR (run.view.getZoom(), std::exp (0.23 * vis::BrainView::kWheelZoomRate), 1.0e-4);
    CHECK (run.view.getZoom() > 1.1f && run.view.getZoom() < 1.2f);
    wheel.deltaY = -0.23f;
    run.view.mouseWheelMove (e, wheel);
    CHECK_NEAR (run.view.getZoom(), 1.0, 1.0e-4);
    // A trackpad pinch (macOS) multiplies the zoom by its scale.
    run.view.mouseMagnify (e, 1.5f);
    CHECK_NEAR (run.view.getZoom(), 1.5, 1.0e-4);

    // Limits, and a non-finite value is ignored.
    run.view.setZoom (10.0f);
    CHECK (run.view.getZoom() == vis::BrainView::kMaxZoom);
    run.view.setZoom (std::nanf (""));
    CHECK (run.view.getZoom() == vis::BrainView::kMaxZoom);
    {
        // At the closest, the menu's Zoom in is greyed out and Zoom out is not.
        juce::PopupMenu menu;
        run.view.addMenuItems (menu);
        int seen = 0;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        {
            const auto& item = it.getItem();
            if (item.text == "Zoom in")
            {
                CHECK (! item.isEnabled);
                ++seen;
            }
            if (item.text == "Zoom out")
            {
                CHECK (item.isEnabled);
                ++seen;
            }
        }
        CHECK (seen == 2);
    }
    run.view.setZoom (0.01f);
    CHECK (run.view.getZoom() == vis::BrainView::kMinZoom);
    // Still a picture at both limits (the brain stays on screen at the furthest, the centre at the closest).
    run.view.renderScene();
    CHECK (run.view.getPlotArea().contains (run.view.getNodeScreenPosition (left)));

    run.view.resetView();
    CHECK (run.view.getZoom() == vis::BrainView::kDefaultZoom);
}

TEST_CASE ("App: brain view draws a real brain, not its mirror image (facing it, its right side is on the viewer's left)")
{
    Runner run;
    run.view.setBounds (0, 0, 1000, 600);
    run.view.setVisible (true);
    const auto& a = brain::BrainAnatomy::get();
    using A = brain::BrainAnatomy;
    const auto front = A::fromMni ({ 0.0f, 70.0f, 0.0f }), back = A::fromMni ({ 0.0f, -104.0f, 0.0f });
    const auto rightEar = a.earCentre (brain::kRight), leftEar = a.earCentre (brain::kLeft);
    const auto rightHeschl = a.getNode (nodeOf (Station::Heschl, brain::kRight)).position;
    const auto leftHeschl = a.getNode (nodeOf (Station::Heschl, brain::kLeft)).position;
    struct Seen
    {
        float x = 0.0f, y = 0.0f, depth = 0.0f;
    };
    const auto see = [&run] (brain::Vec3 p)
    {
        Seen s;
        juce::Point<float> at;
        CHECK (run.view.projectPoint (p, at, s.depth));
        s.x = at.x;
        s.y = at.y;
        return s;
    };
    const auto look = [&run] (float angle, float tilt)
    {
        run.view.setView (angle, tilt);
        run.view.renderScene();
    };

    // Facing it (angle 0, level): the face is nearer than the back of the head, and the right ear and the right
    // Heschl's gyrus are on the viewer's left (as with a person facing you).
    look (0.0f, 0.0f);
    CHECK (see (front).depth < see (back).depth);
    CHECK (see (rightEar).x < see (leftEar).x);
    CHECK (see (rightHeschl).x < see (leftHeschl).x);
    // From behind (angle pi): the back of the head is nearer, the right ear on the viewer's right.
    look (static_cast<float> (kPi), 0.0f);
    CHECK (see (back).depth < see (front).depth);
    CHECK (see (rightEar).x > see (leftEar).x);

    // At any angle and tilt the picture is the brain turned, never reflected: its axes right, front and up (right-handed,
    // as MNI's x, y, z) stay right-handed on screen (x to the right, y up, towards the viewer).
    const brain::Vec3 centre { 0.0f, -0.17f, 0.0f };
    for (const float angle : { -2.5f, -0.5f, 0.0f, 0.7f, 2.0f, 3.14159f, 4.5f })
        for (const float tilt : { -0.5f, 0.15f, 0.8f })
        {
            look (angle, tilt);
            const auto o = see (centre);
            const auto axis = [&] (brain::Vec3 v)
            {
                const auto s = see (centre + v);
                return std::array<double, 3> { s.x - o.x, -(s.y - o.y), -(s.depth - o.depth) };
            };
            const auto r = axis ({ 0.05f, 0.0f, 0.0f }), f = axis ({ 0.0f, 0.0f, 0.05f }), u = axis ({ 0.0f, 0.05f, 0.0f });
            // det [r f u] (columns): screen x, up and towards the viewer as rows; a positive scale per row keeps its sign.
            const double det = r[0] * (f[1] * u[2] - f[2] * u[1]) - f[0] * (r[1] * u[2] - r[2] * u[1]) + u[0] * (r[1] * f[2] - r[2] * f[1]);
            if (det <= 0.0)
                std::printf ("  angle %.2f tilt %.2f: mirrored (det %.4f)\n", static_cast<double> (angle), static_cast<double> (tilt), det);
            CHECK (det > 0.0);
        }

    // The start view (the approved mockup's pose): the face to the viewer's right, the right side of the brain nearer;
    // turning moves the face further to the right first, as the mockup's.
    run.view.resetView();
    run.view.renderScene();
    CHECK (see (front).x > see (back).x);
    CHECK (see (rightHeschl).depth < see (leftHeschl).depth);
    const float faceBefore = see (front).x;
    run.run (30, [] (int64_t, float& l, float& r) { l = r = 0.0f; });
    run.view.renderScene();
    CHECK (see (front).x > faceBefore);
}

TEST_CASE ("App: brain view stops rendering while it is dark and still, and starts again when it lights or turns")
{
    Runner run;
    run.view.setBounds (0, 0, 800, 450);
    run.view.setVisible (true);
    juce::Image screen (juce::Image::ARGB, 800, 450, true, juce::SoftwareImageType());
    const auto frames = [&run, &screen] (int count, const std::function<void (int64_t, float&, float&)>& gen)
    {
        for (int f = 0; f < count; ++f)
        {
            run.run (1, gen);
            juce::Graphics g (screen);
            run.view.paintEntireComponent (g, false);
        }
    };
    const auto silence = [] (int64_t, float& l, float& r) { l = r = 0.0f; };
    const auto tone = [] (int64_t n, float& l, float& r) { l = r = static_cast<float> (0.3 * std::sin (2.0 * kPi * 440.0 * seconds (n))); };
    run.view.setTurning (false);
    frames (30, silence);
    auto before = run.view.getRenderCount();
    frames (30, silence);
    const auto stillDark = run.view.getRenderCount() - before;
    before = run.view.getRenderCount();
    frames (30, tone);
    const auto lit = run.view.getRenderCount() - before;
    // 5 s: everything fades out (the model only: painting these frames is what made the case slow), then the first
    // paint renders the dark brain once and the next one does not.
    run.run (298, silence);
    frames (2, silence);
    before = run.view.getRenderCount();
    frames (30, silence);
    const auto darkAgain = run.view.getRenderCount() - before;
    run.view.setTurning (true);
    before = run.view.getRenderCount();
    frames (30, silence);
    const auto turning = run.view.getRenderCount() - before;
    std::printf ("  renders in 30 frames: dark and still %lld, a tone %lld, dark again %lld, turning %lld\n", static_cast<long long> (stillDark),
                 static_cast<long long> (lit), static_cast<long long> (darkAgain), static_cast<long long> (turning));
    CHECK (stillDark == 0);
    CHECK (lit >= 20); // the display runs 0.116 s (7 frames) behind the sound: the first frames of the tone are still dark
    CHECK (darkAgain == 0);
    CHECK (turning >= 25);
}

// One size per case (each under 2 s on a busy PC).
static void checkFrameCost (juce::Point<int> size)
{
    const TestMusic music (3.2);
    {
        Runner run;
        run.view.setBounds (0, 0, size.x, size.y);
        run.view.setVisible (true);
        const auto feed = [&music] (int64_t n, float& l, float& r)
        {
            const auto i = static_cast<size_t> (n) % music.l.size();
            l = music.l[i];
            r = music.r[i];
        };
        run.run (120, feed); // 2 s: the music lights the paths
        juce::Image screen (juce::Image::ARGB, size.x, size.y, true, juce::SoftwareImageType());

        double advanceMs = 0.0, renderMs = 0.0, renderMax = 0.0, uploadMs = 0.0, paintMs = 0.0;
        int64_t allocations = 0, uploadAllocations = 0;
        constexpr int kFrames = 40;
        for (int f = 0; f < kFrames; ++f)
        {
            for (size_t i = 0; i < static_cast<size_t> (kFrame); ++i)
            {
                float l = 0.0f, r = 0.0f;
                feed (run.n++, l, r);
                run.mid[i] = 0.5f * (l + r);
                run.side[i] = 0.5f * (l - r);
            }
            const auto t0 = juce::Time::getHighResolutionTicks();
            {
                flubapptest::RealtimeProbe probe;
                run.view.pushPost (run.mid.data(), run.side.data(), kFrame);
                run.view.advance (vis::FrameContext { run.meters, kDt, kRate });
                run.view.renderScene();
                allocations += probe.allocations();
            }
            const auto t1 = juce::Time::getHighResolutionTicks();
            {
                flubapptest::RealtimeProbe probe;
                run.view.uploadScene(); // into the native image: JUCE's own staging (a Direct2D image allocates one)
                uploadAllocations += probe.allocations();
            }
            const auto t15 = juce::Time::getHighResolutionTicks();
            {
                juce::Graphics g (screen);
                run.view.paintEntireComponent (g, false); // the blit and the overlay (the scene is already rendered)
            }
            const auto t2 = juce::Time::getHighResolutionTicks();
            uploadMs += juce::Time::highResolutionTicksToSeconds (t15 - t1) * 1000.0;
            renderMs += run.view.getLastRenderMs();
            renderMax = std::max (renderMax, run.view.getLastRenderMs());
            advanceMs += juce::Time::highResolutionTicksToSeconds (t1 - t0) * 1000.0 - run.view.getLastRenderMs();
            paintMs += juce::Time::highResolutionTicksToSeconds (t2 - t15) * 1000.0;
        }
        std::printf ("  %d x %d: analysis + model %.2f ms, scene render %.2f ms (max %.2f), upload %.2f ms (%lld allocations by JUCE in %d frames), "
                     "blit + overlay into a software image %.2f ms per frame (mean of %d)\n",
                     size.x, size.y, advanceMs / kFrames, renderMs / kFrames, renderMax, uploadMs / kFrames, static_cast<long long> (uploadAllocations), kFrames,
                     paintMs / kFrames, kFrames);
        CHECK (allocations == 0);
        CHECK (renderMs / kFrames < 25.0); // loose: CI machines vary; the measured numbers are in docs/06

        // Lit, and nothing turned white: every lit pixel keeps a hue (one channel well under the brightest).
        juce::Image::BitmapData data (run.view.getCanvas(), juce::Image::BitmapData::readOnly);
        int white = 0, lit = 0;
        for (int y = 0; y < data.height; y += 2)
            for (int x = 0; x < data.width; x += 2)
            {
                const auto c = data.getPixelColour (x, y);
                const int hi = std::max ({ c.getRed(), c.getGreen(), c.getBlue() }), lo = std::min ({ c.getRed(), c.getGreen(), c.getBlue() });
                lit += hi > 120 ? 1 : 0;
                white += lo > 235 ? 1 : 0;
            }
        CHECK (lit > 200);
        CHECK (white == 0);
    }
}

TEST_CASE ("App: brain view does not allocate per frame and renders a frame quickly at 1280 x 720 and 1920 x 1080 - 1280 x 720")
{
    checkFrameCost ({ 1280, 720 });
}

TEST_CASE ("App: brain view does not allocate per frame and renders a frame quickly at 1280 x 720 and 1920 x 1080 - 1920 x 1080")
{
    checkFrameCost ({ 1920, 1080 });
}

TEST_CASE ("App: brain view renders PNGs when FLUB_BRAIN_SHOTS names a folder (not a check)")
{
    const auto folder = juce::SystemStats::getEnvironmentVariable ("FLUB_BRAIN_SHOTS", {});
    if (folder.isEmpty())
        return;
    const juce::File dir (folder);
    dir.createDirectory();
    const auto save = [&dir] (vis::BrainView& view, const juce::String& name)
    {
        juce::Image image (juce::Image::ARGB, view.getWidth(), view.getHeight(), true, juce::SoftwareImageType());
        {
            juce::Graphics g (image);
            g.fillAll (juce::Colour (0xff111318));
            view.paintEntireComponent (g, false);
        }
        juce::FileOutputStream out (dir.getChildFile (name));
        out.setPosition (0);
        out.truncate();
        juce::PNGImageFormat().writeImageToStream (image, out);
    };
    {
        const TestMusic music (4.0);
        Runner run;
        run.view.setBounds (0, 0, 1280, 720);
        run.view.setVisible (true);
        run.run (230, [&music] (int64_t n, float& l, float& r) { l = music.l[static_cast<size_t> (n)]; r = music.r[static_cast<size_t> (n)]; });
        save (run.view, "test-brain-music-1280x720.png");
    }
    {
        Runner run;
        run.view.setBounds (0, 0, 1280, 720);
        run.view.setVisible (true);
        BuildUpAndDrop music;
        run.run (400, std::ref (music));
        save (run.view, "test-brain-buildup-1280x720.png");
        run.run (40, std::ref (music));
        save (run.view, "test-brain-drop-1280x720.png");
    }
    {
        Runner run;
        run.view.setBounds (0, 0, 1280, 720);
        run.view.setVisible (true);
        run.run (90, [] (int64_t n, float& l, float& r)
                 {
                     l = static_cast<float> (0.3 * std::sin (2.0 * kPi * 440.0 * static_cast<double> (n) / kRate));
                     r = 0.0f;
                 });
        save (run.view, "test-brain-left-tone-1280x720.png");
    }
}
