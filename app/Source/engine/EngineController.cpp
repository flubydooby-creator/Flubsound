#include "EngineController.h"

#include "flub/engine/MacroMap.h"
#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>

namespace flub::app
{
using namespace flub::param;

namespace
{
constexpr int kTimerHz = 5;
constexpr int kPersistEveryTicks = 5 * kTimerHz; // strip state autosave: every 5 s

constexpr const char* kStateFormat = "flubsound-strip-state";

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
    routing = std::make_unique<AppRouting> (*host, *settings);

    host->onEngineConfigured = [this] { notify (Change::Engine); };
    host->onDeviceError = [this] (const juce::String& message)
    {
        lastDeviceError = message;
        notify (Change::Device);
    };
    presets->onPresetListChanged = [this] { notify (Change::Preset); };
    routing->onChanged = [this] { notify (Change::Routing); };

    for (int i = 0; i < getNumStrips(); ++i)
    {
        const auto name = getStripName (i);
        host->setStripGainDb (i, settings->getStripGainDb (name));
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

    if (options.openAudioDevice)
    {
        const auto savedState = settings->getDeviceState();
        lastDeviceError = host->openDevice (savedState.get());
        getDeviceManager().addChangeListener (this);
        applyDeviceInputPolicy();
    }

    if (options.enableAppRouting)
        routing->start();

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
    routing->shutdown();

    if (options.openAudioDevice)
    {
        getDeviceManager().removeChangeListener (this);
        persistDeviceState();
    }

    persistStripStates (true);
    settings->setSelectedStrip (selectedStrip);
    settings->save();

    host->onEngineConfigured = nullptr;
    host->onDeviceError = nullptr;
    host->closeDevice();
    host->stopAllCaptures();
}

void EngineController::notify (Change change)
{
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
    notify (Change::SelectedStrip);
}

ParameterStore& EngineController::getParams (int strip)
{
    return host->getMixEngine().params (resolveStrip (strip));
}

flub::ProcessingChain& EngineController::getChain (int strip)
{
    return host->getMixEngine().chain (resolveStrip (strip));
}

void EngineController::setStripGainDb (int strip, float gainDb)
{
    const int s = resolveStrip (strip);
    host->setStripGainDb (s, gainDb);
    settings->setStripGainDb (getStripName (s), host->getStripGainDb (s));
}

void EngineController::setStripMuted (int strip, bool muted)
{
    const int s = resolveStrip (strip);
    host->setStripMuted (s, muted);
    settings->setStripMuted (getStripName (s), muted);
}

void EngineController::setStripLayout (const std::vector<flub::StripConfig>& newLayout)
{
    host->setStripLayout (newLayout);
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
    const float bypass = enabled ? 0.0f : 1.0f;
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

ModeValue EngineController::getMode (int strip)
{
    return getParams (resolveStrip (strip)).get (Mode) >= 0.5f ? ModeValue::Gaming : ModeValue::Music;
}

void EngineController::setMode (ModeValue mode, int strip)
{
    getParams (resolveStrip (strip)).set (Mode, static_cast<float> (static_cast<int> (mode)));
    notify (Change::Parameters);
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
        notify (Change::Preset);
    }
    return id;
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

        if (! restored)
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
    // The device manager changed (device / rate / block / channels).
    persistDeviceState();
    applyDeviceInputPolicy();
    notify (Change::Device);
}

// =============================================================================
void EngineController::timerCallback()
{
    // (Structural re-prepares are handled by AudioEngineHost's own 5 Hz poll.)
    if (++timerTicks % kPersistEveryTicks == 0)
        persistStripStates (false);
}

void EngineController::renderOffline (StripSignalSource& source, int numSamples)
{
    host->renderOffline (source, numSamples);
}
} // namespace flub::app
