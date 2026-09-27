// Flubsound Pro - EngineController: the ONE object the UI talks to.
//
// Owns (in construction order): AppSettings, AudioEngineHost (device +
// MixEngine), PresetManager, AppRouting. Everything here is MESSAGE THREAD
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
// Master       isEnabled()/setEnabled() = BypassAll on every strip (both
//              banks). getMasterGainReductionDb() = master safety limiter.
// Mode/Boost   getMode/setMode/toggleMode, getBoost/setBoost/nudgeBoost
//              (strip = -1 means the selected strip). Macro labels:
//              getMacroName(mode, 0..4).
// Presets      getPresetManager() for the list; loadPreset(id, strip) /
//              nextPreset() / previousPreset() / saveUserPreset() here so
//              settings and listeners are updated. A/B: getActiveBank,
//              setActiveBank, toggleAB, copyActiveToOtherBank.
// Device       getDeviceManager() (e.g. for juce::AudioDeviceSelectorComponent;
//              the selection is persisted automatically), getLatencyInfo(),
//              getStatus() (CPU, xruns), getDeviceInputStrip(),
//              getOverloadState() (CPU-overload watchdog), getCaptureStreams()
//              (per-app capture FIFO statistics).
// Routing      getRouting() (per-app routing, executable -> strip).
// Settings     getSettings() (tray / start-up / hotkeys ...).
// Listening    addListener(); Listener::engineControllerChanged(Change) is
//              called on the message thread for state the UI cannot poll
//              cheaply (preset loaded, strip selected, engine rebuilt...).
#pragma once

#include "AppRouting.h"
#include "AudioEngineHost.h"
#include "OverloadWatchdog.h"
#include "presets/PresetManager.h"
#include "settings/AppSettings.h"

#include "flub/engine/DeviceProfiles.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include <array>
#include <memory>
#include <vector>

namespace flub::app
{
class EngineController final : private juce::Timer, private juce::ChangeListener
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
    void setStripGainDb (int strip, float gainDb);
    float getStripGainDb (int strip) const noexcept { return host->getStripGainDb (strip); }
    void setStripMuted (int strip, bool muted);
    bool isStripMuted (int strip) const noexcept { return host->isStripMuted (strip); }
    float getMasterGainReductionDb() noexcept { return host->getMixEngine().getMasterGainReductionDb(); }

    // ---- Master enable / mode / boost (strip -1 = selected strip) ----------------
    bool isEnabled() const noexcept { return enabled; }
    void setEnabled (bool shouldBeEnabled);
    void toggleEnabled() { setEnabled (! enabled); }

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

    /** CPU-overload watchdog (OverloadWatchdog): sustained load >= 90 % or a
        burst of xruns / overrunning callbacks. Notify only: the header shows
        it and the episodes are counted for the session; nothing in the
        engine is changed (docs/01-architecture.md §7). */
    const OverloadWatchdog::State& getOverloadState() const noexcept { return overloadWatchdog.getState(); }
    /** One watchdog poll. The controller's timer calls it at 2 Hz with
        getStatus(); tests feed statuses directly. Broadcasts Change::Device
        when an overload starts or ends. */
    void updateOverloadWatchdog (const EngineStatus& status);

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

    /** Device input -> strip policy (see AppSettings::DeviceInputMode). */
    void setDeviceInputMode (AppSettings::DeviceInputMode mode);
    void setDeviceInputStrip (int strip);
    int getDeviceInputStrip() const noexcept { return host->getDeviceInputStrip(); }
    /** Heuristic: virtual cables / loopback drivers / monitor sources. */
    static bool looksLikeLoopbackDevice (const juce::String& inputDeviceName);

    /** Replaces the strip layout (brief dropout). Stores of surviving strips
        are kept; UI must re-fetch everything (Change::Engine). */
    void setStripLayout (const std::vector<flub::StripConfig>& newLayout);

    // ---- Settings / routing ------------------------------------------------------------------
    AppSettings& getSettings() noexcept { return *settings; }
    AppRouting& getRouting() noexcept { return *routing; }
    /** Writes strip states / device state / settings now. */
    void saveState();

    // ---- Headless ------------------------------------------------------------------------------
    /** Runs audio through the engine without a device (screenshot mode). */
    void renderOffline (StripSignalSource& source, int numSamples);

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
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

    Options options;
    std::unique_ptr<AppSettings> settings;
    std::unique_ptr<AudioEngineHost> host;
    std::unique_ptr<PresetManager> presets;
    std::unique_ptr<AppRouting> routing;
    juce::ListenerList<Listener> listeners;

    bool enabled = true, isShutDown = false;
    int selectedStrip = 0, timerTicks = 0;
    std::array<uint32_t, AudioEngineHost::kMaxStrips> persistedVersions {};
    juce::String lastDeviceError;

    flub::device::Database deviceProfiles;
    flub::device::Match deviceMatch;
    flub::device::Advice deviceAdvice;
    juce::String currentOutputName, preferredOutputName;
    juce::String simulatedOutputName; // headless only, see simulateOutputDevice()
    double simulatedSampleRate = 48000.0;
    int simulatedOutputChannels = 2;
    bool preferredMissing = false, restoringPreferred = false;
    bool adviceForGaming = false; // mode deviceAdvice was computed for (see notify())

    OverloadWatchdog overloadWatchdog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineController)
};
} // namespace flub::app
