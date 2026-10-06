#include "ScreenshotDriver.h"

#include "ui/MainComponent.h"
#include "ui/QuickControls.h"
#include "ui/SettingsDialog.h"
#include "ui/vis/VisualiserRegistry.h"

#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"

#include <algorithm>

namespace flub::app
{
using namespace flub::param;

bool ScreenshotDriver::isVisualiserState (const juce::String& state)
{
    if (state == "vis-beside" || state == "vis-popout-full")
        return true;
    if (state.startsWith ("vis-popout-"))
        return ui::vis::VisualiserWindow::findChoice (state.fromFirstOccurrenceOf ("vis-popout-", false, false)) != nullptr;
    if (state.startsWith ("vis-strip-"))
    {
        const auto* d = ui::vis::findDescriptor (state.fromFirstOccurrenceOf ("vis-strip-", false, false));
        return d != nullptr && d->canBeStrip;
    }
    if (state.startsWith ("vis-"))
    {
        const auto* d = ui::vis::findDescriptor (state.fromFirstOccurrenceOf ("vis-", false, false));
        return d != nullptr && d->canBeMain;
    }
    return false;
}

bool ScreenshotDriver::parseCommandLine (const juce::StringArray& args, Options& options, juce::String& error)
{
    const int index = args.indexOf ("--screenshot");
    if (index < 0)
        return false;

    if (index + 1 >= args.size() || args[index + 1].startsWith ("--"))
    {
        error = "--screenshot needs an output file, e.g. --screenshot out.png";
        return false;
    }
    options.output = juce::File::getCurrentWorkingDirectory().getChildFile (args[index + 1].unquoted());

    const int modeIndex = args.indexOf ("--mode");
    if (modeIndex >= 0)
    {
        const auto mode = args[modeIndex + 1].toLowerCase();
        if (mode != "music" && mode != "gaming")
        {
            error = "--mode must be 'music' or 'gaming'";
            return false;
        }
        options.gamingMode = mode == "gaming";
    }

    const int sizeIndex = args.indexOf ("--size");
    if (sizeIndex >= 0)
    {
        const auto size = args[sizeIndex + 1].toLowerCase();
        const int w = size.upToFirstOccurrenceOf ("x", false, false).getIntValue();
        const int h = size.fromFirstOccurrenceOf ("x", false, false).getIntValue();
        if (w < 64 || h < 64 || w > 8192 || h > 8192)
        {
            error = "--size must look like 1280x820";
            return false;
        }
        options.width = w;
        options.height = h;
    }

    const int secondsIndex = args.indexOf ("--seconds");
    if (secondsIndex >= 0)
        options.seconds = std::clamp (args[secondsIndex + 1].getDoubleValue(), 0.2, 60.0);

    const int scaleIndex = args.indexOf ("--scale");
    if (scaleIndex >= 0)
        options.scale = std::clamp (args[scaleIndex + 1].getFloatValue(), 0.5f, 4.0f);

    const int deviceIndex = args.indexOf ("--device");
    if (deviceIndex >= 0)
    {
        if (deviceIndex + 1 >= args.size() || args[deviceIndex + 1].startsWith ("--"))
        {
            error = "--device needs an output device name, e.g. --device \"Headphones (Stealth 700 Gen 2 MAX)\"";
            return false;
        }
        options.simulatedDevice = args[deviceIndex + 1].unquoted();
    }

    const int viewIndex = args.indexOf ("--view");
    if (viewIndex >= 0)
    {
        const auto view = args[viewIndex + 1].toLowerCase();
        if (view != "simple" && view != "advanced")
        {
            error = "--view must be 'advanced' or 'simple'";
            return false;
        }
        options.simpleView = view == "simple";
    }

    const int stateIndex = args.indexOf ("--state");
    if (stateIndex >= 0)
    {
        static const juce::StringArray known { "device-error",   "loopback",       "preset-warning", "recovery",        "latency-prompt",
                                                "governor",       "preset-browser", "settings-audio", "settings-processing", "ab-matched",
                                                "abx",            "bypass",         "routing-drawer", "governor-normal", "quick-controls",
                                                "module-readings", "contour-curve", "onboard-cap",   "settings-diagnostics",
                                                // docs/11 E32 (c) / E33 / E34 and the batch 5 keys in their cards
                                                "settings-hearing", "settings-hearing-unknown", "hearing-profile", "hearing-readout",
                                                "module-keys",
                                                // the analyser's optional views
                                                "analyzer-diff", "analyzer-lows", "analyzer-width", "analyzer-keys", "analyzer-spectrogram",
                                                "analyzer-hover", "analyzer-freeze", "analyzer-fundamentals" };
        options.states = juce::StringArray::fromTokens (args[stateIndex + 1].toLowerCase(), ",", {});
        options.states.trim();
        options.states.removeEmptyStrings();
        for (const auto& state : options.states)
            if (! known.contains (state) && ! isVisualiserState (state))
            {
                error = "--state must be one or more of " + known.joinIntoString (", ")
                        + ", vis-beside, vis-<id> or vis-strip-<id> for a visualiser id (comma separated)";
                return false;
            }
        if (options.states.isEmpty())
        {
            error = "--state needs a state, e.g. --state device-error";
            return false;
        }
    }

    return true;
}

ScreenshotDriver::ScreenshotDriver (EngineController& c, juce::Component& t, Options o, Completion done)
    : controller (c), target (t), options (std::move (o)), onFinished (std::move (done))
{
}

ScreenshotDriver::~ScreenshotDriver()
{
    stopTimer();
}

void ScreenshotDriver::setUpScene()
{
    const int numStrips = controller.getNumStrips();
    int gameStrip = controller.findStrip ("Game");
    for (int i = 0; gameStrip < 0 && i < numStrips; ++i)
        if (controller.getStripChannels (i) >= 6)
            gameStrip = i;
    if (gameStrip < 0)
        gameStrip = 0;

    int musicStrip = controller.findStrip ("Music");
    for (int i = 0; musicStrip < 0 && i < numStrips; ++i)
        if (i != gameStrip)
            musicStrip = i;
    if (musicStrip < 0)
        musicStrip = gameStrip;

    const bool gaming = options.gamingMode;
    const int focus = gaming ? gameStrip : musicStrip;
    const auto wantedMode = gaming ? ModeValue::Gaming : ModeValue::Music;

    // A factory preset of the right mode is the most realistic starting point.
    for (const auto& preset : controller.getPresetManager().getFactoryPresets())
    {
        if (preset.mode == (gaming ? "Gaming" : "Music"))
        {
            juce::String error;
            controller.loadPreset (preset, focus, error);
            break;
        }
    }

    controller.setMode (wantedMode, focus);
    if (controller.getBoost (focus) < 0.45f)
        controller.setBoost (0.55f, focus);

    auto& store = controller.getParams (focus);
    static constexpr int macros[] = { Macro1, Macro2, Macro3, Macro4, Macro5 };
    static constexpr float macroValues[] = { 0.6f, 0.45f, 0.5f, 0.35f, 0.4f };
    bool anyMacro = false;
    for (const int m : macros)
        anyMacro = anyMacro || store.get (m) > 0.0f;
    if (! anyMacro)
        for (size_t i = 0; i < 5; ++i)
            store.set (macros[i], macroValues[i]);

    controller.setSelectedStrip (focus);
    sceneStrip = focus;
    if (options.simulatedDevice.isNotEmpty())
        controller.simulateOutputDevice (options.simulatedDevice, controller.getHost().getSampleRate(), 2);

    applyStates (gameStrip, focus);

    sampleRate = controller.getHost().getSampleRate();
    generator = std::make_unique<TestSignalGenerator> (sampleRate);
    if (gaming)
    {
        generator->setProgramme (gameStrip, TestSignalGenerator::Programme::Game71, 0.0f);
        if (musicStrip != gameStrip)
        {
            generator->setProgramme (musicStrip, TestSignalGenerator::Programme::Music, -12.0f);
            controller.setMode (ModeValue::Music, musicStrip);
            controller.setBoost (0.3f, musicStrip);
        }
    }
    else
    {
        generator->setProgramme (musicStrip, TestSignalGenerator::Programme::Music, 0.0f);
    }
}

void ScreenshotDriver::applyStates (int gameStrip, int focusStrip)
{
    auto* main = dynamic_cast<ui::MainComponent*> (&target);
    if (main != nullptr)
    {
        main->getNoticeBar().clear(); // the scene's preset loads are not what a screenshot shows
        main->setView (options.simpleView ? ui::MainComponent::View::Simple : ui::MainComponent::View::Advanced, false);
    }
    const auto& states = options.states;

    if (states.contains ("latency-prompt"))
    {
        // The real path: a preset made for another profile, loaded by hand.
        controller.setLatencyProfile (LatencyProfileValue::Balanced);
        const auto* preset = controller.getPresetManager().findByName (options.gamingMode ? "Competitive FPS" : "Audiophile Subtle");
        juce::String error;
        if (preset != nullptr)
            controller.loadPreset (*preset, focusStrip, error);
        controller.setBoost (0.55f, focusStrip);
    }
    if (states.contains ("governor") || states.contains ("governor-normal"))
    {
        auto& store = controller.getParams (focusStrip);
        controller.setBoost (1.0f, focusStrip);
        store.set (options.gamingMode ? Macro3 : Macro4, 1.0f); // Impact / Loudness
        store.set (MaximizerOn, 1.0f);
        store.set (MaxDriveDb, 12.0f);
        controller.setProtectionStrength (states.contains ("governor") ? flub::ProtectionStrength::Strict : flub::ProtectionStrength::Normal);
    }
    if (states.contains ("ab-matched") || states.contains ("abx"))
    {
        // B: the scene's sound pushed louder; A as it was (docs/11 E37).
        auto& store = controller.getParams (focusStrip);
        controller.setActiveBank (Bank::A, focusStrip);
        controller.copyActiveToOtherBank (focusStrip);
        for (const int id : { static_cast<int> (BoostIntensity), static_cast<int> (options.gamingMode ? Macro3 : Macro4) })
            store.set (Bank::B, id, 1.0f);
        controller.setActiveBank (Bank::B, focusStrip);
    }
    if (states.contains ("bypass"))
        bypassAtSeconds = options.seconds * 0.8; // after the processed loudness was read (valid after 3 s)
    if (states.contains ("quick-controls"))
    {
        auto flyout = std::make_unique<ui::QuickControls> (controller);
        flyout->setSize (options.width, options.height);
        settingsView = std::move (flyout);
    }
    if (states.contains ("device-error"))
        controller.getHost().audioDeviceError ("The device \"USB Headset\" was disconnected (the driver stopped the stream)");
    if (states.contains ("loopback"))
    {
        controller.getHost().setDeviceInputRouting (gameStrip, 0);
        controller.getHost().checkLoopbackPair ("CABLE Output (VB-Audio Virtual Cable)", "CABLE Input (VB-Audio Virtual Cable)");
    }
    if (states.contains ("contour-curve"))
    {
        // The Processing page's contour curve and the preamp's hot-programme
        // switch (docs/11 E32 / E11), on the selected strip.
        auto& store = controller.getParams (focusStrip);
        store.set (ContourOn, 1.0f);
        store.set (ContourLevelDb, -30.0f);
        store.set (AutoPreampOn, 1.0f);
        store.set (AutoPreampHot, 1.0f);
    }
    if (states.contains ("onboard-cap"))
    {
        // docs/11 E16: a Turtle Beach headset whose own enhancement is on,
        // answered as the device banner's "Yes" does: every strip capped.
        if (options.simulatedDevice.isEmpty())
            controller.simulateOutputDevice ("Headset Earphone (Stealth 700 Gen 2 MAX)", controller.getHost().getSampleRate(), 2);
        controller.setOnboardEnhancement (true);
    }
    if (states.contains ("settings-audio") || states.contains ("settings-processing") || states.contains ("contour-curve")
        || states.contains ("settings-diagnostics"))
    {
        const bool processing = states.contains ("settings-processing") || states.contains ("contour-curve");
        const bool diagnostics = states.contains ("settings-diagnostics"); // docs/11 E54: the Updates section
        if (processing)
            controller.setContourFollowsVolume (true); // the listening level live (docs/11 E32)
        ui::HotkeyHooks hooks;
        hooks.isSupported = [] { return false; };
        hooks.getFailures = [] { return juce::StringArray(); };
        hooks.reRegister = [] {};
        auto dialog = std::make_unique<ui::SettingsDialog> (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
        dialog->setSize (options.width, options.height);
        dialog->showPage (diagnostics  ? ui::SettingsDialog::Page::Diagnostics
                          : processing ? ui::SettingsDialog::Page::Processing
                                       : ui::SettingsDialog::Page::Audio);
        settingsView = std::move (dialog);
    }
    if (states.contains ("module-readings") && ! options.gamingMode)
    {
        // The controls and readings outside the generic grid (docs/11 E05 /
        // E07 / E14): a named style with LF Limit, the Smoothness cut on a
        // bright Boost, Warmth's Tube choice (the saturator Warmth's alone).
        auto& store = controller.getParams (focusStrip);
        controller.setBoost (1.0f, focusStrip);
        store.set (Macro3, 1.0f); // Clarity
        store.set (Macro5, 0.6f); // Warmth
        store.set (SmoothAmount, 1.0f);
        store.set (SaturationOn, 0.0f);
        store.set (SatType, layout()[static_cast<size_t> (SatType)].defaultValue);
        store.set (MaximizerOn, 1.0f);
        store.set (MaxStyle, static_cast<float> (MaxStyleValue::Punchy));
        store.set (MaxLfLimit, 0.5f);
        if (main != nullptr)
            main->getRack().scrollToCard ("sat"); // Saturation, Compressor and Maximizer in view from 1920 px
    }
    if (states.contains ("settings-hearing") || states.contains ("settings-hearing-unknown") || states.contains ("hearing-profile")
        || states.contains ("hearing-readout"))
    {
        // docs/11 E32 (c): a Turtle Beach headset (no shipped profile has a
        // sensitivity), with the listener's own figure and the cap on, except
        // for "unknown"; E33: a right-ear high-frequency preference.
        if (options.simulatedDevice.isEmpty())
            controller.simulateOutputDevice ("Headset Earphone (Stealth 700 Gen 2 MAX)", controller.getHost().getSampleRate(), 2);
        if (! states.contains ("settings-hearing-unknown"))
        {
            controller.setHearingSensitivity (108.0f);
            controller.setHearingCap (true, 85.0f);
        }
        if (states.contains ("hearing-profile"))
        {
            flub::PersonalProfile profile;
            profile.enabled = true;
            profile.bandDb[1] = { 0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 6.0f, 9.0f, 12.0f };
            controller.setPersonalProfile (profile); // the page below: give a tall --size (860x1400) to see the editor
        }
        if (! states.contains ("hearing-readout"))
        {
            ui::HotkeyHooks hooks;
            hooks.isSupported = [] { return false; };
            hooks.getFailures = [] { return juce::StringArray(); };
            hooks.reRegister = [] {};
            auto dialog = std::make_unique<ui::SettingsDialog> (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
            dialog->setSize (options.width, options.height);
            dialog->showPage (ui::SettingsDialog::Page::Hearing);
            settingsView = std::move (dialog);
        }
    }
    if (states.contains ("module-keys") && main != nullptr)
    {
        // The batch 5 keys outside the generic grid: Presence Mode and the
        // per-band attack on the Clarity card, Crossfeed Type on Stereo &
        // Space, the renderer on the Virtualizer card (when the core has it).
        auto& store = controller.getParams (focusStrip);
        store.set (ClarityOn, 1.0f);
        store.set (ClarityPresenceMode, static_cast<float> (PresenceModeValue::Relative));
        store.set (SpatialOn, 1.0f);
        store.set (SpatialCrossfeedType, static_cast<float> (CrossfeedTypeValue::Meier));
        main->getRack().scrollToCard ("clarity");
    }
    if (main == nullptr)
        return;
    {
        // The analyser's optional views: through setOptions, so nothing is saved.
        auto o = main->getAnalyzerPanel().getOptions();
        o.difference = o.difference || states.contains ("analyzer-diff");
        o.sharpLows = o.sharpLows || states.contains ("analyzer-lows");
        o.width = o.width || states.contains ("analyzer-width");
        o.pianoKeys = o.pianoKeys || states.contains ("analyzer-keys") || states.contains ("analyzer-fundamentals");
        o.fundamentals = o.fundamentals || states.contains ("analyzer-fundamentals");
        o.spectrogram = o.spectrogram || states.contains ("analyzer-spectrogram");
        // Visualisers: vis-<id> (main view), vis-strip-<id>, vis-beside.
        juce::String popOut;
        for (const auto& state : states)
        {
            if (state == "vis-popout-full")
                continue;
            if (state.startsWith ("vis-popout-"))
                popOut = state.fromFirstOccurrenceOf ("vis-popout-", false, false);
            else if (state == "vis-beside")
                o.beside = true;
            else if (state.startsWith ("vis-strip-"))
                o.strip = state.fromFirstOccurrenceOf ("vis-strip-", false, false);
            else if (state.startsWith ("vis-"))
                o.visualiser = state.fromFirstOccurrenceOf ("vis-", false, false);
        }
        main->getAnalyzerPanel().setOptions (o);
        if (popOut.isNotEmpty())
        {
            main->openVisualiserWindow (popOut);
            if (auto* window = main->getVisualiserWindow())
            {
                if (states.contains ("vis-popout-full"))
                {
                    window->setFullScreenMode (true);
                    window->setBounds (window->getBounds().withSize (options.width, options.height));
                }
                else
                {
                    window->setContentComponentSize (options.width, options.height);
                }
                popOutShot = true;
            }
        }
        if (states.contains ("analyzer-freeze"))
            freezeAtSeconds = options.seconds * 0.4;
    }
    if (states.contains ("recovery"))
    {
        AppSettings::Recovery recovery;
        recovery.quarantined = controller.getSettings().getFile().getSiblingFile (controller.getSettings().getFile().getFileName()
                                                                                    + ".corrupt-20260928-091500");
        recovery.restoredFromBackup = 1;
        main->getNoticeBar().post (ui::NoticeBar::recoveryNotice (recovery));
    }
    if (states.contains ("preset-browser"))
    {
        // The browser searched for a late-night preset, the best match
        // selected and previewing (loudness matched against the scene's preset).
        main->getHeader().showPresetBrowser();
        if (auto* browser = main->getHeader().getPresetBrowser())
        {
            browser->setQuery (options.gamingMode ? "night quiet" : "late night quiet");
            const auto shown = browser->getShownPresets();
            if (! shown.empty())
                browser->selectPreset (shown.front()->id);
        }
    }
    if (states.contains ("routing-drawer"))
        main->setRoutingDrawerOpen (true);
    if (states.contains ("abx"))
    {
        main->openBlindTest();
        if (auto* panel = main->getBlindTest(); panel != nullptr && panel->getTest() != nullptr)
            for (const char key : { '1', '2', '1' }) // three trials answered
                panel->keyPressed (juce::KeyPress (key));
    }
    if (states.contains ("preset-warning"))
    {
        // A user preset with a typo'd key and an out-of-range value, read by
        // the same reader as every preset load.
        const std::string text = R"({ "format": "flubsound-preset", "version": 2, "name": "My Club Mix", "mode": "Music",
                                      "params": { "bost": 0.6, "bass.boost": 40 } })";
        flub::json::Value root;
        std::string parseError;
        flub::preset::Preset preset;
        if (flub::json::parse (text, root, parseError) && flub::preset::fromJson (root, preset, parseError))
        {
            EngineController::PresetWarnings warnings;
            warnings.presetName = juce::String::fromUTF8 (preset.name.c_str());
            for (const auto& w : preset.warnings)
                warnings.warnings.add (juce::String::fromUTF8 (w.c_str()));
            if (! warnings.warnings.isEmpty())
                main->getNoticeBar().post (ui::NoticeBar::presetWarningsNotice (warnings));
        }
    }
}

void ScreenshotDriver::start()
{
    setUpScene();
    startMs = lastMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

void ScreenshotDriver::timerCallback()
{
    if (finished)
        return;

    // Real-time pacing: render exactly the audio that "would have played"
    // since the last tick (capped so a stalled message loop cannot explode).
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double elapsedMs = now - lastMs;
    lastMs = now;

    const auto samples = static_cast<int> (std::clamp (elapsedMs * 0.001 * sampleRate, 0.0, 0.1 * sampleRate));
    if (samples > 0)
    {
        controller.renderOffline (*generator, samples);
        renderedSamples += samples;
    }

    if (bypassAtSeconds > 0.0 && now - startMs >= bypassAtSeconds * 1000.0)
    {
        controller.setEnabled (false);
        bypassAtSeconds = 0.0;
    }
    if (freezeAtSeconds > 0.0 && now - startMs >= freezeAtSeconds * 1000.0)
    {
        freezeAtSeconds = 0.0;
        if (auto* main = dynamic_cast<ui::MainComponent*> (&target))
        {
            main->getAnalyzerPanel().freeze();
            auto& store = controller.getParams (sceneStrip);
            store.set (eq (6, EqFieldOn), 1.0f);
            store.set (eq (6, EqFieldGain), 9.0f);
        }
    }

    const auto wanted = static_cast<int64_t> (options.seconds * sampleRate * 0.9);
    if (now - startMs >= options.seconds * 1000.0 && renderedSamples >= wanted)
        finish();
}

void ScreenshotDriver::finish()
{
    finished = true;
    stopTimer();

    if (auto* main = dynamic_cast<ui::MainComponent*> (&target); main != nullptr && options.states.contains ("analyzer-hover"))
    {
        // As if the mouse rested over 62 Hz, a third of the way down the plot.
        auto& panel = main->getAnalyzerPanel();
        const auto plot = panel.getAnalyzer().getPlotArea();
        panel.getEqEditor().showReadoutAt ({ panel.getAnalyzer().xForFrequency (62.0), plot.getY() + plot.getHeight() * 0.35f });
    }

    juce::Component* popOut = nullptr;
    if (auto* main = dynamic_cast<ui::MainComponent*> (&target); main != nullptr && popOutShot && main->getVisualiserWindow() != nullptr)
        popOut = main->getVisualiserWindow()->getContentComponent();
    auto& shown = popOut != nullptr ? *popOut : (settingsView != nullptr ? *settingsView : target);
    const auto image = shown.createComponentSnapshot (shown.getLocalBounds(), true, options.scale);
    bool ok = false;

    if (image.isValid())
    {
        options.output.getParentDirectory().createDirectory();
        options.output.deleteFile();
        juce::FileOutputStream stream (options.output);
        juce::PNGImageFormat png;
        ok = stream.openedOk() && png.writeImageToStream (image, stream);
        stream.flush();
    }

    const auto message = ok ? "Screenshot written: " + options.output.getFullPathName() + " (" + juce::String (image.getWidth()) + "x"
                                  + juce::String (image.getHeight()) + ")"
                            : "Could not write the screenshot to " + options.output.getFullPathName();

    // May destroy this object (the application quits): nothing after this.
    if (onFinished != nullptr)
        onFinished (ok, message);
}
} // namespace flub::app
