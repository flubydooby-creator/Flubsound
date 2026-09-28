#include "AppSettings.h"
#include "UserDataFolder.h"

#include <algorithm>

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
constexpr const char* preferredOutputDevice = "device.preferredOutput";
constexpr const char* routingMethod = "routing.method";
constexpr const char* routingMap = "routing.map";
constexpr const char* autoProfilesEnabled = "autoProfile.enabled";
constexpr const char* autoProfileRules = "autoProfile.rules";
constexpr const char* deviceCorrections = "device.corrections";
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
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    return file.getSiblingFile (file.getFileName() + ".corrupt-" + stamp).getNonexistentSibling (false);
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
juce::String AppSettings::getPreferredOutputDevice() const { return properties->getValue (Keys::preferredOutputDevice); }
void AppSettings::setPreferredOutputDevice (const juce::String& name) { properties->setValue (Keys::preferredOutputDevice, name); }

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
        child->setAttribute ("id", e.endpoint);
        child->setAttribute ("name", e.name);
        child->setAttribute ("enabled", e.enabled);
        child->addTextElement (e.curveText);
    }
    properties.setValue (Keys::deviceCorrections, &xml);
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
            entry.name = e->getStringAttribute ("name");
            entry.enabled = e->getBoolAttribute ("enabled", true);
            entry.curveText = e->getAllSubText();
            if (entry.endpoint.isNotEmpty())
                entries.push_back (entry);
        }
    }
    return entries;
}

std::optional<DeviceCorrectionEntry> AppSettings::getDeviceCorrection (const juce::String& endpoint) const
{
    for (auto& e : getDeviceCorrections())
        if (e.endpoint == endpoint)
            return e;
    return std::nullopt;
}

void AppSettings::setDeviceCorrection (const DeviceCorrectionEntry& entry)
{
    if (entry.endpoint.isEmpty())
        return;
    auto entries = getDeviceCorrections();
    const auto it = std::find_if (entries.begin(), entries.end(), [&entry] (const auto& e) { return e.endpoint == entry.endpoint; });
    if (it != entries.end())
        *it = entry;
    else
        entries.push_back (entry);

    storeDeviceCorrections (*properties, entries);
}

void AppSettings::removeDeviceCorrection (const juce::String& endpoint)
{
    auto entries = getDeviceCorrections();
    entries.erase (std::remove_if (entries.begin(), entries.end(), [&endpoint] (const auto& e) { return e.endpoint == endpoint; }), entries.end());
    storeDeviceCorrections (*properties, entries);
}
} // namespace flub::app
