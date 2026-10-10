// App-level tests: the brain visualiser ("brain", docs/06 §6.4.2): the anatomy
// and its tract graph, the tonotopic maps, the crossed ascending pathway (a
// tone in one ear lights the opposite auditory cortex more), the beat network,
// chord surprise, build-up and drop, the heuristics' false triggers on the
// app's own test music and synthetic loops, intensity, hover names, no
// allocation per frame and the frame time at 1280 x 720 and 1920 x 1080.
// Set FLUB_BRAIN_SHOTS=<folder> to also write rendered PNGs of the view there.
#include "AppTestSupport.h"

#include "engine/TestSignalGenerator.h"
#include "ui/MeterSnapshot.h"
#include "ui/vis/BrainAnatomy.h"
#include "ui/vis/BrainView.h"
#include "ui/vis/PitchEstimator.h"

#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
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
            ++crossing;
    }
    CHECK (auditory == 22);
    CHECK (crossing >= 6); // the trapezoid body (4) and the cerebellothalamic tracts (2)

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
        // 65 % / 35 % above the olive; the cortex adds +15 % for tonal sound on the right (Zatorre and Belin), so a
        // tone in the right ear lights the left gyrus about 0.65 x 0.85 / (0.35 x 1.15) = 1.37 times the right one.
        CHECK (opposite > (ear == brain::kLeft ? 2.0 : 1.25) * same);
        CHECK (icOpposite > 1.5 * icSame);
        const auto& act = run.view.getActivity();
        // Only the ear that hears it lights its cochlea.
        CHECK (act.glow (nodeOf (Station::Cochlea, ear)).level > 0.3f);
        CHECK (act.glow (nodeOf (Station::Cochlea, 1 - ear)).level == 0.0f);
    }
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
    std::printf ("  6 kicks in 3 s: %d kicks, %d snares detected; peaks cerebellum %.2f, VL thalamus %.2f, premotor %.2f, SMA %.2f, putamen %.2f\n",
                 listener.getKicks(), listener.getSnares(), static_cast<double> (peak[0]), static_cast<double> (peak[1]), static_cast<double> (peak[2]),
                 static_cast<double> (peak[3]), static_cast<double> (peak[4]));
    CHECK (listener.getKicks() == 6);
    CHECK (listener.getSnares() == 0);
    // Each detection within 40 ms of its kick (display time is the stream time of the analysis window's centre).
    for (int i = 0; i < listener.getNumBeatTimes(); ++i)
    {
        const double t = listener.getBeatTime (i);
        CHECK (std::abs (t - 0.5 * std::round (t / 0.5)) < 0.04);
    }
    for (const float p : peak)
        CHECK (p > 0.2f);
}

TEST_CASE ("App: brain: a chord outside the key lights the right inferior frontal gyrus more than an in-key change")
{
    // C major (I - IV - V - I, twice) to set the key, then an in-key change (C -> Am) or one outside it (C -> Ab).
    const std::vector<std::vector<int>> progression { { 48, 60, 64, 67 }, { 41, 60, 65, 69 }, { 43, 59, 62, 67 }, { 48, 60, 64, 67 } };
    const auto runWith = [&progression] (const std::vector<int>& last, float& right, float& left, int& surprise, std::string& key)
    {
        Runner run;
        right = left = 0.0f;
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
                if (run.view.getClock() < 8.0)
                    return;
                right = std::max (right, run.glow (Station::Ifg, brain::kRight));
                left = std::max (left, run.glow (Station::Ifg, brain::kLeft));
            });
        surprise = run.view.getListener().getLastChordSurprise();
        key = flub::app::ui::vis::music::keyName (run.view.getListener().getMusic().key.getKey()).toStdString();
    };
    float inRight = 0.0f, inLeft = 0.0f, outRight = 0.0f, outLeft = 0.0f;
    int inSurprise = -1, outSurprise = -1;
    std::string inKey, outKey;
    runWith ({ 45, 57, 60, 64 }, inRight, inLeft, inSurprise, inKey);  // A minor: in C major
    runWith ({ 44, 56, 60, 63 }, outRight, outLeft, outSurprise, outKey); // A-flat major: A-flat and E-flat are outside
    std::printf ("  key %s / %s; C -> Am: IFG right %.2f left %.2f (surprise %d); C -> Ab: right %.2f left %.2f (surprise %d)\n", inKey.c_str(),
                 outKey.c_str(), static_cast<double> (inRight), static_cast<double> (inLeft), inSurprise, static_cast<double> (outRight),
                 static_cast<double> (outLeft), outSurprise);
    CHECK (inKey == "C major");
    CHECK (inSurprise == 0);
    CHECK (outSurprise == 1);
    CHECK (outRight > 2.0f * inRight);
    CHECK (outRight > outLeft);
    CHECK (outRight > 0.5f);
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
                 "peak %.2f at %.2f s; accumbens %.2f before the drop, %.2f at %.2f s\n",
                 listener.getBuildUps(), act.getAnticipations(), act.getLastAnticipationTime(), act.getDrops(), act.getLastDropTime(),
                 static_cast<double> (caudateEarly), static_cast<double> (caudateBefore), static_cast<double> (caudate), caudatePeakAt,
                 static_cast<double> (accumbensBefore), static_cast<double> (accumbensAfter), accumbensPeakAt);
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
    std::printf ("  test music 12 s: %d beats (%d kicks + %d snares) for %d kicks played: %d on a kick, %d false; chord changes %d (%d outside the "
                 "key %s); build-ups %d, drops %d\n",
                 listener.getNumBeatTimes(), listener.getKicks(), listener.getSnares(), kicksPlayed, hits, falseTriggers, act.getChordChanges(),
                 act.getSurprises(), flub::app::ui::vis::music::keyName (listener.getMusic().key.getKey()).toStdString().c_str(), listener.getBuildUps(),
                 listener.getDrops());
    CHECK (hits >= kicksPlayed - 2); // measured on MSVC: 23 of 24
    CHECK (falseTriggers == 0);
    CHECK (act.getChordChanges() >= 4);
    CHECK (act.getSurprises() == 0);
    CHECK (listener.getBuildUps() == 0);
    CHECK (listener.getDrops() == 0);
}

TEST_CASE ("App: brain heuristics on synthetic loops: no beats from bass notes, hats or a pad; no build-up or drop in a steady groove")
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
                       2 }); // measured on MSVC: 1 of the 24 notes reads as a kick (its centroid happened to fall smoothly)
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
    for (auto& c : cases)
    {
        Runner run;
        run.run (360, c.gen);
        const auto& listener = run.view.getListener();
        juce::String times;
        for (int i = 0; i < std::min (6, listener.getNumBeatTimes()); ++i)
            times << juce::String (listener.getBeatTime (i), 2) << " ";
        std::printf ("  %s, 6 s: %d beats (%d kicks, %d snares%s%s), build-ups %d, drops %d\n", c.name, listener.getNumBeatTimes(), listener.getKicks(),
                     listener.getSnares(), times.isEmpty() ? "" : "; at ", times.trimEnd().toRawUTF8(), listener.getBuildUps(), listener.getDrops());
        CHECK (listener.getNumBeatTimes() <= c.maxBeats);
        CHECK (listener.getBuildUps() == 0);
        CHECK (listener.getDrops() == 0);
    }
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
    CHECK (menu.getNumItems() == 2);
    run.view.setTurning (false);
    const float angle = run.view.getAngle();
    run.run (10, [] (int64_t, float& l, float& r) { l = r = 0.0f; });
    CHECK (run.view.getAngle() == angle);
    run.view.setTurning (true);
    run.run (10, [] (int64_t, float& l, float& r) { l = r = 0.0f; });
    CHECK (run.view.getAngle() > angle);
    run.view.resetView();
    CHECK (run.view.getAngle() == vis::BrainView::kStartAngle);
    CHECK (run.view.getTilt() == vis::BrainView::kStartTilt);
}

TEST_CASE ("App: brain view does not allocate per frame and renders a frame quickly at 1280 x 720 and 1920 x 1080")
{
    const TestMusic music (3.2);
    for (const auto& size : { juce::Point<int> (1280, 720), juce::Point<int> (1920, 1080) })
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
