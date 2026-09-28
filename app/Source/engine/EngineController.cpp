#include "EngineController.h"
#include "settings/UserDataFolder.h"

#include "flub/engine/MacroMap.h"
#include "flub/io/Json.h"
#include "flub/io/ParametricEqText.h"
#include "flub/io/PresetIO.h"
#include "platform/PlatformBridge.h"
#include "platform/PlatformServices.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

namespace flub::app
{
using namespace flub::param;

namespace
{
constexpr int kTimerHz = 2;                      // also the overload watchdog's poll rate
constexpr int kPersistEveryTicks = 5 * kTimerHz; // strip state autosave: every 5 s
constexpr int kRescanEveryTicks = 5 * kTimerHz;  // missing preferred output: rescan every 5 s
                                                 // (ALSA probes every PCM device; too slow for every tick)

constexpr const char* kStateFormat = "flubsound-strip-state";
constexpr int kMaxRecentForegroundApps = 8;

// First-run defaults (docs/11 E36): what a strip without saved state loads.
constexpr const char* kFirstRunMusicPreset = "factory:music-flubsound-signature"; // Music, System, other stereo strips
constexpr const char* kFirstRunChatPreset = "factory:music-voice-chat";           // Chat (docs/11 E23)
constexpr const char* kFirstRunGamePreset = "factory:gaming-competitive-fps";     // Game / surround strips, capped:
// "First Run - Game" = Competitive FPS with these caps. Measured with the
// docs/11 E59 slice's definitions (tests/test_known_gaps.cpp): -50 / -60
// dBFS pink beds +2.70 / +2.62 LU (Competitive FPS +4.92 / +9.69), step/bed
// contrast change +0.06 .. +0.49 dB on the 20 / 40 / 80 ms burst scenes at
// -14 / -24 / -40 / -50 LUFS (Competitive FPS -1.67 .. +0.83). Detail at the
// E36 cap of 30 % leaves the beds at +3.55 / +3.87 LU, so it is 15 %.
constexpr float kFirstRunGameBoost = 0.20f;     // below 0.25: Boost adds no maximizer drive
constexpr float kFirstRunGameFootsteps = 0.30f; // Macro 1 (the E16 on-board-processing cap)
constexpr float kFirstRunGameDetail = 0.15f;    // Macro 4

// ChatMix (docs/11 E56): the strips it balances.
constexpr const char* kChatMixGameStrip = "Game";
constexpr const char* kChatMixChatStrip = "Chat";

// Protection strength (docs/11 E06): a host setting, stored by name.
constexpr const char* kProtectionStrengthKey = "protection.strength";

// PipeWire quantum requests (docs/11 E48a): pipewire::planLatencyEnvironment's
// values (PlatformServices_linux.cpp).
constexpr const char* kPipeWireBalancedLatency = "256/48000";
constexpr const char* kPipeWireLowLatency = "128/48000";
constexpr const char* kPipeWireLockQuantumProps = "{ node.lock-quantum = true }";

#if defined(__linux__)
/** PIPEWIRE_LATENCY / PIPEWIRE_PROPS as the user started Flubsound: read
    during static initialisation, before the per-app router exports its own
    request (AppAudioRouter::create), so a value found later can be told
    apart from the user's. */
struct StartupPipeWireEnvironment
{
    std::optional<std::string> latency, props;

    static std::optional<std::string> read (const char* name)
    {
        const char* v = std::getenv (name);
        return v != nullptr && *v != '\0' ? std::optional<std::string> (v) : std::nullopt;
    }
};
const StartupPipeWireEnvironment startupPipeWire { StartupPipeWireEnvironment::read ("PIPEWIRE_LATENCY"),
                                                  StartupPipeWireEnvironment::read ("PIPEWIRE_PROPS") };
#endif

ProtectionStrength protectionStrengthFromName (const juce::String& name)
{
    if (name.equalsIgnoreCase ("normal"))
        return ProtectionStrength::Normal;
    if (name.equalsIgnoreCase ("strict"))
        return ProtectionStrength::Strict;
    return ProtectionStrength::Off;
}

juce::String stripStateToJson (const ParameterStore& store)
{
    flub::json::Value root;
    root.set ("format", kStateFormat);
    root.set ("version", 1);
    root.set ("activeBank", store.getActiveBank() == Bank::A ? "A" : "B");
    root.set ("A", flub::preset::toJson (flub::preset::captureFromStore (store, Bank::A), false));
    root.set ("B", flub::preset::toJson (flub::preset::captureFromStore (store, Bank::B), false));
    return juce::String::fromUTF8 (flub::json::write (root, 0).c_str());
}

bool stripStateFromJson (const juce::String& text, ParameterStore& store)
{
    flub::json::Value root;
    std::string error;
    if (! flub::json::parse (text.toStdString(), root, error) || root["format"].asString() != kStateFormat)
        return false;

    bool any = false;
    for (const auto bank : { Bank::A, Bank::B })
    {
        const auto& v = root[bank == Bank::A ? "A" : "B"];
        flub::preset::Preset p;
        if (v.isObject() && flub::preset::fromJson (v, p, error))
        {
            flub::preset::applyToStore (p, store, bank);
            any = true;
        }
    }
    if (any)
        store.setActiveBank (root["activeBank"].asString() == "B" ? Bank::B : Bank::A);
    return any;
}
} // namespace

// =============================================================================
EngineController::EngineController()
    : EngineController (Options {})
{
}

EngineController::EngineController (Options opts)
    : options (std::move (opts))
{
    settings = options.settingsFile == juce::File() ? std::make_unique<AppSettings>()
                                                    : std::make_unique<AppSettings> (options.settingsFile, options.persistSettings);
    host = std::make_unique<AudioEngineHost>(); // configures a nominal 48 kHz engine: strips exist from here on
    presets = std::make_unique<PresetManager>();
    settings->migratePresetReferences (presets->getLegacyIdAliases()); // docs/11 E52: one-time id -> uuid (settings schema 2)
    routing = std::make_unique<AppRouting> (*host, *settings);

    protectionStrength = protectionStrengthFromName (settings->getPropertiesFile().getValue (kProtectionStrengthKey, "off"));
    host->onEngineConfigured = [this]
    {
        applyProtectionStrength(); // the new engine's chains start at Off
        notify (Change::Engine);
    };
    host->onDeviceError = [this] (const juce::String& message)
    {
        lastDeviceError = message;
        notify (Change::Device);
    };
    host->onDeviceSafetyChanged = [this] { notify (Change::Device); }; // the header's device warning
    presets->onPresetListChanged = [this] { notify (Change::Preset); };
    presets->onPresetWarnings = [this] (const PresetInfo& preset, const juce::StringArray& warnings)
    {
        // Taken by the UI on the Change::Preset that follows (a toast).
        if (warnings.isEmpty())
            return;
        if (pendingPresetWarnings.size() >= kMaxPendingNotices)
            pendingPresetWarnings.erase (pendingPresetWarnings.begin());
        pendingPresetWarnings.push_back ({ preset.name, warnings });
        // A load announces them itself (Change::Preset right after); an
        // import reports them after its list change: announce those once.
        triggerAsyncUpdate();
    };
    routing->onChanged = [this] { notify (Change::Routing); };

    for (int i = 0; i < getNumStrips(); ++i)
    {
        const auto name = getStripName (i);
        userGainDb[static_cast<size_t> (i)] = settings->getStripGainDb (name);
        applyStripGain (i);
        host->setStripMuted (i, settings->getStripMuted (name));
    }

    selectedStrip = std::clamp (settings->getSelectedStrip(), 0, std::max (0, getNumStrips() - 1));
    enabled = settings->getMasterEnabled();

    restoreStripStates();
    for (int i = 0; i < getNumStrips(); ++i)
    {
        applyMasterEnableToStrip (i);
        persistedVersions[static_cast<size_t> (i)] = getParams (i).version();
    }
    applyProtectionStrength();

    loadDeviceProfiles();
    preferredOutputName = settings->getPreferredOutputDevice();

    // Before the device opens: PipeWire reads the request when the stream opens.
    requestGraphQuantum (getLatencyProfile(), false);

    if (options.openAudioDevice)
    {
        const auto savedState = settings->getDeviceState();
        lastDeviceError = host->openDevice (savedState.get());
        getDeviceManager().addChangeListener (this);
        applyDeviceInputPolicy();
        trackPreferredOutput (false);
        updateDeviceProfile();
    }

    if (options.enableAppRouting)
        routing->start();

    // Automatic profiles: the rules are live from here on; the 2 Hz timer polls.
    foregroundApp = options.foregroundAppFactory ? options.foregroundAppFactory() : platform_bridge::createForegroundApp();
    autoProfiles.setEnabled (settings->getAutoProfilesEnabled());
    autoProfiles.setRules (settings->getAutoProfileRules());

    startTimerHz (kTimerHz);
}

EngineController::~EngineController()
{
    shutdown();
}

void EngineController::shutdown()
{
    if (isShutDown)
        return;
    isShutDown = true;

    stopTimer();
    cancelPendingUpdate();
    routing->shutdown();

    // Latched hotkey overrides are per session: the saved state is the user's own.
    releaseAllLatches (true);

    // An auto profile that restores on exit gives its strip back, so the
    // saved state is the user's own and not the game's.
    if (const auto* rule = autoProfiles.getActiveRule(); rule != nullptr && rule->restoreOnExit)
        endAutoProfile ({ AutoProfileSwitcher::Action::Kind::End, *rule, true });
    foregroundApp.reset();

    if (options.openAudioDevice)
    {
        getDeviceManager().removeChangeListener (this);
        persistDeviceState();
    }

    persistStripStates (true);
    settings->setSelectedStrip (selectedStrip);
    settings->save();

    host->onEngineConfigured = nullptr;
    presets->onPresetWarnings = nullptr;
    host->onDeviceError = nullptr;
    host->onDeviceSafetyChanged = nullptr;
    host->closeDevice();
    host->stopAllCaptures();
}

void EngineController::notify (Change change)
{
    // The advice (suggested preset) follows the selected strip's mode, which
    // setMode, preset loads, A/B switches and strip switches can all change.
    if ((change == Change::SelectedStrip || change == Change::Parameters || change == Change::Preset) && host != nullptr
        && (getMode (selectedStrip) == ModeValue::Gaming) != adviceForGaming)
        updateDeviceProfile();
    listeners.call ([change] (Listener& l) { l.engineControllerChanged (change); });
}

// =============================================================================
// Strips
// =============================================================================
int EngineController::resolveStrip (int strip) const noexcept
{
    const int n = host->getNumStrips();
    if (strip < 0)
        strip = selectedStrip;
    return std::clamp (strip, 0, std::max (0, n - 1));
}

juce::String EngineController::getStripName (int strip) const
{
    const auto layout = host->getStripLayout();
    return strip >= 0 && strip < static_cast<int> (layout.size()) ? juce::String (layout[static_cast<size_t> (strip)].name) : juce::String();
}

int EngineController::getStripChannels (int strip) const
{
    const auto layout = host->getStripLayout();
    return strip >= 0 && strip < static_cast<int> (layout.size()) ? layout[static_cast<size_t> (strip)].inputChannels : 0;
}

int EngineController::findStrip (const juce::String& name) const
{
    const auto layout = host->getStripLayout();
    for (size_t i = 0; i < layout.size(); ++i)
        if (juce::String (layout[i].name).equalsIgnoreCase (name))
            return static_cast<int> (i);
    return -1;
}

void EngineController::setSelectedStrip (int strip)
{
    const int s = resolveStrip (strip);
    if (s == selectedStrip)
        return;
    selectedStrip = s;
    settings->setSelectedStrip (s);
    notify (Change::SelectedStrip); // also refreshes the advice for the strip's mode
}

ParameterStore& EngineController::getParams (int strip)
{
    return host->getMixEngine().params (resolveStrip (strip));
}

flub::ProcessingChain& EngineController::getChain (int strip)
{
    return host->getMixEngine().chain (resolveStrip (strip));
}

void EngineController::setAuditionBypass (int strip, int enableParamId, bool bypassed)
{
    getChain (strip).setAuditionBypass (enableParamId, bypassed);
}

void EngineController::setStripGainDb (int strip, float gainDb)
{
    const int s = resolveStrip (strip);
    auto& gain = userGainDb[static_cast<size_t> (s)];
    gain = std::clamp (gainDb, -60.0f, 12.0f); // AudioEngineHost's range
    applyStripGain (s);
    settings->setStripGainDb (getStripName (s), gain);
}

float EngineController::getStripGainDb (int strip) const noexcept
{
    return strip >= 0 && strip < AudioEngineHost::kMaxStrips ? userGainDb[static_cast<size_t> (strip)] : 0.0f;
}

void EngineController::applyStripGain (int strip)
{
    host->setStripGainDb (strip, userGainDb[static_cast<size_t> (strip)] + chatMixOffsetDb (strip));
}

void EngineController::setStripMuted (int strip, bool muted)
{
    const int s = resolveStrip (strip);
    host->setStripMuted (s, muted);
    settings->setStripMuted (getStripName (s), muted);
}

void EngineController::setStripLayout (const std::vector<flub::StripConfig>& newLayout)
{
    // Hotkey state is per strip index: give every strip its own state back first.
    releaseAllLatches (true);
    setChatMix (0.0f);
    stripBypassed.fill (false);

    host->setStripLayout (newLayout);
    for (int i = 0; i < getNumStrips(); ++i)
        userGainDb[static_cast<size_t> (i)] = host->getStripGainDb (i);
    selectedStrip = resolveStrip (selectedStrip);
    for (int i = 0; i < getNumStrips(); ++i)
        applyMasterEnableToStrip (i);
    applyDeviceInputPolicy();
    routing->stripLayoutChanged();
    notify (Change::Engine);
}

// =============================================================================
// Master enable / mode / boost
// =============================================================================
void EngineController::applyMasterEnableToStrip (int strip)
{
    auto& store = getParams (strip);
    const bool stripOff = strip >= 0 && strip < AudioEngineHost::kMaxStrips && stripBypassed[static_cast<size_t> (strip)];
    const float bypass = enabled && ! stripOff ? 0.0f : 1.0f;
    store.set (Bank::A, BypassAll, bypass);
    store.set (Bank::B, BypassAll, bypass);
}

void EngineController::setEnabled (bool shouldBeEnabled)
{
    enabled = shouldBeEnabled;
    for (int i = 0; i < getNumStrips(); ++i)
        applyMasterEnableToStrip (i);
    settings->setMasterEnabled (enabled);
    notify (Change::MasterEnable);
}

void EngineController::setStripBypassed (int strip, bool bypassed)
{
    const int s = resolveStrip (strip);
    stripBypassed[static_cast<size_t> (s)] = bypassed;
    applyMasterEnableToStrip (s);
    notify (Change::MasterEnable);
}

bool EngineController::isStripBypassed (int strip) const noexcept
{
    return strip >= 0 && strip < AudioEngineHost::kMaxStrips && stripBypassed[static_cast<size_t> (strip)];
}

ModeValue EngineController::getMode (int strip)
{
    return getParams (resolveStrip (strip)).get (Mode) >= 0.5f ? ModeValue::Gaming : ModeValue::Music;
}

void EngineController::setMode (ModeValue mode, int strip)
{
    getParams (resolveStrip (strip)).set (Mode, static_cast<float> (static_cast<int> (mode)));
    notify (Change::Parameters); // also refreshes the advice (the suggested preset depends on the mode)
}

void EngineController::toggleMode (int strip)
{
    setMode (getMode (strip) == ModeValue::Music ? ModeValue::Gaming : ModeValue::Music, strip);
}

float EngineController::getBoost (int strip)
{
    return getParams (resolveStrip (strip)).get (BoostIntensity);
}

void EngineController::setBoost (float boost01, int strip)
{
    getParams (resolveStrip (strip)).set (BoostIntensity, std::clamp (boost01, 0.0f, 1.0f));
    notify (Change::Parameters);
}

void EngineController::nudgeBoost (float delta, int strip)
{
    // Snap to whole percent so repeated +-10 % steps land on round values.
    const float next = std::round ((getBoost (strip) + delta) * 100.0f) / 100.0f;
    setBoost (next, strip);
}

juce::String EngineController::getMacroName (ModeValue mode, int macroIndex)
{
    const char* name = flub::MacroMap::macroName (mode, std::clamp (macroIndex, 0, 4));
    return name != nullptr ? juce::String (name) : juce::String ("Macro " + juce::String (macroIndex + 1));
}

// =============================================================================
// Presets
// =============================================================================
bool EngineController::loadPreset (const juce::String& presetId, int strip, juce::String& error)
{
    const auto* info = presets->findById (presetId);
    if (info == nullptr)
    {
        error = "Unknown preset: " + presetId;
        return false;
    }
    const auto copy = *info;
    return loadPreset (copy, strip, error);
}

bool EngineController::loadPreset (const PresetInfo& preset, int strip, juce::String& error)
{
    const int s = resolveStrip (strip);
    if (! presets->loadIntoStrip (s, preset, getParams (s), error))
        return false;
    settings->setLastPreset (getStripName (s), preset.id);
    presetChangedByUser (s);
    presetLoadedByUser (preset);
    notify (Change::Preset);
    return true;
}

bool EngineController::nextPreset (int strip)
{
    const int s = resolveStrip (strip);
    juce::String error;
    if (! presets->stepPreset (s, +1, getParams (s), error))
        return false;
    settings->setLastPreset (getStripName (s), presets->getCurrentPresetId (s));
    presetChangedByUser (s);
    if (const auto* info = presets->findById (presets->getCurrentPresetId (s)))
        presetLoadedByUser (*info);
    notify (Change::Preset);
    return true;
}

bool EngineController::previousPreset (int strip)
{
    const int s = resolveStrip (strip);
    juce::String error;
    if (! presets->stepPreset (s, -1, getParams (s), error))
        return false;
    settings->setLastPreset (getStripName (s), presets->getCurrentPresetId (s));
    presetChangedByUser (s);
    if (const auto* info = presets->findById (presets->getCurrentPresetId (s)))
        presetLoadedByUser (*info);
    notify (Change::Preset);
    return true;
}

juce::String EngineController::getCurrentPresetId (int strip) const
{
    return presets->getCurrentPresetId (resolveStrip (strip));
}

juce::String EngineController::getCurrentPresetName (int strip) const
{
    const auto* info = presets->findById (getCurrentPresetId (strip));
    return info != nullptr ? info->name : juce::String();
}

bool EngineController::isPresetModified (int strip)
{
    const int s = resolveStrip (strip);
    return presets->isModified (s, getParams (s));
}

juce::String EngineController::saveUserPreset (const juce::String& name, const juce::String& category, const juce::String& description,
                                               int strip, juce::String& error)
{
    const int s = resolveStrip (strip);
    auto& store = getParams (s);
    const auto id = presets->saveUserPreset (name, category, description, store, error, true);
    if (id.isNotEmpty())
    {
        presets->setCurrentPresetId (s, id, &store);
        settings->setLastPreset (getStripName (s), id);
        presetChangedByUser (s);
        notify (Change::Preset);
    }
    return id;
}

juce::String EngineController::renameUserPreset (const PresetInfo& preset, const juce::String& newName, juce::String& error)
{
    const auto oldId = preset.id; // `preset` may point into the list the rename rebuilds
    const auto id = presets->renameUserPreset (preset, newName, error);
    if (id.isEmpty())
        return {};

    // A preset keeps its uuid, so usually nothing refers to a new id; one
    // that had none (its file could not be written before) has one now.
    if (id != oldId)
    {
        for (int s = 0; s < getNumStrips(); ++s)
            if (settings->getLastPreset (getStripName (s)) == oldId)
                settings->setLastPreset (getStripName (s), id);
        auto rules = autoProfiles.getRules();
        bool changed = false;
        for (auto& rule : rules)
            if (rule.presetId == oldId)
            {
                rule.presetId = id;
                changed = true;
            }
        if (changed)
            setAutoProfileRules (std::move (rules));
    }
    notify (Change::Preset);
    return id;
}

std::vector<EngineController::PresetWarnings> EngineController::takePresetWarnings()
{
    std::vector<PresetWarnings> taken;
    taken.swap (pendingPresetWarnings);
    return taken;
}

std::optional<EngineController::LatencySuggestion> EngineController::takeLatencySuggestion()
{
    auto taken = pendingLatencySuggestion;
    pendingLatencySuggestion.reset();
    return taken;
}

void EngineController::presetLoadedByUser (const PresetInfo& preset)
{
    // docs/11 E42a: the profile stays (a preset never re-prepares the engine,
    // PresetManager::loadIntoBank); a different suggestion becomes a prompt.
    pendingLatencySuggestion.reset();
    if (! preset.suggestedLatencyProfile.has_value())
        return;
    const auto current = getLatencyProfile();
    if (*preset.suggestedLatencyProfile != current)
        pendingLatencySuggestion = LatencySuggestion { preset.name, *preset.suggestedLatencyProfile, current };
}

Bank EngineController::getActiveBank (int strip)
{
    return getParams (resolveStrip (strip)).getActiveBank();
}

void EngineController::setActiveBank (Bank bank, int strip)
{
    getParams (resolveStrip (strip)).setActiveBank (bank);
    notify (Change::Parameters);
}

void EngineController::toggleAB (int strip)
{
    PresetManager::toggleBank (getParams (resolveStrip (strip)));
    notify (Change::Parameters);
}

void EngineController::copyActiveToOtherBank (int strip)
{
    PresetManager::copyActiveToOther (getParams (resolveStrip (strip)));
    notify (Change::Parameters);
}

// =============================================================================
// State persistence
// =============================================================================
void EngineController::restoreStripStates()
{
    for (int i = 0; i < getNumStrips(); ++i)
    {
        const auto name = getStripName (i);
        auto& store = getParams (i);
        bool restored = false;

        if (options.restoreState)
        {
            const auto stateJson = settings->getStripState (name);
            if (stateJson.isNotEmpty())
                restored = stripStateFromJson (stateJson, store);

            const auto presetId = settings->getLastPreset (name);
            if (const auto* info = presets->findById (presetId))
            {
                if (restored)
                {
                    presets->setCurrentPresetId (i, presetId, &store);
                }
                else
                {
                    const auto copy = *info;
                    juce::String error;
                    restored = presets->loadIntoStrip (i, copy, store, error);
                }
            }
        }

        // A strip with nothing saved (a first run) starts from a safe,
        // audible default preset (docs/11 E36); without state restore
        // (headless runs, tests), or when that preset is missing, from the
        // parameter defaults as before.
        if (! restored && ! (options.restoreState && loadFirstRunDefault (i)))
        {
            // Fresh strip: surround strips default to Gaming mode.
            store.resetToDefaults (Bank::A);
            store.resetToDefaults (Bank::B);
            if (getStripChannels (i) > 2 || name.equalsIgnoreCase ("Game"))
            {
                store.set (Bank::A, Mode, static_cast<float> (static_cast<int> (ModeValue::Gaming)));
                store.set (Bank::B, Mode, static_cast<float> (static_cast<int> (ModeValue::Gaming)));
            }
        }
    }
}

bool EngineController::loadFirstRunDefault (int strip)
{
    // Music and System (and any other stereo strip): Flubsound Signature.
    // Chat: Voice Chat, not Signature, whose Boost 0.35 would switch the
    // maximizer on for voice (docs/11 E23). Game (and any surround strip):
    // "First Run - Game", Competitive FPS capped (see kFirstRunGame*), since
    // Competitive FPS as shipped lifts quiet beds by +5 / +10 LU and cuts
    // step/bed contrast. None of them sets a latency profile: the strips keep
    // the default (Balanced), and a preset never changes it anyway (E40).
    const auto name = getStripName (strip);
    const bool game = getStripChannels (strip) > 2 || name.equalsIgnoreCase ("Game");
    const char* presetId = game ? kFirstRunGamePreset : name.equalsIgnoreCase ("Chat") ? kFirstRunChatPreset : kFirstRunMusicPreset;
    const auto* info = presets->findById (presetId);
    if (info == nullptr)
        return false;

    auto& store = getParams (strip);
    store.resetToDefaults (Bank::A);
    store.resetToDefaults (Bank::B);
    store.setActiveBank (Bank::A);
    const auto preset = *info;
    juce::String error;
    if (! presets->loadIntoStrip (strip, preset, store, error))
        return false;

    if (game)
    {
        // After the preset's snapshot: the strip shows Competitive FPS as modified.
        const auto cap = [&store] (int id, float maxValue) { store.set (Bank::A, id, std::min (store.get (Bank::A, id), maxValue)); };
        cap (BoostIntensity, kFirstRunGameBoost);
        cap (Macro1, kFirstRunGameFootsteps);
        cap (Macro4, kFirstRunGameDetail);
    }
    PresetManager::copyAToB (store); // A/B start equal
    settings->setLastPreset (name, preset.id);
    return true;
}

void EngineController::persistStripStates (bool force)
{
    if (! options.persistSettings)
        return;

    for (int i = 0; i < getNumStrips(); ++i)
    {
        auto& store = getParams (i);
        const auto version = store.version();
        auto& persisted = persistedVersions[static_cast<size_t> (i)];
        if (force || version != persisted)
        {
            settings->setStripState (getStripName (i), stripStateToJson (store));
            persisted = version;
        }
    }
}

void EngineController::persistDeviceState()
{
    // createStateXml() is null while the manager is on an implicit default
    // device: keep whatever was saved before in that case.
    if (auto xml = getDeviceManager().createStateXml())
        settings->setDeviceState (xml.get());
}

void EngineController::saveState()
{
    if (options.openAudioDevice)
        persistDeviceState();
    persistStripStates (true);
    settings->setSelectedStrip (selectedStrip);
    settings->save();
}

// =============================================================================
// Device
// =============================================================================
juce::String EngineController::reopenDevice()
{
    const auto savedState = settings->getDeviceState();
    lastDeviceError = host->openDevice (savedState.get());
    applyDeviceInputPolicy();
    notify (Change::Device);
    return lastDeviceError;
}

juce::String EngineController::retryDevice()
{
    const auto safety = host->getDeviceSafetyState();
    if (safety.kind == DeviceSafetyState::Kind::LoopbackPair)
    {
        // The pair as the device manager has it now (the user may have
        // switched the system output back to the headset meanwhile).
        auto input = safety.inputDeviceName, output = safety.outputDeviceName;
        if (options.openAudioDevice && getDeviceManager().getCurrentAudioDevice() != nullptr)
        {
            const auto setup = getDeviceManager().getAudioDeviceSetup();
            input = setup.inputDeviceName;
            output = setup.outputDeviceName;
        }
        host->checkLoopbackPair (input, output);
        return {};
    }
    if (! options.openAudioDevice)
    {
        host->clearDeviceError(); // nothing to re-open: the banner is dismissed
        return {};
    }
    return reopenDevice();
}

bool EngineController::looksLikeLoopbackDevice (const juce::String& inputDeviceName)
{
    // Only devices that cannot contain our own output. "Stereo Mix" / "What U
    // Hear" / "Monitor of <speakers>" capture the physical output and would
    // create a feedback loop, so they are deliberately not matched.
    const auto name = inputDeviceName.toLowerCase();
    static const char* const patterns[] = { "flubsound", "cable output", "vb-audio", "voicemeeter", "blackhole", "soundflower", "loopback" };
    for (const auto* p : patterns)
        if (name.contains (p))
            return true;
    return false;
}

void EngineController::applyDeviceInputPolicy()
{
    const auto mode = settings->getDeviceInputMode();

    bool useInput = false;
    if (mode == AppSettings::DeviceInputMode::On)
        useInput = true;
    else if (mode == AppSettings::DeviceInputMode::Automatic && getDeviceManager().getCurrentAudioDevice() != nullptr)
        useInput = looksLikeLoopbackDevice (getDeviceManager().getAudioDeviceSetup().inputDeviceName);

    std::array<int, AudioEngineHost::kMaxStrips> map;
    map.fill (-1);

    if (useInput)
    {
        // Multi-strip map "Game=0;Music=8" wins over the single-strip setting.
        bool mapped = false;
        for (const auto& entry : juce::StringArray::fromTokens (settings->getDeviceInputMap(), ";,", {}))
        {
            const int strip = findStrip (entry.upToFirstOccurrenceOf ("=", false, false).trim());
            const auto channelText = entry.fromFirstOccurrenceOf ("=", false, false).trim();
            if (strip >= 0 && channelText.containsOnly ("0123456789") && channelText.isNotEmpty())
            {
                map[static_cast<size_t> (strip)] = channelText.getIntValue();
                mapped = true;
            }
        }

        if (! mapped)
        {
            int strip = findStrip (settings->getDeviceInputStripName());
            map[static_cast<size_t> (strip >= 0 ? strip : 0)] = 0;
        }
    }

    host->setDeviceInputMap (map);
}

void EngineController::setDeviceInputMode (AppSettings::DeviceInputMode mode)
{
    settings->setDeviceInputMode (mode);
    applyDeviceInputPolicy();
    notify (Change::Settings);
}

void EngineController::setDeviceInputStrip (int strip)
{
    settings->setDeviceInputStripName (getStripName (resolveStrip (strip)));
    applyDeviceInputPolicy();
    notify (Change::Settings);
}

void EngineController::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The device manager changed (device / rate / block / channels, or a device
    // appeared / disappeared).
    trackPreferredOutput (false);
    persistDeviceState();
    applyDeviceInputPolicy();
    updateDeviceProfile();
    notify (Change::Device);
}

// =============================================================================
// Output device profiles (headsets)
// =============================================================================
void EngineController::loadDeviceProfiles()
{
    std::string error;
    const auto userFile = userDataFolder().getChildFile ("device-profiles.json");
    if (userFile.existsAsFile())
    {
        // Read through juce::File (the OS's wide-character API on Windows), so
        // a user-data folder with a non-ASCII name works whatever the ANSI
        // code page; the JSON text itself is UTF-8.
        flub::json::Value root;
        if (flub::json::parse (userFile.loadFileAsString().toStdString(), root, error) && deviceProfiles.load (root, error))
            return;
        DBG ("Flubsound: " << userFile.getFullPathName() << " ignored: " << error);
        error.clear();
    }
    if (! deviceProfiles.loadBuiltIn (error))
    {
        DBG ("Flubsound: device profiles unavailable: " << error);
        juce::ignoreUnused (error);
    }
}

juce::String EngineController::getDeviceProfileName() const
{
    return deviceMatch.profile != nullptr ? juce::String::fromUTF8 (deviceMatch.profile->displayName.c_str()) : juce::String();
}

void EngineController::updateDeviceProfile()
{
    adviceForGaming = getMode (selectedStrip) == ModeValue::Gaming;
    auto* device = getDeviceManager().getCurrentAudioDevice();
    if (device == nullptr)
    {
        if (simulatedOutputName.isNotEmpty())
        {
            applyDeviceProfile (simulatedOutputName, simulatedSampleRate, simulatedOutputChannels);
            return;
        }
        deviceMatch = {};
        deviceAdvice = {};
        currentOutputName = {};
        host->setMasterCeilingDb (-1.0f);
        applyDeviceCorrection();
        return;
    }
    applyDeviceProfile (getDeviceManager().getAudioDeviceSetup().outputDeviceName, device->getCurrentSampleRate(),
                        device->getActiveOutputChannels().countNumberOfSetBits());
}

void EngineController::simulateOutputDevice (const juce::String& name, double sampleRate, int outputChannels)
{
    if (options.openAudioDevice)
        return; // never overrides a real device
    simulatedOutputName = name;
    simulatedSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    simulatedOutputChannels = juce::jmax (1, outputChannels);
    updateDeviceProfile();
    notify (Change::Device);
}

void EngineController::applyDeviceProfile (const juce::String& outputName, double sampleRate, int outputChannels)
{
    using flub::device::Connection;
    currentOutputName = outputName;

    // The OS knows the real transport (USB vs Bluetooth vs hands-free) where
    // it can; name / format heuristics cover the rest.
    Connection hint = Connection::Unknown;
    switch (flub::platform::AudioEndpoints::queryOutputTransport (currentOutputName.toStdString()))
    {
        case flub::platform::EndpointTransport::Analog: hint = Connection::Analog; break;
        case flub::platform::EndpointTransport::Usb: hint = Connection::Usb; break;
        case flub::platform::EndpointTransport::Bluetooth: hint = Connection::Bluetooth; break;
        case flub::platform::EndpointTransport::BluetoothHandsFree: hint = Connection::BluetoothHandsFree; break;
        case flub::platform::EndpointTransport::Hdmi:
        case flub::platform::EndpointTransport::Virtual:
        case flub::platform::EndpointTransport::Unknown: break;
    }

    deviceMatch = deviceProfiles.match (currentOutputName.toStdString(), sampleRate, outputChannels, hint);
    deviceAdvice = flub::device::adviceFor (deviceMatch, sampleRate, getMode (selectedStrip) == ModeValue::Gaming);

    // Applied to the master limiter on the audio thread (atomic hand-off); user
    // presets keep their own ceilings, the cap only ever lowers the output.
    host->setMasterCeilingDb (deviceAdvice.ceilingDbTp);

    // The endpoint's own correction curve (never from the family-level
    // profile match above: a family name says nothing about one unit).
    applyDeviceCorrection();
}

// =============================================================================
// Device correction (docs/11 E15)
// =============================================================================
void EngineController::applyDeviceCorrection()
{
    if (currentOutputName != correctionEndpoint)
    {
        correctionEndpoint = currentOutputName;
        correctionCompare = false;
    }

    // No stored (or no readable) curve: the default settings, a flat unity stage.
    flub::DeviceCorrectionSettings next;
    if (correctionEndpoint.isNotEmpty())
        if (const auto entry = settings->getDeviceCorrection (correctionEndpoint))
            if (flub::CorrectionCurve curve; flub::eqtext::parse (entry->curveText.toStdString(), curve).ok)
            {
                next.curve = curve;
                next.enabled = entry->enabled;
                next.compare = correctionCompare && entry->enabled;
            }

    if (! (next == host->getDeviceCorrection()))
        host->setDeviceCorrection (next);
}

EngineController::DeviceCorrectionInfo EngineController::getDeviceCorrection() const
{
    DeviceCorrectionInfo info;
    info.endpoint = currentOutputName;
    if (const auto entry = currentOutputName.isNotEmpty() ? settings->getDeviceCorrection (currentOutputName) : std::nullopt)
    {
        const auto& applied = host->getDeviceCorrection();
        info.hasCurve = true;
        info.name = entry->name;
        info.enabled = entry->enabled;
        info.comparing = applied.compare;
        info.numFilters = applied.curve.numFilters;
        info.curveText = entry->curveText;
        info.preampDb = host->getDeviceCorrectionPreampDb();
        const auto prediction = host->getDeviceCorrectionPrediction();
        info.maxBoostDb = prediction.maxBoostDb;
        info.maxBoostHz = prediction.atHz;
    }
    return info;
}

bool EngineController::importDeviceCorrection (const juce::File& file, juce::String& error, juce::StringArray* warnings)
{
    constexpr juce::int64 kMaxFileBytes = 256 * 1024; // ParametricEQ.txt files are a few hundred bytes
    if (! file.existsAsFile())
    {
        error = "File not found: " + file.getFullPathName();
        return false;
    }
    if (file.getSize() > kMaxFileBytes)
    {
        error = "Not a ParametricEQ.txt (the file is larger than 256 KB).";
        return false;
    }
    return importDeviceCorrectionText (file.loadFileAsString(), file.getFileName(), error, warnings);
}

bool EngineController::importDeviceCorrectionText (const juce::String& text, const juce::String& name, juce::String& error,
                                                   juce::StringArray* warnings)
{
    if (currentOutputName.isEmpty())
    {
        error = "No output device is open: choose one on the Audio page first.";
        return false;
    }

    flub::CorrectionCurve curve;
    const auto result = flub::eqtext::parse (text.toStdString(), curve);
    if (! result.ok)
    {
        error = juce::String::fromUTF8 (result.error.c_str());
        return false;
    }
    if (warnings != nullptr)
        for (const auto& w : result.warnings)
            warnings->add (juce::String::fromUTF8 (w.c_str()));

    DeviceCorrectionEntry entry;
    entry.endpoint = currentOutputName;
    entry.name = name;
    entry.enabled = true;
    entry.curveText = juce::String::fromUTF8 (flub::eqtext::format (curve).c_str());
    settings->setDeviceCorrection (entry);
    correctionCompare = false;
    applyDeviceCorrection();
    notify (Change::Settings);
    return true;
}

void EngineController::setDeviceCorrectionEnabled (bool shouldBeEnabled)
{
    auto entry = currentOutputName.isNotEmpty() ? settings->getDeviceCorrection (currentOutputName) : std::nullopt;
    if (! entry || entry->enabled == shouldBeEnabled)
        return;
    entry->enabled = shouldBeEnabled;
    settings->setDeviceCorrection (*entry);
    applyDeviceCorrection();
    notify (Change::Settings);
}

void EngineController::setDeviceCorrectionCompare (bool comparing)
{
    if (correctionCompare == comparing)
        return;
    correctionCompare = comparing;
    applyDeviceCorrection();
    notify (Change::Settings);
}

void EngineController::removeDeviceCorrection()
{
    if (currentOutputName.isEmpty() || ! settings->getDeviceCorrection (currentOutputName))
        return;
    settings->removeDeviceCorrection (currentOutputName);
    correctionCompare = false;
    applyDeviceCorrection();
    notify (Change::Settings);
}

void EngineController::trackPreferredOutput (bool rescan)
{
    if (restoringPreferred || ! options.openAudioDevice)
        return;

    auto& dm = getDeviceManager();
    const auto current = dm.getAudioDeviceSetup().outputDeviceName;
    if (preferredOutputName.isEmpty())
    {
        if (current.isNotEmpty())
        {
            preferredOutputName = current;
            settings->setPreferredOutputDevice (current);
        }
        return;
    }
    if (current == preferredOutputName)
    {
        preferredMissing = false;
        return;
    }

    auto* type = dm.getCurrentDeviceTypeObject();
    if (type == nullptr)
        return;
    if (rescan)
        type->scanForDevices(); // some backends (e.g. ALSA) do not report hot-plugs themselves

    if (! type->getDeviceNames (false).contains (preferredOutputName))
    {
        // The preferred device (e.g. a USB / wireless headset) is gone and JUCE
        // fell back to another output: keep waiting for it to return.
        preferredMissing = true;
        return;
    }

    if (preferredMissing)
    {
        // It is back: switch to it again.
        auto setup = dm.getAudioDeviceSetup();
        setup.outputDeviceName = preferredOutputName;
        restoringPreferred = true;
        const auto error = dm.setAudioDeviceSetup (setup, true);
        restoringPreferred = false;
        preferredMissing = ! error.isEmpty();
        if (error.isNotEmpty())
            lastDeviceError = error;
    }
    else
    {
        // Both devices exist and the user picked a different one: that is the
        // new preference.
        preferredOutputName = current;
        settings->setPreferredOutputDevice (current);
    }
}

// =============================================================================
void EngineController::updateOverloadWatchdog (const EngineStatus& status)
{
    OverloadWatchdog::Sample sample;
    sample.running = status.deviceOpen && status.running;
    sample.load = status.cpuLoad;
    sample.glitchCount = status.deviceOpen ? static_cast<int64_t> (status.glitches) : -1;

    // The header's CPU readout turns into an overload warning (docs/01 §7).
    bool changed = overloadWatchdog.update (sample) != OverloadWatchdog::Event::None;

    // Opt-in (default off): on a lasting overload, one latency-profile step
    // down (AutoLoadReducer: rate limited, never back up). Applied exactly
    // like a user change - parameter writes on this (message) thread, the
    // re-prepare follows in AudioEngineHost's poll - and reported through the
    // same Change::Device (header tooltip, one tray bubble, Settings text).
    // Only polls that are still stressed count towards the step: the latched
    // isOverloaded() outlasts every episode by exitPolls calm polls, so a
    // single xrun burst at low load would otherwise always step.
    if (const auto next = loadReducer.update (settings->getReduceLoadOnOverload(), overloadWatchdog.isStressed(), getLatencyProfile()))
    {
        applyLatencyProfile (*next);
        changed = true;
    }

    if (changed)
        notify (Change::Device);
}

LatencyProfileValue EngineController::getLatencyProfile()
{
    constexpr int lowest = static_cast<int> (LatencyProfileValue::Quality), highest = static_cast<int> (LatencyProfileValue::LowLatency);
    const int v = static_cast<int> (std::lround (getParams (selectedStrip).get (LatencyProfile)));
    return static_cast<LatencyProfileValue> (std::clamp (v, lowest, highest));
}

void EngineController::applyLatencyProfile (LatencyProfileValue profile)
{
    // Every strip, both banks: the Settings profile is one choice for the
    // whole engine, and A/B switching must not trigger re-prepares.
    const float v = static_cast<float> (static_cast<int> (profile));
    for (int s = 0; s < getNumStrips(); ++s)
    {
        auto& store = getParams (s);
        store.set (Bank::A, LatencyProfile, v);
        store.set (Bank::B, LatencyProfile, v);
    }
}

void EngineController::setLatencyProfile (LatencyProfileValue profile)
{
    applyLatencyProfile (profile);
    loadReducer.profileChangedByUser();
    // A choice by hand only: the automatic overload response steps down to
    // shed DSP load, and a smaller quantum would add callbacks, not remove them.
    requestGraphQuantum (profile, true);
    notify (Change::Settings);
}

EngineController::GraphQuantumPlan EngineController::planGraphQuantum (LatencyProfileValue profile, const char* userLatency,
                                                                       const char* userProps)
{
    GraphQuantumPlan plan;
    if (userLatency != nullptr && *userLatency != '\0')
        return plan; // the user's own request wins, for every profile
    const bool low = profile == LatencyProfileValue::LowLatency;
    plan.latency = low ? kPipeWireLowLatency : kPipeWireBalancedLatency;
    if (userProps == nullptr || *userProps == '\0')
    {
        if (low)
            plan.props = kPipeWireLockQuantumProps;
        else
            plan.clearProps = true;
    }
    return plan;
}

bool EngineController::applyGraphQuantum (LatencyProfileValue profile, const char* userLatency, const char* userProps)
{
   #if defined(__linux__)
    const auto plan = planGraphQuantum (profile, userLatency, userProps);
    const auto current = [] (const char* name) { const char* v = std::getenv (name); return juce::String (v != nullptr ? v : ""); };
    bool changed = false;
    if (plan.latency.isNotEmpty() && current ("PIPEWIRE_LATENCY") != plan.latency)
    {
        ::setenv ("PIPEWIRE_LATENCY", plan.latency.toRawUTF8(), 1);
        changed = true;
    }
    if (plan.props.isNotEmpty() && current ("PIPEWIRE_PROPS") != plan.props)
    {
        ::setenv ("PIPEWIRE_PROPS", plan.props.toRawUTF8(), 1);
        changed = true;
    }
    else if (plan.clearProps && current ("PIPEWIRE_PROPS") == kPipeWireLockQuantumProps)
    {
        ::unsetenv ("PIPEWIRE_PROPS"); // only the lock this process exported
        changed = true;
    }
    return changed;
   #else
    juce::ignoreUnused (profile, userLatency, userProps);
    return false;
   #endif
}

void EngineController::requestGraphQuantum (LatencyProfileValue profile, bool reopen)
{
   #if defined(__linux__)
    // A device session only: tests and headless renders leave the process
    // environment alone.
    if (! options.openAudioDevice)
        return;
    const auto& user = startupPipeWire;
    if (! applyGraphQuantum (profile, user.latency ? user.latency->c_str() : nullptr, user.props ? user.props->c_str() : nullptr))
        return;
    // PipeWire reads the request when a stream opens: re-open a device that
    // talks to it (pipewire-jack, or ALSA through PipeWire's plug-in).
    auto* device = getDeviceManager().getCurrentAudioDevice();
    if (reopen && device != nullptr && (device->getTypeName() == "JACK" || device->getTypeName() == "ALSA"))
    {
        persistDeviceState();
        reopenDevice();
    }
   #else
    juce::ignoreUnused (profile, reopen);
   #endif
}

void EngineController::setProtectionStrength (ProtectionStrength strength)
{
    protectionStrength = strength;
    settings->getPropertiesFile().setValue (kProtectionStrengthKey, getProtectionStrengthName (strength).toLowerCase());
    applyProtectionStrength();
    notify (Change::Settings);
}

void EngineController::applyProtectionStrength()
{
    // A relaxed atomic per chain, taken by the audio thread at its next
    // segment (ProcessingChain::setProtectionStrength). The newest engine's
    // chains: an engine being faded out keeps its own until it is retired.
    for (int s = 0; s < getNumStrips(); ++s)
        if (auto& chain = getChain (s); chain.getProtectionStrength() != protectionStrength)
            chain.setProtectionStrength (protectionStrength);
}

juce::String EngineController::getProtectionStrengthName (ProtectionStrength strength)
{
    switch (strength)
    {
        case ProtectionStrength::Normal: return "Normal";
        case ProtectionStrength::Strict: return "Strict";
        case ProtectionStrength::Off: break;
    }
    return "Off";
}

void EngineController::setReduceLoadOnOverload (bool shouldReduce)
{
    settings->setReduceLoadOnOverload (shouldReduce);
    notify (Change::Settings);
}

juce::String EngineController::describeLoadReduction() const
{
    if (! loadReducer.hasReduced())
        return {};
    const auto& choices = layout()[static_cast<size_t> (LatencyProfile)].choices;
    const auto name = [&choices] (LatencyProfileValue p)
    {
        const auto i = static_cast<size_t> (p);
        return i < choices.size() ? juce::String (choices[i]) : juce::String (static_cast<int> (p));
    };

    // The whole ladder walked since the user's choice: "Quality -> Balanced -> Low Latency".
    const auto& st = loadReducer.getState();
    juce::String path = name (st.restoreProfile);
    auto p = st.restoreProfile;
    for (int i = 0; i < st.steps; ++i)
    {
        if (const auto next = AutoLoadReducer::nextLower (p))
        {
            p = *next;
            path << " -> " << name (p);
        }
    }

    return "Processing load reduced automatically after a sustained CPU overload: latency profile " + path
           + ". Restore " + name (st.restoreProfile) + " in Settings > Processing.";
}

void EngineController::restoreLatencyProfile()
{
    if (loadReducer.hasReduced())
        setLatencyProfile (loadReducer.getState().restoreProfile);
}

std::vector<EngineController::CaptureStream> EngineController::getCaptureStreams() const
{
    std::vector<CaptureStream> streams;
    for (const auto& info : host->getCaptures())
    {
        CaptureStream stream;
        stream.info = info;
        stream.stripName = getStripName (info.strip);
        for (const auto& app : routing->getApps())
            if (app.captureId == info.id && app.processId == info.processId)
                stream.appName = app.displayName.isNotEmpty() ? app.displayName : app.executable;
        streams.push_back (stream);
    }
    return streams;
}

void EngineController::timerCallback()
{
    // (Structural re-prepares are handled by AudioEngineHost's own 5 Hz poll.)
    updateOverloadWatchdog (host->getStatus());
    applyProtectionStrength(); // an engine built since (onEngineConfigured is asynchronous)
    pollForegroundApp();

    if (++timerTicks % kPersistEveryTicks == 0)
        persistStripStates (false);

    // While the preferred output (e.g. a headset) is missing, look for it.
    if (preferredMissing && options.openAudioDevice && timerTicks % kRescanEveryTicks == 0)
    {
        const auto before = getDeviceManager().getAudioDeviceSetup().outputDeviceName;
        trackPreferredOutput (true);
        if (getDeviceManager().getAudioDeviceSetup().outputDeviceName != before)
        {
            updateDeviceProfile();
            notify (Change::Device);
        }
    }
}

void EngineController::handleAsyncUpdate()
{
    if (! pendingPresetWarnings.empty())
        notify (Change::Preset);
}

void EngineController::renderOffline (StripSignalSource& source, int numSamples)
{
    host->renderOffline (source, numSamples);
}

// =============================================================================
// Automatic profiles (foreground application -> preset)
// =============================================================================
bool EngineController::isAutoProfileSupported() const
{
    return foregroundApp != nullptr && foregroundApp->isSupported();
}

juce::String EngineController::getAutoProfileUnsupportedReason() const
{
    if (isAutoProfileSupported())
        return {};
    if (foregroundApp != nullptr)
        return juce::String::fromUTF8 (foregroundApp->unsupportedReason().c_str());
    if (! platform_bridge::servicesCompiledIn())
        return "Automatic profiles need the Flubsound platform services, which are not part of this build.";
    return "The foreground application cannot be detected on this system.";
}

void EngineController::setAutoProfilesEnabled (bool shouldBeEnabled)
{
    settings->setAutoProfilesEnabled (shouldBeEnabled);
    applyAutoProfileActions (autoProfiles.setEnabled (shouldBeEnabled));
    autoProfileError = {};
    notify (Change::Settings);
}

void EngineController::setAutoProfileRules (std::vector<AutoProfileRule> rules)
{
    for (auto& r : rules) // as AppSettings reads them back
    {
        r.executable = r.executable.trim();
        r.stripName = r.stripName.trim();
        r.presetId = r.presetId.trim();
    }
    rules.erase (std::remove_if (rules.begin(), rules.end(),
                                 [] (const AutoProfileRule& r)
                                 { return r.executable.trim().isEmpty() || r.stripName.isEmpty() || r.presetId.isEmpty(); }),
                 rules.end());
    settings->setAutoProfileRules (rules);
    applyAutoProfileActions (autoProfiles.setRules (std::move (rules)));
    autoProfileError = {};
    notify (Change::Settings);
}

bool EngineController::addAutoProfileRule (const AutoProfileRule& rule)
{
    if (rule.executable.trim().isEmpty() || rule.stripName.trim().isEmpty() || rule.presetId.trim().isEmpty())
        return false;
    auto rules = autoProfiles.getRules();
    const auto wanted = rule.executable.trim();
    rules.erase (std::remove_if (rules.begin(), rules.end(),
                                 [&wanted] (const AutoProfileRule& r) { return AppRouting::executablesMatch (r.executable.trim(), wanted); }),
                 rules.end());
    rules.push_back (rule);
    setAutoProfileRules (std::move (rules));
    return true;
}

juce::String EngineController::describeAutoProfile() const
{
    if (autoProfileError.isNotEmpty())
        return autoProfileError;
    const auto* rule = autoProfiles.getActiveRule();
    if (rule == nullptr)
        return {};
    const auto* preset = presets->findById (rule->presetId);
    auto app = rule->executable.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
    if (app.endsWithIgnoreCase (".exe"))
        app = app.dropLastCharacters (4);
    return app + " in front: " + rule->stripName + " plays " + (preset != nullptr ? preset->name : rule->presetId)
           + (rule->restoreOnExit ? " (restored when it leaves)" : "");
}

void EngineController::pollForegroundApp()
{
    if (! isAutoProfileSupported() || ! autoProfiles.isEnabled())
        return;

    // A preset changed on the rule's strip outside loadPreset() & co. (e.g.
    // Reset strip, a renamed preset) counts as a manual change too. A rule
    // that could not be applied (its preset is gone) loaded nothing to watch
    // and keeps its error on show until its application leaves.
    if (const auto* rule = autoProfiles.getActiveRule(); rule != nullptr && autoAppliedPresetId.isNotEmpty())
    {
        const auto stripName = rule->stripName;
        const int s = findStrip (stripName);
        if (s < 0 || presets->getCurrentPresetId (s) != autoAppliedPresetId)
            cancelAutoProfile (stripName);
    }

    flub::platform::ForegroundAppInfo info;
    AutoProfileSwitcher::Sample sample;
    sample.valid = foregroundApp->query (info);
    if (sample.valid)
    {
        sample.isThisProcess = info.isThisProcess;
        sample.processId = info.processId;
        sample.executable = juce::String::fromUTF8 ((info.executablePath.empty() ? info.executableName : info.executablePath).c_str());
        sample.bundleId = juce::String::fromUTF8 (info.bundleId.c_str());

        // Recently seen applications, for the "add rule" menu.
        const auto name = juce::String::fromUTF8 (info.executableName.c_str());
        if (! info.isThisProcess && name.isNotEmpty() && recentForegroundApps[0] != name)
        {
            recentForegroundApps.removeString (name);
            recentForegroundApps.insert (0, name);
            while (recentForegroundApps.size() > kMaxRecentForegroundApps)
                recentForegroundApps.remove (recentForegroundApps.size() - 1);
        }
    }

    applyAutoProfileActions (autoProfiles.update (sample));
}

void EngineController::applyAutoProfileActions (const std::vector<AutoProfileSwitcher::Action>& actions)
{
    for (const auto& action : actions)
    {
        if (action.kind == AutoProfileSwitcher::Action::Kind::Apply)
            applyAutoProfile (action.rule);
        else
            endAutoProfile (action);
    }
    if (! actions.empty())
        notify (Change::Routing); // the routing panel shows the active rule
}

void EngineController::applyAutoProfile (const AutoProfileRule& rule)
{
    autoRestorePoint.reset();
    autoAppliedPresetId = {};
    autoProfileError = {};

    const int s = findStrip (rule.stripName);
    const auto* info = presets->findById (rule.presetId);
    if (s < 0 || info == nullptr)
    {
        autoProfileError = "Automatic profile for " + rule.executable + ": "
                           + (s < 0 ? "there is no " + rule.stripName + " strip." : "the preset " + rule.presetId + " no longer exists.");
        return;
    }

    auto& store = getParams (s);
    if (rule.restoreOnExit)
    {
        AutoProfileRestorePoint point;
        point.stripName = getStripName (s);
        point.presetId = presets->getCurrentPresetId (s);
        point.presetModified = presets->isModified (s, store);
        point.activeBank = store.getActiveBank();
        for (int i = 0; i < kNumParams; ++i)
        {
            point.bankA.push_back (store.get (Bank::A, i));
            point.bankB.push_back (store.get (Bank::B, i));
        }
        autoRestorePoint = std::move (point);
    }

    // Like loadPreset(), but not a manual change (that would cancel the rule).
    forgetLatches (s);
    const auto preset = *info;
    juce::String error;
    if (! presets->loadIntoStrip (s, preset, store, error))
    {
        autoRestorePoint.reset();
        autoProfileError = "Automatic profile for " + rule.executable + ": " + error;
        return;
    }
    if (rule.mode != AutoProfileRule::Mode::Preset)
        store.set (Mode, static_cast<float> (static_cast<int> (rule.mode == AutoProfileRule::Mode::Gaming ? ModeValue::Gaming : ModeValue::Music)));
    autoAppliedPresetId = preset.id;
    settings->setLastPreset (getStripName (s), preset.id);
    notify (Change::Preset);
}

void EngineController::endAutoProfile (const AutoProfileSwitcher::Action& action)
{
    auto point = std::move (autoRestorePoint);
    autoRestorePoint.reset();
    autoAppliedPresetId = {};
    autoProfileError = {};

    const int s = point.has_value() ? findStrip (point->stripName) : -1;
    if (! action.restore || s < 0 || ! point->stripName.equalsIgnoreCase (action.rule.stripName))
        return;

    // The strip's own state again, both banks. Application state that shares
    // the store (Bypass All, latency profile, loudness-matched bypass) stays
    // as it is now. A preset that was modified before is loaded first so the
    // restored values still show as "modified" against it.
    forgetLatches (s);
    auto& store = getParams (s);
    store.setActiveBank (point->activeBank);
    const auto* info = presets->findById (point->presetId);
    juce::String error;
    const bool reference = info != nullptr && point->presetModified && presets->loadIntoStrip (s, PresetInfo (*info), store, error);

    for (const auto bank : { Bank::A, Bank::B })
    {
        const auto& values = bank == Bank::A ? point->bankA : point->bankB;
        for (int i = 0; i < kNumParams && i < static_cast<int> (values.size()); ++i)
            if (i != BypassAll && i != LatencyProfile && i != LoudnessMatchBypass)
                store.set (bank, i, values[static_cast<size_t> (i)]);
    }
    if (! reference)
        presets->setCurrentPresetId (s, info != nullptr ? point->presetId : juce::String(), &store);
    settings->setLastPreset (getStripName (s), info != nullptr ? point->presetId : juce::String());
    notify (Change::Preset);
}

// =============================================================================
// Hotkey target and latched overrides (docs/11 E56)
// =============================================================================
int EngineController::getHotkeyStrip() const
{
    // An automatic profile says which strip the game in front plays on.
    if (const auto* rule = autoProfiles.getActiveRule())
        if (const int s = findStrip (rule->stripName); s >= 0)
            return s;
    if (const int s = findStrip (settings->getHotkeyStripName()); s >= 0)
        return s;
    return 0;
}

void EngineController::setHotkeyStripName (const juce::String& stripName)
{
    settings->setHotkeyStripName (stripName);
    notify (Change::Settings);
}

void EngineController::engageLatch (int strip, Latch& latch, std::vector<Override> overrides)
{
    auto& store = getParams (strip);
    latch.on = true;
    latch.applied = std::move (overrides);
    latch.savedA.clear();
    latch.savedB.clear();
    for (const auto& o : latch.applied)
    {
        latch.savedA.push_back (store.get (Bank::A, o.id));
        latch.savedB.push_back (store.get (Bank::B, o.id));
        store.set (Bank::A, o.id, o.value);
        store.set (Bank::B, o.id, o.value);
    }
}

void EngineController::releaseLatch (int strip, Latch& latch, bool restore)
{
    if (! latch.on)
        return;
    if (restore && strip >= 0 && strip < getNumStrips())
    {
        // A value changed since the override (by hand, a macro edit) is the
        // user's now and stays; the stored value reads back clamped, so
        // compare with what the store made of the override.
        auto& store = getParams (strip);
        for (size_t i = 0; i < latch.applied.size(); ++i)
        {
            const auto& o = latch.applied[i];
            const float overridden = layout()[static_cast<size_t> (o.id)].clamp (o.value);
            if (store.get (Bank::A, o.id) == overridden)
                store.set (Bank::A, o.id, latch.savedA[i]);
            if (store.get (Bank::B, o.id) == overridden)
                store.set (Bank::B, o.id, latch.savedB[i]);
        }
    }
    latch = {};
}

void EngineController::releaseAllLatches (bool restore)
{
    for (int s = 0; s < AudioEngineHost::kMaxStrips; ++s)
    {
        releaseLatch (s, focusLatches[static_cast<size_t> (s)], restore);
        releaseLatch (s, nightLatches[static_cast<size_t> (s)], restore);
    }
}

void EngineController::forgetLatches (int strip)
{
    if (strip < 0 || strip >= AudioEngineHost::kMaxStrips)
        return;
    releaseLatch (strip, focusLatches[static_cast<size_t> (strip)], false);
    releaseLatch (strip, nightLatches[static_cast<size_t> (strip)], false);
}

bool EngineController::setFocus (int strip, bool on)
{
    const int s = resolveStrip (strip);
    auto& latch = focusLatches[static_cast<size_t> (s)];
    if (on == latch.on)
        return true;
    if (on)
    {
        if (getMode (s) != ModeValue::Gaming)
            return false; // Macro 1 is Footsteps in Gaming mode only (Punch in Music)
        engageLatch (s, latch, { { Macro1, 1.0f } });
    }
    else
    {
        releaseLatch (s, latch, true);
    }
    notify (Change::Parameters);
    return true;
}

bool EngineController::isFocused (int strip) const noexcept
{
    return strip >= 0 && strip < AudioEngineHost::kMaxStrips && focusLatches[static_cast<size_t> (strip)].on;
}

void EngineController::setNight (int strip, bool on)
{
    const int s = resolveStrip (strip);
    auto& latch = nightLatches[static_cast<size_t> (s)];
    if (on == latch.on)
        return;
    if (on)
    {
        // The dynamics of the Night Mode Gaming factory preset.
        engageLatch (s, latch,
                     { { AutoLevelOn, 1.0f },
                       { AutoLevelTargetLufs, -20.0f },
                       { CompressorOn, 1.0f },
                       { CompThresholdDb, -24.0f },
                       { CompRatio, 3.0f },
                       { CompMakeupDb, 6.0f },
                       { CompUpThresholdDb, -38.0f },
                       { CompUpRatio, 2.5f },
                       { CompUpMaxGainDb, 6.0f } });
    }
    else
    {
        releaseLatch (s, latch, true);
    }
    notify (Change::Parameters);
}

bool EngineController::isNight (int strip) const noexcept
{
    return strip >= 0 && strip < AudioEngineHost::kMaxStrips && nightLatches[static_cast<size_t> (strip)].on;
}

float EngineController::chatMixOffsetDb (int strip) const
{
    const auto name = getStripName (strip);
    if (name.equalsIgnoreCase (kChatMixGameStrip))
        return -chatMix * kChatMixRangeDb;
    if (name.equalsIgnoreCase (kChatMixChatStrip))
        return chatMix * kChatMixRangeDb;
    return 0.0f;
}

bool EngineController::setChatMix (float balance)
{
    const int game = findStrip (kChatMixGameStrip), chat = findStrip (kChatMixChatStrip);
    if (game < 0 || chat < 0)
        return false;
    // Whole 10 % steps, so repeated nudges land on round values and 0 exactly.
    const float next = std::round (std::clamp (balance, -1.0f, 1.0f) * 10.0f) / 10.0f;
    if (next == chatMix)
        return true;
    chatMix = next;
    applyStripGain (game);
    applyStripGain (chat);
    notify (Change::Parameters);
    return true;
}

juce::String EngineController::describeChatMix() const
{
    if (chatMix == 0.0f)
        return "centred";
    const auto db = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; };
    return juce::String (kChatMixGameStrip) + " " + db (-chatMix * kChatMixRangeDb) + ", " + kChatMixChatStrip + " "
           + db (chatMix * kChatMixRangeDb);
}

void EngineController::presetChangedByUser (int strip)
{
    forgetLatches (strip);
    cancelAutoProfile (getStripName (strip));
}

void EngineController::cancelAutoProfile (const juce::String& stripName)
{
    if (! autoProfiles.cancelStrip (stripName))
        return;
    autoRestorePoint.reset();
    autoAppliedPresetId = {};
    autoProfileError = {};
    notify (Change::Routing);
}
} // namespace flub::app
