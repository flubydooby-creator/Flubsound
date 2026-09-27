// Flubsound Pro - persistent application settings (juce::PropertiesFile).
//
// Stored as XML in the OS's per-user application-data folder:
//   Windows : %APPDATA%\Flubsound\Flubsound Pro.settings
//   macOS   : ~/Library/Application Support/Flubsound/Flubsound Pro.settings
//   Linux   : $XDG_CONFIG_HOME (~/.config)/Flubsound/Flubsound Pro.settings
//
// Contents: audio device state (AudioDeviceManager XML), per-strip last preset
// and full parameter state (both A/B banks), master enable, selected strip,
// the automatic overload response, device-input routing, hotkey chords, start
// minimised / close to tray / start with the OS, the app routing map
// (executable -> strip), the automatic profile rules (foreground app ->
// preset on a strip), the UI scale and theme, and the window position.
//
// Per-strip values are keyed by strip NAME (not index) so a changed strip
// layout does not shuffle profiles between strips.
//
// Message thread only. Writes are debounced (saved ~2 s after a change) and
// flushed by save() / on destruction.
#pragma once

#include "platform/PlatformServices.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>
#include <vector>

namespace flub::app
{
enum class HotkeyAction : int
{
    ToggleEnable = 1,
    ToggleMode,
    BoostUp,
    BoostDown,
    NextPreset,
    PreviousPreset
};

struct AppRoute
{
    juce::String executable; // case-insensitive match, e.g. "cs2.exe", "Spotify"
    juce::String stripName;  // target strip ("Game", "Music", ...)
};

/** Automatic profile switching (roadmap 2.5): while `executable` is the
    foreground application, the strip `stripName` plays `presetId` (see
    AutoProfileSwitcher / EngineController::pollForegroundApp). */
struct AutoProfileRule
{
    enum class Mode
    {
        Preset, // the preset's own Music / Gaming mode
        Music,
        Gaming
    };

    juce::String executable; // matched like AppRoute ("cs2.exe", "/usr/bin/foo"), or a macOS bundle id
    juce::String stripName;  // target strip ("Game", "Music", ...)
    juce::String presetId;   // "factory:..." / "user:..."
    Mode mode = Mode::Preset;
    bool restoreOnExit = false; // put the strip's previous preset back when the app leaves the foreground

    bool operator== (const AutoProfileRule&) const = default;
};

class AppSettings
{
public:
    /** Opens (or creates) the default settings file. */
    AppSettings();

    /** Uses a specific file; with persist == false nothing is ever written
        (headless screenshot runs, tests). */
    AppSettings (const juce::File& file, bool persist);

    ~AppSettings();

    juce::PropertiesFile& getPropertiesFile() noexcept { return *properties; }
    juce::File getFile() const { return properties->getFile(); }
    void save();

    // ---- Audio device ----------------------------------------------------------
    std::unique_ptr<juce::XmlElement> getDeviceState() const;
    void setDeviceState (const juce::XmlElement* state);

    /** Device input -> strip routing: mode "auto" enables it only when the input
        device looks like a loopback / virtual cable (never a microphone). */
    enum class DeviceInputMode { Automatic, On, Off };
    DeviceInputMode getDeviceInputMode() const;
    void setDeviceInputMode (DeviceInputMode mode);
    juce::String getDeviceInputStripName() const; // default "Game"
    void setDeviceInputStripName (const juce::String& stripName);
    /** Optional multi-strip map "Game=0;Music=8;Chat=10;System=12" (strip name =
        first device input channel). When set it replaces the single-strip
        routing above (e.g. Linux: one JACK / PipeWire monitor per strip). */
    juce::String getDeviceInputMap() const;
    void setDeviceInputMap (const juce::String& map);

    // ---- Engine ----------------------------------------------------------------
    bool getMasterEnabled() const;
    void setMasterEnabled (bool enabled);
    int getSelectedStrip() const;
    void setSelectedStrip (int strip);
    /** "Reduce processing load automatically when the CPU overloads" (default
        off): step the latency profile down on a sustained overload
        (AutoLoadReducer, EngineController::updateOverloadWatchdog). */
    bool getReduceLoadOnOverload() const;
    void setReduceLoadOnOverload (bool shouldReduce);

    // ---- Per-strip state -----------------------------------------------------------
    juce::String getLastPreset (const juce::String& stripName) const;
    void setLastPreset (const juce::String& stripName, const juce::String& presetId);
    /** Full A/B parameter state as JSON text (see EngineController). */
    juce::String getStripState (const juce::String& stripName) const;
    void setStripState (const juce::String& stripName, const juce::String& json);
    float getStripGainDb (const juce::String& stripName) const;
    void setStripGainDb (const juce::String& stripName, float gainDb);
    bool getStripMuted (const juce::String& stripName) const;
    void setStripMuted (const juce::String& stripName, bool muted);

    // ---- Hotkeys -------------------------------------------------------------------
    static flub::platform::KeyChord getDefaultHotkey (HotkeyAction action);
    static juce::String getHotkeyActionName (HotkeyAction action);
    static std::vector<HotkeyAction> getAllHotkeyActions();
    flub::platform::KeyChord getHotkey (HotkeyAction action) const;
    void setHotkey (HotkeyAction action, const flub::platform::KeyChord& chord);
    bool getHotkeysEnabled() const;
    void setHotkeysEnabled (bool enabled);

    /** "Ctrl+Alt+F", "Ctrl+Alt+Up", "Shift+Super+F5" <-> KeyChord (VK-style key
        codes: 'A'..'Z', '0'..'9', F1 = 0x70, arrows 0x25..0x28, Space 0x20).
        Implemented here (not KeyChord::toString) so settings work without the
        platform services. An empty / "None" string is an unassigned chord. */
    static juce::String chordToString (const flub::platform::KeyChord& chord);
    static bool chordFromString (const juce::String& text, flub::platform::KeyChord& chord);

    // ---- Window / tray ---------------------------------------------------------------
    bool getStartMinimised() const;
    void setStartMinimised (bool shouldStartMinimised);
    bool getCloseToTray() const;
    void setCloseToTray (bool shouldCloseToTray);
    /** Last known state of "Start Flubsound Pro when I sign in" (default off).
        The OS entry (platform::AutoStart) is the source of truth: Settings >
        General writes this after each change and re-syncs it from the OS
        whenever the page is shown, since the user can also remove the entry
        in the OS's own start-up settings. */
    bool getStartWithOs() const;
    void setStartWithOs (bool shouldStartWithOs);
    juce::String getWindowState() const;
    void setWindowState (const juce::String& state);

    /** Settings > General > UI scale, in percent: kUiScaleFollowSystem (0,
        the default: only the OS's display scaling applies) or 75 .. 200. */
    static constexpr int kUiScaleFollowSystem = 0, kUiScaleMinPercent = 75, kUiScaleMaxPercent = 200;
    /** <= 0 -> kUiScaleFollowSystem; anything else is clamped to 75 .. 200. */
    static int clampUiScalePercent (int percent);
    int getUiScalePercent() const;
    void setUiScalePercent (int percent);
    /** Settings > General > Theme: the high-contrast theme (default off). */
    bool getHighContrast() const;
    void setHighContrast (bool highContrast);

    /** The output device the user chose (e.g. a USB headset); restored when it
        reappears after being unplugged / powered off. */
    juce::String getPreferredOutputDevice() const;
    void setPreferredOutputDevice (const juce::String& name);

    // ---- App routing -------------------------------------------------------------------
    enum class RoutingMethod { Automatic, EndpointRouting, ProcessCapture, Disabled };
    RoutingMethod getRoutingMethod() const;
    void setRoutingMethod (RoutingMethod method);
    std::vector<AppRoute> getAppRoutes() const;
    void setAppRoutes (const std::vector<AppRoute>& routes);
    /** Output endpoint an app mapped to a strip is routed to (endpoint routing).
        Default: the strip's virtual endpoint - "Flubsound <Strip>" (Windows
        endpoint friendly name / macOS device name) or "flubsound_<strip>"
        (Linux PipeWire / PulseAudio null-sink name). */
    juce::String getStripEndpointId (const juce::String& stripName) const;
    void setStripEndpointId (const juce::String& stripName, const juce::String& endpointId);

    // ---- Automatic profiles ------------------------------------------------------------
    /** Master switch for the rules below (default on; rules only exist when the
        user added them). */
    bool getAutoProfilesEnabled() const;
    void setAutoProfilesEnabled (bool enabled);
    /** In priority order: the first rule matching the foreground app wins.
        Rules without an executable, strip or preset are dropped. */
    std::vector<AutoProfileRule> getAutoProfileRules() const;
    void setAutoProfileRules (const std::vector<AutoProfileRule>& rules);

private:
    static juce::PropertiesFile::Options defaultOptions();
    static juce::String stripKey (const juce::String& stripName, const char* field);

    std::unique_ptr<juce::PropertiesFile> properties;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppSettings)
};
} // namespace flub::app
