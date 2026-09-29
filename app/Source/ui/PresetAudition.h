// Flubsound Pro - preset preview for the preset browser (docs/11 E40), with
// a loudness-matched comparison against the current sound (docs/11 E37).
//
// PresetAudition ("plain preview", docs/11 §5.4): a browser session on one
// strip. begin() snapshots the strip's ACTIVE bank; preview() writes a
// preset's sound into that bank with flub::preset::applyPresetToStore (the
// same write a load makes: app state - bypass, loudness-matched bypass, the
// latency profile - is never touched, so a preview never re-prepares the
// engine or pads another strip, and continuous values glide, discrete ones
// crossfade: click-free). previewOriginal() plays the snapshot again.
// cancel() puts the snapshot back, but only into values that still hold what
// the preview wrote (a hotkey that moved Boost during the preview keeps its
// change), and never touches the other bank, so B is bit-identical
// throughout. commit() loads the preset through EngineController::loadPreset
// (current preset, last preset, reader warnings, latency prompt). While a
// preview plays, the controller's strip-state autosave stores the bank as
// cancel() would leave it (EngineController::setPreviewInProgress), so a
// crash during a preview never saves the previewed sound. That registration
// makes the previewed values an audition bank that is never saved or
// copied: a user-preset save (Save As, Save) or an A/B copy during a preview
// takes the bank as cancel() will leave it (EngineController::
// getSavedBankValues), and a save does not end the session (presetChanged:
// the saved preset becomes the session's start). The preview
// reads the preset itself, so its reader warnings are shown by the browser,
// not queued as a toast on every row.
//
// Matching: every side of the comparison is only ever turned DOWN (the
// ComparisonMatcher rule, docs/11 E37): a preview louder than the current
// sound is trimmed to it, and going back to the current sound trims it to a
// quieter preview (matchTrims). The trim is an offset on the strip's gain in
// the mix (after the chain, smoothed in the MixEngine): output.gain would be
// seen by the maximizer's loudness target (AutoDrive measures after it) and
// would change the sound it is meant to compare. The trim is dropped on
// cancel() / commit(): a loaded preset plays at its own level.
//
// PresetLoudnessEstimator: the chain gain of a set of values (output minus
// input integrated loudness, LU) on a reference programme (the
// TestSignalGenerator's music, kProgrammeSeconds at the engine's rate,
// measured after kSettleSeconds) played at a given input level, rendered
// through the CLI's offline renderer on a worker thread (never the audio
// thread). The level matters as much as the preset: Club Loud is 4.5 LU
// louder than Lo-Fi Chill on this music at -13 LUFS and 7.2 LU at -23 LUFS
// (its maximizer lifts a quiet input), so the browser asks at the level the
// strip's input actually has (levelBucket: whole LU, -40 .. -6 LUFS).
// Results are cached by a hash of the values and the level, so a flip is
// matched from its first second once the preset has been estimated; a
// request can jump the queue (the row the user just selected). Message
// thread API; onEstimate is called on the message thread after new results.
// Two variants serve the other comparisons (docs/11 E37, Comparison.h): a
// 7.1 strip is estimated on the TestSignalGenerator's 7.1 game scene
// (channels 8: the virtualiser and the input fold take part), and a module's
// "listen without it" on a render with that module's audition bypass
// (ProcessingChain::setAuditionBypass, as the module card's ear plays it).
#pragma once

#include "engine/EngineController.h"

#include "flub/io/WavFile.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace flub::app::ui
{
/** What an estimate renders besides the values: the strip's channel count
    (8: the 7.1 game scene, anything else: the stereo music) and a module
    whose enable parameter is audition-bypassed (-1: none). */
struct EstimateVariant
{
    int channels = 2;
    int listenBypassId = -1;
    bool operator== (const EstimateVariant& other) const noexcept { return channels == other.channels && listenBypassId == other.listenBypassId; }
};

class PresetLoudnessEstimator final : private juce::Thread, private juce::AsyncUpdater
{
public:
    static constexpr double kProgrammeSeconds = 3.0, kSettleSeconds = 1.0;
    /** The input level used while the strip's own cannot be measured (LUFS). */
    static constexpr float kDefaultLevelLufs = -18.0f;
    static constexpr float kMinLevelLufs = -40.0f, kMaxLevelLufs = -6.0f;
    /** The level an estimate is made at: whole LU within kMin/kMaxLevelLufs. */
    static int levelBucket (float lufs) noexcept;

    explicit PresetLoudnessEstimator (double sampleRate);
    ~PresetLoudnessEstimator() override;

    double getSampleRate() const noexcept { return sampleRate; }

    using Variant = EstimateVariant;

    /** The reference programme: the TestSignalGenerator's music, stereo,
        kProgrammeSeconds long (deterministic); with channels == 8 its 7.1
        game scene instead. */
    static flub::io::AudioFileData makeReferenceProgramme (double sampleRate, int channels = 2);
    /** Renders `programme` through a chain with `values` (param::kNumParams;
        bypass forced off; the module of `listenBypassId` audition-bypassed)
        and returns output minus input integrated loudness after
        kSettleSeconds (LU; for a 7.1 programme the input is its front pair,
        so only differences between estimates are meaningful); nullopt if
        either side is below -70 LUFS or the render failed. Non-RT,
        allocates, any thread. */
    static std::optional<float> estimateGainLu (const flub::io::AudioFileData& programme, const std::vector<float>& values,
                                                const std::atomic<bool>* abort = nullptr, int listenBypassId = -1);

    /** 64-bit FNV-1a of the values' bit patterns, the level bucket and the
        variant (the cache key). */
    static uint64_t keyOf (const std::vector<float>& values, float levelLufs = kDefaultLevelLufs, Variant variant = {}) noexcept;

    /** The estimate for `values` at an input of `levelLufs`: nullopt while it
        is not known; NaN when it cannot be measured. */
    std::optional<float> find (const std::vector<float>& values, float levelLufs, Variant variant = {}) const;
    /** Queues `values` unless known or queued; `urgent` moves it to the front. */
    void request (const std::vector<float>& values, float levelLufs, bool urgent = false, Variant variant = {});
    int getQueuedCount() const;

    /** Called on the message thread after new results: every listener
        (addListener returns its token for removeListener), then onEstimate. */
    int addListener (std::function<void()> listener);
    void removeListener (int token);
    std::function<void()> onEstimate;

private:
    struct Job
    {
        uint64_t key = 0;
        std::vector<float> values;
        int level = 0;
        Variant variant;
    };

    void run() override;
    void handleAsyncUpdate() override;

    const double sampleRate;
    mutable juce::CriticalSection lock;
    std::deque<Job> queue;
    std::map<uint64_t, float> results;
    uint64_t runningKey = 0;
    bool jobRunning = false;
    std::atomic<bool> abortRender { false };
    std::vector<std::pair<int, std::function<void()>>> listeners; // message thread
    int nextListenerToken = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetLoudnessEstimator)
};

class PresetAudition
{
public:
    /** Deepest trim a match applies (dB). */
    static constexpr float kMaxTrimDb = 20.0f;

    struct Trims
    {
        float previewDb = 0.0f;  // applied while the preview plays (<= 0)
        float originalDb = 0.0f; // applied while the current sound plays again (<= 0)
    };
    /** Only the louder side is turned down, to the quieter one (both <= 0,
        at most kMaxTrimDb). Non-finite gains give no trim. */
    static Trims matchTrims (float originalGainLu, float previewGainLu) noexcept;

    explicit PresetAudition (EngineController& controller);
    ~PresetAudition(); // cancels a running session

    bool isActive() const noexcept { return strip >= 0; }
    int getStrip() const noexcept { return strip; }
    /** Starts a session on `strip` (ends a running one with cancel()). */
    void begin (int strip);

    /** Plays the preset's sound in the strip's active bank (starts a session
        on the selected strip if none runs). The preset's reader warnings go
        to `warnings` (may be null). */
    bool preview (const PresetInfo& preset, juce::String& error, juce::StringArray* warnings = nullptr);
    /** Plays the sound the session started with. */
    void previewOriginal();
    /** Id of the preset playing; empty while the current sound plays. */
    const juce::String& getPreviewId() const noexcept { return previewId; }

    /** The values the session started with / the values as the preview plays
        them (the preset's sound with the bank's app state): what the
        loudness estimates are made of. */
    const std::vector<float>& getOriginalValues() const noexcept { return original; }
    const std::vector<float>& getPlayingValues() const noexcept { return written; }
    /** The values `preset` would play with in this session (empty on a read error). */
    std::vector<float> valuesFor (const PresetInfo& preset, juce::StringArray* warnings = nullptr) const;

    /** The matching trim on the strip's gain (dB, <= 0; click-free). */
    void setTrimDb (float db);
    float getTrimDb() const noexcept { return trimDb; }

    /** Restores the session's start (see the file comment) and ends it. */
    void cancel();
    /** Ends the session and loads `preset` into the strip through the controller. */
    bool commit (const PresetInfo& preset, juce::String& error);
    /** Ends the session without restoring any value (someone else loaded a
        preset into the strip); only the trim is removed. */
    void abandon();
    /** The strip's current preset id when the session began (or the preset
        the pre-preview sound was saved as since). */
    const juce::String& getPresetIdAtBegin() const noexcept { return presetIdAtBegin; }
    /** The strip's current preset changed (Change::Preset). A save of the
        pre-preview sound during the preview (EngineController::
        getPreviewSavedPresetId) becomes the session's start and the preview
        plays on; anything else (a load by a hotkey or an automatic profile)
        ends the session without restoring (abandon). True while the session
        goes on. */
    bool presetChanged();

private:
    void write (const std::vector<float>& values);
    void releaseTrim();

    EngineController& controller;
    int strip = -1;
    flub::param::Bank bank = flub::param::Bank::A;
    std::vector<float> original, written;
    juce::String previewId, presetIdAtBegin;
    float baseGainDb = 0.0f, appliedGainDb = 0.0f, trimDb = 0.0f;
};
} // namespace flub::app::ui
