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
constexpr const char* hotkeysEnabled = "hotkeys.enabled";
constexpr const char* startMinimised = "ui.startMinimised";
constexpr const char* closeToTray = "ui.closeToTray";
constexpr const char* startWithOs = "ui.startWithOs";
constexpr const char* windowState = "ui.windowState";
constexpr const char* preferredOutputDevice = "device.preferredOutput";
constexpr const char* routingMethod = "routing.method";
constexpr const char* routingMap = "routing.map";
} // namespace Keys

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
    properties = std::make_unique<juce::PropertiesFile> (file, defaultOptions());
   #else
    properties = std::make_unique<juce::PropertiesFile> (defaultOptions());
   #endif
}

AppSettings::AppSettings (const juce::File& file, bool persist)
{
    auto options = defaultOptions();
    options.doNotSave = ! persist;
    properties = std::make_unique<juce::PropertiesFile> (file, options);
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
    }
    return {};
}

std::vector<HotkeyAction> AppSettings::getAllHotkeyActions()
{
    return { HotkeyAction::ToggleEnable, HotkeyAction::ToggleMode, HotkeyAction::BoostUp,
             HotkeyAction::BoostDown,    HotkeyAction::NextPreset, HotkeyAction::PreviousPreset };
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
} // namespace flub::app
