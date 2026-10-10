#include "BrainAnatomy.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flub::app::ui::vis::brain
{
namespace
{
constexpr float kPi = 3.14159265358979f;

/** Deterministic xorshift32 (the same cloud on every platform and run). */
struct Rng
{
    uint32_t state = 0x9e3779b9u;
    float next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float> (state >> 8) / 16777216.0f; // [0, 1)
    }
    /** A uniformly distributed unit vector. */
    Vec3 direction() noexcept
    {
        const float z = next() * 2.0f - 1.0f;
        const float p = next() * 2.0f * kPi;
        const float r = std::sqrt (std::max (0.0f, 1.0f - z * z));
        return { r * std::cos (p), r * std::sin (p), z };
    }
};

float sideSign (int side) noexcept
{
    return side == kRight ? 1.0f : -1.0f;
}

/** The drawn ear and cochlea (outside the head, as the approved mockup draws them). */
constexpr Vec3 kEarDrawn { 1.2f, -0.12f, 0.0f };
constexpr Vec3 kCochleaDrawn { 1.0f, -0.24f, 0.02f };
constexpr float kSpiralTurns = 2.5f, kSpiralRadius = 0.075f;

// Heschl's gyrus: its two ends (MNI, right side).
constexpr Vec3 kHeschlLowMni { 56.0f, -11.0f, 3.0f };   // anterolateral: low frequencies
constexpr Vec3 kHeschlHighMni { 38.0f, -29.0f, 10.0f }; // posteromedial: high frequencies

Vec3 mirror (Vec3 v, int side) noexcept
{
    return { v.x * sideSign (side), v.y, v.z };
}

/** Uniform Catmull-Rom through the waypoints, resampled to `count` points equally spaced along its length. */
void sampleCurve (const std::vector<Vec3>& way, int count, std::vector<Vec3>& out)
{
    std::vector<Vec3> dense;
    const int spans = static_cast<int> (way.size()) - 1;
    constexpr int kPerSpan = 64;
    const auto at = [&way] (int i) { return way[static_cast<size_t> (std::clamp (i, 0, static_cast<int> (way.size()) - 1))]; };
    for (int s = 0; s < spans; ++s)
    {
        const Vec3 p0 = s == 0 ? at (0) * 2.0f - at (1) : at (s - 1);
        const Vec3 p1 = at (s), p2 = at (s + 1);
        const Vec3 p3 = s + 2 > spans ? at (spans) * 2.0f - at (spans - 1) : at (s + 2);
        for (int k = 0; k < kPerSpan; ++k)
        {
            const float t = static_cast<float> (k) / kPerSpan, t2 = t * t, t3 = t2 * t;
            const Vec3 v = (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
            dense.push_back (v);
        }
    }
    dense.push_back (way.back());
    std::vector<float> cumulative (dense.size(), 0.0f);
    for (size_t i = 1; i < dense.size(); ++i)
        cumulative[i] = cumulative[i - 1] + length (dense[i] - dense[i - 1]);
    const float total = cumulative.back();
    size_t j = 0;
    for (int i = 0; i < count; ++i)
    {
        const float want = total * static_cast<float> (i) / static_cast<float> (count - 1);
        while (j + 2 < dense.size() && cumulative[j + 1] < want)
            ++j;
        const float span = cumulative[j + 1] - cumulative[j];
        const float f = span > 1.0e-9f ? std::clamp ((want - cumulative[j]) / span, 0.0f, 1.0f) : 0.0f;
        out.push_back (lerp (dense[j], dense[j + 1], f));
    }
    out[out.size() - static_cast<size_t> (count)] = way.front();
    out.back() = way.back();
}

float hueToRgb (float p, float q, float t) noexcept
{
    if (t < 0.0f)
        t += 1.0f;
    if (t > 1.0f)
        t -= 1.0f;
    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 0.5f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}
} // namespace

// =============================================================================
// Small vector helpers
// =============================================================================
Vec3 operator+ (Vec3 a, Vec3 b) noexcept { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 operator- (Vec3 a, Vec3 b) noexcept { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 operator* (Vec3 a, float s) noexcept { return { a.x * s, a.y * s, a.z * s }; }
float length (Vec3 v) noexcept { return std::sqrt (v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 lerp (Vec3 a, Vec3 b, float t) noexcept { return a + (b - a) * t; }

double bandHz (int band) noexcept
{
    const double t = static_cast<double> (std::clamp (band, 0, kBands - 1)) / (kBands - 1);
    return kLowHz * std::pow (kHighHz / kLowHz, t);
}

std::array<float, 3> frequencyRgb (double hz) noexcept
{
    const double f = std::log2 (std::max (hz, 1.0));
    const auto x = static_cast<float> (std::clamp ((f - std::log2 (kLowHz)) / (std::log2 (kHighHz) - std::log2 (kLowHz)), 0.0, 1.0));
    const float h = 0.52f * x, s = 0.95f, l = 0.58f;
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    return { hueToRgb (p, q, h + 1.0f / 3.0f), hueToRgb (p, q, h), hueToRgb (p, q, h - 1.0f / 3.0f) };
}

// =============================================================================
// The table
// =============================================================================
const std::vector<BrainAnatomy::MniEntry>& BrainAnatomy::mniTable()
{
    // Approximate MNI coordinates (mm) of the right-side structure; x is mirrored for the left.
    // docs/06 §6.4.2 lists them with their sources.
    static const std::vector<MniEntry> table {
        { Station::Ear, { 108.0f, -18.0f, -1.0f }, "drawn beside the head, not an atlas place" },
        { Station::Cochlea, { 33.0f, -22.0f, -38.0f }, "petrous temporal bone, approximate (drawn outside the head)" },
        { Station::CochlearNucleus, { 14.0f, -43.0f, -45.0f }, "pontomedullary junction, dorsolateral; approximate" },
        { Station::Mso, { 9.0f, -34.0f, -40.0f }, "superior olive, medial part; approximate" },
        { Station::Lso, { 13.0f, -35.0f, -41.0f }, "superior olive, lateral part; approximate" },
        { Station::LateralLemniscus, { 13.0f, -35.0f, -25.0f }, "nuclei of the lateral lemniscus, lateral pons; approximate" },
        { Station::InferiorColliculus, { 6.0f, -35.0f, -10.0f }, "midbrain tectum; approximate" },
        { Station::Mgn, { 15.0f, -26.0f, -6.0f }, "thalamus, medial geniculate body; approximate" },
        { Station::Heschl, { 47.0f, -20.0f, 7.0f }, "Te1.0, Morosan et al. 2001 (approximate centre)" },
        { Station::Premotor, { 30.0f, -4.0f, 58.0f }, "dorsal premotor, after Mayka et al. 2006 (approximate)" },
        { Station::Sma, { 4.0f, -7.0f, 55.0f }, "SMA proper, after Mayka et al. 2006 (approximate)" },
        { Station::Putamen, { 25.0f, 2.0f, 0.0f }, "Harvard-Oxford subcortical atlas centre (approximate)" },
        { Station::Ventrolateral, { 13.0f, -13.0f, 6.0f }, "thalamus, ventrolateral nucleus; approximate" },
        { Station::Cerebellum, { 24.0f, -64.0f, -24.0f }, "cerebellar lobule VI; approximate" },
        { Station::Ifg, { 50.0f, 14.0f, 12.0f }, "pars opercularis (BA 44); approximate" },
        { Station::Caudate, { 13.0f, 12.0f, 10.0f }, "Harvard-Oxford subcortical atlas, caudate head (approximate)" },
        { Station::Accumbens, { 10.0f, 12.0f, -8.0f }, "Harvard-Oxford subcortical atlas centre (approximate)" },
        { Station::Vta, { 0.0f, -16.0f, -12.0f }, "ventral midbrain (both sides at about x = 4); approximate" },
    };
    return table;
}

// =============================================================================
// Mapping
// =============================================================================
Vec3 BrainAnatomy::fromMni (Vec3 mni) noexcept
{
    return { mni.x / kMmPerUnit, (mni.z - 10.0f) / kMmPerUnit, (mni.y + 18.0f) / kMmPerUnit };
}

float BrainAnatomy::cochleaPlace (double hz) noexcept
{
    // Greenwood (1990), human: f = A (10^(a x) - k), x = distance from the apex (0..1).
    constexpr double kA = 165.4, kAlpha = 2.1, kK = 0.88;
    const double fromApex = std::log10 (std::max (hz, 1.0) / kA + kK) / kAlpha;
    return static_cast<float> (std::clamp (1.0 - fromApex, 0.0, 1.0));
}

float BrainAnatomy::heschlCoordinate (double hz) noexcept
{
    const double t = (std::log2 (std::max (hz, 1.0)) - std::log2 (kLowHz)) / (std::log2 (kHighHz) - std::log2 (kLowHz));
    return static_cast<float> (std::clamp (t, 0.0, 1.0));
}

float BrainAnatomy::stationDelay (Station s) noexcept
{
    switch (s)
    {
        case Station::Cochlea: return kCanalSeconds;
        case Station::CochlearNucleus: return kCanalSeconds + kSlowDown * kLatencyCn;
        case Station::Mso:
        case Station::Lso: return kCanalSeconds + kSlowDown * kLatencySoc;
        case Station::LateralLemniscus: return kCanalSeconds + kSlowDown * kLatencyNll;
        case Station::InferiorColliculus: return kCanalSeconds + kSlowDown * kLatencyIc;
        case Station::Mgn: return kCanalSeconds + kSlowDown * kLatencyMgn;
        case Station::Heschl: return kCanalSeconds + kSlowDown * kLatencyCortex;
        default: return 0.0f;
    }
}

Vec3 BrainAnatomy::cochleaPoint (int side, float place) const noexcept
{
    const float u = std::clamp (place, 0.0f, 1.0f);
    const float th = u * kSpiralTurns * 2.0f * kPi;
    const float r = kSpiralRadius * (1.0f - 0.72f * u);
    const Vec3 c = mirror (kCochleaDrawn, side);
    return { c.x, c.y + r * std::sin (th), c.z + r * std::cos (th) };
}

Vec3 BrainAnatomy::heschlPoint (int side, float t) const noexcept
{
    return lerp (fromMni (mirror (kHeschlLowMni, side)), fromMni (mirror (kHeschlHighMni, side)), std::clamp (t, 0.0f, 1.0f));
}

Vec3 BrainAnatomy::earCentre (int side) const noexcept
{
    return mirror (kEarDrawn, side);
}

// =============================================================================
// Build
// =============================================================================
const BrainAnatomy& BrainAnatomy::get()
{
    static const BrainAnatomy instance;
    return instance;
}

BrainAnatomy::BrainAnatomy()
{
    buildNodes();
    buildPoints();
    placeCorticalNodes();
    // Top to bottom: drawn in this order, consecutive points land on nearby rows of the frame (cache friendly;
    // the brain mostly turns about the vertical axis).
    std::stable_sort (points.begin(), points.end(), [] (const Point& a, const Point& b) { return a.p.y > b.p.y; });
    buildTracts();
}

void BrainAnatomy::buildNodes()
{
    struct Info
    {
        Station s;
        NodeKind kind;
        const char* name;
        const char* role;
        bool heuristic;
        float extent;
    };
    static const Info info[] = {
        { Station::Ear, NodeKind::Ear, "Ear", "Sound enters through the ear canal.", false, 0.13f },
        { Station::Cochlea, NodeKind::Cochlea, "Cochlea",
          "Splits the sound into frequencies: high notes at its base (outer turn), low notes at its apex (centre).", false, 0.08f },
        { Station::CochlearNucleus, NodeKind::Nucleus, "Cochlear nucleus",
          "First stop in the brainstem (about 2 ms): every fibre of the auditory nerve ends here, on the same side.", false, 0.05f },
        { Station::Mso, NodeKind::Nucleus, "Superior olive: MSO",
          "Medial superior olive (about 3.5 ms): compares the timing of the two ears for low frequencies (where a sound is).",
          false, 0.03f },
        { Station::Lso, NodeKind::Nucleus, "Superior olive: LSO",
          "Lateral superior olive (about 3.5 ms): compares the level of the two ears for high frequencies.", false, 0.03f },
        { Station::LateralLemniscus, NodeKind::Nucleus, "Lateral lemniscus",
          "The ascending tract to the midbrain and its nuclei (about 4.5 ms); from here on each ear feeds mostly the opposite side.",
          false, 0.03f },
        { Station::InferiorColliculus, NodeKind::Nucleus, "Inferior colliculus",
          "Midbrain hub of the hearing pathway (about 5.5 ms): every ascending route meets here.", false, 0.05f },
        { Station::Mgn, NodeKind::Nucleus, "Medial geniculate nucleus",
          "The thalamus's hearing relay (about 9 ms) to the auditory cortex.", false, 0.05f },
        { Station::Heschl, NodeKind::Heschl, "Heschl's gyrus (primary auditory cortex)",
          "Arrives about 15 ms after the ear, buried in the lateral sulcus. Low notes land at its front outer end, high notes at its "
          "back inner end. Percussive sound weighs a little more on the left, tonal sound on the right (Zatorre and Belin 2001).",
          false, 0.15f },
        { Station::Premotor, NodeKind::Cortex, "Premotor cortex",
          "Movement planning: lights on the beat, from the auditory cortex (dorsal stream) and the thalamus.", true, 0.1f },
        { Station::Sma, NodeKind::Cortex, "Supplementary motor area (SMA)",
          "Timing and the urge to move: part of the beat network (Grahn and Brett 2007).", true, 0.1f },
        { Station::Putamen, NodeKind::Nucleus, "Putamen", "Basal ganglia: keeps the beat (Grahn and Brett 2007).", true, 0.1f },
        { Station::Ventrolateral, NodeKind::Nucleus, "Ventrolateral thalamus",
          "Relays the cerebellum's timing to the premotor cortex (the tract crosses the midline).", true, 0.05f },
        { Station::Cerebellum, NodeKind::Cerebellum, "Cerebellum", "Timing: lights on the beat and sends it to the opposite thalamus.", true,
          0.15f },
        { Station::Ifg, NodeKind::Cortex, "Inferior frontal gyrus",
          "Musical syntax: a chord that does not fit the key answers here about 0.2 s later, mostly on the right (the ERAN, Koelsch).",
          true, 0.1f },
        { Station::Caudate, NodeKind::Nucleus, "Caudate",
          "Dopamine during the anticipation of a peak: lights in a build-up (Salimpoor et al. 2011).", true, 0.08f },
        { Station::Accumbens, NodeKind::Nucleus, "Nucleus accumbens",
          "Dopamine and reward at the peak: lights at the drop (Salimpoor et al. 2011, 2013).", true, 0.05f },
        { Station::Vta, NodeKind::Nucleus, "Ventral tegmental area (VTA)",
          "The midbrain's dopamine source for the caudate and the nucleus accumbens.", true, 0.06f },
    };
    const auto& table = mniTable();
    for (const auto& i : info)
    {
        Vec3 mni;
        for (const auto& e : table)
            if (e.station == i.s)
                mni = e.mni;
        for (int side = kLeft; side <= kRight; ++side)
        {
            if (i.s == Station::Vta && side == kRight)
                continue;
            auto& n = nodes[static_cast<size_t> (node (i.s, side))];
            n.station = i.s;
            n.side = side;
            n.kind = i.kind;
            n.name = i.name;
            n.role = i.role;
            n.heuristic = i.heuristic;
            n.extent = i.extent;
            n.mni = mirror (mni, side);
            n.position = fromMni (n.mni);
        }
    }
    for (int side = kLeft; side <= kRight; ++side)
    {
        nodes[static_cast<size_t> (node (Station::Ear, side))].position = earCentre (side);
        nodes[static_cast<size_t> (node (Station::Cochlea, side))].position = mirror (kCochleaDrawn, side);
        nodes[static_cast<size_t> (node (Station::Heschl, side))].position = heschlPoint (side, 0.5f);
    }
}

void BrainAnatomy::buildPoints()
{
    Rng rng;
    points.reserve (12500);
    const auto add = [this] (Vec3 p, PointKind kind, int side, int nodeIndex, float tonotopy)
    {
        Point pt;
        pt.p = p;
        pt.kind = kind;
        pt.side = static_cast<int8_t> (side);
        pt.node = static_cast<int16_t> (nodeIndex);
        pt.tonotopy = tonotopy;
        points.push_back (pt);
    };

    // Cortex: two folded ellipsoids (the approved mockup's shape), the lateral (Sylvian) and central sulci left open,
    // the medial walls drawn in towards the midline.
    for (int i = 0; i < 9000; ++i)
    {
        const int side = (i % 2) != 0 ? kRight : kLeft;
        const float s = sideSign (side);
        const auto d = rng.direction();
        if (d.x * s < -0.55f)
            continue;
        if (std::abs (d.x) > 0.42f && d.z > -0.35f && std::abs (d.y + 0.12f - 0.2f * d.z) < 0.035f)
            continue; // lateral sulcus
        if (d.y > 0.25f && std::abs (d.z - 0.05f - 0.15f * d.y) < 0.022f)
            continue; // central sulcus
        const float f = 1.0f + 0.05f * std::sin (17.0f * d.z + 5.0f * std::sin (6.0f * d.y)) * std::sin (13.0f * d.y + 4.0f * std::cos (9.0f * d.z));
        float x = s * 0.37f + 0.43f * d.x * f, y = 0.6f * d.y * f;
        const float z = 0.95f * d.z * f;
        if (y < -0.36f)
            y = -0.36f + (y + 0.36f) * 0.35f;
        if (d.x * s < -0.4f)
            x = s * 0.05f + (x - s * 0.05f) * 0.3f;
        add ({ x, y, z }, PointKind::Cortex, side, -1, 0.0f);
    }

    // Cerebellum: two folded halves under the occipital lobe (each half is its node).
    for (int i = 0; i < 900; ++i)
    {
        const auto d = rng.direction();
        const float f = 1.0f + 0.07f * std::sin (60.0f * d.y);
        const int side = d.x < 0.0f ? kLeft : kRight;
        add ({ 0.52f * d.x * f, -0.5f + 0.22f * d.y * f, -0.49f + 0.3f * d.z * f }, PointKind::Cerebellum, side,
             node (Station::Cerebellum, side), 0.0f);
    }

    // Brainstem: midbrain (top) to medulla, wider at the pons.
    const Vec3 top = fromMni ({ 0.0f, -22.0f, -2.0f }), bottom = fromMni ({ 0.0f, -44.0f, -64.0f });
    for (int i = 0; i < 420; ++i)
    {
        const float t = rng.next(), a = rng.next() * 2.0f * kPi;
        const float bulge = 1.0f + 0.25f * std::exp (-((t - 0.4f) / 0.2f) * ((t - 0.4f) / 0.2f));
        const float rx = (0.1f + 0.02f * rng.next()) * bulge, rz = (0.075f + 0.015f * rng.next()) * bulge;
        const Vec3 c = lerp (top, bottom, t);
        add ({ c.x + rx * std::cos (a), c.y, c.z + rz * std::sin (a) }, PointKind::Brainstem, 0, -1, 0.0f);
    }

    // Heschl's gyrus: a strip along its axis, each point with its tonotopic coordinate.
    for (int side = kLeft; side <= kRight; ++side)
        for (int i = 0; i < 560; ++i)
        {
            const float u = rng.next();
            const Vec3 p = heschlPoint (side, u);
            add ({ p.x + (rng.next() - 0.5f) * 0.06f, p.y + (rng.next() - 0.5f) * 0.035f, p.z + (rng.next() - 0.5f) * 0.07f }, PointKind::Heschl,
                 side, node (Station::Heschl, side), u);
        }

    // Nuclei: one blob each, denser in the middle.
    struct Blob
    {
        Station s;
        Vec3 radius;
        int count;
        PointKind kind;
    };
    static const Blob blobs[] = {
        { Station::CochlearNucleus, { 0.045f, 0.04f, 0.05f }, 90, PointKind::Nucleus },
        { Station::Mso, { 0.022f, 0.02f, 0.035f }, 50, PointKind::Nucleus },
        { Station::Lso, { 0.022f, 0.02f, 0.035f }, 50, PointKind::Nucleus },
        { Station::LateralLemniscus, { 0.02f, 0.04f, 0.025f }, 40, PointKind::Nucleus },
        { Station::InferiorColliculus, { 0.045f, 0.035f, 0.04f }, 90, PointKind::Nucleus },
        { Station::Mgn, { 0.04f, 0.035f, 0.045f }, 90, PointKind::Nucleus },
        { Station::Ventrolateral, { 0.045f, 0.04f, 0.05f }, 80, PointKind::Nucleus },
        { Station::Putamen, { 0.055f, 0.085f, 0.13f }, 200, PointKind::Putamen },
        { Station::Caudate, { 0.045f, 0.055f, 0.12f }, 160, PointKind::Caudate },
        { Station::Accumbens, { 0.045f, 0.04f, 0.05f }, 120, PointKind::Limbic },
        { Station::Vta, { 0.06f, 0.03f, 0.045f }, 100, PointKind::Limbic },
    };
    for (const auto& b : blobs)
        for (int side = kLeft; side <= kRight; ++side)
        {
            if (b.s == Station::Vta && side == kRight)
                continue;
            const int n = node (b.s, side);
            const Vec3 c = nodes[static_cast<size_t> (n)].position;
            for (int i = 0; i < b.count; ++i)
            {
                const auto d = rng.direction();
                const float q = std::cbrt (rng.next());
                add ({ c.x + b.radius.x * d.x * q, c.y + b.radius.y * d.y * q, c.z + b.radius.z * d.z * q }, b.kind, side, n, 0.0f);
            }
        }
}

void BrainAnatomy::placeCorticalNodes()
{
    // Premotor, SMA and IFG sit on the drawn cortex: the mean of the 8 cortex points nearest their coordinate.
    for (const Station s : { Station::Premotor, Station::Sma, Station::Ifg })
        for (int side = kLeft; side <= kRight; ++side)
        {
            auto& n = nodes[static_cast<size_t> (node (s, side))];
            const Vec3 target = fromMni (n.mni);
            std::array<std::pair<float, size_t>, 8> best;
            best.fill ({ 1.0e9f, 0 });
            for (size_t i = 0; i < points.size(); ++i)
            {
                const auto& p = points[i];
                if (p.kind != PointKind::Cortex || p.side != side)
                    continue;
                const float d = length (p.p - target);
                if (d < best.back().first)
                {
                    best.back() = { d, i };
                    std::sort (best.begin(), best.end());
                }
            }
            Vec3 sum;
            for (const auto& b : best)
                sum = sum + points[b.second].p;
            n.position = sum * (1.0f / static_cast<float> (best.size()));
        }

    // Cortex points near those patches belong to them (the closest patch, if it is near enough).
    for (auto& p : points)
    {
        if (p.kind != PointKind::Cortex)
            continue;
        float bestWeight = 0.0f;
        int bestNode = -1;
        for (const Station s : { Station::Premotor, Station::Sma, Station::Ifg })
        {
            const int n = node (s, p.side);
            const Vec3 d = p.p - nodes[static_cast<size_t> (n)].position;
            const float w = std::exp (-(d.x * d.x + d.y * d.y + d.z * d.z) / (s == Station::Ifg ? 0.014f : 0.02f));
            if (w > bestWeight)
            {
                bestWeight = w;
                bestNode = n;
            }
        }
        if (bestWeight >= 0.35f)
            p.node = static_cast<int16_t> (bestNode);
    }
}

void BrainAnatomy::addTract (const char* name, TractKind kind, int from, int to, int side, std::vector<Vec3> waypoints, Channel channel,
                             int channelSide, float weight, float radius)
{
    Tract t;
    t.name = name;
    t.kind = kind;
    t.from = from;
    t.to = to;
    t.side = side;
    t.channel = channel;
    t.channelSide = channelSide;
    t.weight = weight;
    t.radius = radius;
    t.firstPoint = static_cast<int> (tractPoints.size());
    if (kind == TractKind::Auditory)
    {
        const auto& a = nodes[static_cast<size_t> (from)];
        const auto& b = nodes[static_cast<size_t> (to)];
        t.delayStart = a.station == Station::Ear ? 0.0f : stationDelay (a.station);
        t.delayEnd = stationDelay (b.station);
    }
    sampleCurve (waypoints, kSegments + 1, tractPoints);
    tracts.push_back (t);
}

int BrainAnatomy::findTract (const char* name) const noexcept
{
    for (size_t i = 0; i < tracts.size(); ++i)
        if (std::strcmp (tracts[i].name, name) == 0)
            return static_cast<int> (i);
    return -1;
}

void BrainAnatomy::buildTracts()
{
    // Cochlea spirals (base first) and ear rings.
    for (int side = kLeft; side <= kRight; ++side)
    {
        auto& spiral = spirals[static_cast<size_t> (side)];
        for (int i = 0; i < kSpiralPoints; ++i)
            spiral.push_back (cochleaPoint (side, spiralPlace (i)));
        auto& ring = rings[static_cast<size_t> (side)];
        const Vec3 c = earCentre (side);
        for (int i = 0; i < kRingPoints; ++i)
        {
            const float a = 2.0f * kPi * static_cast<float> (i) / static_cast<float> (kRingPoints);
            ring.push_back ({ c.x, c.y + 0.13f * std::sin (a), c.z + 0.13f * std::cos (a) });
        }
    }

    const auto pos = [this] (Station s, int side) { return nodes[static_cast<size_t> (node (s, side))].position; };
    const auto at = [] (float x, float y, float z, int side) { return Vec3 { x * sideSign (side), y, z }; };
    static const char* const names[2][21] = {
        { "canal-L", "nerve-L", "cn-mso-ipsi-L", "cn-mso-contra-L", "cn-lso-ipsi-L", "cn-lso-contra-L", "mso-nll-L", "lso-nll-L", "nll-ic-L",
          "ic-mgn-L", "radiation-L", "pons-cerebellum-L", "cerebellum-thalamus-L", "thalamus-premotor-L", "dorsal-L", "premotor-sma-L",
          "sma-putamen-L", "ventral-L", "heschl-accumbens-L", "vta-accumbens-L", "vta-caudate-L" },
        { "canal-R", "nerve-R", "cn-mso-ipsi-R", "cn-mso-contra-R", "cn-lso-ipsi-R", "cn-lso-contra-R", "mso-nll-R", "lso-nll-R", "nll-ic-R",
          "ic-mgn-R", "radiation-R", "pons-cerebellum-R", "cerebellum-thalamus-R", "thalamus-premotor-R", "dorsal-R", "premotor-sma-R",
          "sma-putamen-R", "ventral-R", "heschl-accumbens-R", "vta-accumbens-R", "vta-caudate-R" },
    };
    for (int s = kLeft; s <= kRight; ++s)
    {
        const int o = 1 - s;
        const auto* nm = names[s];
        const auto N = [s] (Station st) { return node (st, s); };
        const Vec3 cn = pos (Station::CochlearNucleus, s);

        // ---- Ascending pathway ----------------------------------------------------------
        addTract (nm[0], TractKind::Auditory, N (Station::Ear), N (Station::Cochlea), s,
                  { earCentre (s), at (1.12f, -0.15f, 0.04f, s), cochleaPoint (s, 0.0f) }, Channel::EarAll, s, 1.0f);
        addTract (nm[1], TractKind::Auditory, N (Station::Cochlea), N (Station::CochlearNucleus), s,
                  { pos (Station::Cochlea, s), at (0.7f, -0.4f, -0.08f, s), at (0.42f, -0.53f, -0.2f, s), cn }, Channel::EarAll, s, 1.0f);
        // Each cochlear nucleus feeds both olives (low bands -> MSO, high -> LSO): 35 % on its own side, 65 % across
        // (the trapezoid body); above the olive each side so carries mostly the other ear.
        const Vec3 crossing { 0.0f, -0.6f, -0.17f };
        addTract (nm[2], TractKind::Auditory, N (Station::CochlearNucleus), N (Station::Mso), s,
                  { cn, lerp (cn, pos (Station::Mso, s), 0.5f) + Vec3 { 0.0f, -0.03f, 0.03f }, pos (Station::Mso, s) }, Channel::EarLow, s,
                  kIpsilateral, 0.0045f);
        addTract (nm[3], TractKind::Auditory, N (Station::CochlearNucleus), node (Station::Mso, o), s,
                  { cn, crossing + Vec3 { 0.0f, 0.0f, 0.01f }, pos (Station::Mso, o) }, Channel::EarLow, s, kContralateral, 0.0045f);
        addTract (nm[4], TractKind::Auditory, N (Station::CochlearNucleus), N (Station::Lso), s,
                  { cn, lerp (cn, pos (Station::Lso, s), 0.5f) + Vec3 { 0.0f, -0.02f, 0.02f }, pos (Station::Lso, s) }, Channel::EarHigh, s,
                  kIpsilateral, 0.0045f);
        addTract (nm[5], TractKind::Auditory, N (Station::CochlearNucleus), node (Station::Lso, o), s,
                  { cn, crossing + Vec3 { 0.0f, -0.015f, 0.0f }, pos (Station::Lso, o) }, Channel::EarHigh, s, kContralateral, 0.0045f);
        const Vec3 nll = pos (Station::LateralLemniscus, s);
        addTract (nm[6], TractKind::Auditory, N (Station::Mso), N (Station::LateralLemniscus), s,
                  { pos (Station::Mso, s), at (0.12f, -0.48f, -0.17f, s), nll }, Channel::AscendLow, s, 1.0f, 0.005f);
        addTract (nm[7], TractKind::Auditory, N (Station::Lso), N (Station::LateralLemniscus), s,
                  { pos (Station::Lso, s), at (0.155f, -0.48f, -0.2f, s), nll }, Channel::AscendHigh, s, 1.0f, 0.005f);
        addTract (nm[8], TractKind::Auditory, N (Station::LateralLemniscus), N (Station::InferiorColliculus), s,
                  { nll, at (0.11f, -0.3f, -0.2f, s), pos (Station::InferiorColliculus, s) }, Channel::AscendAll, s, 1.0f);
        addTract (nm[9], TractKind::Auditory, N (Station::InferiorColliculus), N (Station::Mgn), s,
                  { pos (Station::InferiorColliculus, s), at (0.13f, -0.2f, -0.15f, s), pos (Station::Mgn, s) }, Channel::AscendAll, s, 1.0f);
        addTract (nm[10], TractKind::Auditory, N (Station::Mgn), N (Station::Heschl), s,
                  { pos (Station::Mgn, s), at (0.3f, -0.15f, -0.06f, s), pos (Station::Heschl, s) }, Channel::Radiation, s, 1.0f, 0.0075f);

        // ---- Beat network (Grahn and Brett 2007) ---------------------------------------
        const Vec3 cer = pos (Station::Cerebellum, s);
        addTract (nm[11], TractKind::Beat, N (Station::InferiorColliculus), N (Station::Cerebellum), s,
                  { pos (Station::InferiorColliculus, s), at (0.1f, -0.35f, -0.3f, s), at (0.18f, -0.4f, -0.42f, s), cer });
        // Cerebellum -> the opposite ventrolateral thalamus (crossing in the midbrain).
        addTract (nm[12], TractKind::Beat, N (Station::Cerebellum), node (Station::Ventrolateral, o), s,
                  { cer, at (0.1f, -0.33f, -0.33f, s), Vec3 { 0.0f, -0.22f, -0.2f }, at (-0.06f, -0.12f, -0.06f, s), pos (Station::Ventrolateral, o) });
        addTract (nm[13], TractKind::Beat, N (Station::Ventrolateral), N (Station::Premotor), s,
                  { pos (Station::Ventrolateral, s), at (0.25f, 0.2f, 0.08f, s), pos (Station::Premotor, s) });
        addTract (nm[14], TractKind::Beat, N (Station::Heschl), N (Station::Premotor), s,
                  { heschlPoint (s, 1.0f), at (0.5f, 0.15f, -0.12f, s), at (0.45f, 0.35f, 0.02f, s), pos (Station::Premotor, s) });
        addTract (nm[15], TractKind::Beat, N (Station::Premotor), N (Station::Sma), s,
                  { pos (Station::Premotor, s), at (0.2f, 0.55f, 0.15f, s), pos (Station::Sma, s) });
        addTract (nm[16], TractKind::Beat, N (Station::Sma), N (Station::Putamen), s,
                  { pos (Station::Sma, s), at (0.15f, 0.25f, 0.18f, s), pos (Station::Putamen, s) });

        // ---- Chord surprise: the ventral stream to the inferior frontal gyrus -------------
        addTract (nm[17], TractKind::Chord, N (Station::Heschl), N (Station::Ifg), s,
                  { heschlPoint (s, 0.0f), at (0.62f, -0.12f, 0.25f, s), pos (Station::Ifg, s) });

        // ---- Reward (Salimpoor et al. 2011, 2013) ----------------------------------------
        addTract (nm[18], TractKind::Dopamine, N (Station::Heschl), N (Station::Accumbens), s,
                  { heschlPoint (s, 0.3f), at (0.3f, -0.2f, 0.2f, s), pos (Station::Accumbens, s) });
        const Vec3 vta = pos (Station::Vta, s);
        addTract (nm[19], TractKind::Dopamine, node (Station::Vta, s), N (Station::Accumbens), s,
                  { vta, at (0.05f, -0.25f, 0.18f, s), pos (Station::Accumbens, s) });
        addTract (nm[20], TractKind::Dopamine, node (Station::Vta, s), N (Station::Caudate), s,
                  { vta, at (0.08f, -0.1f, 0.12f, s), pos (Station::Caudate, s) });
    }
}
} // namespace flub::app::ui::vis::brain
