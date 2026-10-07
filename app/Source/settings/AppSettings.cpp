#include "AppSettings.h"
#include "UserDataFolder.h"

#include <algorithm>
#include <cmath>

namespace flub::app
{
using flub::platform::KeyChord;

namespace
{
namespace Keys
{
constexpr const char* deviceState = "device.state";
constexpr const char* deviceInputMode = "deviceInput.mode";
constexpr const char* deviceInputStrip = "deviceInput.strip";
constexpr const char* deviceInputMap = "deviceInput.map";
constexpr const char* masterEnabled = "engine.enabled";
constexpr const char* selectedStrip = "engine.selectedStrip";
constexpr const char* reduceLoadOnOverload = "engine.reduceLoadOnOverload";
constexpr const char* hotkeysEnabled = "hotkeys.enabled";
constexpr const char* hotkeyStrip = "hotkeys.strip";
constexpr const char* startMinimised = "ui.startMinimised";
constexpr const char* closeToTray = "ui.closeToTray";
constexpr const char* startWithOs = "ui.startWithOs";
constexpr const char* windowState = "ui.windowState";
constexpr const char* uiScale = "ui.scalePercent";
constexpr const char* uiTheme = "ui.theme";
constexpr const char* uiView = "ui.view";
constexpr const char* presetFavourites = "presets.favourites";
constexpr const char* presetRecent = "presets.recent";
constexpr const char* presetPreview = "presets.preview";
constexpr const char* presetPreviewMatched = "presets.previewMatched";
constexpr const char* comparisonMatched = "compare.matched";
constexpr const char* preferredOutputDevice = "device.preferredOutput";
constexpr const char* preferredOutputId = "device.preferredOutputId";
constexpr const char* preferredOutputHardwareId = "device.preferredOutputHardwareId";
constexpr const char* followSystemDefault = "device.followSystemDefault";
constexpr const char* routingMethod = "routing.method";
constexpr const char* routingMap = "routing.map";
constexpr const char* routingMoveAway = "routing.moveOriginalAway";
constexpr const char* routingSilentEndpoint = "routing.silentEndpoint";
constexpr const char* routingSilentEndpointName = "routing.silentEndpointName";
constexpr const char* autoProfilesEnabled = "autoProfile.enabled";
constexpr const char* autoProfileRules = "autoProfile.rules";
constexpr const char* deviceCorrections = "device.corrections";
constexpr const char* deviceEndpoints = "device.endpoints";
constexpr const char* contourFollowVolume = "contour.followVolume";
constexpr const char* contourReferenceVolume = "contour.referenceVolumeDb";
constexpr const char* allowedLoopbackPairs = "device.allowedLoopbackPairs";
constexpr const char* tournamentMode = "tournament.mode";
constexpr const char* tournamentAuto = "tournament.auto";
constexpr const char* chatDuck = "chat.duck";
constexpr const char* chatDuckDepth = "chat.duckDepthDb";
constexpr const char* schemaVersion = "settings.schemaVersion";
} // namespace Keys

// PropertiesFile's XML layout (juce_PropertiesFile.cpp PropertyFileConstants).
constexpr const char* kFileTag = "PROPERTIES";
constexpr const char* kValueTag = "VALUE";

/** A schemaVersion value: a positive integer ("2"); 0 if it is not one. */
int parseSchemaVersion (const juce::String& text)
{
    const auto t = text.trim();
    return t.isNotEmpty() && t.length() <= 6 && t.containsOnly ("0123456789") ? t.getIntValue() : 0;
}

/** "<file>.corrupt-<yyyymmdd-hhmmss>" next to the file (never an existing one). */
juce::File quarantineFileFor (const juce::File& file)
{
    const auto name = file.getFileName() + ".corrupt-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    auto target = file.getSiblingFile (name);
    for (int i = 2; target.exists(); ++i) // (getNonexistentSibling would number the name before ".corrupt-...")
        target = file.getSiblingFile (name + "-" + juce::String (i));
    return target;
}

struct NamedKey
{
    const char* name;
    uint32_t code;
};

// VK-style codes (Windows virtual-key values; the platform layer maps them).
constexpr NamedKey kNamedKeys[] = {
    { "Space", 0x20 },  { "PageUp", 0x21 }, { "PageDown", 0x22 }, { "End", 0x23 },    { "Home", 0x24 },
    { "Left", 0x25 },   { "Up", 0x26 },     { "Right", 0x27 },     { "Down", 0x28 },   { "Insert", 0x2D },
    { "Delete", 0x2E },
};

juce::String keyCodeToString (uint32_t code)
{
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
        return juce::String::charToString (static_cast<juce::juce_wchar> (code));
    if (code >= 0x70 && code <= 0x87)
        return "F" + juce::String (static_cast<int> (code - 0x70 + 1));
    for (const auto& k : kNamedKeys)
        if (k.code == code)
            return k.name;
    return "0x" + juce::String::toHexString (static_cast<int> (code)).toUpperCase();
}

bool keyCodeFromString (const juce::String& raw, uint32_t& code)
{
    const auto token = raw.trim();
    if (token.isEmpty())
        return false;

    if (token.length() == 1)
    {
        const auto ch = juce::CharacterFunctions::toUpperCase (token[0]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
        {
            code = static_cast<uint32_t> (ch);
            return true;
        }
        return false;
    }

    if ((token[0] == 'F' || token[0] == 'f') && token.substring (1).containsOnly ("0123456789"))
    {
        const int n = token.substring (1).getIntValue();
        if (n >= 1 && n <= 24)
        {
            code = static_cast<uint32_t> (0x70 + n - 1);
            return true;
        }
        return false;
    }

    for (const auto& k : kNamedKeys)
        if (token.equalsIgnoreCase (k.name))
        {
            code = k.code;
            return true;
        }

    if (token.startsWithIgnoreCase ("0x") && token.length() > 2)
    {
        const int value = token.substring (2).getHexValue32();
        if (value > 0 && value < 0x100)
        {
            code = static_cast<uint32_t> (value);
            return true;
        }
    }
    return false;
}

const char* hotkeySettingKey (HotkeyAction action)
{
    switch (action)
    {
        case HotkeyAction::ToggleEnable: return "hotkey.toggleEnable";
        case HotkeyAction::ToggleMode: return "hotkey.toggleMode";
        case HotkeyAction::BoostUp: return "hotkey.boostUp";
        case HotkeyAction::BoostDown: return "hotkey.boostDown";
        case HotkeyAction::NextPreset: return "hotkey.nextPreset";
        case HotkeyAction::PreviousPreset: return "hotkey.previousPreset";
        case HotkeyAction::ToggleFocus: return "hotkey.toggleFocus";
        case HotkeyAction::ChatMixToChat: return "hotkey.chatMixToChat";
        case HotkeyAction::ChatMixToGame: return "hotkey.chatMixToGame";
        case HotkeyAction::ToggleNight: return "hotkey.toggleNight";
        case HotkeyAction::ToggleBypass: return "hotkey.toggleBypass";
    }
    return "hotkey.unknown";
}
} // namespace

// =============================================================================
juce::PropertiesFile::Options AppSettings::defaultOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "Flubsound Pro";
    options.filenameSuffix = ".settings";
    options.folderName = "Flubsound";
    options.osxLibrarySubFolder = "Application Support";
    options.commonToAllUsers = false;
    options.ignoreCaseOfKeyNames = false;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = 2000;
    return options;
}

AppSettings::AppSettings()
{
   #if JUCE_LINUX || JUCE_BSD
    // JUCE's default on Linux is ~/<folderName>; follow XDG instead
    // ($XDG_CONFIG_HOME or ~/.config), next to the user presets.
    const auto file = userDataFolder().getChildFile ("Flubsound Pro.settings");
   #else
    const auto file = defaultOptions().getDefaultFile();
   #endif
    open (file, defaultOptions());
}

AppSettings::AppSettings (const juce::File& file, bool persist)
{
    auto options = defaultOptions();
    options.doNotSave = ! persist;
    open (file, options);
}

void AppSettings::open (const juce::File& file, juce::PropertiesFile::Options options)
{
    const bool persist = ! options.doNotSave && file != juce::File();
    if (persist && file.existsAsFile() && ! isValidSettingsFile (file))
    {
        // Quarantine, never overwrite: the next autosave would otherwise
        // replace the damaged file with an empty one. Then the newest valid
        // backup takes its place.
        const auto quarantine = quarantineFileFor (file);
        if (file.moveFileTo (quarantine))
        {
            recovery.quarantined = quarantine;
            for (int i = 1; i <= kNumBackups; ++i)
            {
                const auto backup = getBackupFile (file, i);
                if (isValidSettingsFile (backup) && backup.copyFileTo (file))
                {
                    recovery.restoredFromBackup = i;
                    break;
                }
            }
        }
        else
        {
            options.doNotSave = true; // cannot be moved aside: run on defaults, but never write over it
        }
    }

    properties = std::make_unique<juce::PropertiesFile> (file, options);

    // One backup per start, of a file that loaded and differs from the newest
    // backup (restarting without a change must not push out older backups).
    // Best effort: a failed copy only costs a backup.
    if (persist && ! options.doNotSave && file.existsAsFile() && properties->isValidFile() && isValidSettingsFile (file))
    {
        const auto newest = getBackupFile (file, 1);
        if (! (newest.existsAsFile() && newest.hasIdenticalContentTo (file)))
        {
            getBackupFile (file, kNumBackups).deleteFile();
            for (int i = kNumBackups - 1; i >= 1; --i)
                if (const auto from = getBackupFile (file, i); from.existsAsFile())
                    from.moveFileTo (getBackupFile (file, i + 1));
            file.copyFileTo (newest);
        }
    }
}

AppSettings::~AppSettings()
{
    save();
}

void AppSettings::save()
{
    properties->saveIfNeeded();
}

juce::String AppSettings::stripKey (const juce::String& stripName, const char* field)
{
    return "strip." + stripName.removeCharacters (" .") + "." + field;
}

// ---- File integrity and schema -------------------------------------------------------
int AppSettings::getSchemaVersion() const
{
    return properties->containsKey (Keys::schemaVersion) ? std::max (1, parseSchemaVersion (properties->getValue (Keys::schemaVersion))) : 1;
}

bool AppSettings::isValidSettingsFile (const juce::File& file)
{
    if (! file.existsAsFile() || file.getSize() == 0)
        return false;
    const auto doc = juce::parseXMLIfTagMatches (file, kFileTag);
    if (doc == nullptr)
        return false;
    for (auto* e : doc->getChildWithTagNameIterator (kValueTag))
        if (e->getStringAttribute ("name") == Keys::schemaVersion)
            return parseSchemaVersion (e->getStringAttribute ("val")) > 0;
    return true;
}

juce::File AppSettings::getBackupFile (const juce::File& settingsFile, int index)
{
    return settingsFile.getSiblingFile (settingsFile.getFileName() + ".bak" + juce::String (index));
}

int AppSettings::migratePresetReferences (const std::map<juce::String, juce::String>& aliases)
{
    if (getSchemaVersion() >= kSchemaVersion)
        return 0;

    const auto canonical = [&aliases] (const juce::String& id)
    {
        const auto it = aliases.find (id);
        return it != aliases.end() ? it->second : id;
    };

    int changed = 0;
    const auto keys = properties->getAllProperties().getAllKeys(); // a copy: the loop writes values
    for (const auto& key : keys)
    {
        if (! (key.startsWith ("strip.") && key.endsWith (".preset")))
            continue;
        const auto id = properties->getValue (key);
        if (const auto to = canonical (id); to != id)
        {
            properties->setValue (key, to);
            ++changed;
        }
    }

    // Rewritten as read (the RULE attributes), so a rule this build would
    // drop as incomplete is kept for a later version to judge.
    if (auto xml = properties->getXmlValue (Keys::autoProfileRules))
    {
        int changedRules = 0;
        for (auto* e : xml->getChildWithTagNameIterator ("RULE"))
        {
            const auto id = e->getStringAttribute ("preset").trim();
            if (const auto to = canonical (id); to != id)
            {
                e->setAttribute ("preset", to);
                ++changedRules;
            }
        }
        if (changedRules > 0)
            properties->setValue (Keys::autoProfileRules, xml.get());
        changed += changedRules;
    }

    properties->setValue (Keys::schemaVersion, kSchemaVersion);
    return changed;
}

// ---- Audio device ------------------------------------------------------------------
std::unique_ptr<juce::XmlElement> AppSettings::getDeviceState() const
{
    return properties->getXmlValue (Keys::deviceState);
}

void AppSettings::setDeviceState (const juce::XmlElement* state)
{
    if (state != nullptr)
        properties->setValue (Keys::deviceState, state);
    else
        properties->removeValue (Keys::deviceState);
}

AppSettings::DeviceInputMode AppSettings::getDeviceInputMode() const
{
    const auto v = properties->getValue (Keys::deviceInputMode, "auto");
    if (v == "on")
        return DeviceInputMode::On;
    if (v == "off")
        return DeviceInputMode::Off;
    return DeviceInputMode::Automatic;
}

void AppSettings::setDeviceInputMode (DeviceInputMode mode)
{
    properties->setValue (Keys::deviceInputMode, mode == DeviceInputMode::On ? "on" : (mode == DeviceInputMode::Off ? "off" : "auto"));
}

juce::String AppSettings::getDeviceInputStripName() const
{
    return properties->getValue (Keys::deviceInputStrip, "Game");
}

void AppSettings::setDeviceInputStripName (const juce::String& stripName)
{
    properties->setValue (Keys::deviceInputStrip, stripName);
}

juce::String AppSettings::getDeviceInputMap() const
{
    return properties->getValue (Keys::deviceInputMap);
}

void AppSettings::setDeviceInputMap (const juce::String& map)
{
    properties->setValue (Keys::deviceInputMap, map);
}

// ---- Engine ------------------------------------------------------------------------------
bool AppSettings::getMasterEnabled() const { return properties->getBoolValue (Keys::masterEnabled, true); }
void AppSettings::setMasterEnabled (bool enabled) { properties->setValue (Keys::masterEnabled, enabled); }
int AppSettings::getSelectedStrip() const { return properties->getIntValue (Keys::selectedStrip, 0); }
void AppSettings::setSelectedStrip (int strip) { properties->setValue (Keys::selectedStrip, strip); }
bool AppSettings::getReduceLoadOnOverload() const { return properties->getBoolValue (Keys::reduceLoadOnOverload, false); }
void AppSettings::setReduceLoadOnOverload (bool shouldReduce) { properties->setValue (Keys::reduceLoadOnOverload, shouldReduce); }

// ---- Per strip --------------------------------------------------------------------------------
juce::String AppSettings::getLastPreset (const juce::String& stripName) const
{
    return properties->getValue (stripKey (stripName, "preset"));
}

void AppSettings::setLastPreset (const juce::String& stripName, const juce::String& presetId)
{
    properties->setValue (stripKey (stripName, "preset"), presetId);
}

juce::String AppSettings::getStripState (const juce::String& stripName) const
{
    return properties->getValue (stripKey (stripName, "state"));
}

void AppSettings::setStripState (const juce::String& stripName, const juce::String& json)
{
    properties->setValue (stripKey (stripName, "state"), json);
}

float AppSettings::getStripGainDb (const juce::String& stripName) const
{
    return static_cast<float> (properties->getDoubleValue (stripKey (stripName, "gain"), 0.0));
}

void AppSettings::setStripGainDb (const juce::String& stripName, float gainDb)
{
    properties->setValue (stripKey (stripName, "gain"), gainDb);
}

bool AppSettings::getStripMuted (const juce::String& stripName) const
{
    return properties->getBoolValue (stripKey (stripName, "muted"), false);
}

void AppSettings::setStripMuted (const juce::String& stripName, bool muted)
{
    properties->setValue (stripKey (stripName, "muted"), muted);
}

// ---- Hotkeys -------------------------------------------------------------------------------------
KeyChord AppSettings::getDefaultHotkey (HotkeyAction action)
{
    KeyChord chord;
    chord.modifiers = KeyChord::Ctrl | KeyChord::Alt;
    switch (action)
    {
        case HotkeyAction::ToggleEnable: chord.keyCode = 'F'; break;
        case HotkeyAction::ToggleMode: chord.keyCode = 'M'; break;
        case HotkeyAction::BoostUp: chord.keyCode = 0x26; break;        // Up
        case HotkeyAction::BoostDown: chord.keyCode = 0x28; break;      // Down
        case HotkeyAction::NextPreset: chord.keyCode = 0x27; break;     // Right
        case HotkeyAction::PreviousPreset: chord.keyCode = 0x25; break; // Left
        case HotkeyAction::ToggleFocus: chord.keyCode = 'S'; break;
        case HotkeyAction::ChatMixToChat: chord.keyCode = 0x21; break;  // PageUp
        case HotkeyAction::ChatMixToGame: chord.keyCode = 0x22; break;  // PageDown
        case HotkeyAction::ToggleNight: chord.keyCode = 'N'; break;
        case HotkeyAction::ToggleBypass: chord.keyCode = 'B'; break;
    }
    return chord;
}

juce::String AppSettings::getHotkeyActionName (HotkeyAction action)
{
    switch (action)
    {
        case HotkeyAction::ToggleEnable: return "Enable / Disable";
        case HotkeyAction::ToggleMode: return "Toggle Music / Gaming";
        case HotkeyAction::BoostUp: return "Boost +10%";
        case HotkeyAction::BoostDown: return "Boost -10%";
        case HotkeyAction::NextPreset: return "Next Preset";
        case HotkeyAction::PreviousPreset: return "Previous Preset";
        case HotkeyAction::ToggleFocus: return "Focus (footsteps)";
        case HotkeyAction::ChatMixToChat: return "ChatMix: more chat";
        case HotkeyAction::ChatMixToGame: return "ChatMix: more game";
        case HotkeyAction::ToggleNight: return "Night listening";
        case HotkeyAction::ToggleBypass: return "Bypass hotkey strip";
    }
    return {};
}

std::vector<HotkeyAction> AppSettings::getAllHotkeyActions()
{
    return { HotkeyAction::ToggleEnable,  HotkeyAction::ToggleMode,    HotkeyAction::BoostUp,     HotkeyAction::BoostDown,
             HotkeyAction::NextPreset,    HotkeyAction::PreviousPreset, HotkeyAction::ToggleFocus, HotkeyAction::ChatMixToChat,
             HotkeyAction::ChatMixToGame, HotkeyAction::ToggleNight,   HotkeyAction::ToggleBypass };
}

KeyChord AppSettings::getHotkey (HotkeyAction action) const
{
    const auto key = hotkeySettingKey (action);
    if (! properties->containsKey (key))
        return getDefaultHotkey (action);

    KeyChord chord;
    if (! chordFromString (properties->getValue (key), chord))
        return {}; // explicitly unassigned (or unparsable)
    return chord;
}

void AppSettings::setHotkey (HotkeyAction action, const KeyChord& chord)
{
    properties->setValue (hotkeySettingKey (action), chordToString (chord));
}

bool AppSettings::getHotkeysEnabled() const { return properties->getBoolValue (Keys::hotkeysEnabled, true); }
void AppSettings::setHotkeysEnabled (bool enabled) { properties->setValue (Keys::hotkeysEnabled, enabled); }

juce::String AppSettings::getHotkeyStripName() const
{
    const auto name = properties->getValue (Keys::hotkeyStrip, "Game").trim();
    return name.isNotEmpty() ? name : juce::String ("Game");
}

void AppSettings::setHotkeyStripName (const juce::String& stripName) { properties->setValue (Keys::hotkeyStrip, stripName.trim()); }

juce::String AppSettings::chordToString (const KeyChord& chord)
{
    if (chord.keyCode == 0)
        return "None";

    juce::StringArray parts;
    if ((chord.modifiers & KeyChord::Ctrl) != 0)
        parts.add ("Ctrl");
    if ((chord.modifiers & KeyChord::Alt) != 0)
        parts.add ("Alt");
    if ((chord.modifiers & KeyChord::Shift) != 0)
        parts.add ("Shift");
    if ((chord.modifiers & KeyChord::Super) != 0)
        parts.add ("Super");
    parts.add (keyCodeToString (chord.keyCode));
    return parts.joinIntoString ("+");
}

bool AppSettings::chordFromString (const juce::String& text, KeyChord& chord)
{
    chord = {};
    const auto trimmed = text.trim();
    if (trimmed.isEmpty() || trimmed.equalsIgnoreCase ("None"))
        return false;

    auto tokens = juce::StringArray::fromTokens (trimmed, "+", {});
    tokens.trim();
    tokens.removeEmptyStrings();
    if (tokens.isEmpty())
        return false;

    uint32_t modifiers = KeyChord::None;
    for (int i = 0; i < tokens.size() - 1; ++i)
    {
        const auto& t = tokens[i];
        if (t.equalsIgnoreCase ("Ctrl") || t.equalsIgnoreCase ("Control"))
            modifiers |= KeyChord::Ctrl;
        else if (t.equalsIgnoreCase ("Alt") || t.equalsIgnoreCase ("Option"))
            modifiers |= KeyChord::Alt;
        else if (t.equalsIgnoreCase ("Shift"))
            modifiers |= KeyChord::Shift;
        else if (t.equalsIgnoreCase ("Super") || t.equalsIgnoreCase ("Win") || t.equalsIgnoreCase ("Cmd") || t.equalsIgnoreCase ("Command")
                 || t.equalsIgnoreCase ("Meta"))
            modifiers |= KeyChord::Super;
        else
            return false;
    }

    uint32_t code = 0;
    if (! keyCodeFromString (tokens[tokens.size() - 1], code))
        return false;

    chord.modifiers = modifiers;
    chord.keyCode = code;
    return true;
}

// ---- Window / tray ------------------------------------------------------------------------------------
bool AppSettings::getStartMinimised() const { return properties->getBoolValue (Keys::startMinimised, false); }
void AppSettings::setStartMinimised (bool shouldStartMinimised) { properties->setValue (Keys::startMinimised, shouldStartMinimised); }
bool AppSettings::getCloseToTray() const { return properties->getBoolValue (Keys::closeToTray, true); }
void AppSettings::setCloseToTray (bool shouldCloseToTray) { properties->setValue (Keys::closeToTray, shouldCloseToTray); }
bool AppSettings::getStartWithOs() const { return properties->getBoolValue (Keys::startWithOs, false); }
void AppSettings::setStartWithOs (bool shouldStartWithOs) { properties->setValue (Keys::startWithOs, shouldStartWithOs); }
juce::String AppSettings::getWindowState() const { return properties->getValue (Keys::windowState); }
void AppSettings::setWindowState (const juce::String& state) { properties->setValue (Keys::windowState, state); }

int AppSettings::clampUiScalePercent (int percent)
{
    return percent <= kUiScaleFollowSystem ? kUiScaleFollowSystem : std::clamp (percent, kUiScaleMinPercent, kUiScaleMaxPercent);
}

int AppSettings::getUiScalePercent() const { return clampUiScalePercent (properties->getIntValue (Keys::uiScale, kUiScaleFollowSystem)); }
void AppSettings::setUiScalePercent (int percent) { properties->setValue (Keys::uiScale, clampUiScalePercent (percent)); }
// Stored as a name so a later theme can be added without renumbering.
bool AppSettings::getHighContrast() const { return properties->getValue (Keys::uiTheme) == "high-contrast"; }
void AppSettings::setHighContrast (bool highContrast) { properties->setValue (Keys::uiTheme, highContrast ? "high-contrast" : "standard"); }

AppSettings::MainView AppSettings::getMainView() const
{
    // Anything but "advanced" (absent, damaged) is the Simple default.
    return properties->getValue (Keys::uiView) == "advanced" ? MainView::Advanced : MainView::Simple;
}

void AppSettings::setMainView (MainView view) { properties->setValue (Keys::uiView, view == MainView::Advanced ? "advanced" : "simple"); }

// ---- Preset browser ------------------------------------------------------------------------------------
// Lists of preset ids, comma separated (a uuid or a legacy id never holds a comma).
namespace
{
juce::StringArray readIdList (const juce::PropertiesFile& file, const char* key)
{
    auto ids = juce::StringArray::fromTokens (file.getValue (key), ",", {});
    ids.trim();
    ids.removeEmptyStrings();
    ids.removeDuplicates (false);
    return ids;
}
} // namespace

juce::StringArray AppSettings::getFavouritePresets() const { return readIdList (*properties, Keys::presetFavourites); }
bool AppSettings::isFavouritePreset (const juce::String& presetId) const { return presetId.isNotEmpty() && getFavouritePresets().contains (presetId); }

void AppSettings::setFavouritePreset (const juce::String& presetId, bool favourite)
{
    const auto id = presetId.trim();
    if (id.isEmpty() || id.containsChar (','))
        return;
    auto ids = getFavouritePresets();
    ids.removeString (id);
    if (favourite)
        ids.add (id);
    properties->setValue (Keys::presetFavourites, ids.joinIntoString (","));
}

juce::StringArray AppSettings::getRecentPresets() const
{
    auto ids = readIdList (*properties, Keys::presetRecent);
    ids.removeRange (kMaxRecentPresets, ids.size());
    return ids;
}

void AppSettings::addRecentPreset (const juce::String& presetId)
{
    const auto id = presetId.trim();
    if (id.isEmpty() || id.containsChar (','))
        return;
    auto ids = getRecentPresets();
    ids.removeString (id);
    ids.insert (0, id);
    ids.removeRange (kMaxRecentPresets, ids.size());
    properties->setValue (Keys::presetRecent, ids.joinIntoString (","));
}

bool AppSettings::getPresetPreview() const { return properties->getBoolValue (Keys::presetPreview, true); }
void AppSettings::setPresetPreview (bool preview) { properties->setValue (Keys::presetPreview, preview); }
bool AppSettings::getPresetPreviewMatched() const { return properties->getBoolValue (Keys::presetPreviewMatched, true); }
void AppSettings::setPresetPreviewMatched (bool matched) { properties->setValue (Keys::presetPreviewMatched, matched); }
bool AppSettings::getComparisonMatched() const { return properties->getBoolValue (Keys::comparisonMatched, true); }
void AppSettings::setComparisonMatched (bool matched) { properties->setValue (Keys::comparisonMatched, matched); }
juce::String AppSettings::getPreferredOutputDevice() const { return properties->getValue (Keys::preferredOutputDevice); }
void AppSettings::setPreferredOutputDevice (const juce::String& name) { properties->setValue (Keys::preferredOutputDevice, name); }

DeviceEndpointEntry AppSettings::getPreferredOutput() const
{
    DeviceEndpointEntry e;
    e.name = properties->getValue (Keys::preferredOutputDevice);
    e.endpointId = properties->getValue (Keys::preferredOutputId);
    e.hardwareId = properties->getValue (Keys::preferredOutputHardwareId);
    return e;
}

void AppSettings::setPreferredOutput (const DeviceEndpointEntry& output)
{
    properties->setValue (Keys::preferredOutputDevice, output.name);
    properties->setValue (Keys::preferredOutputId, output.endpointId);
    properties->setValue (Keys::preferredOutputHardwareId, output.hardwareId);
}

bool AppSettings::getFollowSystemDefaultOutput() const { return properties->getBoolValue (Keys::followSystemDefault, false); }
void AppSettings::setFollowSystemDefaultOutput (bool follow) { properties->setValue (Keys::followSystemDefault, follow); }

// ---- App routing ----------------------------------------------------------------------------------------
AppSettings::RoutingMethod AppSettings::getRoutingMethod() const
{
    const auto v = properties->getValue (Keys::routingMethod, "auto");
    if (v == "endpoint")
        return RoutingMethod::EndpointRouting;
    if (v == "capture")
        return RoutingMethod::ProcessCapture;
    if (v == "off")
        return RoutingMethod::Disabled;
    return RoutingMethod::Automatic;
}

void AppSettings::setRoutingMethod (RoutingMethod method)
{
    const char* v = "auto";
    switch (method)
    {
        case RoutingMethod::EndpointRouting: v = "endpoint"; break;
        case RoutingMethod::ProcessCapture: v = "capture"; break;
        case RoutingMethod::Disabled: v = "off"; break;
        case RoutingMethod::Automatic: break;
    }
    properties->setValue (Keys::routingMethod, v);
}

std::vector<AppRoute> AppSettings::getAppRoutes() const
{
    std::vector<AppRoute> routes;
    if (auto xml = properties->getXmlValue (Keys::routingMap))
    {
        for (auto* e : xml->getChildWithTagNameIterator ("ROUTE"))
        {
            AppRoute r;
            r.executable = e->getStringAttribute ("exe").trim();
            r.stripName = e->getStringAttribute ("strip").trim();
            if (r.executable.isNotEmpty() && r.stripName.isNotEmpty())
                routes.push_back (r);
        }
    }
    return routes;
}

void AppSettings::setAppRoutes (const std::vector<AppRoute>& routes)
{
    juce::XmlElement xml ("APPROUTES");
    for (const auto& r : routes)
    {
        auto* e = xml.createNewChildElement ("ROUTE");
        e->setAttribute ("exe", r.executable);
        e->setAttribute ("strip", r.stripName);
    }
    properties->setValue (Keys::routingMap, &xml);
}

juce::String AppSettings::getStripEndpointId (const juce::String& stripName) const
{
   #if JUCE_LINUX || JUCE_BSD
    const auto fallback = "flubsound_" + stripName.toLowerCase().replaceCharacter (' ', '_');
   #else
    const auto fallback = "Flubsound " + stripName;
   #endif
    return properties->getValue (stripKey (stripName, "endpoint"), fallback);
}

void AppSettings::setStripEndpointId (const juce::String& stripName, const juce::String& endpointId)
{
    properties->setValue (stripKey (stripName, "endpoint"), endpointId);
}

bool AppSettings::getMoveOriginalAway() const { return properties->getBoolValue (Keys::routingMoveAway, false); }
void AppSettings::setMoveOriginalAway (bool shouldMove) { properties->setValue (Keys::routingMoveAway, shouldMove); }

juce::String AppSettings::getSilentEndpointId() const { return properties->getValue (Keys::routingSilentEndpoint).trim(); }
juce::String AppSettings::getSilentEndpointName() const { return properties->getValue (Keys::routingSilentEndpointName).trim(); }

void AppSettings::setSilentEndpoint (const juce::String& endpointId, const juce::String& name)
{
    if (endpointId.trim().isEmpty())
    {
        properties->removeValue (Keys::routingSilentEndpoint);
        properties->removeValue (Keys::routingSilentEndpointName);
        return;
    }
    properties->setValue (Keys::routingSilentEndpoint, endpointId.trim());
    properties->setValue (Keys::routingSilentEndpointName, name.trim());
}

// ---- Automatic profiles -------------------------------------------------------------------------------------
bool AppSettings::getAutoProfilesEnabled() const { return properties->getBoolValue (Keys::autoProfilesEnabled, true); }
void AppSettings::setAutoProfilesEnabled (bool enabled) { properties->setValue (Keys::autoProfilesEnabled, enabled); }

std::vector<AutoProfileRule> AppSettings::getAutoProfileRules() const
{
    std::vector<AutoProfileRule> rules;
    if (auto xml = properties->getXmlValue (Keys::autoProfileRules))
    {
        for (auto* e : xml->getChildWithTagNameIterator ("RULE"))
        {
            AutoProfileRule r;
            r.executable = e->getStringAttribute ("exe").trim();
            r.stripName = e->getStringAttribute ("strip").trim();
            r.presetId = e->getStringAttribute ("preset").trim();
            const auto mode = e->getStringAttribute ("mode");
            r.mode = mode == "music" ? AutoProfileRule::Mode::Music : (mode == "gaming" ? AutoProfileRule::Mode::Gaming : AutoProfileRule::Mode::Preset);
            r.restoreOnExit = e->getBoolAttribute ("restore", false);
            if (r.executable.isNotEmpty() && r.stripName.isNotEmpty() && r.presetId.isNotEmpty())
                rules.push_back (r);
        }
    }
    return rules;
}

void AppSettings::setAutoProfileRules (const std::vector<AutoProfileRule>& rules)
{
    juce::XmlElement xml ("AUTOPROFILES");
    for (const auto& r : rules)
    {
        auto* e = xml.createNewChildElement ("RULE");
        e->setAttribute ("exe", r.executable);
        e->setAttribute ("strip", r.stripName);
        e->setAttribute ("preset", r.presetId);
        e->setAttribute ("mode", r.mode == AutoProfileRule::Mode::Music ? "music" : (r.mode == AutoProfileRule::Mode::Gaming ? "gaming" : "preset"));
        e->setAttribute ("restore", r.restoreOnExit);
    }
    properties->setValue (Keys::autoProfileRules, &xml);
}

// ---- Device correction ---------------------------------------------------------------
namespace
{
void storeDeviceCorrections (juce::PropertiesFile& properties, const std::vector<DeviceCorrectionEntry>& entries)
{
    juce::XmlElement xml ("CORRECTIONS");
    for (const auto& e : entries)
    {
        auto* child = xml.createNewChildElement ("ENDPOINT");
        child->setAttribute ("id", e.endpoint); // the name: what files before docs/11 E51 keyed by
        child->setAttribute ("endpointId", e.endpointId);
        child->setAttribute ("hardwareId", e.hardwareId);
        child->setAttribute ("name", e.name);
        child->setAttribute ("enabled", e.enabled);
        child->addTextElement (e.curveText);
    }
    properties.setValue (Keys::deviceCorrections, &xml);
}

int findCorrectionEntry (const std::vector<DeviceCorrectionEntry>& entries, const flub::platform::OutputEndpointIdentity& endpoint)
{
    std::vector<flub::platform::OutputEndpointIdentity> stored;
    stored.reserve (entries.size());
    for (const auto& e : entries)
        stored.push_back (e.identity());
    return flub::platform::AudioDeviceWatcher::findEndpoint (stored, endpoint);
}
} // namespace

std::vector<DeviceCorrectionEntry> AppSettings::getDeviceCorrections() const
{
    std::vector<DeviceCorrectionEntry> entries;
    if (auto xml = properties->getXmlValue (Keys::deviceCorrections))
    {
        for (auto* e : xml->getChildWithTagNameIterator ("ENDPOINT"))
        {
            DeviceCorrectionEntry entry;
            entry.endpoint = e->getStringAttribute ("id");
            entry.endpointId = e->getStringAttribute ("endpointId");
            entry.hardwareId = e->getStringAttribute ("hardwareId");
            entry.name = e->getStringAttribute ("name");
            entry.enabled = e->getBoolAttribute ("enabled", true);
            entry.curveText = e->getAllSubText();
            if (entry.endpoint.isNotEmpty() || entry.endpointId.isNotEmpty())
                entries.push_back (entry);
        }
    }
    return entries;
}

std::optional<DeviceCorrectionEntry> AppSettings::findDeviceCorrection (const flub::platform::OutputEndpointIdentity& endpoint) const
{
    const auto entries = getDeviceCorrections();
    if (const int found = findCorrectionEntry (entries, endpoint); found >= 0)
        return entries[static_cast<size_t> (found)];
    return std::nullopt;
}

std::optional<DeviceCorrectionEntry> AppSettings::getDeviceCorrection (const juce::String& endpointName) const
{
    flub::platform::OutputEndpointIdentity endpoint;
    endpoint.name = endpointName.toStdString();
    return findDeviceCorrection (endpoint);
}

void AppSettings::setDeviceCorrection (const DeviceCorrectionEntry& entry)
{
    if (entry.endpoint.isEmpty() && entry.endpointId.isEmpty())
        return;
    auto entries = getDeviceCorrections();
    if (const int found = findCorrectionEntry (entries, entry.identity()); found >= 0)
        entries[static_cast<size_t> (found)] = entry;
    else
        entries.push_back (entry);

    storeDeviceCorrections (*properties, entries);
}

void AppSettings::removeDeviceCorrection (const flub::platform::OutputEndpointIdentity& endpoint)
{
    auto entries = getDeviceCorrections();
    if (const int found = findCorrectionEntry (entries, endpoint); found >= 0)
    {
        entries.erase (entries.begin() + found);
        storeDeviceCorrections (*properties, entries);
    }
}

// ---- Per-endpoint settings (docs/11 E16) -----------------------------------------------
std::vector<DeviceEndpointEntry> AppSettings::getDeviceEndpoints() const
{
    std::vector<DeviceEndpointEntry> entries;
    if (auto xml = properties->getXmlValue (Keys::deviceEndpoints))
    {
        for (auto* e : xml->getChildWithTagNameIterator ("ENDPOINT"))
        {
            DeviceEndpointEntry entry;
            entry.endpointId = e->getStringAttribute ("id");
            entry.hardwareId = e->getStringAttribute ("hardwareId");
            entry.name = e->getStringAttribute ("name");
            entry.onboardEnhancement = e->getBoolAttribute ("onboardEnhancement", false);
            if (entry.endpointId.isNotEmpty() || entry.name.isNotEmpty())
                entries.push_back (entry);
        }
    }
    return entries;
}

namespace
{
int findEndpointEntry (const std::vector<DeviceEndpointEntry>& entries, const flub::platform::OutputEndpointIdentity& endpoint)
{
    std::vector<flub::platform::OutputEndpointIdentity> stored;
    stored.reserve (entries.size());
    for (const auto& e : entries)
        stored.push_back (e.identity());
    return flub::platform::AudioDeviceWatcher::findEndpoint (stored, endpoint);
}
} // namespace

std::optional<DeviceEndpointEntry> AppSettings::findDeviceEndpoint (const flub::platform::OutputEndpointIdentity& endpoint) const
{
    const auto entries = getDeviceEndpoints();
    if (const int found = findEndpointEntry (entries, endpoint); found >= 0)
        return entries[static_cast<size_t> (found)];
    return std::nullopt;
}

void AppSettings::setDeviceEndpoint (const DeviceEndpointEntry& entry)
{
    if (entry.endpointId.isEmpty() && entry.name.isEmpty())
        return;
    auto entries = getDeviceEndpoints();
    if (const int found = findEndpointEntry (entries, entry.identity()); found >= 0)
        entries[static_cast<size_t> (found)] = entry;
    else
        entries.push_back (entry);

    juce::XmlElement xml ("ENDPOINTS");
    for (const auto& e : entries)
    {
        auto* child = xml.createNewChildElement ("ENDPOINT");
        child->setAttribute ("id", e.endpointId);
        child->setAttribute ("hardwareId", e.hardwareId);
        child->setAttribute ("name", e.name);
        child->setAttribute ("onboardEnhancement", e.onboardEnhancement);
    }
    properties->setValue (Keys::deviceEndpoints, &xml);
}

bool AppSettings::getContourFollowsVolume() const { return properties->getBoolValue (Keys::contourFollowVolume, false); }
void AppSettings::setContourFollowsVolume (bool follow) { properties->setValue (Keys::contourFollowVolume, follow); }

std::optional<float> AppSettings::getContourReferenceVolumeDb() const
{
    if (! properties->containsKey (Keys::contourReferenceVolume))
        return std::nullopt;
    const auto db = static_cast<float> (properties->getDoubleValue (Keys::contourReferenceVolume, 0.0));
    return std::isfinite (db) ? std::optional<float> (std::clamp (db, flub::platform::EndpointVolume::kSilentDb, 24.0f)) : std::nullopt;
}

void AppSettings::setContourReferenceVolumeDb (float volumeDb)
{
    if (std::isfinite (volumeDb))
        properties->setValue (Keys::contourReferenceVolume, std::clamp (volumeDb, flub::platform::EndpointVolume::kSilentDb, 24.0f));
}

std::vector<AppSettings::LoopbackPair> AppSettings::getAllowedLoopbackPairs() const
{
    std::vector<LoopbackPair> pairs;
    if (auto xml = properties->getXmlValue (Keys::allowedLoopbackPairs))
    {
        for (auto* e : xml->getChildWithTagNameIterator ("PAIR"))
        {
            LoopbackPair p { e->getStringAttribute ("input").trim(), e->getStringAttribute ("output").trim() };
            if (p.input.isNotEmpty() && p.output.isNotEmpty())
                pairs.push_back (p);
        }
    }
    return pairs;
}

void AppSettings::setAllowedLoopbackPairs (const std::vector<LoopbackPair>& pairs)
{
    juce::XmlElement xml ("LOOPBACKPAIRS");
    for (const auto& p : pairs)
    {
        if (p.input.trim().isEmpty() || p.output.trim().isEmpty())
            continue;
        auto* e = xml.createNewChildElement ("PAIR");
        e->setAttribute ("input", p.input.trim());
        e->setAttribute ("output", p.output.trim());
    }
    properties->setValue (Keys::allowedLoopbackPairs, &xml);
}

bool AppSettings::getTournamentMode() const { return properties->getBoolValue (Keys::tournamentMode, false); }
void AppSettings::setTournamentMode (bool on) { properties->setValue (Keys::tournamentMode, on); }
bool AppSettings::getTournamentAuto() const { return properties->getBoolValue (Keys::tournamentAuto, true); }
void AppSettings::setTournamentAuto (bool automatic) { properties->setValue (Keys::tournamentAuto, automatic); }

bool AppSettings::getChatDuck() const { return properties->getBoolValue (Keys::chatDuck, false); }
void AppSettings::setChatDuck (bool on) { properties->setValue (Keys::chatDuck, on); }
float AppSettings::getChatDuckDepthDb() const
{
    const auto depth = static_cast<float> (properties->getDoubleValue (Keys::chatDuckDepth, 4.5));
    return std::isfinite (depth) ? std::clamp (depth, 3.0f, 6.0f) : 4.5f;
}
void AppSettings::setChatDuckDepthDb (float depthDb) { properties->setValue (Keys::chatDuckDepth, depthDb); }

// ---- Hearing (docs/11 E32 (c), E34) ----------------------------------------------------
namespace
{
namespace HearingKeys
{
constexpr const char* sensitivities = "hearing.sensitivities";
constexpr const char* capOn = "hearing.capOn";
constexpr const char* capDbA = "hearing.capDbA";
constexpr const char* doses = "hearing.dailyDoses";
} // namespace HearingKeys

// HearingGuard's ranges (core/include/flub/engine/HearingGuard.h), repeated
// here so the settings do not pull in the engine.
constexpr float kSensitivityMin = 60.0f, kSensitivityMax = 150.0f;
constexpr float kCapMin = 60.0f, kCapMax = 100.0f, kCapDefault = 85.0f;

struct SensitivityEntry
{
    flub::platform::OutputEndpointIdentity identity;
    float dbSpl = 0.0f;
};

std::vector<SensitivityEntry> readSensitivities (const juce::PropertiesFile& file)
{
    std::vector<SensitivityEntry> entries;
    if (auto xml = file.getXmlValue (HearingKeys::sensitivities))
        for (auto* e : xml->getChildWithTagNameIterator ("ENDPOINT"))
        {
            SensitivityEntry entry;
            entry.identity.id = e->getStringAttribute ("id").toStdString();
            entry.identity.hardwareId = e->getStringAttribute ("hardwareId").toStdString();
            entry.identity.name = e->getStringAttribute ("name").toStdString();
            entry.dbSpl = static_cast<float> (e->getDoubleAttribute ("dbSpl", 0.0));
            if ((! entry.identity.id.empty() || ! entry.identity.name.empty()) && std::isfinite (entry.dbSpl))
                entries.push_back (entry);
        }
    return entries;
}

int findSensitivity (const std::vector<SensitivityEntry>& entries, const flub::platform::OutputEndpointIdentity& endpoint)
{
    std::vector<flub::platform::OutputEndpointIdentity> stored;
    stored.reserve (entries.size());
    for (const auto& e : entries)
        stored.push_back (e.identity);
    return flub::platform::AudioDeviceWatcher::findEndpoint (stored, endpoint);
}
} // namespace

std::optional<float> AppSettings::findHearingSensitivity (const flub::platform::OutputEndpointIdentity& endpoint) const
{
    const auto entries = readSensitivities (*properties);
    if (const int found = findSensitivity (entries, endpoint); found >= 0)
        return std::clamp (entries[static_cast<size_t> (found)].dbSpl, kSensitivityMin, kSensitivityMax);
    return std::nullopt;
}

void AppSettings::setHearingSensitivity (const flub::platform::OutputEndpointIdentity& endpoint, std::optional<float> dbSpl)
{
    if (endpoint.id.empty() && endpoint.name.empty())
        return;
    if (dbSpl.has_value() && ! std::isfinite (*dbSpl))
        return;
    auto entries = readSensitivities (*properties);
    const int found = findSensitivity (entries, endpoint);
    if (dbSpl.has_value())
    {
        const SensitivityEntry entry { endpoint, std::clamp (*dbSpl, kSensitivityMin, kSensitivityMax) };
        if (found >= 0)
            entries[static_cast<size_t> (found)] = entry;
        else
            entries.push_back (entry);
    }
    else if (found >= 0)
    {
        entries.erase (entries.begin() + found);
    }
    else
    {
        return;
    }

    juce::XmlElement xml ("HEARINGSENSITIVITIES");
    for (const auto& e : entries)
    {
        auto* child = xml.createNewChildElement ("ENDPOINT");
        child->setAttribute ("id", juce::String (e.identity.id));
        child->setAttribute ("hardwareId", juce::String (e.identity.hardwareId));
        child->setAttribute ("name", juce::String (e.identity.name));
        child->setAttribute ("dbSpl", static_cast<double> (e.dbSpl));
    }
    properties->setValue (HearingKeys::sensitivities, &xml);
}

bool AppSettings::getHearingCapEnabled() const { return properties->getBoolValue (HearingKeys::capOn, false); }
void AppSettings::setHearingCapEnabled (bool on) { properties->setValue (HearingKeys::capOn, on); }

float AppSettings::getHearingCapDbA() const
{
    const auto db = static_cast<float> (properties->getDoubleValue (HearingKeys::capDbA, kCapDefault));
    return std::isfinite (db) ? std::clamp (db, kCapMin, kCapMax) : kCapDefault;
}

void AppSettings::setHearingCapDbA (float dbA)
{
    if (std::isfinite (dbA))
        properties->setValue (HearingKeys::capDbA, std::clamp (dbA, kCapMin, kCapMax));
}

std::vector<AppSettings::DailyDose> AppSettings::getDailyDoses() const
{
    std::vector<DailyDose> doses;
    if (auto xml = properties->getXmlValue (HearingKeys::doses))
        for (auto* e : xml->getChildWithTagNameIterator ("DAY"))
        {
            DailyDose d { e->getStringAttribute ("date").trim(), e->getDoubleAttribute ("dose", 0.0) };
            if (d.day.length() == 10 && std::isfinite (d.fraction) && d.fraction >= 0.0)
                doses.push_back (d);
        }
    std::sort (doses.begin(), doses.end(), [] (const DailyDose& a, const DailyDose& b) { return a.day > b.day; });
    return doses;
}

void AppSettings::setDailyDose (const juce::String& day, double fraction)
{
    if (day.length() != 10 || ! std::isfinite (fraction))
        return;
    auto doses = getDailyDoses();
    doses.erase (std::remove_if (doses.begin(), doses.end(), [&day] (const DailyDose& d) { return d.day == day; }), doses.end());
    doses.push_back ({ day, std::max (0.0, fraction) });
    // ISO dates sort as text; keep `day`, the kDoseDaysKept - 1 days before it and any later one.
    const auto parsed = juce::Time (day.substring (0, 4).getIntValue(), day.substring (5, 7).getIntValue() - 1, day.substring (8, 10).getIntValue(), 12, 0);
    const auto oldest = (parsed - juce::RelativeTime::days (kDoseDaysKept - 1)).formatted ("%Y-%m-%d");
    doses.erase (std::remove_if (doses.begin(), doses.end(), [&oldest] (const DailyDose& d) { return d.day < oldest; }), doses.end());
    std::sort (doses.begin(), doses.end(), [] (const DailyDose& a, const DailyDose& b) { return a.day > b.day; });

    juce::XmlElement xml ("DAILYDOSES");
    for (const auto& d : doses)
    {
        auto* e = xml.createNewChildElement ("DAY");
        e->setAttribute ("date", d.day);
        e->setAttribute ("dose", d.fraction);
    }
    properties->setValue (HearingKeys::doses, &xml);
}

bool AppSettings::getSmartMacros (const juce::String& stripName) const { return properties->getBoolValue (stripKey (stripName, "smart"), false); }
void AppSettings::setSmartMacros (const juce::String& stripName, bool on) { properties->setValue (stripKey (stripName, "smart"), on); }
} // namespace flub::app
