// Flubsound Pro - persistent application settings (juce::PropertiesFile).
//
// Stored as XML in the OS's per-user application-data folder:
//   Windows : %APPDATA%\Flubsound\Flubsound Pro.settings
//   macOS   : ~/Library/Application Support/Flubsound/Flubsound Pro.settings
//   Linux   : $XDG_CONFIG_HOME (~/.config)/Flubsound/Flubsound Pro.settings
//
// Contents: audio device state (AudioDeviceManager XML), per-strip last preset
// and full parameter state (both A/B banks), master enable, selected strip,
// the automatic overload response, device-input routing, hotkey chords, the
// hotkey strip and the hotkey failures the notice already named, start
// minimised / close to tray / start with the OS, the app routing map
// (executable -> strip), the automatic profile rules (foreground app ->
// preset on a strip), the device corrections (one per output endpoint,
// docs/11 E15), the per-endpoint headset enhancement switch (docs/11 E16),
// the UI scale and theme, the main window's view (Simple /
// Advanced, docs/11 E39), the window position, the loudness contour's
// system-volume follow and reference volume (docs/11 E32), the loopback
// pairs the feedback-loop guard allows (docs/11 E51), Tournament mode
// (docs/11 E55), the hearing guard's headset sensitivity per endpoint, its
// listening-level cap and the estimated dose per day (docs/11 E32 (c)) and
// Smart macros per strip (docs/11 E34). The personal hearing profile (docs/11
// E33) is its own file next to this one (EngineController).
//
// Per-strip values are keyed by strip NAME (not index) so a changed strip
// layout does not shuffle profiles between strips.
//
// File integrity and schema (docs/11 E52 Phase B):
// * "settings.schemaVersion" (kSchemaVersion): a file without it is schema 1.
//   Schema 2 stores preset references (a strip's last preset, automatic
//   profile rules) as canonical PresetManager ids, i.e. the preset's uuid, so
//   a renamed preset file keeps its rules. The 1 -> 2 step needs the preset
//   library, so it runs once through migratePresetReferences (called by
//   EngineController with PresetManager::getLegacyIdAliases()); a newer
//   schema is read as it is and never lowered.
// * A file that does not parse (truncated, garbage, not a PROPERTIES
//   document, a bad schemaVersion: isValidSettingsFile) is never overwritten:
//   it is renamed to "<file>.corrupt-<yyyymmdd-hhmmss>" and the newest valid
//   "<file>.bak1" .. ".bak3" is restored in its place (getRecovery()); with no
//   valid backup the app starts from defaults. Without that, the next
//   autosave replaced a damaged file with an empty one, which silently wiped
//   routing, rules and hotkeys.
// * Backups rotate once per start: a valid file that differs from .bak1 is
//   copied to .bak1 (.bak1 -> .bak2 -> .bak3, the oldest dropped), so the
//   backups are the last three distinct files that loaded.
// Nothing of this runs with persist == false (nothing is ever written).
//
// Message thread only. Writes are debounced (saved ~2 s after a change) and
// flushed by save() / on destruction.
#pragma once

#include "platform/PlatformServices.h"

#include <juce_data_structures/juce_data_structures.h>

#include <map>
#include <memory>
#include <optional>
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
    PreviousPreset,
    // docs/11 E56 Phase A (the values are the ids registered with the OS and
    // persisted per key name, so new actions only ever append):
    ToggleFocus,   // latched Footsteps override (Macro 1 at 100 %)
    ChatMixToChat, // ChatMix balance one step towards Chat
    ChatMixToGame, // ... and towards Game
    ToggleNight,   // latched night-listening override (Auto Level + dynamics)
    ToggleBypass   // bypass of the hotkey strip only (loudness matched when set)
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
    juce::String presetId;   // a PresetManager id: the preset's uuid (docs/11 E52), "factory:..." / "user:..." without one
    Mode mode = Mode::Preset;
    bool restoreOnExit = false; // put the strip's previous preset back when the app leaves the foreground

    bool operator== (const AutoProfileRule&) const = default;
};

/** A headphone / speaker correction stored for one output endpoint (docs/11
    E15). The curve is kept as Equalizer APO text (flub::eqtext::format), so
    the settings file stays readable and a curve survives format changes. */
struct DeviceCorrectionEntry
{
    juce::String endpoint;  // the output device name it was last stored under (the key before docs/11 E51)
    juce::String name;      // what the user imported, e.g. "HD 600 ParametricEQ.txt"
    bool enabled = true;
    juce::String curveText; // APO / AutoEQ ParametricEQ syntax
    // docs/11 E51: the endpoint's identity, as DeviceEndpointEntry; empty
    // when unknown (files from before E51 carry the name only).
    juce::String endpointId, hardwareId;

    flub::platform::OutputEndpointIdentity identity() const
    {
        flub::platform::OutputEndpointIdentity e;
        e.id = endpointId.toStdString();
        e.name = endpoint.toStdString();
        e.hardwareId = hardwareId.toStdString();
        return e;
    }
    bool operator== (const DeviceCorrectionEntry&) const = default;
};

/** The user's settings for one output endpoint that are not a correction
    curve (docs/11 E16). Keyed by the endpoint's identity (docs/11 E51: the
    OS's endpoint id and the hardware id, with the name as the fallback), so
    an entry follows its device through a rename, a re-plug into another USB
    port ("Headset Earphone (2- Stealth 700 Gen 2)") and platforms without
    ids (the name alone, without Windows' instance number). */
struct DeviceEndpointEntry
{
    juce::String endpointId; // platform::OutputEndpointIdentity::id; empty when unknown
    juce::String hardwareId; // platform::OutputEndpointIdentity::hardwareId; empty when unknown
    juce::String name;       // the output device name it was last stored under
    bool onboardEnhancement = false; // "Headset enhancement is ON": the on-board DSP cap (EngineController)

    flub::platform::OutputEndpointIdentity identity() const
    {
        flub::platform::OutputEndpointIdentity e;
        e.id = endpointId.toStdString();
        e.name = name.toStdString();
        e.hardwareId = hardwareId.toStdString();
        return e;
    }
    bool operator== (const DeviceEndpointEntry&) const = default;
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

    // ---- File integrity and schema (docs/11 E52, see the file comment) ----------------
    static constexpr int kSchemaVersion = 2;
    /** The file's schema: 1 when it has no "settings.schemaVersion". */
    int getSchemaVersion() const;
    /** True when the file loaded (or did not exist yet). */
    bool isValidFile() const { return properties->isValidFile(); }
    /** True if `file` parses as a settings file (a PROPERTIES document whose
        schemaVersion, if present, is a positive integer). False for a missing
        or empty file. */
    static bool isValidSettingsFile (const juce::File& file);
    /** "<settings file>.bak<index>", index 1 (newest) .. kNumBackups. */
    static constexpr int kNumBackups = 3;
    static juce::File getBackupFile (const juce::File& settingsFile, int index);

    /** What opening the file found. A damaged file was moved to `quarantined`
        and backup `restoredFromBackup` (1 .. 3; 0: none was valid, the
        settings start from defaults) put in its place. For a notice in the UI. */
    struct Recovery
    {
        juce::File quarantined;
        int restoredFromBackup = 0;

        bool wasDamaged() const { return quarantined != juce::File(); }
    };
    const Recovery& getRecovery() const noexcept { return recovery; }

    /** The one-time schema 1 -> 2 step: every stored preset reference (each
        strip's last preset, each automatic profile rule) found as a key of
        `aliases` (legacy id -> canonical id) is replaced by its value; others
        are kept (a deleted preset stays "missing"). Then records schema 2.
        Does nothing for schema 2 or newer. Returns the references changed. */
    int migratePresetReferences (const std::map<juce::String, juce::String>& aliases);

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
        routing above (e.g. Linux: one JACK / PipeWire monitor per strip).
        Settings > Routing edits it (EngineController::setDeviceInputMap). */
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
    /** The short list Settings > Hotkeys' "Pick a free combination" tries, in
        order (R4.4): the default, then Ctrl+Alt+Shift with the default key,
        with a second key per action and with F1 - F11 (the action's place in
        getAllHotkeyActions). The second keys are distinct across actions and
        from every default key. No Ctrl+Alt+letter fallback: that is AltGr+
        letter on Windows, which types a character on many layouts. */
    static std::vector<flub::platform::KeyChord> getAlternativeHotkeys (HotkeyAction action);
    /** Same modifiers and key (two unassigned chords are the same). */
    static bool sameChord (const flub::platform::KeyChord& a, const flub::platform::KeyChord& b) noexcept;
    static juce::String getHotkeyActionName (HotkeyAction action);
    static std::vector<HotkeyAction> getAllHotkeyActions();
    /** The saved chord, else the default (so a user who never changed a
        hotkey follows a new default, and a saved one is kept). */
    flub::platform::KeyChord getHotkey (HotkeyAction action) const;
    void setHotkey (HotkeyAction action, const flub::platform::KeyChord& chord);
    /** True when the settings hold a chord (or "None") for the action. */
    bool hasSavedHotkey (HotkeyAction action) const;
    /** Whether the notice under the header already named this action's
        failure with this chord (R4.4: a failure is announced once; the entry
        goes when the action registers or gets another chord). */
    bool isHotkeyFailureAnnounced (HotkeyAction action, const juce::String& chord) const;
    void setHotkeyFailureAnnounced (HotkeyAction action, const juce::String& chord, bool announced);
    bool getHotkeysEnabled() const;
    void setHotkeysEnabled (bool enabled);
    /** The strip the strip-level hotkeys act on (docs/11 E56): never the GUI
        selection, so a mid-match Boost+ cannot land on the Music strip because
        it was the last one clicked. Default "Game". An active automatic
        profile's strip takes precedence (EngineController::getHotkeyStrip). */
    juce::String getHotkeyStripName() const;
    void setHotkeyStripName (const juce::String& stripName);

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
    /** The main window's view (docs/11 E39): Simple (the default: mode,
        preset, Boost, the five macros, what is active now, the headset and one
        loudness meter) or Advanced (the full window with routing, analyser, EQ
        and module rack). The last choice is kept. */
    enum class MainView { Simple, Advanced };
    MainView getMainView() const;
    void setMainView (MainView view);

    // ---- Preset browser (docs/11 E40) --------------------------------------------------
    /** Favourite presets by id (the uuid, PresetManager), in the order they
        were marked. An id that no longer names a preset is kept (a user
        preset folder on a disconnected drive) and simply not shown. */
    juce::StringArray getFavouritePresets() const;
    bool isFavouritePreset (const juce::String& presetId) const;
    void setFavouritePreset (const juce::String& presetId, bool favourite);
    /** Presets the user picked (header or browser), newest first, at most
        kMaxRecentPresets; picking one again moves it to the front. */
    static constexpr int kMaxRecentPresets = 8;
    juce::StringArray getRecentPresets() const;
    void addRecentPreset (const juce::String& presetId);
    /** The browser's switches: play the selected preset while browsing
        (default on) and match its loudness to the current sound (default on). */
    bool getPresetPreview() const;
    void setPresetPreview (bool preview);
    bool getPresetPreviewMatched() const;
    void setPresetPreviewMatched (bool matched);

    // ---- Comparisons (docs/11 E37) -------------------------------------------------------
    /** Loudness-matched A/B and module listen (default on): the louder side
        of a comparison is turned down to the quieter one. */
    bool getComparisonMatched() const;
    void setComparisonMatched (bool matched);

    /** The output device the user chose (e.g. a USB headset); restored when it
        reappears after being unplugged / powered off. */
    juce::String getPreferredOutputDevice() const;
    void setPreferredOutputDevice (const juce::String& name);
    /** The same with its identity (docs/11 E51: endpoint id, hardware id;
        empty in settings from before E51, which stored the name only).
        onboardEnhancement is not used. */
    DeviceEndpointEntry getPreferredOutput() const;
    void setPreferredOutput (const DeviceEndpointEntry& output);
    /** Settings > Audio > "Follow the system default output" (default off;
        docs/11 E51, AudioEngineHost::setFollowSystemDefault). */
    bool getFollowSystemDefaultOutput() const;
    void setFollowSystemDefaultOutput (bool follow);

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
    /** docs/11 E47 (R4.5), Windows: "Move the app's own sound away
        automatically" (routing.moveOriginalAway, default off): an app captured
        while it plays to Flubsound's output device is moved to the silent
        device below instead of being held back by the doubling guard. */
    bool getMoveOriginalAway() const;
    void setMoveOriginalAway (bool shouldMove);
    /** The silent device chosen in Settings > Routing (an output endpoint id
        as the router lists it, and its name for when it is unplugged); empty
        = automatic (routing.silentEndpoint / routing.silentEndpointName). An
        empty id removes both. */
    juce::String getSilentEndpointId() const;
    juce::String getSilentEndpointName() const;
    void setSilentEndpoint (const juce::String& endpointId, const juce::String& name);

    // ---- Automatic profiles ------------------------------------------------------------
    /** Master switch for the rules below (default on; rules only exist when the
        user added them). */
    bool getAutoProfilesEnabled() const;
    void setAutoProfilesEnabled (bool enabled);
    /** In priority order: the first rule matching the foreground app wins.
        Rules without an executable, strip or preset are dropped. */
    std::vector<AutoProfileRule> getAutoProfileRules() const;
    void setAutoProfileRules (const std::vector<AutoProfileRule>& rules);

    // ---- Device correction (docs/11 E15) ------------------------------------------------
    /** Every stored correction, one per endpoint. Never part of a preset, a
        strip state or an automatic profile. */
    std::vector<DeviceCorrectionEntry> getDeviceCorrections() const;
    /** The endpoint's correction, found by its identity like
        findDeviceEndpoint (docs/11 E51: the same endpoint id, else the same
        hardware id and name without Windows' instance number, else that
        name; entries from before E51 carry the name only); nullopt if none. */
    std::optional<DeviceCorrectionEntry> findDeviceCorrection (const flub::platform::OutputEndpointIdentity& endpoint) const;
    /** findDeviceCorrection by an output device name alone. */
    std::optional<DeviceCorrectionEntry> getDeviceCorrection (const juce::String& endpointName) const;
    /** Stores `entry` in place of the entry that matches its identity (which
        then takes entry's ids and name), else adds it. Ignored without a
        name and an id. */
    void setDeviceCorrection (const DeviceCorrectionEntry& entry);
    /** Removes the entry that matches `endpoint` (as findDeviceCorrection). */
    void removeDeviceCorrection (const flub::platform::OutputEndpointIdentity& endpoint);

    // ---- Per-endpoint settings (docs/11 E16) --------------------------------------------
    std::vector<DeviceEndpointEntry> getDeviceEndpoints() const;
    /** The entry stored for `endpoint`: the best match (platform::
        AudioDeviceWatcher::findEndpoint: the same endpoint id, else the same
        hardware id and name without the instance number, else that name);
        nullopt if none. */
    std::optional<DeviceEndpointEntry> findDeviceEndpoint (const flub::platform::OutputEndpointIdentity& endpoint) const;
    /** Stores `entry` in place of the entry that matches it (which then
        takes entry's id, hardware id and name), else adds it. Ignored
        without a name and an id. */
    void setDeviceEndpoint (const DeviceEndpointEntry& entry);

    // ---- Loudness contour and the system volume (docs/11 E32) ---------------------------
    /** Settings > Processing > "Follow the system volume" (default off): the
        app reads the output endpoint's OS volume a few times a second and the
        loudness contour (contour.on, per preset) follows it, relative to the
        reference volume below (EngineController::pollEndpointVolume). Off:
        the contour follows its contour.level parameter alone. */
    bool getContourFollowsVolume() const;
    void setContourFollowsVolume (bool follow);
    /** "This is my reference volume": the OS volume (dB) at which the strips
        sound as their presets intend; below it the contour lifts the bass.
        nullopt until the user (or switching the follow on) sets one. */
    std::optional<float> getContourReferenceVolumeDb() const;
    void setContourReferenceVolumeDb (float volumeDb);

    // ---- Feedback-loop guard override (docs/11 E51) ----------------------------------------
    /** Input / output device pairs the user allowed although they look like a
        loopback pair (a deliberate cable monitor): the guard does not mute
        them (AudioEngineHost::allowLoopbackPair). Names compare ignoring case;
        empty names are dropped. */
    struct LoopbackPair
    {
        juce::String input, output;
        bool operator== (const LoopbackPair&) const = default;
    };
    std::vector<LoopbackPair> getAllowedLoopbackPairs() const;
    void setAllowedLoopbackPairs (const std::vector<LoopbackPair>& pairs);

    // ---- Tournament mode (docs/11 E55) ---------------------------------------------------
    /** The user's switch (tray, header badge; default off). While Tournament
        mode is on, per-app routing is frozen, automatic profiles hold and the
        foreground application is not polled (EngineController). */
    bool getTournamentMode() const;
    void setTournamentMode (bool on);
    /** Switch Tournament mode on by itself while a known anti-cheat service
        runs (platform::AntiCheatServices; default on); the user's switch
        applies again once the services stop. */
    bool getTournamentAuto() const;
    void setTournamentAuto (bool automatic);

    // ---- Voice chat (docs/11 E22) ------------------------------------------------------
    /** "Duck game under voice chat" (default off) and its depth in dB
        (3 - 6, default 4.5): EngineController::setChatDuck. */
    bool getChatDuck() const;
    void setChatDuck (bool on);
    float getChatDuckDepthDb() const;
    void setChatDuckDepthDb (float depthDb);

    // ---- Hearing (docs/11 E32 (c), E34; Settings > Hearing) -----------------------------
    /** The listener's own headset sensitivity (dB SPL of a 0 dBFS sine at
        full volume), stored per output endpoint (matched as
        findDeviceEndpoint matches); it wins over the device profile's
        figure (HearingGuard::chooseSensitivity). nullopt: none stored. */
    std::optional<float> findHearingSensitivity (const flub::platform::OutputEndpointIdentity& endpoint) const;
    /** Stores (clamped to HearingGuard's 60 .. 150 dB SPL) or, with nullopt,
        removes the figure of `endpoint`. Ignored without a name and an id. */
    void setHearingSensitivity (const flub::platform::OutputEndpointIdentity& endpoint, std::optional<float> dbSpl);
    /** The listening-level cap (off by default) and its level (60 .. 100
        dB(A), 85 by default). */
    bool getHearingCapEnabled() const;
    void setHearingCapEnabled (bool on);
    float getHearingCapDbA() const;
    void setHearingCapDbA (float dbA);
    /** The estimated dose per calendar day ("2026-09-30", local time) as a
        fraction of the weekly allowance, newest first; days older than
        kDoseDaysKept before `today` are dropped when a day is stored. */
    struct DailyDose
    {
        juce::String day;
        double fraction = 0.0;
        bool operator== (const DailyDose&) const = default;
    };
    static constexpr int kDoseDaysKept = 7;
    std::vector<DailyDose> getDailyDoses() const;
    void setDailyDose (const juce::String& day, double fraction);
    /** Smart macros (docs/11 E34, ProcessingChain::setSmartMacros) per strip
        name; off by default. */
    bool getSmartMacros (const juce::String& stripName) const;
    void setSmartMacros (const juce::String& stripName, bool on);

private:
    static juce::PropertiesFile::Options defaultOptions();
    static juce::String stripKey (const juce::String& stripName, const char* field);
    void open (const juce::File& file, juce::PropertiesFile::Options options);

    std::unique_ptr<juce::PropertiesFile> properties;
    Recovery recovery;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppSettings)
};
} // namespace flub::app
