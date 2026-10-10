// Flubsound Pro - the brain visualiser's activity model (visualiser "brain", docs/06 §6.4.2).
//
// What lights where and when, from what BrainListener heard:
//
//   * Activity is "dB above a floor" scaled to 0..1 (x = (dB - floor) / 60 dB,
//     up to 1.3), roughly how the auditory population's firing grows with the
//     level in dB. The display intensity of anything is
//     brightness (x) = x^1.2 (soft, so loud and very loud stay apart; colours
//     never turn white: a hue at full brightness).
//   * Ascending pathway: every analysis hop (BrainListener::HopFrame) goes into
//     a history ring. A tract carries one channel (an ear's activity, all /
//     low / high bands; the MSO: 0.5 x each ear's low bands; the LSO: its own
//     ear's high bands less 0.5 x the other ear's; from the lateral lemniscus
//     up 0.35 x the same-side ear + 0.65 x the other ear; the radiation to the
//     cortex with percussive onsets +15 % on the left and tonal sound +15 % on
//     the right, a simplification of Zatorre and Belin 2001).
//     Segment j of a tract shows what entered the pathway delayStart +
//     (delayEnd - delayStart) x j / kSegments seconds ago (display time, 20 x
//     the real latency): an onset runs along it as a bright segment and leaves
//     a fading trail (kTrailSeconds). Onsets count fully, sustained sound pulses
//     every kPulseSeconds at kPulse x its level and keeps a floor of kFloor x
//     its level, so a held tone keeps its path lit with repeated weaker
//     activity. Landing spots (the nuclei, Heschl's gyrus per band, the
//     cochlea per band) take the onsets and kSteady x the level at their own
//     delay and hold it (kRegionSeconds).
//   * Beyond hearing: detected events (beat, chord change, build-up, drop)
//     start waves on their tracts at fixed delays (the mockup's): a wave's head
//     runs from the start to the end in its duration with a trail behind it,
//     then fades (kWaveFadeSeconds); on arrival it lights the target.
//   * Held activity that decays under 1e-4 is set to 0, so a faded brain is
//     exactly dark (isDark(): the view then stops rendering while it is still).
// Message thread only; no allocation after construction.
#pragma once

#include "BrainAnatomy.h"

#include <array>
#include <vector>

namespace flub::app::ui::vis::brain
{
/** One analysis hop: per ear, what the bands held (activity x and the colour of what made it). */
struct HopFrame
{
    struct Part
    {
        float x = 0.0f;               // activity (0 .. 1.3)
        float r = 0.0f, g = 0.0f, b = 0.0f; // colour (0..1, the activity-weighted mean of the bands' colours)
    };
    enum Class
    {
        OnsetTonal,
        OnsetPercussive,
        Level,
        kClasses
    };
    enum Group
    {
        All,
        Low,
        High,
        kGroups
    };
    double time = 0.0; // display seconds (when the sound reached the ear)
    std::array<std::array<std::array<Part, kGroups>, kClasses>, 2> ear {};       // [ear][class][group]
    std::array<std::array<std::array<float, kBands>, kClasses>, 2> band {};      // [ear][class][band]: activity
};

class BrainActivity
{
public:
    static constexpr int kHistory = 256;
    static constexpr int kMaxWaves = 384;
    static constexpr float kSteady = 0.75f, kPulse = 0.7f, kFloor = 0.35f;
    static constexpr double kPulseSeconds = 0.125;
    static constexpr float kTrailSeconds = 0.18f, kRegionSeconds = 0.35f, kHeschlSeconds = 0.2f, kCochleaSeconds = 0.15f;
    static constexpr float kWaveFadeSeconds = 0.18f, kWaveTrail = 0.22f;
    static constexpr float kPercussiveLeft = 1.15f, kTonalRight = 1.15f, kOtherSide = 0.85f;

    /** Event colours (0..1 RGB): beat purple #7F77DD, chord surprise teal #1D9E75, dopamine pink #D4537E. */
    static constexpr std::array<float, 3> kBeatRgb { 127.0f / 255.0f, 119.0f / 255.0f, 221.0f / 255.0f };
    static constexpr std::array<float, 3> kChordRgb { 29.0f / 255.0f, 158.0f / 255.0f, 117.0f / 255.0f };
    static constexpr std::array<float, 3> kDopamineRgb { 212.0f / 255.0f, 83.0f / 255.0f, 126.0f / 255.0f };

    /** A display state: intensity 0 .. ~1.37 (brightness and size) and colour (0..1, max channel 1 when lit). */
    struct Glow
    {
        float level = 0.0f;
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };

    explicit BrainActivity (const BrainAnatomy& anatomy = BrainAnatomy::get());

    void reset() noexcept;

    // ---- Input (in time order) -----------------------------------------------------------
    void addHop (const HopFrame& frame) noexcept;
    /** A beat onset (kick or snare) at `time` with strength 0..1. */
    void beat (double time, float strength) noexcept;
    /** A chord change: the ERAN-like response on the right and left inferior frontal gyrus (0..1 each). */
    void chord (double time, float right, float left) noexcept;
    /** A dopamine pulse of anticipation (build-up): VTA -> caudate, progress 0..1. */
    void anticipation (double time, float progress) noexcept;
    /** The drop: VTA -> nucleus accumbens, auditory cortex -> nucleus accumbens. */
    void drop (double time) noexcept;

    /** Once per display frame: everything up to `now` (display seconds), `dt` since the last frame. */
    void update (double now, double dt) noexcept;

    // ---- Output ---------------------------------------------------------------------------
    const Glow& segment (int tract, int j) const noexcept
    {
        return segments[static_cast<size_t> (tract * (BrainAnatomy::kSegments + 1) + j)];
    }
    const Glow& glow (int nodeIndex) const noexcept { return nodeGlow[static_cast<size_t> (nodeIndex)]; }
    /** Heschl's gyrus, band b on a side (intensity) and its held activity. */
    float heschl (int side, int band) const noexcept { return brightness (hgHeld[static_cast<size_t> (side)][static_cast<size_t> (band)]); }
    /** The cochlea, band b of an ear (intensity). */
    float cochlea (int side, int band) const noexcept { return brightness (cochHeld[static_cast<size_t> (side)][static_cast<size_t> (band)]); }
    /** The ear ring's intensity (what enters the ear now). */
    float ear (int side) const noexcept { return brightness (earHeld[static_cast<size_t> (side)]); }
    /** Strongest Heschl band on a side (intensity). */
    float heschlPeak (int side) const noexcept;

    /** Display intensity of an activity x. */
    static float brightness (float x) noexcept;
    /** Nothing is lit (every tract, spot, Heschl band and cochlea band at exactly 0; held activity under 1e-4 counts
        as gone) as of the last update. */
    bool isDark() const noexcept { return dark; }

    // ---- Counters (tests, the status line) --------------------------------------------------
    int getBeats() const noexcept { return beats; }
    int getChordChanges() const noexcept { return chords; }
    int getSurprises() const noexcept { return surprises; }
    int getAnticipations() const noexcept { return anticipations; }
    int getDrops() const noexcept { return drops; }
    double getLastBeatTime() const noexcept { return lastBeat; }
    double getLastDropTime() const noexcept { return lastDrop; }
    double getLastAnticipationTime() const noexcept { return lastAnticipation; }
    double getLastSurpriseTime() const noexcept { return lastSurprise; }
    double getNow() const noexcept { return now; }
    int getActiveWaves() const noexcept;

private:
    struct Drive
    {
        float tract = 0.0f, region = 0.0f;
        float r = 0.0f, g = 0.0f, b = 0.0f;
    };
    static constexpr int kChannels = 2 * kNumChannelKinds;
    struct Record
    {
        double time = 0.0;
        std::array<Drive, kChannels> channel {};
        std::array<std::array<float, kBands>, 2> hg {}, coch {};
    };
    struct Wave
    {
        bool active = false, hit = false;
        int tract = 0;
        double start = 0.0;
        float duration = 0.1f, amplitude = 0.0f;
        std::array<float, 3> rgb {};
    };
    struct Held
    {
        float x = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f;
        void take (float value, float cr, float cg, float cb) noexcept
        {
            if (value > x && value > 0.0f)
            {
                x = value;
                r = cr;
                g = cg;
                b = cb;
            }
        }
    };

    static int channelIndex (Channel c, int side) noexcept { return 2 * static_cast<int> (c) + side; }
    /** Logical index (0 oldest) of the first record later than t. */
    int firstAfter (double t) const noexcept;
    const Record& record (int logical) const noexcept
    {
        return history[static_cast<size_t> ((oldest + logical) % kHistory)];
    }
    void addWave (int tract, double start, float duration, float amplitude, const std::array<float, 3>& rgb) noexcept;
    void hitNode (int nodeIndex, float amplitude, const std::array<float, 3>& rgb) noexcept;

    const BrainAnatomy& anatomy;
    std::vector<Record> history;
    int oldest = 0, count = 0;
    double lastPulseSlot = -1.0;
    std::vector<Held> segmentHeld;  // auditory tracts' segments
    std::vector<Glow> segments;     // every tract's display
    std::vector<std::array<float, 4>> eventAccum; // event tracts: per segment RGB + amplitude
    std::array<Held, kNumNodes> nodeHeld {};
    std::array<Glow, kNumNodes> nodeGlow {};
    std::array<std::array<float, kBands>, 2> hgHeld {}, cochHeld {};
    std::array<float, 2> earHeld {};
    std::array<Wave, kMaxWaves> waves {};
    int nextWave = 0;
    double now = 0.0, previous = 0.0;
    bool started = false, dark = true;

    // Tract indices of the event networks, per side.
    struct EventTracts
    {
        int pons = -1, cerebellumThalamus = -1, thalamusPremotor = -1, dorsal = -1, premotorSma = -1, smaPutamen = -1, ventral = -1,
            heschlAccumbens = -1, vtaAccumbens = -1, vtaCaudate = -1;
    };
    std::array<EventTracts, 2> event {};

    int beats = 0, chords = 0, surprises = 0, anticipations = 0, drops = 0;
    double lastBeat = -1.0e9, lastDrop = -1.0e9, lastAnticipation = -1.0e9, lastSurprise = -1.0e9;
};
} // namespace flub::app::ui::vis::brain
