#include "EngineController.h"
#include "settings/UserDataFolder.h"

#include "flub/engine/MacroMap.h"
#include "flub/io/Json.h"
#include "flub/io/ParametricEqText.h"
#include "flub/io/PresetIO.h"
#include "platform/PlatformBridge.h"
#include "platform/PlatformServices.h"
#include "platform/pipewire/PipeWireDeviceType.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace flub::app
{
using namespace flub::param;

namespace
{
constexpr int kTimerHz = 2;                      // also the overload watchdog's poll rate
constexpr int kPersistEveryTicks = 5 * kTimerHz; // strip state autosave: every 5 s
constexpr int kRescanEveryTicks = 5 * kTimerHz;  // missing preferred output: rescan every 5 s
                                                 // (ALSA probes every PCM device; too slow for every tick)
constexpr int kAntiCheatPollTicks = 10 * kTimerHz; // docs/11 E55: anti-cheat services, every 10 s

constexpr const char* kStateFormat = "flubsound-strip-state";
constexpr int kMaxRecentForegroundApps = 8;

// First-run defaults (docs/11 E36): what a strip without saved state loads.
constexpr const char* kFirstRunMusicPreset = "factory:music-flubsound-signature"; // Music, System, other stereo strips
constexpr const char* kFirstRunChatPreset = "factory:music-voice-chat";           // Chat (docs/11 E23)
constexpr const char* kFirstRunGamePreset = "factory:gaming-competitive-fps";     // Game / surround strips, capped:
// "First Run - Game" = Competitive FPS with Boost capped. Measured through
// the app's 8-channel Game strip with the docs/11 E59 slice's definitions
// (tests/app/test_app_first_run.cpp): -50 / -60 dBFS pink beds +0.50 / +0.48
// LU (Competitive FPS as shipped +0.81 at -60), step/bed contrast change
// +1.37 .. +1.50 / +4.79 .. +5.03 / +5.22 .. +5.60 dB on the 20 / 40 / 80 ms
// burst scenes at -14 / -24 / -40 / -50 LUFS. docs/11 E36's review dropped
// the earlier Footsteps 30 % / Detail 15 % caps: since E19 they no longer
// held the beds down (+0.49 / +0.47 LU with them) and cost about 3 dB of
// that contrast (-1.68 .. -1.58 / +2.28 .. +2.46 / +2.42 .. +2.70 dB).
constexpr float kFirstRunGameBoost = 0.20f; // below 0.25: Boost adds no maximizer drive

// Night listening (docs/11 E56 / E21): the latch copies these parameters from
// the Night Mode Gaming factory preset; kNightFallback are its values (the
// E21 Phase 3 retune) for a library without it (tests pin the two together).
constexpr const char* kNightPreset = "factory:gaming-night-mode";
constexpr std::pair<int, float> kNightFallback[] = {
    { AutoLevelOn, 1.0f },     { AutoLevelTargetLufs, -14.0f },   { GuardRange, static_cast<float> (GuardRangeValue::Lu20) },
    { CompressorOn, 1.0f },    { CompThresholdDb, -18.0f },       { CompRatio, 3.0f },
    { CompKneeDb, 10.0f },     { CompAttackMs, 3.0f },            { CompReleaseMs, 250.0f },
    { CompAutoRelease, 1.0f }, { CompMakeupDb, 0.0f },            { CompUpThresholdDb, -32.0f },
    { CompUpRatio, 2.5f },     { CompUpMaxGainDb, 6.0f },         { CompUpFloorDb, -62.0f },
};

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

/** The strip state; `substitute` (param::kNumParams values), when given,
    is saved as bank `substituteBank` instead of what the store holds. */
juce::String stripStateToJson (const ParameterStore& store, const std::vector<float>* substitute = nullptr, Bank substituteBank = Bank::A)
{
    const auto bankJson = [&] (Bank bank)
    {
        auto p = flub::preset::captureFromStore (store, bank);
        if (substitute != nullptr && bank == substituteBank && substitute->size() == p.values.size())
            p.values = *substitute;
        return flub::preset::toJson (p, false);
    };
    flub::json::Value root;
    root.set ("format", kStateFormat);
    root.set ("version", 1);
    root.set ("activeBank", store.getActiveBank() == Bank::A ? "A" : "B");
    root.set ("A", bankJson (Bank::A));
    root.set ("B", bankJson (Bank::B));
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
/** Reads the output endpoint's volume every kEndpointVolumePollMs off the
    message thread (a pactl child on Linux takes milliseconds) and hands the
    latest reading over; `onReading` is called on this thread after each read
    (the controller's triggerAsyncUpdate, which is thread-safe). */
class EngineController::EndpointVolumePoller final : private juce::Thread
{
public:
    using Reader = std::function<flub::platform::EndpointVolume (const std::string&)>;

    EndpointVolumePoller (Reader r, std::function<void()> ready, const juce::String& device)
        : juce::Thread ("Flubsound endpoint volume"), reader (std::move (r)), onReading (std::move (ready)), deviceName (device.toStdString())
    {
        startThread (juce::Thread::Priority::low);
    }

    ~EndpointVolumePoller() override { stopThread (4000); }

    void setDevice (const juce::String& device)
    {
        const std::scoped_lock hold (lock);
        deviceName = device.toStdString();
    }

    /** The newest reading and the device it is of, once. */
    std::optional<std::pair<flub::platform::EndpointVolume, juce::String>> take()
    {
        const std::scoped_lock hold (lock);
        if (! fresh)
            return std::nullopt;
        fresh = false;
        return std::make_pair (latest, juce::String (latestDevice));
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            std::string device;
            {
                const std::scoped_lock hold (lock);
                device = deviceName;
            }
            auto reading = reader (device);
            {
                const std::scoped_lock hold (lock);
                latest = std::move (reading);
                latestDevice = device;
                fresh = true;
            }
            onReading();
            wait (kEndpointVolumePollMs);
        }
    }

    const Reader reader;
    const std::function<void()> onReading;
    std::mutex lock;
    std::string deviceName, latestDevice;
    flub::platform::EndpointVolume latest;
    bool fresh = false;
};

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
        applyListeningLevel();     // ... and at a 0 dB listening level
        applyOnboardCap();         // ... and without the headset enhancement cap (docs/11 E16)
        applyHearingGuard();       // ... the hearing guard's settings, the personal profile and
        applyPersonalProfile();    // Smart macros (docs/11 E32 (c), E33, E34) where the new
        applySmartMacros();        // engine did not carry them over
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
        announcePresetWarnings = true;
        triggerAsyncUpdate();
    };
    routing->onChanged = [this] { notify (Change::Routing); };
    // docs/11 E40: a save of a strip whose preview plays takes the bank as the preview's end leaves it.
    presets->setAuditionFilter ([this] (const ParameterStore& store, Bank bank, std::vector<float>& values)
                                {
                                    for (int s = 0; s < getNumStrips(); ++s)
                                        if (&getParams (s) == &store)
                                            return getSavedBankValues (s, bank, values);
                                    return false;
                                });

    for (int i = 0; i < getNumStrips(); ++i)
    {
        const auto name = getStripName (i);
        userGainDb[static_cast<size_t> (i)] = settings->getStripGainDb (name);
        applyStripGain (i);
        host->setStripMuted (i, settings->getStripMuted (name));
    }
    // docs/11 E22: the chat duck (every engine built later copies it).
    host->getMixEngine().setChatDuck (settings->getChatDuck(), settings->getChatDuckDepthDb());

    selectedStrip = std::clamp (settings->getSelectedStrip(), 0, std::max (0, getNumStrips() - 1));
    enabled = settings->getMasterEnabled();

    restoreStripStates();
    for (int i = 0; i < getNumStrips(); ++i)
    {
        applyMasterEnableToStrip (i);
        persistedVersions[static_cast<size_t> (i)] = getParams (i).version();
    }
    applyProtectionStrength();
    applyListeningLevel();

    loadDeviceProfiles();
    preferredOutput = settings->getPreferredOutput();
    // docs/11 E51: "Follow the system default output", before the device opens;
    // picking an output in Settings > Audio ends it (the host says so).
    host->setFollowSystemDefault (settings->getFollowSystemDefaultOutput());
    host->onFollowSystemDefaultChanged = [this]
    {
        settings->setFollowSystemDefaultOutput (host->getFollowSystemDefault());
        notify (Change::Settings);
    };

    // Before the device opens: PipeWire reads the request when the stream opens.
    requestGraphQuantum (getLatencyProfile(), false);

    applyAllowedLoopbackPairs(); // before the device starts: its first check sees them
    if (options.openAudioDevice)
    {
        const auto savedState = settings->getDeviceState();
        lastDeviceError = host->openDevice (savedState.get());
        getDeviceManager().addChangeListener (this);
        applyNodeLatency (getLatencyProfile()); // docs/11 E48: the native node opens on Balanced
        applyDeviceInputPolicy();
        trackPreferredOutput (false);
        updateDeviceProfile();
    }

    // Tournament mode (docs/11 E55) before routing starts: a frozen router
    // makes no first pass.
    tournament.userChoice = settings->getTournamentMode();
    tournament.autoEnabled = settings->getTournamentAuto();
    applyTournament();
    pollAntiCheatServices();

    if (options.enableAppRouting)
        routing->start();

    // Automatic profiles: the rules are live from here on; the 2 Hz timer polls.
    foregroundApp = options.foregroundAppFactory ? options.foregroundAppFactory() : platform_bridge::createForegroundApp();
    autoProfiles.setEnabled (settings->getAutoProfilesEnabled());
    autoProfiles.setRules (settings->getAutoProfileRules());

    // docs/11 E33: the personal profile (its own file), on every strip's
    // chain; E32 (c): the hearing guard's settings and the day's dose so far;
    // E34: Smart macros.
    if (const auto file = getPersonalProfileFile(); file.existsAsFile())
    {
        std::string error;
        flub::PersonalProfile loaded;
        if (flub::personal::load (file.getFullPathName().toStdString(), loaded, error))
            personalProfile = loaded.sanitised();
        else
            personalError = juce::String::fromUTF8 (error.c_str());
    }
    applyPersonalProfile();
    applyHearingGuard();
    pollHearingDose();
    applySmartMacros();

    updateEndpointVolumePoller(); // docs/11 E32: while the contour follows the volume (or the hearing guard estimates)
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
    volumePoller.reset(); // before its async updates are cancelled
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
    pollHearingDose();     // docs/11 E32 (c): the day's dose as it stands
    savePersonalProfile(); // docs/11 E33
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
    host->setStripGainDb (strip, userGainDb[static_cast<size_t> (strip)] + getTotalComparisonTrimDb (strip));
}

void EngineController::setComparisonTrimDb (int strip, float db, ComparisonSlot slot)
{
    if (strip < 0 || strip >= getNumStrips() || ! std::isfinite (db))
        return;
    auto& trim = comparisonTrimDb[static_cast<size_t> (strip)][static_cast<size_t> (slot)];
    const float clamped = std::clamp (db, -kMaxComparisonTrimDb, 0.0f);
    if (clamped == trim)
        return;
    trim = clamped;
    applyStripGain (strip);
}

float EngineController::getComparisonTrimDb (int strip, ComparisonSlot slot) const noexcept
{
    return strip >= 0 && strip < AudioEngineHost::kMaxStrips ? comparisonTrimDb[static_cast<size_t> (strip)][static_cast<size_t> (slot)] : 0.0f;
}

float EngineController::getTotalComparisonTrimDb (int strip) const noexcept
{
    if (strip < 0 || strip >= AudioEngineHost::kMaxStrips)
        return 0.0f;
    const auto& t = comparisonTrimDb[static_cast<size_t> (strip)];
    return std::max (-kMaxComparisonTrimDb, t[0] + t[1]);
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
    for (int i = 0; i < getNumStrips(); ++i) // the host's gains are read back below
        for (const auto slot : { ComparisonSlot::Banks, ComparisonSlot::Listen })
            setComparisonTrimDb (i, 0.0f, slot);

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
    bool smart = false;
    if (! presets->loadIntoStrip (s, preset, getParams (s), error, &smart))
        return false;
    setPresetSmartMacros (s, smart);
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
    bool smart = false;
    if (! presets->stepPreset (s, +1, getParams (s), error, &smart))
        return false;
    setPresetSmartMacros (s, smart);
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
    bool smart = false;
    if (! presets->stepPreset (s, -1, getParams (s), error, &smart))
        return false;
    setPresetSmartMacros (s, smart);
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
    const auto id = presets->saveUserPreset (name, category, description, store, error, true, getSmartMacros (s));
    if (id.isNotEmpty())
    {
        // docs/11 E40: the saved sound (the pre-preview one while a preview
        // plays) is the one that counts as unmodified.
        presets->setCurrentPresetIdSaved (s, id, store);
        if (isPreviewInProgress (s))
            preview.savedPresetId = id;
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
    const int s = resolveStrip (strip);
    auto& store = getParams (s);
    const auto active = store.getActiveBank();
    // docs/11 E40: during a preview the copy takes the bank as the preview's
    // end will leave it (the audition bank is never copied).
    std::vector<float> values;
    const bool audition = getSavedBankValues (s, active, values);
    PresetManager::copyActiveToOther (store);
    if (audition)
    {
        const auto other = active == Bank::A ? Bank::B : Bank::A;
        for (int i = 0; i < kNumParams; ++i)
            if (store.get (other, i) != values[static_cast<size_t> (i)])
                store.set (other, i, values[static_cast<size_t> (i)]);
    }
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
    // "First Run - Game", Competitive FPS with Boost capped at
    // kFirstRunGameBoost, since its 0.35 switches the maximizer on (0.25 and
    // up). None of them sets a latency profile: the strips keep
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
        store.set (Bank::A, BoostIntensity, std::min (store.get (Bank::A, BoostIntensity), kFirstRunGameBoost));
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
            settings->setStripState (getStripName (i), getPersistedStripState (i));
            persisted = version;
        }
    }
}

juce::String EngineController::getPersistedStripState (int strip)
{
    const auto& store = getParams (strip);
    // A preview plays (docs/11 E40): save the bank as the preview's end will
    // leave it (PresetAudition::cancel), never the previewed sound.
    std::vector<float> values;
    if (! getSavedBankValues (strip, preview.bank, values))
        return stripStateToJson (store);
    return stripStateToJson (store, &values, preview.bank);
}

bool EngineController::getSavedBankValues (int strip, Bank bank, std::vector<float>& values) const
{
    if (strip < 0 || strip >= getNumStrips())
        return false;
    const auto& store = host->getMixEngine().params (strip);
    values.resize (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        values[static_cast<size_t> (i)] = store.get (bank, i);
    if (preview.strip != strip || preview.bank != bank)
        return false;
    // Every value that still holds what the preview wrote is the value
    // before it; one someone else moved meanwhile (a hotkey) stays as it is.
    for (int i = 0; i < kNumParams; ++i)
    {
        const auto k = static_cast<size_t> (i);
        if (! flub::preset::isAppState (i) && values[k] == preview.written[k] && preview.written[k] != preview.original[k])
            values[k] = preview.original[k];
    }
    return true;
}

void EngineController::setPreviewInProgress (int strip, Bank bank, std::vector<float> original, std::vector<float> written)
{
    const auto n = static_cast<size_t> (kNumParams);
    if (strip < 0 || strip >= getNumStrips() || original.size() != n || written.size() != n)
    {
        clearPreviewInProgress();
        return;
    }
    auto savedPresetId = preview.strip == strip ? preview.savedPresetId : juce::String();
    preview = { strip, bank, std::move (original), std::move (written), std::move (savedPresetId) };
}

void EngineController::clearPreviewInProgress()
{
    preview = {};
}

void EngineController::persistDeviceState()
{
    // createStateXml() is null while the manager is on an implicit default
    // device: keep whatever was saved before in that case.
    if (auto xml = host->createDeviceStateXml())
        settings->setDeviceState (xml.get());
}

void EngineController::saveState()
{
    if (options.openAudioDevice)
        persistDeviceState();
    persistStripStates (true);
    pollHearingDose();     // docs/11 E32 (c): the day's dose as it stands
    savePersonalProfile(); // docs/11 E33
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
    applyNodeLatency (getLatencyProfile()); // docs/11 E48
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

        // docs/11 E48: the native PipeWire device's inputs are the strips'
        // sinks, named by strip ("Game:FL" ... "System:FR"): each strip reads
        // its own channels.
        auto* device = getDeviceManager().getCurrentAudioDevice();
        if (! mapped && device != nullptr && host->isNativeNodeDevice())
        {
            const auto names = device->getInputChannelNames();
            const auto active = device->getActiveInputChannels();
            int index = 0; // among the active channels, as the callback gets them
            for (int c = 0; c < names.size(); ++c)
            {
                if (! active[c])
                    continue;
                const int strip = findStrip (names[c].upToFirstOccurrenceOf (":", false, false));
                if (strip >= 0 && map[static_cast<size_t> (strip)] < 0)
                {
                    map[static_cast<size_t> (strip)] = index;
                    mapped = true;
                }
                ++index;
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

std::vector<int> EngineController::parseDeviceInputMap (const juce::String& map, const juce::StringArray& stripNames)
{
    std::vector<int> channels (static_cast<size_t> (stripNames.size()), -1);
    for (const auto& entry : juce::StringArray::fromTokens (map, ";,", {}))
    {
        const auto name = entry.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto channelText = entry.fromFirstOccurrenceOf ("=", false, false).trim();
        const int strip = stripNames.indexOf (name, true);
        if (strip >= 0 && channelText.isNotEmpty() && channelText.containsOnly ("0123456789") && channelText.length() <= 4)
            channels[static_cast<size_t> (strip)] = channelText.getIntValue();
    }
    return channels;
}

juce::String EngineController::formatDeviceInputMap (const std::vector<int>& firstChannels, const juce::StringArray& stripNames)
{
    juce::StringArray entries;
    for (size_t i = 0; i < firstChannels.size() && static_cast<int> (i) < stripNames.size(); ++i)
        if (firstChannels[i] >= 0)
            entries.add (stripNames[static_cast<int> (i)] + "=" + juce::String (firstChannels[i]));
    return entries.joinIntoString (";");
}

std::vector<int> EngineController::consecutiveInputMap (const std::vector<int>& stripChannels)
{
    std::vector<int> first;
    int next = 0;
    for (const int channels : stripChannels)
    {
        first.push_back (next);
        next += juce::jmax (1, channels);
    }
    return first;
}

std::vector<int> EngineController::getDeviceInputMapChannels() const
{
    juce::StringArray names;
    for (int i = 0; i < getNumStrips(); ++i)
        names.add (getStripName (i));
    return parseDeviceInputMap (settings->getDeviceInputMap(), names);
}

void EngineController::setDeviceInputMapChannels (const std::vector<int>& firstChannels)
{
    juce::StringArray names;
    for (int i = 0; i < getNumStrips(); ++i)
        names.add (getStripName (i));
    settings->setDeviceInputMap (formatDeviceInputMap (firstChannels, names));
    applyDeviceInputPolicy();
    notify (Change::Settings);
}

void EngineController::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The device manager changed (device / rate / block / channels, or a device
    // appeared / disappeared).
    trackPreferredOutput (false);
    applyNodeLatency (getLatencyProfile()); // docs/11 E48: e.g. the native node picked in Settings
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
        updateOutputIdentity(); // no output: no headset cap (docs/11 E16)
        applyDeviceCorrection();
        applyOnboardCap();
        applyHearingGuard(); // docs/11 E32 (c): no output, no sensitivity
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
    if (volumePoller != nullptr)
        volumePoller->setDevice (currentOutputName); // docs/11 E32: the volume of the new output

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

    // The output's identity (docs/11 E51), which keys the per-endpoint
    // settings below.
    updateOutputIdentity();

    // The endpoint's own correction curve (never from the family-level
    // profile match above: a family name says nothing about one unit).
    applyDeviceCorrection();

    // The endpoint's headset enhancement answer (docs/11 E16), applied or
    // removed with every output change.
    applyOnboardCap();
    applyHearingGuard(); // docs/11 E32 (c): this output's sensitivity
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
    const auto entry = correctionEndpoint.isNotEmpty() ? findDeviceCorrectionEntry() : std::nullopt;
    // An entry from before docs/11 E51 (the name only) gains the output's ids
    // the first time it is seen with them.
    if (entry && entry->endpointId.isEmpty() && entry->hardwareId.isEmpty() && (! outputIdentity.id.empty() || ! outputIdentity.hardwareId.empty()))
        storeDeviceCorrectionEntry (*entry);
    if (flub::CorrectionCurve curve; entry && flub::eqtext::parse (entry->curveText.toStdString(), curve).ok)
    {
        next.curve = curve;
        next.enabled = entry->enabled;
        next.compare = correctionCompare && entry->enabled;
    }

    if (! (next == host->getDeviceCorrection()))
        host->setDeviceCorrection (next);
}

std::optional<DeviceCorrectionEntry> EngineController::findDeviceCorrectionEntry() const
{
    // By the output's identity (docs/11 E51): the curve follows its headset
    // through a re-plug into another USB port ("2- ") and a rename; entries
    // from before E51 are keyed by the name alone and still match it.
    return currentOutputName.isNotEmpty() ? settings->findDeviceCorrection (outputIdentity) : std::nullopt;
}

void EngineController::storeDeviceCorrectionEntry (DeviceCorrectionEntry entry)
{
    // Stored under the output's current identity: an entry found by name
    // (from before E51) or by hardware id gains the ids it lacked.
    entry.endpoint = currentOutputName;
    if (! outputIdentity.id.empty())
        entry.endpointId = juce::String (outputIdentity.id);
    if (! outputIdentity.hardwareId.empty())
        entry.hardwareId = juce::String (outputIdentity.hardwareId);
    settings->setDeviceCorrection (entry);
}

EngineController::DeviceCorrectionInfo EngineController::getDeviceCorrection() const
{
    DeviceCorrectionInfo info;
    info.endpoint = currentOutputName;
    if (const auto entry = findDeviceCorrectionEntry())
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
    entry.name = name;
    entry.enabled = true;
    entry.curveText = juce::String::fromUTF8 (flub::eqtext::format (curve).c_str());
    storeDeviceCorrectionEntry (entry);
    correctionCompare = false;
    applyDeviceCorrection();
    notify (Change::Settings);
    return true;
}

void EngineController::setDeviceCorrectionEnabled (bool shouldBeEnabled)
{
    auto entry = findDeviceCorrectionEntry();
    if (! entry || entry->enabled == shouldBeEnabled)
        return;
    entry->enabled = shouldBeEnabled;
    storeDeviceCorrectionEntry (*entry);
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
    if (! findDeviceCorrectionEntry())
        return;
    settings->removeDeviceCorrection (outputIdentity);
    correctionCompare = false;
    applyDeviceCorrection();
    notify (Change::Settings);
}

// =============================================================================
// Headset enhancement cap (docs/11 E16)
// =============================================================================
void EngineController::updateOutputIdentity()
{
    // The output's identity (docs/11 E51) keys its settings: the endpoint
    // JUCE's device name names, JUCE's duplicate numbering undone, as the
    // host's output selection matches them. A blocking list (COM calls on
    // Windows), once per output change, on the message thread.
    outputIdentity = {};
    onboardCapOn = false;
    if (currentOutputName.isEmpty())
        return;
    outputIdentity.name = currentOutputName.toStdString();

    std::vector<flub::platform::OutputEndpointIdentity> endpoints;
    if (options.outputEndpoints)
        endpoints = options.outputEndpoints();
    else if (options.openAudioDevice)
    {
        if (endpointLister == nullptr)
            endpointLister = flub::platform::AudioDeviceWatcher::create();
        if (endpointLister->isSupported())
            endpoints = endpointLister->listOutputs();
    }
    std::vector<flub::platform::AppAudioRouter::OutputEndpoint> plain;
    plain.reserve (endpoints.size());
    for (const auto& e : endpoints)
        plain.push_back ({ e.id, e.name });
    if (const auto id = flub::platform::AppAudioRouter::matchOutputDeviceName (plain, outputIdentity.name); ! id.empty())
        for (const auto& e : endpoints)
            if (e.id == id)
            {
                outputIdentity.id = e.id;
                outputIdentity.hardwareId = e.hardwareId;
                outputIdentity.transport = e.transport;
                outputIdentity.formFactor = e.formFactor;
            }

    if (const auto entry = settings->findDeviceEndpoint (outputIdentity))
        onboardCapOn = entry->onboardEnhancement;
}

void EngineController::applyOnboardCap()
{
    // One relaxed atomic per chain (ProcessingChain::setOnboardEnhancementCap):
    // the newest engine's chains, which glide it in or out over 250 ms; a
    // swapped-in engine inherits it (adoptGovernorState), a rebuilt one gets
    // it here (onEngineConfigured and the timer).
    for (int s = 0; s < getNumStrips(); ++s)
        if (auto& chain = getChain (s); chain.getOnboardEnhancementCap() != onboardCapOn)
            chain.setOnboardEnhancementCap (onboardCapOn);
}

EngineController::OnboardEnhancementInfo EngineController::getOnboardEnhancement() const
{
    OnboardEnhancementInfo info;
    if (currentOutputName.isEmpty())
        return info;
    info.endpoint = currentOutputName;
    info.offered = deviceMatch.profile != nullptr && deviceMatch.profile->onboardDsp;
    info.answered = settings->findDeviceEndpoint (outputIdentity).has_value();
    info.on = onboardCapOn;
    return info;
}

bool EngineController::setOnboardEnhancement (bool on)
{
    if (currentOutputName.isEmpty())
        return false;
    DeviceEndpointEntry entry;
    if (const auto stored = settings->findDeviceEndpoint (outputIdentity))
        entry = *stored; // keeps what else is stored for the endpoint, and ids this read lacks
    if (! outputIdentity.id.empty())
        entry.endpointId = juce::String (outputIdentity.id);
    if (! outputIdentity.hardwareId.empty())
        entry.hardwareId = juce::String (outputIdentity.hardwareId);
    entry.name = currentOutputName;
    entry.onboardEnhancement = on;
    settings->setDeviceEndpoint (entry);
    onboardCapOn = on;
    applyOnboardCap();
    notify (Change::Device); // the device banner and the macro chips
    notify (Change::Settings);
    return true;
}

void EngineController::trackPreferredOutput (bool rescan)
{
    if (! options.openAudioDevice)
        return;

    // docs/11 E51: the host selects the output by the chosen endpoint's
    // identity (the same endpoint id, else the same vendor / product and name
    // without Windows' "2- ", else that name) and switches back to it by
    // itself; the name alone never matched a headset re-plugged into another
    // USB port. The tracker stores that choice with its identity (settings
    // from before E51 have the name only; the host fills the ids in once it
    // sees the device).
    const auto& choice = host->getChosenOutput();
    if (choice.deviceName.isNotEmpty())
    {
        DeviceEndpointEntry next;
        next.name = choice.deviceName;
        next.endpointId = choice.endpointId;
        next.hardwareId = choice.hardwareId;
        if (! (next == preferredOutput))
        {
            preferredOutput = next;
            settings->setPreferredOutput (next);
        }
    }

    // While the chosen output is missing, device types that report no
    // hot-plug (ALSA, and every type on Linux and macOS, which have no
    // AudioDeviceWatcher) are rescanned and the host looks again. JUCE's
    // WASAPI types watch the endpoints themselves: a rescan here would make
    // them miss the change (as in AudioEngineHost::handleDeviceEvents).
    if (! rescan || ! host->getOutputSelection().fallback)
        return;
    if (auto* type = getDeviceManager().getCurrentDeviceTypeObject(); type != nullptr && ! type->getTypeName().startsWith ("Windows Audio"))
        type->scanForDevices();
    host->reselectOutput();
}

void EngineController::setFollowSystemDefaultOutput (bool follow)
{
    if (settings->getFollowSystemDefaultOutput() == follow && host->getFollowSystemDefault() == follow)
        return;
    settings->setFollowSystemDefaultOutput (follow);
    host->setFollowSystemDefault (follow);
    if (options.openAudioDevice)
    {
        trackPreferredOutput (false);
        updateDeviceProfile();
    }
    notify (Change::Device);
    notify (Change::Settings);
}

// =============================================================================
void EngineController::updateOverloadWatchdog (const EngineStatus& status)
{
    OverloadWatchdog::Sample sample;
    sample.running = status.deviceOpen && status.running;
    sample.load = status.cpuLoad;
    sample.glitchCount = status.deviceOpen ? static_cast<int64_t> (status.glitches) : -1;
    // docs/11 E45: the peak callback of this poll's window, and the callback
    // timing's counts. Late callbacks (gaps in the host's timestamps) only
    // for a device that counts no xruns itself: one that does counts the
    // same stalls. A timing whose count went backwards belongs to a new host.
    const auto& timing = status.callbackTiming;
    if (timing.callbacks < lastCallbackTiming.callbacks)
        lastCallbackTiming = {};
    const auto window = timing.since (lastCallbackTiming);
    lastCallbackTiming = timing;
    if (window.callbacks > 0)
        sample.peakLoad = window.loadAt (0.999);
    if (timing.callbacks > 0)
    {
        sample.overBudget = static_cast<int64_t> (timing.overBudget);
        if (status.xruns < 0)
            sample.late = static_cast<int64_t> (timing.late);
    }

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
    const bool changed = applyGraphQuantum (profile, user.latency ? user.latency->c_str() : nullptr, user.props ? user.props->c_str() : nullptr);
    // docs/11 E48: the native PipeWire node changes its own node.latency in
    // place (the graph moves to the new quantum between two cycles): no
    // re-open and no dropout.
    if (reopen && applyNodeLatency (profile))
        return;
    if (! changed)
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

bool EngineController::applyNodeLatency (LatencyProfileValue profile)
{
    if (! options.openAudioDevice)
        return false;
    using NodeLatency = flub::platform::NativeAudioNodeConfig::Latency;
    return flub::platform::pipewire::setDeviceLatency (getDeviceManager().getCurrentAudioDevice(),
                                                       profile == LatencyProfileValue::LowLatency ? NodeLatency::LowLatency : NodeLatency::Balanced);
}

void EngineController::feedGraphQuantum()
{
    // docs/11 E42: the quantum the graph runs at, as the native PipeWire node
    // saw it at its last cycle (the profile's node.latency, or less when
    // another client asks for less), is part of the header's total. Other
    // devices report their own latency; nothing is fed for them.
    const auto node = host->getNativeNodeStatus();
    if (node.running && node.quantumFrames != 0 && node.sampleRate != 0)
    {
        host->setGraphQuantumMs (1000.0 * static_cast<double> (node.quantumFrames) / static_cast<double> (node.sampleRate));
        feedingGraphQuantum = true;
    }
    else if (std::exchange (feedingGraphQuantum, false))
    {
        host->setGraphQuantumMs (0.0);
    }
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
    feedGraphQuantum();        // docs/11 E42 / E48
    applyProtectionStrength(); // an engine built since (onEngineConfigured is asynchronous)
    applyListeningLevel();
    applyOnboardCap();
    applyHearingGuard();    // docs/11 E32 (c)
    applyPersonalProfile(); // docs/11 E33
    applySmartMacros();     // docs/11 E34
    if (! tournament.active) // docs/11 E55: no foreground poll in Tournament mode
        pollForegroundApp();

    if (++timerTicks % kPersistEveryTicks == 0)
    {
        persistStripStates (false);
        pollHearingDose();     // docs/11 E32 (c): today's dose, stored every 5 s
        savePersonalProfile(); // docs/11 E33: an edit is written within 5 s
    }
    if (timerTicks % kAntiCheatPollTicks == 0)
        pollAntiCheatServices();

    // While the chosen output (e.g. a headset) is missing, look for it (docs/11 E51).
    if (options.openAudioDevice && host->getOutputSelection().fallback && timerTicks % kRescanEveryTicks == 0)
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
    if (volumePoller != nullptr)
        if (auto reading = volumePoller->take())
            applyEndpointVolume (reading->first, reading->second);
    if (std::exchange (announcePresetWarnings, false) && ! pendingPresetWarnings.empty())
        notify (Change::Preset);
}

// =============================================================================
// Listening level: the loudness contour follows the system volume (docs/11 E32)
// =============================================================================
flub::platform::EndpointVolume EngineController::readEndpointVolume (const juce::String& device) const
{
    if (options.endpointVolumeReader)
        return options.endpointVolumeReader (device.toStdString());
    return flub::platform::AudioEndpoints::queryOutputVolume (device.toStdString());
}

void EngineController::updateEndpointVolumePoller()
{
    // docs/11 E32 (c): the hearing guard's estimate needs the volume as well.
    const bool wanted = (options.openAudioDevice || options.pollEndpointVolumeHeadless) && ! isShutDown
                        && (settings->getContourFollowsVolume() || hearingKnown);
    if (! wanted)
    {
        volumePoller.reset();
        return;
    }
    if (volumePoller == nullptr)
    {
        auto reader = options.endpointVolumeReader ? options.endpointVolumeReader
                                                    : [] (const std::string& device) { return flub::platform::AudioEndpoints::queryOutputVolume (device); };
        volumePoller = std::make_unique<EndpointVolumePoller> (std::move (reader), [this] { triggerAsyncUpdate(); }, currentOutputName);
    }
}

void EngineController::pollEndpointVolume()
{
    applyEndpointVolume (readEndpointVolume (currentOutputName), currentOutputName);
}

void EngineController::applyEndpointVolume (const flub::platform::EndpointVolume& volume, const juce::String& device)
{
    if (device != listening.device)
    {
        // Another output: the last one's volume says nothing about it.
        listening.device = device;
        listeningHasRead = false;
    }
    listening.known = volume.known;
    listening.muted = volume.known && volume.muted;
    listening.error = volume.known ? juce::String() : juce::String (volume.error);
    if (volume.known)
    {
        listening.volumeDb = volume.volumeDb;
        listeningHasRead = true;
        // Following without a reference yet: this volume becomes it, so
        // nothing changes until the volume moves.
        if (settings->getContourFollowsVolume() && ! settings->getContourReferenceVolumeDb().has_value())
            settings->setContourReferenceVolumeDb (volume.volumeDb);
        // docs/11 E32 (c): the hearing guard's estimate (a failed read keeps the last).
        host->getMixEngine().getHearingGuard().setEndpointVolumeDb (volume.muted ? -std::numeric_limits<float>::infinity() : volume.volumeDb);
    }
    applyListeningLevel();
}

void EngineController::applyListeningLevel()
{
    listening.following = settings->getContourFollowsVolume();
    listening.referenceDb = settings->getContourReferenceVolumeDb();
    // A failed read holds the last volume of the same output (a muted one
    // reads its volume as usual); nothing read yet: the parameter alone.
    listening.levelDb = listening.following && listening.referenceDb.has_value() && listeningHasRead
                            ? listening.volumeDb - *listening.referenceDb
                            : 0.0f;
    for (int s = 0; s < getNumStrips(); ++s)
        if (auto& chain = getChain (s); chain.getListeningLevelDb() != listening.levelDb)
            chain.setListeningLevelDb (listening.levelDb);
}

EngineController::ListeningLevel EngineController::getListeningLevel() const
{
    auto l = listening;
    l.following = settings->getContourFollowsVolume();
    l.referenceDb = settings->getContourReferenceVolumeDb();
    return l;
}

void EngineController::setContourFollowsVolume (bool follow)
{
    settings->setContourFollowsVolume (follow);
    if (follow)
        pollEndpointVolume(); // now: sets the reference if there is none, and the level
    else
        listeningHasRead = false;
    updateEndpointVolumePoller();
    applyListeningLevel();
    notify (Change::Settings);
}

void EngineController::setContourReferenceVolumeDb (float volumeDb)
{
    if (! std::isfinite (volumeDb))
        return;
    settings->setContourReferenceVolumeDb (volumeDb);
    applyListeningLevel();
    notify (Change::Settings);
}

bool EngineController::useCurrentVolumeAsReference()
{
    const auto volume = readEndpointVolume (currentOutputName);
    applyEndpointVolume (volume, currentOutputName);
    if (! volume.known)
        return false;
    setContourReferenceVolumeDb (volume.volumeDb);
    return true;
}

// =============================================================================
// Feedback-loop guard override (docs/11 E51)
// =============================================================================
void EngineController::applyAllowedLoopbackPairs()
{
    // clear / allow each re-check the current pair; the last call leaves the
    // guard as the whole list decides.
    host->clearAllowedLoopbackPairs();
    for (const auto& pair : settings->getAllowedLoopbackPairs())
        host->allowLoopbackPair (pair.input, pair.output);
}

void EngineController::setLoopbackPairAllowed (const juce::String& inputDeviceName, const juce::String& outputDeviceName, bool allowed)
{
    const auto input = inputDeviceName.trim(), output = outputDeviceName.trim();
    if (input.isEmpty() || output.isEmpty())
        return;
    auto pairs = settings->getAllowedLoopbackPairs();
    pairs.erase (std::remove_if (pairs.begin(), pairs.end(),
                                 [&] (const auto& p) { return p.input.equalsIgnoreCase (input) && p.output.equalsIgnoreCase (output); }),
                 pairs.end());
    if (allowed)
        pairs.push_back ({ input, output });
    settings->setAllowedLoopbackPairs (pairs);
    applyAllowedLoopbackPairs(); // the host broadcasts a banner change itself (Change::Device)
    notify (Change::Settings);
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
    if (! isAutoProfileSupported() || ! autoProfiles.isEnabled() || tournament.active)
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

// =============================================================================
// Tournament mode (docs/11 E55)
// =============================================================================
void EngineController::setTournamentMode (bool on)
{
    settings->setTournamentMode (on);
    tournament.userChoice = on;
    // Off while services hold it on: off until they stop (and start again).
    tournamentDismissed = ! on && tournament.autoEnabled && ! tournament.services.empty();
    applyTournament();
}

void EngineController::setTournamentAuto (bool automatic)
{
    settings->setTournamentAuto (automatic);
    tournament.autoEnabled = automatic;
    tournamentDismissed = false;
    applyTournament();
}

juce::String EngineController::antiCheatDisplayName (const std::string& serviceName)
{
    const auto name = juce::String::fromUTF8 (serviceName.c_str());
    if (name.equalsIgnoreCase ("vgc"))
        return "Vanguard";
    if (name.equalsIgnoreCase ("BEService"))
        return "BattlEye";
    if (name.startsWithIgnoreCase ("EasyAntiCheat"))
        return "Easy Anti-Cheat";
    if (name.equalsIgnoreCase ("FACEITService"))
        return "FACEIT";
    if (name.startsWithIgnoreCase ("ESEA"))
        return "ESEA";
    return name;
}

juce::String EngineController::describeTournament() const
{
    if (! tournament.active)
        return {};
    if (! tournament.automatic)
        return "Tournament mode on";
    juce::StringArray names;
    for (const auto& service : tournament.services)
        names.addIfNotAlreadyThere (antiCheatDisplayName (service));
    return "Tournament mode on: " + names.joinIntoString (", ") + (names.size() == 1 ? " is" : " are") + " running";
}

void EngineController::pollAntiCheatServices()
{
    auto running = options.antiCheatServices ? options.antiCheatServices() : flub::platform::AntiCheatServices::running();
    if (running.empty())
    {
        // A service restarting (an update between matches) does not flap the mode.
        if (tournament.services.empty() || ++tournamentQuietPolls < kTournamentOffPolls)
            return;
        tournament.services.clear();
        tournamentDismissed = false; // the next service start switches it on again
    }
    else
    {
        tournamentQuietPolls = 0;
        if (running == tournament.services)
            return;
        tournament.services = std::move (running);
    }
    applyTournament();
}

void EngineController::applyTournament()
{
    const bool held = tournament.autoEnabled && ! tournament.services.empty() && ! tournamentDismissed;
    const bool active = tournament.userChoice || held;
    tournament.automatic = active && ! tournament.userChoice;
    if (active != tournament.active)
    {
        tournament.active = active;
        routing->setTournamentMode (active);    // no enumeration, move or new capture
        autoProfiles.setTournamentMode (active); // the active rule stays, nothing switches
    }
    notify (Change::Settings);
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
        point.smartMacros = getSmartMacros (s);
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
    bool smart = false;
    if (! presets->loadIntoStrip (s, preset, store, error, &smart))
    {
        autoRestorePoint.reset();
        autoProfileError = "Automatic profile for " + rule.executable + ": " + error;
        return;
    }
    setPresetSmartMacros (s, smart);
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
    setPresetSmartMacros (s, point->smartMacros);
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
        std::vector<Override> overrides;
        for (const auto& [id, value] : getNightOverrides())
            overrides.push_back ({ id, value });
        engageLatch (s, latch, std::move (overrides));
    }
    else
    {
        releaseLatch (s, latch, true);
    }
    notify (Change::Parameters);
}

std::vector<std::pair<int, float>> EngineController::getNightOverrides() const
{
    // Read from the factory preset, so the latch follows its tuning (docs/11
    // E21 retuned it: Auto Level -20 -> -14 LUFS, the compressor's make-up
    // gone and its thresholds up 6 dB, the Startle Guard at 20 LU).
    std::vector<std::pair<int, float>> overrides (std::begin (kNightFallback), std::end (kNightFallback));
    flub::preset::Preset preset;
    juce::String error;
    if (const auto* info = presets->findById (kNightPreset);
        info != nullptr && presets->readPreset (*info, preset, error) && preset.values.size() == static_cast<size_t> (kNumParams))
        for (auto& [id, value] : overrides)
            value = preset.values[static_cast<size_t> (id)];
    return overrides;
}

bool EngineController::isNight (int strip) const noexcept
{
    return strip >= 0 && strip < AudioEngineHost::kMaxStrips && nightLatches[static_cast<size_t> (strip)].on;
}

bool EngineController::hasChatMix() const
{
    return findStrip (kChatMixGameStrip) >= 0 && findStrip (kChatMixChatStrip) >= 0;
}

bool EngineController::setChatMix (float balance)
{
    // Whole 10 % steps, so repeated nudges land on round values and 0 exactly;
    // centred (and nothing to balance) without a Game and a Chat strip.
    const bool possible = hasChatMix();
    const float next = possible && std::isfinite (balance) ? std::round (std::clamp (balance, -1.0f, 1.0f) * 10.0f) / 10.0f : 0.0f;
    if (next != chatMix)
    {
        chatMix = next;
        host->getMixEngine().setChatMix (chatMix); // docs/11 E22: the complementary gains, gliding over 50 ms
        notify (Change::Parameters);
    }
    return possible;
}

float EngineController::getChatMixGainDb (int strip) const
{
    const auto name = getStripName (strip);
    const auto role = name.equalsIgnoreCase (kChatMixGameStrip)   ? flub::MixEngine::StripRole::Game
                      : name.equalsIgnoreCase (kChatMixChatStrip) ? flub::MixEngine::StripRole::Chat
                                                                  : flub::MixEngine::StripRole::Other;
    const float gain = flub::MixEngine::chatMixGain (chatMix, role);
    return gain > 0.0f ? juce::Decibels::gainToDecibels (gain, -1000.0f) : -std::numeric_limits<float>::infinity();
}

juce::String EngineController::describeChatMix() const
{
    if (chatMix == 0.0f)
        return "centred";
    const auto db = [] (float v)
    {
        if (! std::isfinite (v))
            return juce::String ("muted");
        return (v < -0.05f ? juce::String (v, 1) : juce::String ("0")) + " dB";
    };
    return juce::String (kChatMixGameStrip) + " " + db (getChatMixGainDb (findStrip (kChatMixGameStrip))) + ", " + kChatMixChatStrip + " "
           + db (getChatMixGainDb (findStrip (kChatMixChatStrip)));
}

void EngineController::setChatDuck (bool on, float depthDb)
{
    const float depth = std::isfinite (depthDb) ? std::clamp (depthDb, kMinChatDuckDepthDb, kMaxChatDuckDepthDb) : kDefaultChatDuckDepthDb;
    if (on == getChatDuck() && depth == getChatDuckDepthDb())
        return;
    settings->setChatDuck (on);
    settings->setChatDuckDepthDb (depth);
    host->getMixEngine().setChatDuck (on, depth);
    notify (Change::Settings);
}

bool EngineController::getChatDuck() const { return settings->getChatDuck(); }

float EngineController::getChatDuckDepthDb() const
{
    const float depth = settings->getChatDuckDepthDb();
    return std::isfinite (depth) ? std::clamp (depth, kMinChatDuckDepthDb, kMaxChatDuckDepthDb) : kDefaultChatDuckDepthDb;
}

bool EngineController::isChatVoiceActive() const noexcept { return host->getMixEngine().isChatVoiceActive(); }

float EngineController::getChatDuckAmount() const noexcept { return host->getMixEngine().getChatDuckAmount(); }

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

// =============================================================================
// Hearing guard: listening-level estimate, dose and cap (docs/11 E32 (c))
// =============================================================================
namespace
{
/** "2026-09-24" for ("2026-09-30", 6): an ISO day `days` before `day`. */
juce::String dayBefore (const juce::String& day, int days)
{
    const juce::Time noon (day.substring (0, 4).getIntValue(), day.substring (5, 7).getIntValue() - 1, day.substring (8, 10).getIntValue(), 12, 0);
    return (noon - juce::RelativeTime::days (days)).formatted ("%Y-%m-%d");
}
} // namespace

void EngineController::applyHearingGuard()
{
    // Relaxed atomics on the newest engine's guard (HearingGuard's setters);
    // a swapped-in engine inherits them (MixEngine::configureFrom), a
    // rebuilt one gets them here (onEngineConfigured and the timer).
    auto& guard = host->getMixEngine().getHearingGuard();
    const auto user = currentOutputName.isNotEmpty() ? settings->findHearingSensitivity (outputIdentity) : std::nullopt;
    hearingUserDbSpl = user; // getHearing() reads it without parsing the settings
    const float profile = deviceMatch.profile != nullptr ? flub::device::splAtFullScale (deviceMatch.profile->sensitivity)
                                                         : std::numeric_limits<float>::quiet_NaN();
    float chosen = std::numeric_limits<float>::quiet_NaN();
    flub::HearingGuard::chooseSensitivity (user.value_or (std::numeric_limits<float>::quiet_NaN()), profile, chosen);
    const float current = guard.getSensitivityDbSpl();
    if (std::isnan (chosen) != std::isnan (current) || (! std::isnan (chosen) && chosen != current))
        guard.setSensitivityDbSpl (chosen);
    const bool capOn = settings->getHearingCapEnabled();
    const float capDb = settings->getHearingCapDbA();
    if (guard.getCapEnabled() != capOn || guard.getCapDbA() != capDb)
        guard.setCap (capOn, capDb);

    // The estimate needs the system volume: read it while a sensitivity is known.
    if (const bool known = ! std::isnan (chosen); known != hearingKnown)
    {
        hearingKnown = known;
        updateEndpointVolumePoller();
        if (known && ! settings->getContourFollowsVolume())
            pollEndpointVolume(); // the volume now, not only at the next poll
    }
}

EngineController::HearingInfo EngineController::getHearing() const
{
    HearingInfo info;
    info.output = currentOutputName;
    if (currentOutputName.isNotEmpty())
        info.userDbSpl = hearingUserDbSpl;
    if (const auto* profile = deviceMatch.profile)
    {
        info.profileName = juce::String::fromUTF8 (profile->displayName.c_str());
        info.profileDbSpl = flub::device::splAtFullScale (profile->sensitivity);
        info.profileFigureSource = juce::String::fromUTF8 (profile->sensitivity.source.c_str());
        info.profileLabVerified = profile->labVerified;
    }
    info.source = flub::HearingGuard::chooseSensitivity (info.userDbSpl.value_or (std::numeric_limits<float>::quiet_NaN()), info.profileDbSpl,
                                                         info.sensitivityDbSpl);
    info.known = info.source != flub::HearingGuard::SensitivitySource::Unknown;
    info.capEnabled = settings->getHearingCapEnabled();
    info.capDbA = settings->getHearingCapDbA();
    info.volumeKnown = listening.known && listening.device == currentOutputName;
    info.volumeDb = info.volumeKnown ? listening.volumeDb : 0.0f;

    const auto& meters = host->getMixEngine().getHearingGuard().meters();
    constexpr auto rx = std::memory_order_relaxed;
    if (info.known && meters.known.load (rx))
    {
        info.levelDbA = meters.levelDbA.load (rx);
        info.leq5sDbA = meters.leq5sDbA.load (rx);
        info.sessionLeqDbA = meters.sessionLeqDbA.load (rx);
        info.capActive = meters.capActive.load (rx);
        info.capGainDb = meters.capGainDb.load (rx);
    }
    // Today's dose as the bookkeeping counts it (pollHearingDose), and the
    // stored days before it.
    const double session = meters.sessionDose.load (rx);
    info.doseToday = doseDay.isNotEmpty() ? doseBaseline + std::max (0.0, session - doseMark) : 0.0;
    info.doseWeek = info.doseToday + doseEarlierDays;
    return info;
}

bool EngineController::setHearingSensitivity (std::optional<float> dbSpl)
{
    if (currentOutputName.isEmpty())
        return false;
    if (dbSpl.has_value() && ! std::isfinite (*dbSpl))
        return false;
    settings->setHearingSensitivity (outputIdentity, dbSpl);
    applyHearingGuard();
    notify (Change::Settings);
    return true;
}

void EngineController::setHearingCap (bool on, float dbA)
{
    settings->setHearingCapEnabled (on);
    settings->setHearingCapDbA (dbA);
    applyHearingGuard();
    notify (Change::Settings);
}

juce::String EngineController::getDoseDay() const
{
    return (options.clock ? options.clock() : juce::Time::getCurrentTime()).formatted ("%Y-%m-%d");
}

void EngineController::pollHearingDose()
{
    auto& guard = host->getMixEngine().getHearingGuard();
    const double session = guard.meters().sessionDose.load (std::memory_order_relaxed);
    const auto today = getDoseDay();
    const auto storedDose = [this] (const juce::String& day)
    {
        for (const auto& d : settings->getDailyDoses())
            if (d.day == day)
                return d.fraction;
        return 0.0;
    };

    double dose = doseBaseline + std::max (0.0, session - doseMark);
    if (doseDay.isEmpty() || session < lastSessionDose)
    {
        // The first poll (the day's stored dose so far) or a guard that
        // started afresh (an engine the host built without carrying it):
        // count on from what was counted.
        doseBaseline = doseDay.isEmpty() ? storedDose (today) : dose;
        doseDay = today;
        doseMark = session;
        guard.setDoseBaseline (doseBaseline);
        dose = doseBaseline;
    }
    else if (today != doseDay)
    {
        // Midnight: the day's dose is final; the new day starts from its own.
        settings->setDailyDose (doseDay, dose);
        doseDay = today;
        doseBaseline = storedDose (today);
        doseMark = session;
        guard.setDoseBaseline (doseBaseline);
        dose = doseBaseline;
    }
    lastSessionDose = session;
    // Stored once there is something to store (no entry for a day nothing was estimated on).
    if (dose > 0.0 && dose != storedDose (today))
        settings->setDailyDose (today, dose);

    // The 6 days before today, for the weekly sum (getHearing).
    doseEarlierDays = 0.0;
    const auto oldest = dayBefore (doseDay, AppSettings::kDoseDaysKept - 1);
    for (const auto& d : settings->getDailyDoses())
        if (d.day < doseDay && d.day >= oldest)
            doseEarlierDays += d.fraction;
}

// =============================================================================
// Personal hearing profile (docs/11 E33)
// =============================================================================
juce::File EngineController::getPersonalProfileFile() const
{
    return settings->getFile().getSiblingFile ("personal-profile.json");
}

void EngineController::applyPersonalProfile()
{
    // Every strip's chain designs it on this thread and crossfades to it
    // (ProcessingChain::setPersonalProfile); an unchanged one is only handed
    // over again if its ring was full.
    for (int s = 0; s < getNumStrips(); ++s)
    {
        auto& chain = getChain (s);
        if (chain.getPersonalProfile() != personalProfile)
            chain.setPersonalProfile (personalProfile);
        else
            chain.retryPersonalProfile();
    }
}

void EngineController::setPersonalProfile (const flub::PersonalProfile& profile)
{
    const auto sanitised = profile.sanitised();
    if (sanitised == personalProfile)
        return;
    personalProfile = sanitised;
    personalDirty = true;
    applyPersonalProfile();
    notify (Change::Settings);
}

bool EngineController::savePersonalProfile()
{
    if (! personalDirty || ! options.persistSettings)
        return true;
    std::string error;
    const auto file = getPersonalProfileFile();
    file.getParentDirectory().createDirectory();
    if (! flub::personal::save (file.getFullPathName().toStdString(), personalProfile, error))
    {
        DBG ("Flubsound: the personal profile was not saved: " << error);
        return false;
    }
    personalDirty = false;
    return true;
}

// =============================================================================
// Smart macros (docs/11 E34)
// =============================================================================
void EngineController::applySmartMacros()
{
    // One relaxed atomic per chain; a swapped-in engine inherits it
    // (adoptGovernorState), a rebuilt one gets it here.
    for (int s = 0; s < getNumStrips(); ++s)
        if (const bool on = settings->getSmartMacros (getStripName (s)); getChain (s).getSmartMacros() != on)
            getChain (s).setSmartMacros (on);
}

bool EngineController::getSmartMacros (int strip) const
{
    return settings->getSmartMacros (getStripName (resolveStrip (strip)));
}

void EngineController::setSmartMacros (bool on, int strip)
{
    settings->setSmartMacros (getStripName (resolveStrip (strip)), on);
    applySmartMacros();
    notify (Change::Settings);
}

void EngineController::setPresetSmartMacros (int strip, bool on)
{
    // Part of the preset (docs/11 E34): the switch follows it, and the
    // Settings page hears about it only when it moved.
    if (getSmartMacros (strip) != on)
        setSmartMacros (on, strip);
}
} // namespace flub::app
