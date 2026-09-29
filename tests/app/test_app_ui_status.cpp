// App-level tests: the status the UI surfaces (Phase 2 batch 2, app track).
//
// * docs/11 E51 Phase A: the device banner appears for a device error and a
//   loopback pair, with Retry / Choose output / Sound settings; Retry clears
//   what can be cleared and keeps what cannot.
// * docs/11 E52: reader warnings of a preset loaded or imported become a
//   toast (EngineController::takePresetWarnings -> NoticeBar); a damaged
//   settings file becomes a notice; a rename keeps the preset's uuid, so the
//   strip and the automatic profile rule that play it keep it.
// * docs/11 E42a: loading a preset made for another latency profile leaves
//   the profile and raises the prompt; its action switches every strip.
// * docs/11 E06: protection strength is persisted and reaches every chain,
//   including the chains of an engine built later; the Boost panel's chip
//   names the governor's state and reason (end to end on a hot programme).
// * docs/11 E38 slice / E11: active-now chips from effective values
//   (subsonic at Boost 0, >= 3 chips at Boost 55), the in -> out loudness
//   difference and the limiter-active share ("Limiter active x %" on hover).
// * docs/11 E06 (Phase 3 batch 3): at Normal / Strict the governor chip's
//   tooltip quotes the budgets the chain publishes for its mode and
//   strength, end to end on the Music and Game strips.
// * The whole window: MainComponent shows the banner, the toast and the
//   prompt from controller events and lays them out under the header.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "engine/TestSignalGenerator.h"
#include "shell/ScreenshotDriver.h"
#include "ui/BoostPanel.h"
#include "ui/LoudnessPanel.h"
#include "ui/MainComponent.h"
#include "ui/NoticeBanners.h"

#include "flub/engine/Parameters.h"

#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace flub::app;
using namespace flub::param;
using ui::MeterSnapshot;

namespace
{
EngineController::Options headlessOptions (const flubapptest::TempFolder& temp, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    return o;
}

struct ChangeLog final : EngineController::Listener
{
    int device = 0, engine = 0, preset = 0, settings = 0;
    void engineControllerChanged (EngineController::Change change) override
    {
        using C = EngineController::Change;
        device += change == C::Device ? 1 : 0;
        engine += change == C::Engine ? 1 : 0;
        preset += change == C::Preset ? 1 : 0;
        settings += change == C::Settings ? 1 : 0;
    }
};

const PresetInfo& presetNamed (EngineController& c, const char* name)
{
    const auto* info = c.getPresetManager().findByName (name);
    REQUIRE (info != nullptr);
    return *info;
}

/** Renders `seconds` of the test programme through the engine in 512-sample blocks. */
void render (EngineController& c, TestSignalGenerator& source, double seconds)
{
    const int total = static_cast<int> (seconds * c.getHost().getSampleRate());
    for (int done = 0; done < total; done += 512)
        c.renderOffline (source, std::min (512, total - done));
}

MeterSnapshot snapshotOf (EngineController& c, int strip)
{
    MeterSnapshot s;
    auto& chain = c.getChain (strip);
    s.read (chain.meters());
    s.active = c.isStripActive (strip);
    s.autoPreampDb = chain.getAutoPreampDb();
    s.predictedBoostDb = chain.getPredictedBoostDb();
    return s;
}

std::vector<juce::String> chipTexts (EngineController& c, int strip)
{
    auto& chain = c.getChain (strip);
    std::vector<juce::String> texts;
    for (const auto& st : ui::BoostPanel::describeActiveStages ([&chain] (int id) { return chain.effectiveValue (id); }, snapshotOf (c, strip),
                                                               c.getStripChannels (strip)))
        texts.push_back (st.text);
    return texts;
}

bool anyStartsWith (const std::vector<juce::String>& texts, const char* prefix)
{
    return std::any_of (texts.begin(), texts.end(), [prefix] (const juce::String& t) { return t.startsWith (prefix); });
}
} // namespace

// =============================================================================
// E51: the device banner
// =============================================================================
TEST_CASE ("App UI: the device banner shows a device error and a loopback pair with Retry / Choose output / Sound settings (E51)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ChangeLog changes;
    controller.addListener (&changes);
    auto& host = controller.getHost();

    ui::DeviceErrorBanner banner (controller);
    int chooseClicks = 0, soundClicks = 0;
    banner.onChooseOutput = [&] { ++chooseClicks; };
    banner.onOpenSoundSettings = [&] { ++soundClicks; };
    CHECK (! banner.refresh());
    CHECK (! banner.shouldShow());
    CHECK (! banner.isVisible());

    // A device error (the driver stopped the stream): the host's message.
    host.audioDeviceError ("USB Headset was disconnected");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return changes.device >= 1; }, 5000));
    CHECK (banner.refresh()); // visibility changed
    CHECK (banner.shouldShow());
    CHECK (banner.getHeadline() == "Audio device error");
    CHECK (banner.getMessage() == "USB Headset was disconnected");
    banner.setBounds (0, 0, 1100, ui::DeviceErrorBanner::kHeight);
    CHECK (banner.getRetryButton().isVisible());
    CHECK (banner.getChooseOutputButton().getRight() <= banner.getSoundSettingsButton().getX());

    banner.getChooseOutputButton().triggerClick();
    banner.getSoundSettingsButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return chooseClicks == 1 && soundClicks == 1; }, 2000));

    // Retry: headless there is no device to re-open, so the error is dismissed.
    banner.getRetryButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return ! banner.shouldShow(); }, 2000));
    CHECK (controller.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);

    // A loopback pair whose input feeds the Game strip: the output is muted.
    host.setDeviceInputRouting (0, 0);
    host.checkLoopbackPair ("CABLE Output (VB-Audio Virtual Cable)", "CABLE Input (VB-Audio Virtual Cable)");
    REQUIRE (controller.getDeviceSafetyState().kind == DeviceSafetyState::Kind::LoopbackPair);
    CHECK (banner.refresh());
    CHECK (banner.getHeadline() == "Output muted: feedback loop");
    CHECK (banner.getMessage().contains ("feedback loop"));
    CHECK (! banner.getMessage().startsWith ("Output muted")); // the headline says it
    CHECK (banner.getTooltip() == banner.getMessage());

    // Retry with the same pair keeps it; once the input no longer feeds a
    // strip, Retry's re-check clears it.
    CHECK (controller.retryDevice().isEmpty());
    CHECK (controller.getDeviceSafetyState().kind == DeviceSafetyState::Kind::LoopbackPair);
    host.setDeviceInputRouting (-1);
    banner.getRetryButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return ! banner.shouldShow(); }, 2000));
    CHECK (controller.getDeviceSafetyState().kind == DeviceSafetyState::Kind::None);

    // The fallbacks when the host has no text.
    DeviceSafetyState silent;
    silent.kind = DeviceSafetyState::Kind::DeviceError;
    CHECK (ui::DeviceErrorBanner::messageFor (silent).startsWith ("The audio device reported an error"));
    CHECK (ui::DeviceErrorBanner::headlineFor ({}).isEmpty());

    controller.removeListener (&changes);
}

// =============================================================================
// Notices: the bar, preset warnings, recovery, the latency prompt
// =============================================================================
TEST_CASE ("App UI: the notice bar shows the newest notice, replaces by key, expires timed ones and runs the action")
{
    ui::NoticeBar bar;
    bar.setBounds (0, 0, 900, 34);
    int visibilityChanges = 0, actions = 0;
    bar.onVisibilityChanged = [&] { ++visibilityChanges; };
    CHECK (! bar.shouldShow());

    ui::NoticeBar::Notice toast;
    toast.key = "toast";
    toast.kind = ui::NoticeBar::Notice::Kind::Warning;
    toast.text = "first";
    toast.seconds = 12.0;
    bar.post (toast);
    CHECK (bar.shouldShow());
    CHECK (bar.isVisible());
    CHECK (visibilityChanges == 1);
    CHECK (! bar.getActionButton().isVisible());

    ui::NoticeBar::Notice prompt;
    prompt.key = "prompt";
    prompt.kind = ui::NoticeBar::Notice::Kind::Prompt;
    prompt.text = "do it?";
    prompt.actionLabel = "Do it";
    prompt.action = [&] { ++actions; };
    bar.post (prompt);
    REQUIRE (bar.current() != nullptr);
    CHECK (bar.current()->key == "prompt");
    CHECK (bar.getNumNotices() == 2);
    CHECK (bar.getActionButton().isVisible());
    CHECK (bar.getActionButton().getButtonText() == "Do it");

    // The same key replaces the older notice and comes to the front.
    toast.text = "second";
    bar.post (toast);
    CHECK (bar.getNumNotices() == 2);
    CHECK (bar.current()->text == "second");

    // A timed notice expires; an untimed one stays.
    bar.expire (juce::Time::getMillisecondCounterHiRes() + 13000.0);
    CHECK (bar.getNumNotices() == 1);
    CHECK (bar.current()->key == "prompt");

    // The action runs once and takes its notice away.
    bar.getActionButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return actions == 1; }, 2000));
    CHECK (! bar.shouldShow());
    CHECK (! bar.isVisible());
    CHECK (visibilityChanges == 2);
}

TEST_CASE ("App UI: reader warnings of an imported or loaded preset reach the toast through EngineController (E52)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ChangeLog changes;
    controller.addListener (&changes);
    auto& presets = controller.getPresetManager();
    presets.setUserPresetFolder (temp.file ("Presets"));
    CHECK (controller.takePresetWarnings().empty()); // the factory presets are clean

    const auto file = temp.file ("typo.flubpreset.json");
    REQUIRE (file.replaceWithText (R"({ "format": "flubsound-preset", "version": 2, "name": "Typo", "params": { "bost": 0.5, "boost": 7 } })"));
    juce::String error;
    const auto id = presets.importPresetFile (file, error);
    REQUIRE (id.isNotEmpty());
    CHECK (changes.preset >= 1); // the list changed: the UI takes the warnings now
    auto taken = controller.takePresetWarnings();
    REQUIRE (taken.size() == 1);
    CHECK (taken[0].presetName == "Typo");
    CHECK (taken[0].warnings.size() == 2);
    CHECK (controller.takePresetWarnings().empty()); // taken once

    // Loading it into a strip reports them again (the file is unchanged).
    REQUIRE (controller.loadPreset (id, 1, error));
    taken = controller.takePresetWarnings();
    REQUIRE (taken.size() == 1);

    const auto notice = ui::NoticeBar::presetWarningsNotice (taken[0]);
    CHECK (notice.key == ui::NoticeBar::kPresetWarningsKey);
    CHECK (notice.kind == ui::NoticeBar::Notice::Kind::Warning);
    CHECK (notice.seconds == ui::NoticeBar::kPresetWarningSeconds);
    CHECK (notice.text.startsWith ("Preset \"Typo\": "));
    CHECK (notice.text.contains ("\"bost\""));
    CHECK (notice.text.endsWith ("(+1 other warning)"));
    CHECK (notice.detail.contains (taken[0].warnings[1]));

    // The queue is bounded while no UI takes it.
    for (size_t i = 0; i < EngineController::kMaxPendingNotices + 3; ++i)
        REQUIRE (controller.loadPreset (id, 1, error));
    CHECK (controller.takePresetWarnings().size() == EngineController::kMaxPendingNotices);

    controller.removeListener (&changes);
}

TEST_CASE ("App UI: a damaged settings file becomes a notice naming the backup and the kept file (E52)")
{
    AppSettings::Recovery none;
    CHECK (ui::NoticeBar::recoveryNotice (none).text.isEmpty());

    AppSettings::Recovery restored;
    restored.quarantined = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("settings.xml.corrupt-20260928-091500");
    restored.restoredFromBackup = 2;
    const auto n = ui::NoticeBar::recoveryNotice (restored);
    CHECK (n.key == ui::NoticeBar::kRecoveryKey);
    CHECK (n.seconds == 0.0); // stays until dismissed
    CHECK (n.text.contains ("restored from the backup"));
    CHECK (n.text.contains ("(2)"));
    CHECK (n.detail.contains ("settings.xml.corrupt-20260928-091500"));

    AppSettings::Recovery defaults;
    defaults.quarantined = restored.quarantined;
    CHECK (ui::NoticeBar::recoveryNotice (defaults).text.contains ("default settings"));
}

TEST_CASE ("App UI: a preset made for another latency profile leaves the profile and raises the prompt; its action switches every strip (E42a)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music");
    REQUIRE (music >= 0);
    REQUIRE (controller.getLatencyProfile() == LatencyProfileValue::Balanced);
    juce::String error;

    // Audiophile Subtle was made for Quality: the profile stays Balanced.
    REQUIRE (controller.loadPreset (presetNamed (controller, "Audiophile Subtle"), music, error));
    CHECK (controller.getLatencyProfile() == LatencyProfileValue::Balanced);
    for (int s = 0; s < controller.getNumStrips(); ++s)
        CHECK (controller.getParams (s).get (LatencyProfile) == 1.0f);
    auto suggestion = controller.takeLatencySuggestion();
    REQUIRE (suggestion.has_value());
    CHECK (suggestion->presetName == "Audiophile Subtle");
    CHECK (suggestion->suggested == LatencyProfileValue::Quality);
    CHECK (suggestion->current == LatencyProfileValue::Balanced);
    CHECK (! controller.takeLatencySuggestion().has_value()); // taken once

    // The prompt, and its action through the bar.
    ui::NoticeBar bar;
    bar.setBounds (0, 0, 900, 34);
    bar.post (ui::NoticeBar::latencyNotice (*suggestion, [&] { controller.setLatencyProfile (suggestion->suggested); }));
    REQUIRE (bar.current() != nullptr);
    CHECK (bar.current()->text == "\"Audiophile Subtle\" was made for Quality; the engine runs Balanced.");
    CHECK (bar.getActionButton().getButtonText() == "Use Quality");
    bar.getActionButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getLatencyProfile() == LatencyProfileValue::Quality; }, 2000));
    for (int s = 0; s < controller.getNumStrips(); ++s)
        for (const auto bank : { Bank::A, Bank::B })
            CHECK (controller.getParams (s).get (bank, LatencyProfile) == 0.0f);

    // A preset without a suggestion, or one matching the profile, asks nothing.
    REQUIRE (controller.loadPreset (presetNamed (controller, "Audiophile Subtle"), music, error));
    CHECK (! controller.takeLatencySuggestion().has_value());
    REQUIRE (controller.loadPreset (presetNamed (controller, "Flubsound Signature"), music, error));
    CHECK (! controller.takeLatencySuggestion().has_value());

    // Competitive FPS on Quality: Low Latency is suggested; next / previous
    // preset loads count too.
    const int game = controller.findStrip ("Game");
    REQUIRE (controller.loadPreset (presetNamed (controller, "Competitive FPS"), game, error));
    suggestion = controller.takeLatencySuggestion();
    REQUIRE (suggestion.has_value());
    CHECK (suggestion->suggested == LatencyProfileValue::LowLatency);
    bool sawStepSuggestion = false;
    for (int i = 0; i < 30 && ! sawStepSuggestion; ++i)
    {
        REQUIRE (controller.nextPreset (game));
        const auto* info = controller.getPresetManager().findById (controller.getCurrentPresetId (game));
        REQUIRE (info != nullptr);
        const auto step = controller.takeLatencySuggestion();
        CHECK (step.has_value() == (info->suggestedLatencyProfile.has_value() && *info->suggestedLatencyProfile != LatencyProfileValue::Quality));
        sawStepSuggestion = step.has_value();
    }
    CHECK (sawStepSuggestion);
}

// =============================================================================
// E48a: the PipeWire quantum follows the profile chosen by hand
// =============================================================================
TEST_CASE ("App UI: the PipeWire quantum request follows the latency profile; a user's own values win (E48a)")
{
    using P = LatencyProfileValue;
    auto plan = EngineController::planGraphQuantum (P::LowLatency, nullptr, nullptr);
    CHECK (plan.latency == "128/48000");
    CHECK (plan.props == "{ node.lock-quantum = true }");
    for (const auto profile : { P::Balanced, P::Quality })
    {
        plan = EngineController::planGraphQuantum (profile, nullptr, "");
        CHECK (plan.latency == "256/48000");
        CHECK (plan.props.isEmpty());
        CHECK (plan.clearProps); // a lock this process exported goes
    }
    // A user's PIPEWIRE_LATENCY wins for every profile; their props stay.
    plan = EngineController::planGraphQuantum (P::LowLatency, "1024/48000", nullptr);
    CHECK (plan.latency.isEmpty());
    CHECK (plan.props.isEmpty());
    CHECK (! plan.clearProps);
    plan = EngineController::planGraphQuantum (P::LowLatency, "", "{ node.name = x }");
    CHECK (plan.latency == "128/48000");
    CHECK (plan.props.isEmpty());
    plan = EngineController::planGraphQuantum (P::Balanced, nullptr, "{ node.name = x }");
    CHECK (! plan.clearProps);

   #if defined(__linux__)
    // Applied to the process environment (restored afterwards).
    const auto save = [] (const char* name) { const char* v = std::getenv (name); return v != nullptr ? std::optional<std::string> (v) : std::nullopt; };
    const auto latencyBefore = save ("PIPEWIRE_LATENCY"), propsBefore = save ("PIPEWIRE_PROPS");
    ::unsetenv ("PIPEWIRE_LATENCY");
    ::unsetenv ("PIPEWIRE_PROPS");
    CHECK (EngineController::applyGraphQuantum (P::LowLatency, nullptr, nullptr));
    CHECK (std::string (std::getenv ("PIPEWIRE_LATENCY")) == "128/48000");
    CHECK (std::string (std::getenv ("PIPEWIRE_PROPS")) == "{ node.lock-quantum = true }");
    CHECK (! EngineController::applyGraphQuantum (P::LowLatency, nullptr, nullptr)); // unchanged: no device re-open
    CHECK (EngineController::applyGraphQuantum (P::Balanced, nullptr, nullptr));
    CHECK (std::string (std::getenv ("PIPEWIRE_LATENCY")) == "256/48000");
    CHECK (std::getenv ("PIPEWIRE_PROPS") == nullptr);
    // Props the user set are not the lock: Balanced leaves them.
    ::setenv ("PIPEWIRE_PROPS", "{ node.name = x }", 1);
    CHECK (! EngineController::applyGraphQuantum (P::Balanced, nullptr, "{ node.name = x }"));
    CHECK (std::string (std::getenv ("PIPEWIRE_PROPS")) == "{ node.name = x }");
    for (const auto& [name, value] : { std::pair { "PIPEWIRE_LATENCY", latencyBefore }, std::pair { "PIPEWIRE_PROPS", propsBefore } })
    {
        if (value)
            ::setenv (name, value->c_str(), 1);
        else
            ::unsetenv (name);
    }
   #endif
}

// =============================================================================
// E52: rename keeps the uuid
// =============================================================================
TEST_CASE ("App UI: renaming a user preset keeps its uuid; the strip and the automatic profile rule that play it keep it (E52)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    controller.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    const int music = controller.findStrip ("Music");
    juce::String error;
    const auto id = controller.saveUserPreset ("Mine", "User", {}, music, error);
    REQUIRE (id.isNotEmpty());

    AutoProfileRule rule;
    rule.executable = "game.exe";
    rule.stripName = "Music";
    rule.presetId = id;
    REQUIRE (controller.addAutoProfileRule (rule));

    const auto info = *controller.getPresetManager().findById (id);
    const auto oldFile = info.file;
    CHECK (controller.renameUserPreset (info, "Renamed", error) == id);
    const auto* renamed = controller.getPresetManager().findById (id);
    REQUIRE (renamed != nullptr);
    CHECK (renamed->name == "Renamed");
    CHECK (! oldFile.existsAsFile());
    CHECK (renamed->file.existsAsFile());
    CHECK (controller.getCurrentPresetId (music) == id);
    CHECK (controller.getCurrentPresetName (music) == "Renamed");
    CHECK (controller.getSettings().getLastPreset ("Music") == id);
    REQUIRE (controller.getAutoProfileRules().size() == 1);
    CHECK (controller.getAutoProfileRules()[0].presetId == id);

    // Factory presets and taken names are refused, nothing changes.
    CHECK (controller.renameUserPreset (presetNamed (controller, "Flubsound Signature"), "X", error).isEmpty());
    REQUIRE (controller.saveUserPreset ("Other", "User", {}, music, error).isNotEmpty());
    CHECK (controller.renameUserPreset (*controller.getPresetManager().findById (id), "Other", error).isEmpty());
    CHECK (controller.getPresetManager().findById (id)->name == "Renamed");
}

// =============================================================================
// E06: protection strength and the governor readout
// =============================================================================
TEST_CASE ("App UI: protection strength is persisted and reaches every chain, also those of an engine built later (E06)")
{
    const flubapptest::TempFolder temp;
    {
        EngineController controller (headlessOptions (temp, true));
        ChangeLog changes;
        controller.addListener (&changes);
        CHECK (controller.getProtectionStrength() == flub::ProtectionStrength::Off);
        for (int s = 0; s < controller.getNumStrips(); ++s)
            CHECK (controller.getChain (s).getProtectionStrength() == flub::ProtectionStrength::Off);

        controller.setProtectionStrength (flub::ProtectionStrength::Normal);
        CHECK (changes.settings == 1);
        for (int s = 0; s < controller.getNumStrips(); ++s)
            CHECK (controller.getChain (s).getProtectionStrength() == flub::ProtectionStrength::Normal);

        // A rebuilt engine (device restart, layout, profile change) starts
        // its chains at Off; the controller re-applies the setting.
        const auto generation = controller.getEngineGeneration();
        controller.getHost().reconfigure();
        REQUIRE (controller.getEngineGeneration() != generation);
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return changes.engine >= 1; }, 5000));
        for (int s = 0; s < controller.getNumStrips(); ++s)
            CHECK (controller.getChain (s).getProtectionStrength() == flub::ProtectionStrength::Normal);

        controller.setProtectionStrength (flub::ProtectionStrength::Strict);
        controller.saveState();
        controller.removeListener (&changes);
    }
    EngineController again (headlessOptions (temp, true));
    CHECK (again.getProtectionStrength() == flub::ProtectionStrength::Strict);
    for (int s = 0; s < again.getNumStrips(); ++s)
        CHECK (again.getChain (s).getProtectionStrength() == flub::ProtectionStrength::Strict);
    CHECK (EngineController::getProtectionStrengthName (flub::ProtectionStrength::Normal) == "Normal");
}

TEST_CASE ("App UI: the governor chip names the state and the reason; its tooltip gives the averages, the budgets and the strength (E06)")
{
    using G = flub::SafetyGovernor;
    MeterSnapshot s;
    auto r = ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Off);
    CHECK (r.text == "Safety governor OK");
    CHECK (! r.limiting);
    CHECK (r.detail.contains ("Within its budgets"));
    CHECK (r.detail.contains ("Protection strength: Off"));

    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
    s.governorScale = 0.72f;
    s.governorState = static_cast<int> (G::State::BackingOff);
    s.governorReason = G::kReasonLimiter;
    s.governorGrDb = -7.2f;
    s.governorDistortionDb = -41.0f;
    r = ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Normal);
    CHECK (r.text == "Governor 72%" + dot + "limiter");
    CHECK (r.limiting);
    CHECK (r.detail.contains ("applies 72%"));
    CHECK (r.detail.contains ("Limiter, 3 s average: -7.2 dB (budget -6 dB)"));
    CHECK (r.detail.contains ("Distortion (THD+N), 3 s average: -41 dB (budget -30 dB)"));
    CHECK (r.detail.contains ("Protection strength: Normal (also the preset's own"));

    s.governorReason = G::kReasonLimiter | G::kReasonDistortion;
    CHECK (ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Off).text == "Governor 72%" + dot + "limiter + distortion");
    s.governorState = static_cast<int> (G::State::Holding);
    CHECK (ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Off).text == "Governor 72%" + dot + "holding");
    s.governorScale = 0.9f;
    s.governorState = static_cast<int> (G::State::Recovering);
    CHECK (ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Strict).text == "Governor 90%" + dot + "recovering");
}

TEST_CASE ("App UI: on a hot programme at Boost 100 with the clipper off the Boost panel's chip reads the limiter as the reason (E06, end to end)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music");
    controller.setSelectedStrip (music);
    auto& store = controller.getParams (music);
    controller.setMode (ModeValue::Music, music);
    controller.setBoost (1.0f, music);
    store.set (Macro4, 1.0f); // Loudness
    store.set (MaximizerOn, 1.0f);
    store.set (MaxDriveDb, 24.0f);
    store.set (MaxClipAmount, 0.0f); // everything goes to the limiter

    TestSignalGenerator source (controller.getHost().getSampleRate());
    source.setProgramme (music, TestSignalGenerator::Programme::Music, 0.0f);
    ui::BoostPanel panel (controller);
    render (controller, source, 6.0);
    const auto s = snapshotOf (controller, music);
    panel.update (s);

    CHECK (s.governorScale < 0.9f);
    CHECK ((s.governorReason & flub::SafetyGovernor::kReasonLimiter) != 0);
    CHECK (panel.getGovernorReadout().limiting);
    CHECK (panel.getGovernorReadout().text.contains ("limiter"));
    CHECK (panel.getGovernorReadout().text.startsWith ("Governor " + juce::String (juce::roundToInt (s.governorScale * 100.0f)) + "%"));
}

TEST_CASE ("App UI: at Normal / Strict the governor chip's tooltip quotes the budgets of the strip's mode and strength, not Off's (E06)")
{
    // What the snapshot carries is what the tooltip quotes.
    MeterSnapshot s;
    s.governorStrength = static_cast<int> (flub::ProtectionStrength::Strict);
    s.governorGrDb = -2.0f;
    s.governorGrBudgetDb = -4.0f;
    s.governorDriveResidualDb = -38.0f;
    s.governorResidualBudgetDb = -41.0f;
    auto detail = ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Strict).detail;
    CHECK (detail.contains ("Limiter, 3 s average: -2.0 dB (budget -4 dB)"));
    CHECK (detail.contains ("Audible distortion (weighted residual): -38 dB (budget -41 dB)"));
    CHECK (! detail.contains ("THD+N")); // Off's -30 dB THD+N budget does not apply
    s.governorDriveResidualDb = -160.0f;  // before the first reading: the budget all the same
    CHECK (ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Strict).detail.contains ("not measured yet (budget -41 dB)"));
    s.governorStrength = 0;              // the chain ran at Off: its fixed budgets
    detail = ui::BoostPanel::describeGovernor (s, flub::ProtectionStrength::Off).detail;
    CHECK (detail.contains ("(budget -6 dB)"));
    CHECK (detail.contains ("Distortion (THD+N), 3 s average"));

    // End to end: the budgets each strip's chain publishes for its mode.
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music"), game = controller.findStrip ("Game");
    REQUIRE (music >= 0);
    REQUIRE (game >= 0);
    controller.setMode (ModeValue::Music, music);
    controller.setMode (ModeValue::Gaming, game);
    TestSignalGenerator source (controller.getHost().getSampleRate());
    source.setProgramme (music, TestSignalGenerator::Programme::Music, -12.0f);
    source.setProgramme (game, TestSignalGenerator::Programme::Music, -12.0f);
    struct Row
    {
        flub::ProtectionStrength strength;
        int strip;
        const char *residual, *gr;
    };
    for (const auto& row : { Row { flub::ProtectionStrength::Normal, music, "(budget -35 dB)", "(budget -6 dB)" },
                             Row { flub::ProtectionStrength::Normal, game, "(budget -30 dB)", "(budget -6 dB)" },
                             Row { flub::ProtectionStrength::Strict, music, "(budget -41 dB)", "(budget -4 dB)" },
                             Row { flub::ProtectionStrength::Strict, game, "(budget -36 dB)", "(budget -4 dB)" } })
    {
        controller.setProtectionStrength (row.strength);
        render (controller, source, 0.1);
        const auto readout = ui::BoostPanel::describeGovernor (snapshotOf (controller, row.strip), row.strength);
        CHECK (readout.detail.contains (juce::String ("Audible distortion (weighted residual): ")));
        CHECK (readout.detail.contains (row.residual));
        CHECK (readout.detail.contains (juce::String ("dB ") + row.gr));
        CHECK (readout.detail.contains (row.strength == flub::ProtectionStrength::Strict ? "Protection strength: Strict" : "Protection strength: Normal"));
    }
}

// =============================================================================
// E38 slice / E11: what the sound is doing
// =============================================================================
TEST_CASE ("App UI: active-now chips name the engaged stages: the subsonic at Boost 0, >= 3 chips at Boost 55, a limiter only while it acts (E38)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    const int music = controller.findStrip ("Music");
    juce::String error;
    REQUIRE (controller.loadPreset (presetNamed (controller, "Flubsound Signature"), music, error));
    controller.setMode (ModeValue::Music, music);
    controller.setBoost (0.0f, music);
    auto& store = controller.getParams (music);
    for (const int m : { Macro1, Macro2, Macro3, Macro4, Macro5 })
        store.set (m, 0.0f);

    TestSignalGenerator source (controller.getHost().getSampleRate());
    source.setProgramme (music, TestSignalGenerator::Programme::Music, 0.0f);
    render (controller, source, 0.5);
    const auto atZero = chipTexts (controller, music);
    CHECK (anyStartsWith (atZero, "Subsonic 20 Hz"));

    controller.setBoost (0.55f, music);
    render (controller, source, 0.5);
    const auto at55 = chipTexts (controller, music);
    CHECK (at55.size() >= 3);
    CHECK (at55.size() > atZero.size());

    // Pure: nothing on -> nothing; a module switched off hides its stages;
    // the limiter chip follows the meter, not the setting.
    std::vector<float> e (static_cast<size_t> (kNumParams), 0.0f);
    const auto effective = [&e] (int id) { return e[static_cast<size_t> (id)]; };
    MeterSnapshot s;
    CHECK (ui::BoostPanel::describeActiveStages (effective, s, 2).empty());
    e[BassOn] = 1.0f;
    e[BassBoostDb] = 3.1f;
    e[BassBoostFreq] = 70.0f;
    e[SpatialOn] = 1.0f;
    e[SpatialWidth] = 1.18f;
    e[MaximizerOn] = 1.0f;
    e[MaxDriveDb] = 4.0f;
    auto stages = ui::BoostPanel::describeActiveStages (effective, s, 2);
    REQUIRE (stages.size() == 3);
    CHECK (stages[0].text == "Bass +3.1 dB @ 70 Hz");
    CHECK (stages[1].text == "Width 118%");
    CHECK (stages[2].text == "Maximizer +4.0 dB drive");
    s.maxGainReductionDb = -2.14f;
    stages = ui::BoostPanel::describeActiveStages (effective, s, 2);
    REQUIRE (stages.size() == 4);
    CHECK (stages[3].text == "Limiter -2.1 dB");
    e[BassOn] = 0.0f;
    CHECK (ui::BoostPanel::describeActiveStages (effective, s, 2).size() == 3);
    // The virtualiser counts only for surround input it renders.
    e[VirtualizerOn] = 1.0f;
    CHECK (ui::BoostPanel::describeActiveStages (effective, s, 2).size() == 3);
    CHECK (ui::BoostPanel::describeActiveStages (effective, s, 8).size() == 4);
    s.inputFold = 1; // stereo passthrough
    CHECK (ui::BoostPanel::describeActiveStages (effective, s, 8).size() == 3);
}

TEST_CASE ("App UI: LoudnessPanel's in -> out difference and the limiter's active share (E38 / E11)")
{
    CHECK (ui::LoudnessPanel::formatInOutDelta (-23.0f, -20.7f) == "+2.3 LU");
    CHECK (ui::LoudnessPanel::formatInOutDelta (-14.0f, -16.5f) == "-2.5 LU");
    CHECK (ui::LoudnessPanel::formatInOutDelta (-80.0f, -20.0f) == "--");
    CHECK (ui::LoudnessPanel::formatInOutDelta (-20.0f, -160.0f) == "--");

    ui::LoudnessPanel panel;
    MeterSnapshot s;
    s.active = true;
    s.maxGainReductionDb = -3.0f;
    constexpr double frame = 1.0 / 60.0;
    for (int i = 0; i < 60 * 40; ++i) // 40 s limiting
        panel.update (s, frame);
    CHECK (panel.getLimiterActiveShare() > 0.95f);
    s.maxGainReductionDb = -0.5f; // under the 1 dB threshold
    for (int i = 0; i < 60 * 40; ++i)
        panel.update (s, frame);
    CHECK (panel.getLimiterActiveShare() < 0.05f);
    // Half the time (alternating seconds) settles near one half.
    for (int i = 0; i < 60 * 60; ++i)
    {
        s.maxGainReductionDb = (i / 60) % 2 == 0 ? -2.0f : 0.0f;
        panel.update (s, frame);
    }
    CHECK_NEAR (panel.getLimiterActiveShare(), 0.5, 0.06);
    // "Limiter active x %" (E11) spelled out on hover over LIM.
    CHECK (ui::LoudnessPanel::describeLimiterActive (0.123f).startsWith ("Limiter active 12 % of the last 10 s"));
    CHECK (ui::LoudnessPanel::describeLimiterActive (0.123f).contains ("more than 1 dB"));
    CHECK (ui::LoudnessPanel::describeLimiterActive (2.0f).startsWith ("Limiter active 100 %"));
}

// =============================================================================
// The window
// =============================================================================
TEST_CASE ("App UI: MainComponent shows the device banner, the preset toast and the latency prompt under the header")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    controller.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    ui::MainComponent main (controller);
    main.setSize (1280, 820);
    auto& notices = main.getNoticeBar();
    auto& banner = main.getDeviceErrorBanner();
    CHECK (! notices.shouldShow());
    CHECK (! banner.isVisible());
    juce::String error;

    // A preset made for Quality, loaded by hand: the prompt.
    const int music = controller.findStrip ("Music");
    controller.setSelectedStrip (music);
    REQUIRE (controller.loadPreset (presetNamed (controller, "Audiophile Subtle"), music, error));
    REQUIRE (notices.current() != nullptr);
    CHECK (notices.current()->key == ui::NoticeBar::kLatencyKey);
    CHECK (notices.isVisible());
    CHECK (notices.getY() >= 56);
    CHECK (notices.getHeight() == ui::DeviceAdviceBanner::kHeight);

    // Another preset: the prompt no longer applies.
    REQUIRE (controller.loadPreset (presetNamed (controller, "Flubsound Signature"), music, error));
    CHECK (! notices.hasNotice (ui::NoticeBar::kLatencyKey));

    // A preset with a typo: the toast.
    const auto file = temp.file ("typo.flubpreset.json");
    REQUIRE (file.replaceWithText (R"({ "format": "flubsound-preset", "version": 2, "name": "Typo", "params": { "bost": 0.5 } })"));
    REQUIRE (controller.getPresetManager().importPresetFile (file, error).isNotEmpty());
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return notices.hasNotice (ui::NoticeBar::kPresetWarningsKey); }, 2000));
    CHECK (notices.current()->key == ui::NoticeBar::kPresetWarningsKey);
    CHECK (notices.current()->text.contains ("\"bost\""));

    // The prompt's action switches the profile and removes the prompt.
    REQUIRE (controller.loadPreset (presetNamed (controller, "Audiophile Subtle"), music, error));
    REQUIRE (notices.current() != nullptr);
    REQUIRE (notices.current()->key == ui::NoticeBar::kLatencyKey);
    notices.getActionButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getLatencyProfile() == LatencyProfileValue::Quality; }, 2000));
    CHECK (! notices.hasNotice (ui::NoticeBar::kLatencyKey));

    // A device error: the banner sits above the notices.
    ChangeLog changes;
    controller.addListener (&changes);
    controller.getHost().audioDeviceError ("USB Headset was disconnected");
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return changes.device >= 1; }, 5000));
    CHECK (banner.isVisible());
    CHECK (banner.getMessage() == "USB Headset was disconnected");
    CHECK (banner.getBottom() <= notices.getY());
    controller.removeListener (&changes);
}

TEST_CASE ("App UI: the screenshot driver's --state option parses the known states and refuses others")
{
    ScreenshotDriver::Options o;
    juce::String error;
    CHECK (ScreenshotDriver::parseCommandLine ({ "--screenshot", "out.png", "--state", "device-error,latency-prompt" }, o, error));
    CHECK (error.isEmpty());
    CHECK (o.states.size() == 2);
    CHECK (o.states.contains ("latency-prompt"));
    ScreenshotDriver::Options readings; // Phase 3 batch 3: the controls outside the generic grid
    CHECK (ScreenshotDriver::parseCommandLine ({ "--screenshot", "out.png", "--state", "module-readings,contour-curve" }, readings, error));
    CHECK (readings.states.size() == 2);
    ScreenshotDriver::Options bad;
    CHECK (! ScreenshotDriver::parseCommandLine ({ "--screenshot", "out.png", "--state", "bogus" }, bad, error));
    CHECK (error.startsWith ("--state must be one or more of"));
}
