// Flubsound Pro - EngineController: the ONE object the UI talks to.
//
// Owns (in construction order): AppSettings, AudioEngineHost (device +
// MixEngine), PresetManager, AppRouting, the platform ForegroundApp service
// (automatic profiles). Everything here is MESSAGE THREAD
// only unless noted; the audio thread is reached exclusively through
// ParameterStore atomics, MeterBus atomics and SPSC rings.
//
// ---------------------------------------------------------------------------
// Guide for UI code
// ---------------------------------------------------------------------------
// Strips       getNumStrips(), getStripName(i), getStripChannels(i); the UI
//              edits one strip at a time: getSelectedStrip() /
//              setSelectedStrip(). Strip index 0.. getNumStrips()-1.
// Parameters   getParams(strip) -> flub::param::ParameterStore&. Read with
//              store.get(id), write with store.set(id, value) (clamped,
//              lock-free, RT-safe). Poll store.version() in a Timer to
//              refresh controls. Parameter metadata: flub::param::layout().
//              The reference stays valid until the strip LAYOUT changes.
// Metering     getChain(strip).meters() (flub::MeterBus atomics) and
//              getChain(strip).taps() (AnalyzerTaps SPSC rings; the UI's
//              analyser must be their ONLY consumer) plus effectiveValue(id)
//              for post-macro "ghost" markers. RE-FETCH getChain() on every
//              timer tick - never cache the reference: a reconfiguration
//              (device rate change, latency profile, layout) re-creates the
//              chains. Change::Engine is broadcast afterwards; reset any
//              analyser state then.
// A/B listen   setAuditionBypass(strip, enableId, true / false): momentary
//              "hear the strip without this module" (not a parameter).
//              setComparisonTrimDb(): the loudness match of a comparison
//              (matched A/B, module listen, blind A/B/X; docs/11 E37).
// Master       isEnabled()/setEnabled() = BypassAll on every strip (both
//              banks); setStripBypassed() bypasses one strip on top of it.
//              getMasterGainReductionDb() = master safety limiter.
// Hotkeys      getHotkeyStrip() (the strip hotkeys act on, never the GUI
//              selection), setFocus() / setNight() latched overrides,
//              setChatMix() Game <-> Chat balance (docs/11 E56, E22).
// Chat         setChatMix() (MixEngine's complementary balance), setChatDuck()
//              (the voice-keyed duck, persisted), isChatVoiceActive() (docs/11 E22).
// Mode/Boost   getMode/setMode/toggleMode, getBoost/setBoost/nudgeBoost
//              (strip = -1 means the selected strip). Macro labels:
//              getMacroName(mode, 0..4).
// Presets      getPresetManager() for the list; loadPreset(id, strip) /
//              nextPreset() / previousPreset() / saveUserPreset() here so
//              settings and listeners are updated. A/B: getActiveBank,
//              setActiveBank, toggleAB, copyActiveToOtherBank. A strip with
//              no saved state starts from a default preset (docs/11 E36):
//              Signature (Music, System), Voice Chat (Chat), a capped
//              Competitive FPS (Game). renameUserPreset keeps the uuid.
// Notices      takePresetWarnings() (reader warnings of a preset loaded or
//              imported, docs/11 E52) and takeLatencySuggestion() (a loaded
//              preset was made for another latency profile, docs/11 E42a):
//              queued here, taken by the UI on Change::Preset.
// Device       getDeviceManager() (e.g. for juce::AudioDeviceSelectorComponent;
//              the selection is persisted automatically), getLatencyInfo(),
//              getStatus() (CPU, xruns), getDeviceInputStrip(),
//              getOverloadState() (CPU-overload watchdog), getCaptureStreams()
//              (per-app capture FIFO statistics), getDeviceSafetyState()
//              (loopback pair / device error warning) and retryDevice()
//              for the device banner (docs/11 E51).
// Profile      getLatencyProfile() / setLatencyProfile() (every strip, both
//              banks); the opt-in automatic overload response:
//              setReduceLoadOnOverload(), hasReducedLoad(),
//              describeLoadReduction(), restoreLatencyProfile(). On Linux a
//              profile chosen by hand also asks PipeWire for its quantum and
//              re-opens a JACK / ALSA device (docs/11 E48a, planGraphQuantum).
// Buffer /     the device buffer follows the profile chosen by hand
//   latency    (docs/11 E42c, AudioEngineHost DEVICE BUFFER SIZE):
//              setAutomaticBufferSize(), getBufferInfo(); glitches raise it
//              one size (buffer::Backoff, from the watchdog's poll; the floor
//              is persisted per device). The live latency measurement
//              (docs/11 E42d, LatencyMeasurer): startLatencyMeasurement(),
//              cancelLatencyMeasurement(), getLatencyMeasurement(),
//              whyCannotMeasureLatency(); polled by the timer, the result
//              logged and announced with Change::Device.
// Protection   getProtectionStrength() / setProtectionStrength(): how far the
//              SafetyGovernor reaches (docs/11 E06; persisted, every strip,
//              re-applied to every engine the host builds).
// Listening    the loudness contour follows the OS output volume (docs/11
//   level      E32): setContourFollowsVolume() (Settings > Processing, off by
//              default) reads the output endpoint's volume a few times a
//              second off the audio thread and hands every strip's chain
//              the volume minus the reference volume
//              (setContourReferenceVolumeDb / useCurrentVolumeAsReference;
//              ProcessingChain::setListeningLevelDb). getListeningLevel()
//              for the UI. The contour itself is contour.on, per preset.
// Hearing      getHearing() / setHearingSensitivity() / setHearingCap():
//              the hearing guard's listening-level estimate, the daily dose
//              (pollHearingDose, persisted per day) and the listening-level
//              cap (docs/11 E32 (c); Settings > Hearing). getPersonalProfile()
//              / setPersonalProfile(): the per-ear listening preference
//              (docs/11 E33), its own file, applied to every strip's chain.
//              getSmartMacros() / setSmartMacros(): Smart macros per strip
//              (docs/11 E34).
// Loopback     getAllowedLoopbackPairs() / setLoopbackPairAllowed(): the
//   override   feedback-loop guard's per-pair override (docs/11 E51),
//              persisted and applied to every device start.
// Output       getFollowSystemDefaultOutput() / setFollowSystemDefault-
//   choice     Output(): Settings > Audio's "Follow the system default
//              output" (docs/11 E51, AudioEngineHost::setFollowSystemDefault),
//              persisted; the chosen output is otherwise tracked by its
//              endpoint identity (trackPreferredOutput mirrors the host's
//              choice; the host re-selects it).
// Headset      getOnboardEnhancement() / setOnboardEnhancement(): "Headset
//   enhance-   enhancement (Superhuman Hearing / on-board EQ) is ON" for the
//   ment       output endpoint (docs/11 E16): stored per endpoint (its E51
//              identity, the name as the fallback) and applied to every
//              strip's chain (ProcessingChain::setOnboardEnhancementCap:
//              Footsteps / Detail at most 30 %, the virtualiser off), on
//              every output change and every engine the host builds.
// Correction   getDeviceCorrection(): the output endpoint's headphone /
//              speaker correction (docs/11 E15; import an AutoEQ / Equalizer
//              APO ParametricEQ.txt with importDeviceCorrection, enable /
//              compare / remove it). Stored per output endpoint, applied on
//              the master sum before the limiter with an automatic preamp;
//              presets, A/B banks and automatic profiles never touch it.
// Routing      getRouting() (per-app routing, executable -> strip).
// Auto profile getAutoProfileRules() / setAutoProfileRules(): "while <app> is
//              in the foreground, <strip> plays <preset>" (roadmap 2.5,
//              AutoProfile.h). The controller polls the foreground app at
//              2 Hz (pollForegroundApp) and loads / restores presets itself;
//              isAutoProfileSupported(), describeAutoProfile() for the UI.
// Tournament   getTournamentState() / setTournamentMode(): Tournament mode
//   mode       (docs/11 E55) freezes per-app routing, holds automatic
//              profiles and stops the foreground poll; it switches itself
//              on while a known anti-cheat service runs
//              (pollAntiCheatServices, every 10 s) unless
//              setTournamentAuto (false). isTournamentActive() also
//              silences the on-screen display (ui/Osd.h, docs/11 E56).
// Settings     getSettings() (tray / start-up / hotkeys ...).
// Listening    addListener(); Listener::engineControllerChanged(Change) is
//              called on the message thread for state the UI cannot poll
//              cheaply (preset loaded, strip selected, engine rebuilt...).
#pragma once

#include "AppRouting.h"
#include "AudioEngineHost.h"
#include "AutoLoadReducer.h"
#include "AutoProfile.h"
#include "BufferPolicy.h"
#include "LatencyMeasurer.h"
#include "OverloadWatchdog.h"
#include "presets/PresetManager.h"
#include "settings/AppSettings.h"

#include "flub/engine/DeviceProfiles.h"
#include "flub/engine/HearingGuard.h"
#include "flub/engine/PersonalProfile.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace flub::app
{
class EngineController final : private juce::Timer, private juce::ChangeListener, private juce::AsyncUpdater
{
public:
    enum class Change
    {
        MasterEnable,  // setEnabled / toggleEnabled
        SelectedStrip, // setSelectedStrip
        Preset,        // a preset was loaded / saved / the list changed
        Parameters,    // mode / boost / A-B changed through the controller
        Engine,        // engine (re)configured: re-fetch chains, reset analysers
        Device,        // device opened / changed / error
        Routing,       // per-app routing state changed
        Settings       // AppSettings changed through the controller
    };

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void engineControllerChanged (Change change) = 0;
    };

    struct Options
    {
        bool openAudioDevice = true;  // false: headless (screenshot / tests)
        bool restoreState = true;     // apply saved strip state / last presets
        bool enableAppRouting = true; // enumerate sessions / captures
        juce::File settingsFile;      // empty: default location
        bool persistSettings = true;  // false: never write the settings file
        /** Source of the foreground application for automatic profiles; empty =
            the platform's (platform_bridge::createForegroundApp). Tests inject
            a fake; a factory returning nullptr turns the feature off. */
        std::function<std::unique_ptr<flub::platform::ForegroundApp>()> foregroundAppFactory;
        /** Reads the output endpoint's OS volume for the loudness contour
            (docs/11 E32); empty = the platform's
            (platform::AudioEndpoints::queryOutputVolume). Tests inject a
            fake. With an open device a background thread calls it while the
            contour follows the volume; headless, only pollEndpointVolume(). */
        std::function<flub::platform::EndpointVolume (const std::string& deviceName)> endpointVolumeReader;
        /** Tests: run that background poll without an open device too. */
        bool pollEndpointVolumeHeadless = false;
        /** docs/11 E55: the known anti-cheat services running now (Tournament
            mode's automatic switch); empty = the platform's
            (platform::AntiCheatServices::running). Tests inject a fake. */
        std::function<std::vector<std::string>()> antiCheatServices;
        /** docs/11 E16: the active output endpoints with their identities
            (endpoint id, hardware id), which key the per-endpoint settings;
            empty = the platform's (platform::AudioDeviceWatcher::listOutputs,
            read on each output change) with an open device, none headless
            (the name alone). Tests inject a fake. */
        std::function<std::vector<flub::platform::OutputEndpointIdentity>()> outputEndpoints;
        /** docs/11 E32 (c): the calendar the estimated daily dose is kept by
            (local time); empty = juce::Time::getCurrentTime. Tests inject one. */
        std::function<juce::Time()> clock;
        /** docs/11 E53, the real-device soak (shell/DeviceSoak.h): the device
            state (a JUCE DEVICESETUP element) to open instead of the saved
            one; it replaces the settings' device state in memory, so a
            re-open uses it too. nullptr = the settings' own. */
        std::shared_ptr<const juce::XmlElement> deviceState;
        /** docs/11 E53: the only output the host may play to
            (AudioEngineHost::setOutputPin); empty = any. */
        juce::String outputPin;
        /** Called with the host just before the device opens (a soak's
            virtual device type, a test's fake device watcher). */
        std::function<void (AudioEngineHost&)> beforeDeviceOpen;
    };

    EngineController();
    explicit EngineController (Options options);
    ~EngineController() override;

    /** Persists state, stops routing and closes the device. Idempotent; also
        called by the destructor. */
    void shutdown();

    void addListener (Listener* listener) { listeners.add (listener); }
    void removeListener (Listener* listener) { listeners.remove (listener); }

    // ---- Strips ---------------------------------------------------------------
    int getNumStrips() const noexcept { return host->getNumStrips(); }
    juce::String getStripName (int strip) const;
    int getStripChannels (int strip) const;
    int findStrip (const juce::String& name) const; // -1 if not found
    int getSelectedStrip() const noexcept { return selectedStrip; }
    void setSelectedStrip (int strip);

    flub::param::ParameterStore& getParams (int strip);
    flub::param::ParameterStore& getSelectedParams() { return getParams (selectedStrip); }
    flub::ProcessingChain& getChain (int strip);
    uint32_t getEngineGeneration() const noexcept { return host->getStructureGeneration(); }

    /** Hold-to-bypass A/B (a module card's "ear"): forces the module whose
        enable parameter is `enableParamId` off on the strip's chain whatever
        the preset or the macros say (ProcessingChain::setAuditionBypass,
        click-free). Not a parameter: never stored, never marks the preset
        modified. The caller must release it; a reconfiguration re-creates the
        chains, which drops every audition. */
    void setAuditionBypass (int strip, int enableParamId, bool bypassed);

    bool isStripActive (int strip) const noexcept { return host->isStripActive (strip); }
    /** The user's strip gain (persisted). The engine plays it (plus a
        comparison trim); ChatMix is a separate gain in the MixEngine (see
        setChatMix), so it never moves this value. */
    void setStripGainDb (int strip, float gainDb);
    float getStripGainDb (int strip) const noexcept;
    /** A loudness-matched comparison's trim (docs/11 E37): the matched A/B,
        the matched module / virtualiser listen and the blind A/B/X test turn
        the louder side down with it (dB, -kMaxComparisonTrimDb .. 0). Added
        to the strip's gain in the mix, after the chain, so the maximizer's
        loudness target never sees it; click-free through the MixEngine's
        smoothed gain. Per session (not persisted, not part of the user's
        strip gain); a layout change clears it. Two slots, summed: the
        banks' comparison (A/B and the blind test) and a module listen. */
    static constexpr float kMaxComparisonTrimDb = 20.0f;
    enum class ComparisonSlot
    {
        Banks,
        Listen
    };
    void setComparisonTrimDb (int strip, float db, ComparisonSlot slot = ComparisonSlot::Banks);
    /** One slot's trim. */
    float getComparisonTrimDb (int strip, ComparisonSlot slot = ComparisonSlot::Banks) const noexcept;
    /** Both slots together (what the strip's gain carries). */
    float getTotalComparisonTrimDb (int strip) const noexcept;
    void setStripMuted (int strip, bool muted);
    bool isStripMuted (int strip) const noexcept { return host->isStripMuted (strip); }
    float getMasterGainReductionDb() noexcept { return host->getMixEngine().getMasterGainReductionDb(); }

    // ---- Master enable / mode / boost (strip -1 = selected strip) ----------------
    bool isEnabled() const noexcept { return enabled; }
    void setEnabled (bool shouldBeEnabled);
    void toggleEnabled() { setEnabled (! enabled); }
    /** Bypass of one strip on top of the master enable (the "Bypass hotkey
        strip" hotkey, docs/11 E56): the strip plays its input, loudness
        matched while its "Loudness-matched bypass" is on. Per session (not
        persisted); a layout change clears it. Broadcasts Change::MasterEnable. */
    void setStripBypassed (int strip, bool bypassed);
    bool isStripBypassed (int strip) const noexcept;

    flub::param::ModeValue getMode (int strip = -1);
    void setMode (flub::param::ModeValue mode, int strip = -1);
    void toggleMode (int strip = -1);
    float getBoost (int strip = -1);
    void setBoost (float boost01, int strip = -1);
    void nudgeBoost (float delta, int strip = -1);
    static juce::String getMacroName (flub::param::ModeValue mode, int macroIndex);

    // ---- Presets -----------------------------------------------------------------------
    PresetManager& getPresetManager() noexcept { return *presets; }
    bool loadPreset (const juce::String& presetId, int strip, juce::String& error);
    bool loadPreset (const PresetInfo& preset, int strip, juce::String& error);
    bool nextPreset (int strip = -1);
    bool previousPreset (int strip = -1);
    juce::String getCurrentPresetId (int strip = -1) const;
    juce::String getCurrentPresetName (int strip = -1) const;
    bool isPresetModified (int strip = -1);
    /** Saves the strip's active bank as a user preset; returns its id. */
    juce::String saveUserPreset (const juce::String& name, const juce::String& category, const juce::String& description, int strip,
                                 juce::String& error);
    /** Renames a user preset's file and name, keeping its uuid (docs/11 E52:
        strips and automatic profile rules that play it keep working). A
        preset that had no uuid gets one; the strips' saved last preset and
        the rules follow. Returns the id, empty with `error` on failure. */
    juce::String renameUserPreset (const PresetInfo& preset, const juce::String& newName, juce::String& error);

    /** Reader warnings (unknown keys, clamped values, a newer minor version)
        of a preset loaded into a strip or imported (docs/11 E52,
        PresetManager::onPresetWarnings), oldest first. Queued (at most
        kMaxPendingNotices) until the UI takes them on Change::Preset. */
    struct PresetWarnings
    {
        juce::String presetName;
        juce::StringArray warnings;
    };
    std::vector<PresetWarnings> takePresetWarnings();

    /** A preset loaded by hand (loadPreset, next / previous) that was made for
        another latency profile than the engine runs (docs/11 E42a). Loading
        never changes the profile; the UI offers the switch
        (setLatencyProfile) instead. Only the latest one is kept. */
    struct LatencySuggestion
    {
        juce::String presetName;
        flub::param::LatencyProfileValue suggested = flub::param::LatencyProfileValue::Balanced;
        flub::param::LatencyProfileValue current = flub::param::LatencyProfileValue::Balanced;
    };
    std::optional<LatencySuggestion> takeLatencySuggestion();
    static constexpr size_t kMaxPendingNotices = 8;

    flub::param::Bank getActiveBank (int strip = -1);
    void setActiveBank (flub::param::Bank bank, int strip = -1);
    void toggleAB (int strip = -1);
    void copyActiveToOtherBank (int strip = -1);

    // ---- Device / engine ------------------------------------------------------------------
    juce::AudioDeviceManager& getDeviceManager() noexcept { return host->getDeviceManager(); }
    AudioEngineHost& getHost() noexcept { return *host; }
    LatencyInfo getLatencyInfo() const { return host->getLatencyInfo(); }
    EngineStatus getStatus() const { return host->getStatus(); }
    juce::String getLastDeviceError() const { return lastDeviceError; }

    /** Re-opens the device from the saved state (e.g. after a failure). */
    juce::String reopenDevice();
    /** The device banner's Retry (docs/11 E51): a device error re-opens the
        device from the saved state (without an open device, e.g. headless,
        the error is dismissed); a loopback pair is checked again with the
        current device names (it clears once the output is no longer the
        input's partner). Returns the error of a failed re-open. */
    juce::String retryDevice();
    /** What the header shows as a device warning (docs/11 E51): a loopback
        pair holding the output at silence, or a device error.
        Change::Device is broadcast whenever it changes. */
    DeviceSafetyState getDeviceSafetyState() const { return host->getDeviceSafetyState(); }

    /** CPU-overload watchdog (OverloadWatchdog): sustained load >= 90 % (the
        higher of the average and the p99.9 callback of each poll's window,
        docs/11 E45) or a burst of xruns / overrunning or late callbacks. The header shows it and the
        episodes are counted for the session. By default that is all; with
        the opt-in setting (AppSettings::getReduceLoadOnOverload) a lasting
        overload also steps the latency profile down (AutoLoadReducer,
        docs/01-architecture.md §7). */
    const OverloadWatchdog::State& getOverloadState() const noexcept { return overloadWatchdog.getState(); }
    /** One watchdog poll. The controller's timer calls it at 2 Hz with
        getStatus(); tests feed statuses directly. The window of
        status.callbackTiming since the previous poll gives the peak (p99.9)
        load, and its overBudget / late counts the glitches no counter
        reports. Broadcasts Change::Device when an overload starts or ends and
        when it stepped the profile down. */
    void updateOverloadWatchdog (const EngineStatus& status);

    // ---- Latency profile / automatic overload response ---------------------------------
    /** The selected strip's latency profile (every strip carries the same one). */
    flub::param::LatencyProfileValue getLatencyProfile();
    /** A user choice (Settings > Processing): every strip, both banks; the
        engine re-prepares on the message thread (AudioEngineHost's poll, a
        brief dropout). Resets the automatic ladder. On Linux it also asks
        PipeWire for the profile's quantum (planGraphQuantum) and, when that
        changed the request, re-opens a JACK / ALSA device, because PipeWire
        reads the request only when a stream opens (another brief dropout).
        Broadcasts Change::Settings. */
    void setLatencyProfile (flub::param::LatencyProfileValue profile);

    /** docs/11 E48a: what to export for a latency profile. The values are the
        platform layer's (pipewire::planLatencyEnvironment): Quality and
        Balanced ask for 256/48000 (5.3 ms) and never lock the quantum; Low
        Latency asks for 128/48000 (2.7 ms) and locks it while it runs
        (node.lock-quantum through PIPEWIRE_PROPS). userLatency / userProps
        are what the user exported before Flubsound started (nullptr = unset):
        a user's PIPEWIRE_LATENCY wins over every profile, a user's
        PIPEWIRE_PROPS is never replaced or removed. */
    struct GraphQuantumPlan
    {
        juce::String latency;    // PIPEWIRE_LATENCY to export; empty: leave it alone
        juce::String props;      // PIPEWIRE_PROPS to export; empty: see clearProps
        bool clearProps = false; // remove the quantum lock this process exported
    };
    static GraphQuantumPlan planGraphQuantum (flub::param::LatencyProfileValue profile, const char* userLatency, const char* userProps);
    /** Exports planGraphQuantum's plan into this process's environment
        (Linux; elsewhere it does nothing and returns false). Message thread,
        while no other thread reads the environment. True when it changed. */
    static bool applyGraphQuantum (flub::param::LatencyProfileValue profile, const char* userLatency, const char* userProps);
    /** Turns the automatic overload response on / off (persisted; default off).
        Broadcasts Change::Settings. */
    void setReduceLoadOnOverload (bool shouldReduce);
    bool getReduceLoadOnOverload() const { return settings->getReduceLoadOnOverload(); }
    const AutoLoadReducer::State& getLoadReductionState() const noexcept { return loadReducer.getState(); }
    /** True after an automatic step, until the profile is chosen by hand or restored. */
    bool hasReducedLoad() const noexcept { return loadReducer.hasReduced(); }
    /** What the automatic response changed ("latency profile Quality ->
        Balanced ..."), for the header tooltip, the tray bubble and Settings;
        empty while hasReducedLoad() is false. */
    juce::String describeLoadReduction() const;
    /** Manual "Restore": back to the profile the user had before the first
        automatic step (a user change: the ladder resets; it never steps back
        up by itself). Does nothing while hasReducedLoad() is false. */
    void restoreLatencyProfile();

    // ---- Device buffer size (docs/11 E42c) ---------------------------------------------
    /** Settings > Audio "Automatic buffer size" (persisted, default on): the
        device buffer follows the latency profile chosen by hand
        (AudioEngineHost DEVICE BUFFER SIZE). Off: the size stays as it is
        and the device list's buffer sets it; the Audio page turns it off
        when the user picks a size there. On again: the device's back-off
        floor is forgotten. Broadcasts Change::Device and Change::Settings. */
    void setAutomaticBufferSize (bool automatic);
    bool getAutomaticBufferSize() const noexcept { return host->getAutomaticBufferSize(); }
    AudioEngineHost::BufferInfo getBufferInfo() const { return host->getBufferInfo(); }
    /** Times the back-off raised the buffer this session. */
    uint64_t getBufferBackoffSteps() const noexcept { return bufferBackoffSteps; }

    // ---- Live latency measurement (docs/11 E42d) ------------------------------------------
    using LatencyMode = LatencyMeasurer::Mode;
    /** Starts measuring through the selected strip (Through / Both); "" or
        why it cannot (LatencyMeasurer::whyNot). The result arrives with
        Change::Device (the timer polls) and goes to the log. */
    juce::String startLatencyMeasurement (LatencyMode mode);
    void cancelLatencyMeasurement();
    const LatencyMeasurer::State& getLatencyMeasurement() const noexcept { return latencyMeasurer->getState(); }
    juce::String whyCannotMeasureLatency (LatencyMode mode) const;
    LatencyMeasurer& getLatencyMeasurer() noexcept { return *latencyMeasurer; }

    // ---- Protection strength (docs/11 E06) --------------------------------------------
    /** How far the SafetyGovernor reaches (flub::ProtectionStrength): Off
        (default) governs the macro amounts only, Normal also the base
        max.drive / sat.drive / bass.harmonics, Strict as Normal with the
        scale's floor at 0. A host setting, not a preset parameter: persisted
        in the settings, applied to every strip at once (a relaxed atomic,
        click-free) and to every engine the host builds later (device
        restarts, swaps). Broadcasts Change::Settings. */
    void setProtectionStrength (flub::ProtectionStrength strength);
    flub::ProtectionStrength getProtectionStrength() const noexcept { return protectionStrength; }
    static juce::String getProtectionStrengthName (flub::ProtectionStrength strength);

    /** A running per-app capture with its FIFO statistics
        (DriftCompensatedFifo::Stats), the application's display name (from
        AppRouting; empty if unknown) and the name of the strip it feeds. */
    struct CaptureStream
    {
        AudioEngineHost::CaptureInfo info;
        juce::String appName, stripName;
    };
    std::vector<CaptureStream> getCaptureStreams() const;

    // ---- Output device profile (headsets, e.g. Turtle Beach families) ------------------
    /** Advice for the current output device: the ceiling cap already applied to
        the master limiter (-1 / -2 Bluetooth / -3 dBTP hands-free), whether the
        format is narrowband, a suggested preset and user-facing guidance. */
    const flub::device::Advice& getDeviceAdvice() const noexcept { return deviceAdvice; }
    /** Matched profile ("Turtle Beach Stealth series"); empty for generic devices. */
    juce::String getDeviceProfileName() const;
    flub::device::Connection getDeviceConnection() const noexcept { return deviceMatch.connection; }
    juce::String getOutputDeviceName() const { return currentOutputName; }
    /** Headless runs only (Options::openAudioDevice == false, e.g. --screenshot):
        treats `name` as the open output device, so the device profile, advice
        and ceiling cap can be shown and checked without hardware. */
    void simulateOutputDevice (const juce::String& name, double sampleRate, int outputChannels);
    /** The profile database (shipped, or the user override file
        <app data>/Flubsound/device-profiles.json when present). */
    const flub::device::Database& getDeviceProfiles() const noexcept { return deviceProfiles; }

    // ---- Headset enhancement cap (docs/11 E16) --------------------------------------------
    /** "Headset enhancement (Superhuman Hearing / on-board EQ) is ON" for the
        current output endpoint. */
    struct OnboardEnhancementInfo
    {
        juce::String endpoint;  // the output it belongs to; empty while no output is open
        bool offered = false;   // the matched profile's headset (or its software) has its own enhancement (Profile::onboardDsp)
        bool answered = false;  // the user said on or off for this endpoint (the device banner asks until then)
        bool on = false;        // the cap applies to every strip
    };
    OnboardEnhancementInfo getOnboardEnhancement() const;
    /** The user's answer for the current output endpoint: stored with its
        identity (AppSettings::setDeviceEndpoint: endpoint id, hardware id
        and name, so a rename or a re-plug into another port keeps it) and
        applied to every strip at once. While on, each chain glides Gaming
        Footsteps and Detail to at most 30 % and holds its virtualiser off
        (ProcessingChain::setOnboardEnhancementCap); presets and the
        parameter values stay as they are. False (nothing stored) while no
        output is open. Broadcasts Change::Device and Change::Settings. */
    bool setOnboardEnhancement (bool on);
    /** The cap every strip's chain was handed (the current endpoint's answer). */
    bool isOnboardCapApplied() const noexcept { return onboardCapOn; }
    /** The current output endpoint's identity (the platform's endpoint id and
        hardware id where known, else the name alone). */
    const flub::platform::OutputEndpointIdentity& getOutputIdentity() const noexcept { return outputIdentity; }

    // ---- Device correction (docs/11 E15) ------------------------------------------------
    /** What the current output endpoint's correction is and does. */
    struct DeviceCorrectionInfo
    {
        juce::String endpoint;      // the output endpoint it is stored for; empty while no output is open
        bool hasCurve = false;      // a curve is stored for the endpoint
        juce::String name;          // what was imported ("HD 600 ParametricEQ.txt")
        bool enabled = false;       // stored on / off switch
        bool comparing = false;     // momentary compare (session only)
        int numFilters = 0;
        float preampDb = 0.0f;      // automatic preamp applied (<= 0 dB; 0 while off)
        double maxBoostDb = 0.0;    // predicted maximum boost of the curve incl. its own Preamp (docs/11 E11)
        double maxBoostHz = 0.0;
        juce::String curveText;     // the stored curve (APO syntax)
    };
    DeviceCorrectionInfo getDeviceCorrection() const;
    /** Imports a ParametricEQ.txt for the current output endpoint, replacing
        any curve it had, switched on, and persists it. Returns false with
        `error` set (nothing changed) for an unsupported file or when no output
        endpoint is open. `warnings` receives what was ignored or assumed.
        Broadcasts Change::Settings. */
    bool importDeviceCorrection (const juce::File& file, juce::String& error, juce::StringArray* warnings = nullptr);
    bool importDeviceCorrectionText (const juce::String& text, const juce::String& name, juce::String& error,
                                     juce::StringArray* warnings = nullptr);
    /** On / off for the current endpoint's curve (persisted). */
    void setDeviceCorrectionEnabled (bool shouldBeEnabled);
    /** Compare: the curve's filters off, its broadband gain (preamp) kept, so
        the A/B is not a loudness comparison. Per session; ends when the
        endpoint changes. */
    void setDeviceCorrectionCompare (bool comparing);
    /** Forgets the current endpoint's curve. */
    void removeDeviceCorrection();

    /** Device input -> strip policy (see AppSettings::DeviceInputMode). */
    void setDeviceInputMode (AppSettings::DeviceInputMode mode);
    void setDeviceInputStrip (int strip);
    int getDeviceInputStrip() const noexcept { return host->getDeviceInputStrip(); }
    /** Heuristic: virtual cables / loopback drivers / monitor sources. */
    static bool looksLikeLoopbackDevice (const juce::String& inputDeviceName);

    /** The multi-strip device input map (R4.6 / docs/11 E48; Settings >
        Routing > Input map): per strip, the first device input channel that
        feeds it, -1 = not fed. All -1 = no map (the single "Input feeds
        strip" applies). Setting it persists AppSettings' deviceInput.map
        ("Game=0;Music=8") and re-applies the device input policy; on Linux
        the router then links each flubsound_<strip> sink's monitor to those
        inputs (pw-link / registry). Broadcasts Change::Settings. */
    std::vector<int> getDeviceInputMapChannels() const;
    void setDeviceInputMapChannels (const std::vector<int>& firstChannels);
    /** "Game=0;Music=8" <-> per-strip first channels for these strip names
        (unknown names and malformed entries are skipped; -1 = not fed). Pure. */
    static std::vector<int> parseDeviceInputMap (const juce::String& map, const juce::StringArray& stripNames);
    static juce::String formatDeviceInputMap (const std::vector<int>& firstChannels, const juce::StringArray& stripNames);
    /** The strips one after another from channel 0 (each takes its own channel
        count): Game 7.1 = 0, Music = 8, Chat = 10, System = 12, as the Linux
        setup script's sinks and the native PipeWire node order them. Pure. */
    static std::vector<int> consecutiveInputMap (const std::vector<int>& stripChannels);

    /** Replaces the strip layout (brief dropout). Stores of surviving strips
        are kept; UI must re-fetch everything (Change::Engine). */
    void setStripLayout (const std::vector<flub::StripConfig>& newLayout);

    // ---- Hotkey target and latched overrides (docs/11 E56) ----------------------------------
    /** The strip the strip-level hotkeys act on: the active automatic
        profile's strip, else the Settings > Hotkeys strip
        (AppSettings::getHotkeyStripName, default Game), else strip 0. Never
        the GUI selection. */
    int getHotkeyStrip() const;
    /** Persists the Settings > Hotkeys strip. Broadcasts Change::Settings. */
    void setHotkeyStripName (const juce::String& stripName);

    /** Focus: a latched Footsteps override - Macro 1 at 100 % on both banks
        of a Gaming-mode strip, through the smoothed parameter path - until it
        is switched off, which puts back the value it replaced (a parameter
        changed since keeps its new value). Returns false and changes nothing
        when switching on a strip that is not in Gaming mode. Loading a preset
        on the strip ends it without restoring. */
    bool setFocus (int strip, bool on);
    bool isFocused (int strip) const noexcept;
    /** Night listening: a latched override of the strip's dynamics with the
        Night Mode Gaming factory preset's values (docs/11 E21, read from the
        preset: Auto Level at -14 LUFS, Dynamic Range 20 LU - the Startle
        Guard and, in Gaming mode, the Tame band - and its compressor: 3:1 at
        -18 dB without makeup, upward 2.5:1 below -32 dB up to +6 dB, floor
        -62 dB). Switched off, released like Focus. */
    void setNight (int strip, bool on);
    bool isNight (int strip) const noexcept;
    /** The parameters the Night latch sets, with their values. */
    std::vector<std::pair<int, float>> getNightOverrides() const;

    /** ChatMix (docs/11 E22, E56): one balance between the Game and the Chat
        strip, -1 (towards Game) .. +1 (towards Chat), in whole 10 % steps.
        MixEngine::setChatMix plays it as complementary gains on top of the
        strip gains: the side it moves towards stays at 0 dB, the other falls
        (to silence at the end); both 0 dB at the centre; no other strip
        changes, and the user's strip gains (getStripGainDb) never move.
        The tray flyout, the Chat strip's row and the ChatMix hotkeys all
        call this. Per session (a new layout centres it). Returns false when
        there is no Game or no Chat strip (the balance is then centred). */
    bool setChatMix (float balance);
    bool nudgeChatMix (float delta) { return setChatMix (chatMix + delta); }
    float getChatMix() const noexcept { return chatMix; }
    bool hasChatMix() const;
    /** The balance's gain on `strip` in dB (0 for any strip but Game and
        Chat; -inf when muted at an end). */
    float getChatMixGainDb (int strip) const;
    /** "Game -1.9 dB, Chat 0 dB", "Game muted, Chat 0 dB", or "centred". */
    juce::String describeChatMix() const;

    /** "Duck game under voice chat" (docs/11 E22, persisted; off by
        default): while the Chat strip carries speech the Game and Music
        strips dip 1 - 4 kHz by `depthDb` (3 - 6 dB, default 4.5), the
        footstep band kept (MixEngine::setChatDuck). Broadcasts
        Change::Settings. */
    static constexpr float kDefaultChatDuckDepthDb = 4.5f, kMinChatDuckDepthDb = 3.0f, kMaxChatDuckDepthDb = 6.0f;
    void setChatDuck (bool on, float depthDb);
    void setChatDuck (bool on) { setChatDuck (on, getChatDuckDepthDb()); }
    bool getChatDuck() const;
    float getChatDuckDepthDb() const;
    /** The Chat strip's voice activity (MixEngine::isChatVoiceActive; false
        without a Chat strip) and how far the duck is in (0..1). Any thread. */
    bool isChatVoiceActive() const noexcept;
    float getChatDuckAmount() const noexcept;

    // ---- Listening level: the contour follows the system volume (docs/11 E32) -------------------
    struct ListeningLevel
    {
        bool following = false;         // Settings > Processing switch (persisted, default off)
        bool known = false;             // the last read of the endpoint volume succeeded
        bool muted = false;             // the endpoint is muted (the level is held)
        float volumeDb = 0.0f;          // the endpoint's OS volume (dB), when known
        std::optional<float> referenceDb; // the user's reference volume (dB)
        float levelDb = 0.0f;           // what the chains get: volume - reference (0 while not following)
        juce::String device;            // the output endpoint read
        juce::String error;             // why the read failed
    };
    ListeningLevel getListeningLevel() const;
    /** Turns following on / off (persisted). On without a reference volume
        takes the current volume as the reference (read now), so switching it
        on changes nothing until the volume moves. Off hands every chain 0 dB.
        Broadcasts Change::Settings. */
    void setContourFollowsVolume (bool follow);
    bool getContourFollowsVolume() const { return settings->getContourFollowsVolume(); }
    /** Sets the reference volume (dB, persisted). Broadcasts Change::Settings. */
    void setContourReferenceVolumeDb (float volumeDb);
    /** "This is my reference volume": the volume read now. False (nothing
        changed) when it cannot be read. */
    bool useCurrentVolumeAsReference();
    /** One read of the output endpoint's volume, applied to every chain. The
        background poll does the same off the message thread; tests call it
        directly. Message thread. */
    void pollEndpointVolume();
    /** How often the background poll reads the volume. */
    static constexpr int kEndpointVolumePollMs = 250;

    // ---- Hearing guard: listening-level estimate, dose and cap (docs/11 E32 (c)) ---------------
    /** Settings > Hearing and the LoudnessPanel's dose readout. An ESTIMATE
        (flub::HearingGuard, after the master limiter): the output's
        A-weighted level + the system volume + the headset's sensitivity.
        Without a sensitivity nothing is estimated and the output is not
        touched. The sensitivity is the listener's own figure for the output
        endpoint (persisted per endpoint) or else the matched device
        profile's (none of the shipped profiles has one yet). While a
        sensitivity is known, the system volume is read as for the loudness
        contour (kEndpointVolumePollMs, off the audio thread); an unreadable
        volume counts as full volume (the loudest case). */
    struct HearingInfo
    {
        juce::String output;                        // the output endpoint ("" = none)
        bool known = false;                         // a sensitivity is in use: the guard estimates
        float sensitivityDbSpl = std::numeric_limits<float>::quiet_NaN();
        flub::HearingGuard::SensitivitySource source = flub::HearingGuard::SensitivitySource::Unknown;
        std::optional<float> userDbSpl;             // the listener's own figure for this output
        juce::String profileName;                   // the matched device profile ("" = generic)
        float profileDbSpl = std::numeric_limits<float>::quiet_NaN(); // its figure (NaN: none)
        juce::String profileFigureSource;           // "manufacturer", "lab", ... as the profile says
        bool profileLabVerified = false;
        bool capEnabled = false;
        float capDbA = flub::HearingGuard::kDefaultCapDbA;
        bool capActive = false;                     // the cap holds the level down now
        float capGainDb = 0.0f;
        float levelDbA = flub::HearingMeters::kUnknown, leq5sDbA = flub::HearingMeters::kUnknown,
              sessionLeqDbA = flub::HearingMeters::kUnknown;
        double doseToday = 0.0;                     // fraction of the weekly allowance (80 dB(A) for 40 h)
        double doseWeek = 0.0;                      // today and the 6 days before
        bool volumeKnown = false;                   // the system volume was read
        float volumeDb = 0.0f;
    };
    HearingInfo getHearing() const;
    /** The listener's own sensitivity for the current output (dB SPL of a 0
        dBFS sine at full volume, 60 .. 150), persisted per endpoint; nullopt
        goes back to the profile's figure (or unknown). False (nothing
        stored) without an output. Broadcasts Change::Settings. */
    bool setHearingSensitivity (std::optional<float> dbSpl);
    /** The listening-level cap (persisted; off by default, 60 .. 100 dB(A)).
        It acts only while a sensitivity is known. Broadcasts Change::Settings. */
    void setHearingCap (bool on, float dbA);
    /** One step of the daily dose's bookkeeping: today's dose (the day's
        stored dose + what the guard counted since) is stored under today's
        date; at midnight the day's dose is final and the next day starts
        from its own. The timer calls it; tests call it directly. */
    void pollHearingDose();
    /** "2026-09-30": the day the dose is kept under now (Options::clock). */
    juce::String getDoseDay() const;

    // ---- Personal hearing profile (docs/11 E33) ------------------------------------------------
    /** A listening preference (flub::PersonalProfile: per ear a gain and 8
        bands, and a balance), not a hearing test. Stored in its own file
        (getPersonalProfileFile, flub::personal::save) outside ParameterStore,
        so presets, A/B banks, macros and automatic profiles never touch it;
        applied to every strip's chain (ProcessingChain::setPersonalProfile)
        at start, on every edit and on every engine the host builds. */
    const flub::PersonalProfile& getPersonalProfile() const noexcept { return personalProfile; }
    /** Sanitised, applied to every chain at once (a 20 ms crossfade) and
        saved a moment later (the controller's timer and shutdown, while the
        settings persist). Broadcasts Change::Settings. */
    void setPersonalProfile (const flub::PersonalProfile& profile);
    /** Next to the settings file: "personal-profile.json". */
    juce::File getPersonalProfileFile() const;
    /** Why the file could not be read at start ("" = read, or no file). */
    juce::String getPersonalProfileError() const { return personalError; }
    /** Writes the profile now if it changed since the last write (and the
        settings persist). False when the write failed. */
    bool savePersonalProfile();

    // ---- Smart macros (docs/11 E34) ------------------------------------------------------------
    /** Content-aware macro scaling per strip (ProcessingChain::setSmartMacros:
        the content analysis takes back attack and drive on a limited master,
        bass on bass-heavy and air on bright programme). Persisted per strip
        name, off by default; strip = -1 is the selected strip. Broadcasts
        Change::Settings. A preset carries it as its "smart" flag: loading a
        preset (loadPreset, next / previousPreset, an automatic profile) sets
        the strip's switch to the preset's flag (off without one), saving
        writes the switch into the preset, and the end of an automatic
        profile restores the switch it found. Restoring the session at start
        keeps the persisted switch. */
    bool getSmartMacros (int strip = -1) const;
    void setSmartMacros (bool on, int strip = -1);

    // ---- Feedback-loop guard override (docs/11 E51) ------------------------------------------
    /** The pairs the guard lets through (AppSettings::getAllowedLoopbackPairs). */
    std::vector<AppSettings::LoopbackPair> getAllowedLoopbackPairs() const { return settings->getAllowedLoopbackPairs(); }
    /** Allows (or stops allowing) one input / output pair, persisted and
        applied at once: the current pair is checked again, so allowing the
        pair that tripped the guard lets the output play. Broadcasts
        Change::Device (the banner) and Change::Settings. */
    void setLoopbackPairAllowed (const juce::String& inputDeviceName, const juce::String& outputDeviceName, bool allowed);

    // ---- Output choice (docs/11 E51) ------------------------------------------------------
    /** Settings > Audio > "Follow the system default output" (default off):
        the output follows the system default (never into a virtual or
        looping output) instead of the chosen one. Persisted and applied at
        once; picking an output in Settings > Audio turns it off again.
        Broadcasts Change::Device and Change::Settings. */
    bool getFollowSystemDefaultOutput() const { return settings->getFollowSystemDefaultOutput(); }
    void setFollowSystemDefaultOutput (bool follow);

    // ---- Preset preview (docs/11 E40) ----------------------------------------------------------
    /** A preset preview plays in a strip's active bank (ui::PresetAudition).
        While one is registered, the strip-state autosave stores `bank` as it
        will be once the preview ends: every value that still holds what the
        preview wrote (`written`) as the value before it (`original`), so a
        crash during a preview never saves the previewed sound. Values
        changed meanwhile (a hotkey, the other bank) are saved as they are.
        Both vectors have param::kNumParams values. */
    void setPreviewInProgress (int strip, flub::param::Bank bank, std::vector<float> original, std::vector<float> written);
    void clearPreviewInProgress();
    /** The strip-state JSON the autosave writes for `strip` now (tests). */
    juce::String getPersistedStripState (int strip);
    /** The audition bank (docs/11 E40): what the preview writes plays in the
        active bank's slots, but every path that SAVES or COPIES a bank (the
        autosave above, saveUserPreset, PresetManager::saveCurrent,
        copyActiveToOtherBank) takes the bank as the preview's end will leave
        it, so the previewed sound is never saved. `values` gets `bank` of
        `strip` so (param::kNumParams values); false (and `values` the bank
        as it is) while no preview plays in that bank. */
    bool getSavedBankValues (int strip, flub::param::Bank bank, std::vector<float>& values) const;
    bool isPreviewInProgress (int strip) const noexcept { return strip >= 0 && preview.strip == strip; }
    /** The id of a preset saved on `strip` (saveUserPreset) during the
        current preview: the pre-preview sound, which the preview session
        then treats as its start (PresetAudition::presetChanged). */
    juce::String getPreviewSavedPresetId (int strip) const { return isPreviewInProgress (strip) ? preview.savedPresetId : juce::String(); }

    // ---- Settings / routing ------------------------------------------------------------------
    AppSettings& getSettings() noexcept { return *settings; }
    AppRouting& getRouting() noexcept { return *routing; }
    /** Writes strip states / device state / settings now. */
    void saveState();

    // ---- Automatic profiles (foreground application -> preset, AutoProfile.h) -----------------
    /** True when the foreground application can be detected on this system. */
    bool isAutoProfileSupported() const;
    /** Why it cannot (user-presentable, e.g. "Wayland does not let ..."); empty when supported. */
    juce::String getAutoProfileUnsupportedReason() const;
    bool getAutoProfilesEnabled() const noexcept { return autoProfiles.isEnabled(); }
    /** Master switch (persisted, default on). Off ends an active rule without
        restoring. Broadcasts Change::Settings. */
    void setAutoProfilesEnabled (bool shouldBeEnabled);
    const std::vector<AutoProfileRule>& getAutoProfileRules() const noexcept { return autoProfiles.getRules(); }
    /** Replaces the rules (first match wins; persisted). An active rule that is
        no longer in the list ends without restoring. Broadcasts Change::Settings. */
    void setAutoProfileRules (std::vector<AutoProfileRule> rules);
    /** Appends a rule, replacing any rule for the same application (only the
        first match could ever apply). Returns false (nothing changed) for a
        rule without an executable, strip or preset. */
    bool addAutoProfileRule (const AutoProfileRule& rule);
    /** The rule currently applied, nullptr if none. */
    const AutoProfileRule* getActiveAutoProfile() const noexcept { return autoProfiles.getActiveRule(); }
    /** One line for the UI: what is active, the last error, or "". */
    juce::String describeAutoProfile() const;
    /** Executable names recently seen in the foreground (newest first, at most
        8, never Flubsound itself): the "add rule" menu offers them. */
    const juce::StringArray& getRecentForegroundApps() const noexcept { return recentForegroundApps; }
    /** One foreground poll. The controller's timer calls it at 2 Hz (cheap: a
        few system calls; the process path is only looked up when the
        foreground window changes); tests call it directly. Loads the rule's
        preset (+ mode) on its strip, or restores / releases the strip, and
        broadcasts Change::Preset and Change::Routing when it did. */
    void pollForegroundApp();

    // ---- Tournament mode (docs/11 E55) ---------------------------------------------------------
    struct TournamentState
    {
        bool active = false;               // in effect now
        bool userChoice = false;           // the user's switch (persisted)
        bool automatic = false;            // on only because an anti-cheat service runs
        bool autoEnabled = true;           // the automatic switch-on is allowed (persisted)
        std::vector<std::string> services; // known anti-cheat services running (last poll)
    };
    const TournamentState& getTournamentState() const noexcept { return tournament; }
    /** While true: no session enumeration, no foreground poll, no automatic
        profile switch, and no on-screen display (ui::Osd hides itself on
        the Change::Settings that turns it on). */
    bool isTournamentActive() const noexcept { return tournament.active; }
    /** The user's switch (persisted). Switching it off while an anti-cheat
        service holds Tournament mode on turns it off until those services
        stop. Broadcasts Change::Settings when the state changes. */
    void setTournamentMode (bool on);
    /** Allow the automatic switch-on (persisted, default on). */
    void setTournamentAuto (bool automatic);
    /** One line for the UI: "Tournament mode on: Vanguard is running",
        "Tournament mode on", or "" while off. */
    juce::String describeTournament() const;
    /** "Vanguard" for "vgc", "BattlEye" for "BEService" ... (the name itself
        when unknown). */
    static juce::String antiCheatDisplayName (const std::string& serviceName);
    /** Reads the running anti-cheat services (service manager only: no
        process is opened). The timer calls it every 10 s; tests call it
        directly. A service switches Tournament mode on at the first poll
        that sees it; it goes off after kTournamentOffPolls polls without one. */
    void pollAntiCheatServices();
    static constexpr int kTournamentOffPolls = 2;

    // ---- Headless ------------------------------------------------------------------------------
    /** Runs audio through the engine without a device (screenshot mode). */
    void renderOffline (StripSignalSource& source, int numSamples);

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void handleAsyncUpdate() override; // announces preset warnings no preset load announced
    void notify (Change change);
    int resolveStrip (int strip) const noexcept;
    void applyMasterEnableToStrip (int strip);
    void restoreStripStates();
    void persistStripStates (bool force);
    void applyDeviceInputPolicy();
    void persistDeviceState();
    void loadDeviceProfiles();
    void updateDeviceProfile();
    void applyDeviceProfile (const juce::String& outputName, double sampleRate, int outputChannels);
    void trackPreferredOutput (bool rescan);
    void applyDeviceCorrection();
    std::optional<DeviceCorrectionEntry> findDeviceCorrectionEntry() const;
    void storeDeviceCorrectionEntry (DeviceCorrectionEntry entry);
    void updateOutputIdentity();
    void applyOnboardCap();
    void applyLatencyProfile (flub::param::LatencyProfileValue profile);
    void applyProtectionStrength();
    void applyListeningLevel();
    void applyEndpointVolume (const flub::platform::EndpointVolume& volume, const juce::String& device);
    void applyAllowedLoopbackPairs();
    void updateEndpointVolumePoller();
    flub::platform::EndpointVolume readEndpointVolume (const juce::String& device) const;
    /** Exports the profile's PipeWire quantum request (a device session on
        Linux only); with `reopen`, re-opens a JACK / ALSA device when the
        request changed. */
    void requestGraphQuantum (flub::param::LatencyProfileValue profile, bool reopen);
    /** docs/11 E48: a native PipeWire device (the "PipeWire" type) takes the
        profile's node.latency in place; false for any other device or none. */
    bool applyNodeLatency (flub::param::LatencyProfileValue profile);
    /** docs/11 E42: the native node's reported quantum into the host's
        LatencyInfo::graphQuantumMs (every timer tick). */
    void feedGraphQuantum();
    void presetLoadedByUser (const PresetInfo& preset);
    void applyAutoProfileActions (const std::vector<AutoProfileSwitcher::Action>& actions);
    void applyAutoProfile (const AutoProfileRule& rule);
    void endAutoProfile (const AutoProfileSwitcher::Action& action);
    void presetChangedByUser (int strip);
    void cancelAutoProfile (const juce::String& stripName);
    bool loadFirstRunDefault (int strip);

    /** A latched parameter override (Focus, Night): the values it replaced,
        both banks, put back on release where the override still stands. */
    struct Override
    {
        int id = 0;
        float value = 0.0f;
    };
    struct Latch
    {
        bool on = false;
        std::vector<Override> applied;
        std::vector<float> savedA, savedB;
    };
    void engageLatch (int strip, Latch& latch, std::vector<Override> overrides);
    void releaseLatch (int strip, Latch& latch, bool restore);
    void releaseAllLatches (bool restore);
    void forgetLatches (int strip);
    void applyStripGain (int strip);

    Options options;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<AudioEngineHost> host;
    std::unique_ptr<PresetManager> presets;
    std::unique_ptr<AppRouting> routing;
    std::unique_ptr<LatencyMeasurer> latencyMeasurer; // docs/11 E42d (destroyed before the host)
    juce::ListenerList<Listener> listeners;

    bool enabled = true, isShutDown = false;
    int selectedStrip = 0, timerTicks = 0;
    bool feedingGraphQuantum = false; // docs/11 E42: feedGraphQuantum set the host's graph quantum
    std::array<uint32_t, AudioEngineHost::kMaxStrips> persistedVersions {};
    juce::String lastDeviceError;

    flub::device::Database deviceProfiles;
    flub::device::Match deviceMatch;
    flub::device::Advice deviceAdvice;
    juce::String currentOutputName;
    DeviceEndpointEntry preferredOutput; // docs/11 E51: the host's choice as last stored
    juce::String simulatedOutputName; // headless only, see simulateOutputDevice()
    double simulatedSampleRate = 48000.0;
    int simulatedOutputChannels = 2;
    bool adviceForGaming = false; // mode deviceAdvice was computed for (see notify())
    juce::String correctionEndpoint; // endpoint the applied device correction belongs to
    bool correctionCompare = false;
    flub::platform::OutputEndpointIdentity outputIdentity; // docs/11 E16: what keys the per-endpoint settings
    bool onboardCapOn = false;                             // the cap handed to every chain
    std::unique_ptr<flub::platform::AudioDeviceWatcher> endpointLister; // lists the outputs' identities (never started)

    // Listening level (docs/11 E32): the last read, and the background poll.
    ListeningLevel listening;
    bool listeningHasRead = false; // a successful read for listening.device since following / the device changed
    class EndpointVolumePoller;
    std::unique_ptr<EndpointVolumePoller> volumePoller;

    // A preset preview's autosave substitute (docs/11 E40).
    struct PreviewInProgress
    {
        int strip = -1;
        flub::param::Bank bank = flub::param::Bank::A;
        std::vector<float> original, written;
        juce::String savedPresetId; // saved during the preview (the pre-preview sound)
    };
    PreviewInProgress preview;

    OverloadWatchdog overloadWatchdog;
    flub::CallbackTiming::Snapshot lastCallbackTiming; // the previous poll's (docs/11 E45)
    AutoLoadReducer loadReducer;
    buffer::Backoff bufferBackoff; // docs/11 E42c: glitches raise the automatic buffer
    uint64_t bufferBackoffSteps = 0;
    flub::ProtectionStrength protectionStrength = flub::ProtectionStrength::Off;

    std::vector<PresetWarnings> pendingPresetWarnings;
    bool announcePresetWarnings = false; // an import's warnings wait for handleAsyncUpdate
    std::optional<LatencySuggestion> pendingLatencySuggestion;

    // Hotkey-driven state (docs/11 E56), per session.
    std::array<Latch, AudioEngineHost::kMaxStrips> focusLatches, nightLatches;
    std::array<bool, AudioEngineHost::kMaxStrips> stripBypassed {};
    std::array<float, AudioEngineHost::kMaxStrips> userGainDb {};
    std::array<std::array<float, 2>, AudioEngineHost::kMaxStrips> comparisonTrimDb {}; // docs/11 E37, per session and slot
    float chatMix = 0.0f;

    // Automatic profiles (message thread)
    /** A strip's state before an auto profile with "restore on exit" replaced it. */
    struct AutoProfileRestorePoint
    {
        juce::String stripName, presetId;
        bool presetModified = false;
        bool smartMacros = false; // the strip's Smart macros switch (docs/11 E34)
        flub::param::Bank activeBank = flub::param::Bank::A;
        std::vector<float> bankA, bankB;
    };
    std::unique_ptr<flub::platform::ForegroundApp> foregroundApp;
    AutoProfileSwitcher autoProfiles;
    std::optional<AutoProfileRestorePoint> autoRestorePoint;
    juce::String autoAppliedPresetId, autoProfileError;
    juce::StringArray recentForegroundApps;

    // Tournament mode (docs/11 E55, message thread)
    void applyTournament();
    TournamentState tournament;
    bool tournamentDismissed = false; // switched off by the user while services held it on
    int tournamentQuietPolls = 0;     // polls without a service since one was seen

    // Hearing guard (docs/11 E32 (c)), personal profile (docs/11 E33) and
    // Smart macros (docs/11 E34), message thread.
    void applyHearingGuard();
    void applyPersonalProfile();
    void applySmartMacros();
    void setPresetSmartMacros (int strip, bool on); // a preset's "smart" flag, on load
    bool hearingKnown = false;           // the guard has a sensitivity (the volume poll runs for it)
    std::optional<float> hearingUserDbSpl; // the listener's figure for the output (as last applied)
    double doseEarlierDays = 0.0;        // the stored doses of the 6 days before doseDay
    juce::String doseDay;                // the day the dose below counts for ("" before the first poll)
    double doseBaseline = 0.0;           // that day's stored dose when counting started
    double doseMark = 0.0;               // the guard's session dose then
    double lastSessionDose = 0.0;
    flub::PersonalProfile personalProfile;
    bool personalDirty = false;          // changed since the last write
    juce::String personalError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineController)
};
} // namespace flub::app
