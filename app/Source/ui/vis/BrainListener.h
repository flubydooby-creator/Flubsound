// Flubsound Pro - what the brain visualiser hears (visualiser "brain", docs/06 §6.4.2).
//
// From the post tap's aligned mid / side pairs (L = mid + side, R = mid - side):
//
//   * Bands: every hop (512 samples, 10.7 ms at 48 kHz; the sizes scale with
//     the rate) one complex FFT of L + iR (two real spectra at once) for each
//     of two Hann windows centred on the same instant: 4096 samples (85 ms) for
//     the bands under kSplitHz, 1024 (21 ms) above. kBands log-spaced bands per
//     ear, 30 Hz .. 16 kHz; a sine of amplitude a reads 20 log10 a dB in its
//     band. Per band and ear: the level (instant attack, 80 ms release) and an
//     onset detector (the band's power kOnsetDb above its mean of the previous
//     hops, at least kOnsetFloorDb, 80 ms refractory). An onset is percussive
//     when at least kBroadBands bands of that ear start within three hops (a
//     broadband hit), else tonal. Activity: x = (dB - floor) / 60 dB (a band:
//     floor -72 dBFS; an ear's summed power: -66 dBFS), what BrainActivity draws.
//     A hop's time is its windows' centre in display time (the newest sample is
//     "now"; getLatency() says how far behind the display runs).
//   * Beat (heuristic), from the short window, L + R: a candidate is a 40 -
//     150 Hz rise of kKickRiseDb (the low band within 12 dB of the whole) or a
//     noisy (spectral flatness kSnareFlatness) 1 - 4 kHz rise of kSnareRiseDb
//     with a 150 - 500 Hz rise; it is decided kLookahead hops (43 ms) later: a
//     kick when the low band's spectral centroid has sunk smoothly to kSweep of
//     where it started (a kick drum's falling pitch; a bass note's centroid
//     stays or jitters), a snare when the 1 - 4 kHz band is still noise (an
//     onset from silence is flat only for a moment). 150 ms refractory.
//     Hi-hats (nothing under 1 kHz), pads and bass notes do not count. A
//     snare-like hit is a beat only when it repeats like a drum beat (owner
//     decision 2026-10-10: gunshots and bursts in a game read as snares): its
//     gap to the previous snare-like hit and the gap before that are both
//     0.2 - 2.3 s and match within 8 % (so a backbeat counts from its third
//     snare); or nothing snare-like came since the last counted snare and it
//     lands one or two of that pattern's gaps after it (one missed snare); or
//     it lands one beat (within 8 %) after the last of three counted beats
//     whose two gaps match (a snare on a steady kick's grid). Single shots at
//     irregular gaps, automatic fire (gaps under 0.2 s), 3-round bursts,
//     irregular bursts and noise stay sound: their onsets light the auditory
//     pathway, not the beat network. Limits: game sounds that repeat steadily
//     count from their third hit (a gun fired at its cycle rate, steady
//     footsteps, about half of a human's taps at +-10 %); after a fill or a
//     tempo change the next two snares do not count; syncopated snares
//     (breakbeats) mostly do not. Footsteps with a low thud can read as kicks.
//   * Chord change (heuristic): the shared music listener (PitchEstimator +
//     KeyDetector, as the chord view) and a ChordTracker; a change from one
//     chord to another is "unexpected" when the new chord has a pitch class
//     outside the estimated key's scale (minor keys also allow the melodic
//     minor's raised sixth and seventh) while the key is known with a
//     confidence of at least 0.3.
//   * Build-up and drop (heuristic), on 0.25 s slots of the summed power,
//     the 40 - 150 Hz power, the power from 4 kHz and the high-band onsets;
//     a slot under kSilentDb (-60 dBFS) is a pause and counts in no mean:
//     a build-up is a sustained rise over 3 s without a pause (the level by
//     2.5 dB, or the highs by 4 dB while they are at least -60 dBFS and within
//     30 dB of the whole, each second higher than the one before, or the
//     high-band onsets per second up by 3 to at least 6: snare rolls), not the
//     bass returning and not within 4 s of a drop; while it lasts a dopamine
//     pulse every 0.5 s. A drop is the 40 - 150 Hz band jumping 10 dB above
//     its mean of the 2 s before (at least 6 of its 8 slots not a pause) and
//     carrying the mix again (within 9 dB of the whole, at least -40 dBFS)
//     with the level up (1 dB after a build-up, 3 dB after a quieter passage),
//     after a build-up (or within 1.5 s of its end) or a quieter passage
//     (5 dB under the 8 s before it, at least 16 of those slots not a pause);
//     6 s refractory; nothing in the first 4 s of a stream.
//   * Input: NaN / inf samples are taken as 0 (and values clamped to +-16), so
//     one bad block cannot latch the followers.
// Message thread only; allocates only in setSampleRate.
#pragma once

#include "BrainActivity.h"
#include "MusicTheory.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <vector>

namespace flub::app::ui::vis::brain
{
class BrainListener
{
public:
    static constexpr int kRing = 1 << 16;
    static constexpr int kMaxHopsPerFrame = 64;
    static constexpr double kSplitHz = 300.0; // long window below, short above
    static constexpr float kBandFloorDb = -72.0f, kSumFloorDb = -66.0f, kRangeDb = 60.0f;
    static constexpr float kOnsetDb = 8.0f, kOnsetFloorDb = -62.0f;
    static constexpr int kBroadBands = 6;
    static constexpr float kKickRiseDb = 8.0f, kSnareRiseDb = 6.0f, kBodyRiseDb = 3.0f, kSnareFlatness = 0.25f, kSweep = 0.8f, kSweepStepHz = 12.0f;
    static constexpr int kLookahead = 4; // hops before a beat candidate is decided (43 ms at 48 kHz)
    static constexpr double kBeatRefractory = 0.15, kSlotSeconds = 0.25;
    /** A snare-like hit is a beat only when it repeats like a drum beat: gaps of kSnareMinGap .. kSnareMaxGap s
        (300 .. 26 per minute; 2.3 s keeps a 60 BPM backbeat's 2.0 s, which the hop grid measures as 1.995 / 2.005 s)
        that match the one before within kSnareGapTolerance of it (8 %: a drummer's +-30 ms at 120 BPM passes, about
        half of a human's taps at +-10 % do not; the hop grid's step is at most 5.3 % of a 0.2 s gap). */
    static constexpr double kSnareMinGap = 0.2, kSnareMaxGap = 2.3, kSnareGapTolerance = 0.08;
    static constexpr int kSlots = 64;
    /** A 0.25 s slot under this (summed power, dBFS) is a pause, left out of the build-up's and the drop's means. */
    static constexpr float kSilentDb = -60.0f;

    BrainListener();

    /** Allocates the FFTs and windows for the rate (sizes scale with it). */
    void setSampleRate (double rate);
    double getSampleRate() const noexcept { return sampleRate; }
    void reset() noexcept;

    /** Copies the aligned pairs into the ear rings (no analysis). */
    void push (const float* mid, const float* side, int numSamples) noexcept;
    /** Analyses the pending hops, then the chord tracker; hands frames and events to `activity`.
        `clock` is the display time now (seconds), `dt` the frame's length. */
    void advance (double clock, double dt, BrainActivity& activity) noexcept;

    /** How far behind the newest sample the display runs (s): half the long window (the analysis
        centre), the beat decision's look-ahead and a margin for the feed's jitter, so a hop is in
        the history before the display reaches its time (0.116 s at 48 kHz). */
    double getLatency() const noexcept { return static_cast<double> (longSize / 2 + (kLookahead + 1) * hop) / sampleRate + 0.02; }

    // ---- State (tests, the status line) ------------------------------------------------------
    int getHopSize() const noexcept { return hop; }
    int64_t getHops() const noexcept { return hops; }
    /** Band level (dB) of an ear at the latest hop. */
    float getBandDb (int ear, int band) const noexcept { return bandDb[static_cast<size_t> (ear)][static_cast<size_t> (band)]; }
    int getKicks() const noexcept { return kicks; }
    /** Snare beats: the snare-like hits that repeated like a drum beat (the beat network's). */
    int getSnares() const noexcept { return snares; }
    /** Every snare-like hit, counted as a beat or not. */
    int getSnareHits() const noexcept { return snareHits; }
    /** Display times of the last kBeatLog beats (oldest first: getBeatTime (0)). */
    static constexpr int kBeatLog = 64;
    int getNumBeatTimes() const noexcept { return beatLogCount; }
    double getBeatTime (int i) const noexcept;
    bool isBuildUp() const noexcept { return buildActive; }
    int getBuildUps() const noexcept { return buildUps; }
    int getDrops() const noexcept { return dropCount; }
    const music::MusicListener& getMusic() const noexcept { return musicListener; }
    const music::ChordTracker& getTracker() const noexcept { return tracker; }
    /** The last chord change: -1 none yet, 0 expected (in the key), 1 unexpected. */
    int getLastChordSurprise() const noexcept { return lastSurprise; }

private:
    struct BandMap
    {
        bool longWindow = false, interpolate = false;
        int k0 = 0, k1 = 0;  // bins summed (inclusive)
        double centreBin = 0.0;
    };
    struct Feature
    {
        std::array<float, 4> power {}; // the last hops (newest at [0])
        float db = -120.0f, rise = 0.0f;
        void add (float p) noexcept;
    };

    void analyseHop (int64_t hopEnd, double time, BrainActivity& activity) noexcept;
    void transform (int size, int64_t centre, juce::dsp::FFT& fft, const std::vector<float>& window, std::vector<juce::dsp::Complex<float>>& in,
                    std::vector<juce::dsp::Complex<float>>& out, std::vector<float>& powerL, std::vector<float>& powerR) noexcept;
    float bandPower (const BandMap& m, const std::vector<float>& shortP, const std::vector<float>& longP) const noexcept;
    float rangeSum (const std::vector<float>& p, double fromHz, double toHz, int size) const noexcept;
    void detectBeat (double time, float centroid, float flatness, float allDb, BrainActivity& activity) noexcept;
    /** Whether a snare-like hit at t continues a steady pattern: its own (the two gaps to the previous snare-like hits
        match), the counted snares' across one missed hit (nothing snare-like since the last counted snare, and t one
        or two of its gaps after it) or the counted beats' (it lands one beat after the last of three steady beats).
        `period` gets the snare pattern's gap it continues (0 when not known). */
    bool snareRepeats (double t, double& period) const noexcept;
    void updateMacro (double time, double streamSeconds, float totalP, float lowP, float highP, bool hfHit, BrainActivity& activity) noexcept;
    void updateChords (double clock, double dt, BrainActivity& activity) noexcept;

    double sampleRate = 0.0;
    int shortSize = 1024, longSize = 4096, hop = 512;
    std::unique_ptr<juce::dsp::FFT> shortFft, longFft;
    std::vector<float> shortWindow, longWindow;
    std::vector<juce::dsp::Complex<float>> shortIn, shortOut, longIn, longOut;
    std::vector<float> shortL, shortR, longL, longR; // power spectra, bins 0..N/2
    std::array<BandMap, kBands> bands {};
    std::array<std::array<float, 3>, kBands> bandRgb {};
    std::vector<float> ringL, ringR;
    int64_t received = 0, processed = 0, hops = 0;
    double offset = 0.0, lastHopTime = -1.0e9;
    bool offsetValid = false;

    // Per ear and band.
    std::array<std::array<float, kBands>, 2> bandDb {}, levelPower {};
    std::array<std::array<std::array<float, 6>, kBands>, 2> bandHistory {}; // previous powers, newest at [0]
    std::array<std::array<int, kBands>, 2> sinceOnset {};
    std::array<std::array<int, 3>, 2> onsetCounts {}; // onsets per hop, newest at [0]

    // Beat.
    Feature low, click, body;
    struct Candidate
    {
        bool active = false, lowLike = false, snareLike = false;
        int age = 0;
        double time = 0.0;
        std::array<float, kLookahead + 1> centroids {}; // the low band's centroid (Hz) at ages 0 .. kLookahead
        float lowDb = -120.0f, clickDb = -120.0f;
    };
    Candidate candidate;
    double lastBeatTime = -1.0e9;
    double snareLast = -1.0e9, snareBefore = -1.0e9; // the last two snare-like hits, counted or not
    double lastSnareBeat = -1.0e9, snarePeriod = 0.0;  // the last counted snare and its pattern's gap (0: none)
    bool snareSinceBeat = false;                       // a snare-like hit was heard after the last counted snare
    int kicks = 0, snares = 0, snareHits = 0;
    std::array<double, kBeatLog> beatLog {};
    int beatLogCount = 0, beatLogNext = 0;

    // Build-up and drop.
    struct Slot
    {
        float total = -120.0f, low = -120.0f, high = -120.0f;
        int hfHits = 0;
    };
    std::array<Slot, kSlots> slots {};
    int slotsFilled = 0, slotNewest = -1;
    int64_t currentSlot = -1;
    double slotTotal = 0.0, slotLow = 0.0, slotHigh = 0.0;
    int slotHops = 0, slotHits = 0;
    float momLow = 0.0f, momTotal = 0.0f;
    bool buildActive = false;
    int buildRising = 0, buildQuiet = 0, buildUps = 0, dropCount = 0;
    double buildStart = 0.0, buildEnd = -1.0e9, lastDropTime = -1.0e9, nextAnticipation = 0.0;

    // Chords.
    music::MusicListener musicListener;
    music::ChordTracker tracker;
    int lastSurprise = -1;
};
} // namespace flub::app::ui::vis::brain
