// Flubsound Pro - the brain visualiser's anatomy (visualiser "brain", docs/06 §6.4.2).
//
// A fixed, approximate model of the human auditory pathway and the areas music
// reaches beyond it, built once (deterministic) and shared by every BrainView:
//
//   * Stations (nodes): the ear and cochlea, cochlear nucleus, superior olive
//     (MSO and LSO), the nuclei of the lateral lemniscus, inferior colliculus,
//     medial geniculate nucleus, Heschl's gyrus (primary auditory cortex),
//     premotor cortex, SMA, putamen, ventrolateral thalamus, cerebellum,
//     inferior frontal gyrus, caudate, nucleus accumbens (each on both sides)
//     and the VTA (midline). Positions come from approximate MNI coordinates
//     (kMni in BrainAnatomy.cpp, the table in docs/06 §6.4.2) mapped to the
//     view's axes by fromMni: x lateral (+ right), y up, z front, 90 mm per
//     unit. The ear and cochlea are drawn outside the head (their real place
//     in the petrous bone would be hidden by the temporal lobe); cortical
//     areas sit on the drawn cortex nearest their coordinate.
//   * Tracts: polylines (kSegments segments each, Catmull-Rom through their
//     waypoints) from one station to another, the auditory ones with the
//     display delays (seconds, 20 x the real latency) at their two ends.
//   * The point cloud: two folded hemispheres with the lateral and central
//     sulci left open, the cerebellum, the brainstem, Heschl's gyrus (each
//     point with its tonotopic coordinate) and one blob per nucleus.
//   * Tonotopic maps: cochleaPlace (Greenwood 1990: the base answers high
//     frequencies, the apex low) and heschlCoordinate (low anterolateral,
//     high posteromedial along Heschl's gyrus).
// Pure data and geometry, no JUCE graphics. Any thread once built.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace flub::app::ui::vis::brain
{
struct Vec3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 operator+ (Vec3 a, Vec3 b) noexcept;
Vec3 operator- (Vec3 a, Vec3 b) noexcept;
Vec3 operator* (Vec3 a, float s) noexcept;
float length (Vec3 v) noexcept;
Vec3 lerp (Vec3 a, Vec3 b, float t) noexcept;

constexpr int kLeft = 0, kRight = 1;

/** Frequency analysis shared with the activity model: kBands log-spaced bands, kLowHz .. kHighHz. */
constexpr int kBands = 32;
constexpr double kLowHz = 30.0, kHighHz = 16000.0;
/** Centre frequency of band b. */
double bandHz (int band) noexcept;
/** The signal colour of a frequency (HSL hue 0 (red) at 30 Hz .. 0.52 (cyan) at 16 kHz on a log axis, s 0.95, l 0.58), 0..1 RGB. */
std::array<float, 3> frequencyRgb (double hz) noexcept;

/** The kinds of station; a node is a station on one side (the VTA is one midline node). */
enum class Station : uint8_t
{
    Ear,
    Cochlea,
    CochlearNucleus,
    Mso,
    Lso,
    LateralLemniscus,
    InferiorColliculus,
    Mgn,
    Heschl,
    Premotor,
    Sma,
    Putamen,
    Ventrolateral,
    Cerebellum,
    Ifg,
    Caudate,
    Accumbens,
    Vta,
    Count
};

constexpr int kNumStations = static_cast<int> (Station::Count);
constexpr int kNumNodes = 2 * (kNumStations - 1) + 1; // the VTA once

/** Node index of a station on a side (the VTA ignores the side). */
constexpr int node (Station s, int side) noexcept
{
    return s == Station::Vta ? kNumNodes - 1 : 2 * static_cast<int> (s) + (side == kRight ? 1 : 0);
}

/** What lights a node's points. */
enum class NodeKind : uint8_t
{
    Ear,       // drawn as a ring
    Cochlea,   // drawn as a spiral (its own tonotopic display)
    Nucleus,   // a blob of points
    Cortex,    // a patch of the drawn cortex
    Heschl,    // the tonotopic strip
    Cerebellum // the cerebellar hemisphere
};

struct Node
{
    Station station = Station::Ear;
    int side = kLeft;              // kLeft / kRight; the VTA is kLeft
    NodeKind kind = NodeKind::Nucleus;
    const char* name = "";         // "Cochlear nucleus"
    const char* role = "";         // what it does here (tooltip)
    bool heuristic = false;        // lit by a detected musical feature (beat, chord, build-up / drop)
    Vec3 mni;                      // approximate MNI (mm), real place (kMni)
    Vec3 position;                 // where it is drawn (view units)
    float extent = 0.05f;          // drawn size (radius, view units): tracts start / end within it
};

/** What a tract carries. */
enum class TractKind : uint8_t
{
    Auditory, // the band activity, delayed along the ascending pathway
    Beat,     // beat onsets (Grahn and Brett 2007)
    Chord,    // chord changes, the ERAN (Koelsch)
    Dopamine  // build-up and drop (Salimpoor et al. 2011, 2013)
};

/** The activity an auditory tract carries (BrainActivity's channels). */
enum class Channel : uint8_t
{
    EarAll,      // one ear, all bands (side = the ear)
    EarLow,      // one ear, the bands below kLowHighSplitHz (-> MSO)
    EarHigh,     // one ear, the bands from kLowHighSplitHz (-> LSO)
    AscendAll,   // one side above the olive: 0.35 x the same-side ear + 0.65 x the other ear
    AscendLow,
    AscendHigh,
    Radiation,   // AscendAll with the hemispheric weighting (Zatorre and Belin 2001)
    None         // not auditory
};
constexpr int kNumChannelKinds = 7;
constexpr double kLowHighSplitHz = 1500.0;
constexpr float kIpsilateral = 0.35f, kContralateral = 0.65f;

struct Tract
{
    const char* name = "";
    TractKind kind = TractKind::Auditory;
    int from = 0, to = 0;   // node indices
    int side = kLeft;       // the side it belongs to (its from side; the midline VTA's tracts: their target side)
    Channel channel = Channel::None;
    int channelSide = kLeft;
    float weight = 1.0f;    // activity scale (the 35 / 65 split)
    float delayStart = 0.0f, delayEnd = 0.0f; // display seconds after the sound reaches the ear (auditory only)
    float radius = 0.0065f; // tube radius (view units)
    int firstPoint = 0;     // into BrainAnatomy::tractPoints: kSegments + 1 points
};

enum class PointKind : uint8_t
{
    Cortex,
    Cerebellum,
    Brainstem,
    Heschl,
    Nucleus,
    Putamen,
    Caudate,
    Limbic // accumbens, VTA
};

struct Point
{
    Vec3 p;
    PointKind kind = PointKind::Cortex;
    int8_t side = 0;
    int16_t node = -1;   // the landing spot it belongs to (-1: none)
    float tonotopy = 0.0f; // Heschl points: 0 low .. 1 high
};

class BrainAnatomy
{
public:
    static constexpr int kSegments = 48;
    static constexpr int kSpiralPoints = 91;
    static constexpr int kRingPoints = 48;
    static constexpr float kMmPerUnit = 90.0f;
    /** The display slow-down of the ascending pathway (real latency x kSlowDown). */
    static constexpr float kSlowDown = 20.0f;
    /** Real latencies after the sound reaches the cochlea (s): CN, SOC, NLL, IC, MGN, cortex. */
    static constexpr float kLatencyCn = 0.002f, kLatencySoc = 0.0035f, kLatencyNll = 0.0045f, kLatencyIc = 0.0055f, kLatencyMgn = 0.009f,
                           kLatencyCortex = 0.015f;
    /** Display lead-in for the ear canal (the sound is drawn entering the cochlea). */
    static constexpr float kCanalSeconds = 0.02f;

    /** The shared instance (built on first use). */
    static const BrainAnatomy& get();

    BrainAnatomy();

    // ---- Mapping ----------------------------------------------------------------------
    /** MNI (mm) to the view's axes: x = x / 90, y = (z - 10) / 90, z = (y + 18) / 90. */
    static Vec3 fromMni (Vec3 mni) noexcept;
    /** Place on the basilar membrane for a frequency (Greenwood 1990, human: f = 165.4 (10^(2.1 x) - 0.88),
        x from the apex): 0 at the base (high frequencies) .. 1 at the apex (low). */
    static float cochleaPlace (double hz) noexcept;
    /** Position along Heschl's gyrus for a frequency: 0 (30 Hz, anterolateral) .. 1 (16 kHz, posteromedial). */
    static float heschlCoordinate (double hz) noexcept;
    /** Display delay (s) at which the sound reaches a station (auditory stations; 0 for others). */
    static float stationDelay (Station s) noexcept;

    /** A point on the drawn cochlea's spiral: place 0 = base (outer turn) .. 1 = apex (centre). */
    Vec3 cochleaPoint (int side, float place) const noexcept;
    /** A point on Heschl's gyrus: t 0 = the low-frequency (anterolateral) end .. 1 = the high (posteromedial) end. */
    Vec3 heschlPoint (int side, float t) const noexcept;
    Vec3 earCentre (int side) const noexcept;

    // ---- Data -------------------------------------------------------------------------
    const std::array<Node, kNumNodes>& getNodes() const noexcept { return nodes; }
    const Node& getNode (int index) const noexcept { return nodes[static_cast<size_t> (index)]; }
    const std::vector<Tract>& getTracts() const noexcept { return tracts; }
    const std::vector<Vec3>& getTractPoints() const noexcept { return tractPoints; }
    /** The index of the tract with this name, or -1. */
    int findTract (const char* name) const noexcept;
    const std::vector<Point>& getPoints() const noexcept { return points; }
    /** The cochlea spirals (kSpiralPoints each, base first) and the ear rings (kRingPoints each, closed). */
    const std::vector<Vec3>& getSpiral (int side) const noexcept { return spirals[static_cast<size_t> (side)]; }
    const std::vector<Vec3>& getRing (int side) const noexcept { return rings[static_cast<size_t> (side)]; }
    /** Place (0 base .. 1 apex) of spiral point i. */
    static float spiralPlace (int i) noexcept { return static_cast<float> (i) / static_cast<float> (kSpiralPoints - 1); }

    /** The approximate MNI table (right side; x is mirrored for the left). */
    struct MniEntry
    {
        Station station;
        Vec3 mni;
        const char* source;
    };
    static const std::vector<MniEntry>& mniTable();

private:
    void buildNodes();
    void buildPoints();
    void placeCorticalNodes();
    void buildTracts();
    void addTract (const char* name, TractKind kind, int from, int to, int side, std::vector<Vec3> waypoints, Channel channel = Channel::None,
                   int channelSide = kLeft, float weight = 1.0f, float radius = 0.0065f);

    std::array<Node, kNumNodes> nodes {};
    std::vector<Tract> tracts;
    std::vector<Vec3> tractPoints;
    std::vector<Point> points;
    std::array<std::vector<Vec3>, 2> spirals, rings;
};
} // namespace flub::app::ui::vis::brain
