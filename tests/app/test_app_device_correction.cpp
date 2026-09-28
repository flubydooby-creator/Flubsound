// App-level tests: per-output-endpoint device correction (docs/11 E15).
//
// * EngineController imports an AutoEQ / Equalizer APO ParametricEQ.txt for
//   the current output endpoint (a synthetic curve file: no third-party
//   measurement data is bundled), stores it per endpoint in AppSettings and
//   hands it to AudioEngineHost, which runs it on the master sum before the
//   limiter with the automatic preamp (docs/11 E11).
// * Preset loads, A/B switches and automatic profiles never touch it; an
//   endpoint change swaps it (to flat for an endpoint without one, back
//   again when the endpoint returns); it survives a restart.
// * Off / compare / remove, refused files and "no output open".
// * Rendered through the real engine: a +6 dB bell's automatic preamp lowers
//   the rest of the spectrum by 6 dB; off is flat again.
// * Settings > Correction shows it and drives it.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "ui/SettingsDialog.h"

#include <cmath>
#include <functional>
#include <memory>

using namespace flub::app;
using flub::param::Bank;

namespace
{
constexpr const char* kSyntheticCurve = "Preamp: -6.2 dB\n"
                                        "Filter 1: ON LSC Fc 105 Hz Gain 5.8 dB Q 0.70\n"
                                        "Filter 2: ON PK Fc 180 Hz Gain -2.6 dB Q 0.53\n"
                                        "Filter 3: ON PK Fc 1200 Hz Gain 1.9 dB Q 1.20\n"
                                        "Filter 4: ON PK Fc 2600 Hz Gain -3.4 dB Q 2.10\n"
                                        "Filter 5: ON PK Fc 3700 Hz Gain 4.2 dB Q 3.20\n"
                                        "Filter 6: ON PK Fc 5400 Hz Gain -5.1 dB Q 4.00\n"
                                        "Filter 7: ON PK Fc 7900 Hz Gain 3.3 dB Q 2.50\n"
                                        "Filter 8: ON PK Fc 11000 Hz Gain -2.2 dB Q 1.10\n"
                                        "Filter 9: ON PK Fc 16000 Hz Gain 1.5 dB Q 0.90\n"
                                        "Filter 10: ON HSC Fc 10000 Hz Gain -1.2 dB Q 0.70\n";

constexpr const char* kHeadset = "Headphones (Test Headset USB)";
constexpr const char* kSpeakers = "Speakers (Test Speakers)";

/** What the fake foreground-app service reports (message thread). */
struct Foreground
{
    std::string executable = "explorer.exe";
};

class FakeForegroundApp final : public flub::platform::ForegroundApp
{
public:
    explicit FakeForegroundApp (Foreground& f) : fg (f) {}
    bool isSupported() const override { return true; }
    bool query (flub::platform::ForegroundAppInfo& info) override
    {
        info = {};
        info.processId = 100;
        info.executableName = fg.executable;
        info.executablePath = "C:\\Games\\" + fg.executable;
        return true;
    }
    std::string unsupportedReason() const override { return {}; }

private:
    Foreground& fg;
};

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp, bool persist = false, Foreground* foreground = nullptr)
{
    EngineController::Options o;
    if (foreground != nullptr)
        o.foregroundAppFactory = [foreground] { return std::make_unique<FakeForegroundApp> (*foreground); };
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    return o;
}

/** A sine on the Music strip only. */
class SineSource final : public StripSignalSource
{
public:
    SineSource (int strip, double freq, double sampleRate, float amplitude) : target (strip), step (2.0 * juce::MathConstants<double>::pi * freq / sampleRate), amp (amplitude) {}

    bool renderStrip (int strip, const flub::AudioBlock& block) override
    {
        if (strip != target)
            return false;
        for (int i = 0; i < block.numSamples; ++i)
        {
            const float v = amp * static_cast<float> (std::sin (phase));
            for (int c = 0; c < block.numChannels; ++c)
                block.channel (c)[i] = v;
            phase += step;
        }
        return true;
    }

private:
    int target;
    double step, phase = 0.0;
    float amp;
};

/** Output amplitude (dB re the input amplitude) of a steady sine through the
    whole engine, over the last half of `seconds`. */
double renderedGainDb (EngineController& controller, int strip, double freq, double seconds = 1.0)
{
    auto& host = controller.getHost();
    const int block = host.getBlockSize();
    const int total = static_cast<int> (seconds * host.getSampleRate());
    std::vector<float> left (static_cast<size_t> (block)), right (static_cast<size_t> (block)), tail;
    float* outs[] = { left.data(), right.data() };
    constexpr float kAmplitude = 0.05f;
    SineSource source (strip, freq, host.getSampleRate(), kAmplitude);
    for (int done = 0; done < total; done += block)
    {
        host.renderOffline (source, block, outs, 2);
        if (done >= total / 2)
            tail.insert (tail.end(), left.begin(), left.end());
    }
    double re = 0.0, im = 0.0;
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / host.getSampleRate();
    for (size_t i = 0; i < tail.size(); ++i)
    {
        re += tail[i] * std::cos (w * static_cast<double> (i));
        im -= tail[i] * std::sin (w * static_cast<double> (i));
    }
    const double amplitude = 2.0 * std::sqrt (re * re + im * im) / static_cast<double> (tail.size());
    return 20.0 * std::log10 (std::max (1.0e-12, amplitude / kAmplitude));
}

template <typename T>
T* findChild (juce::Component& root, const std::function<bool (T&)>& match)
{
    for (auto* child : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*> (child); t != nullptr && match (*t))
            return t;
        if (auto* found = findChild<T> (*child, match))
            return found;
    }
    return nullptr;
}
} // namespace

TEST_CASE ("App: device correction (E15): an imported ParametricEQ.txt belongs to its output endpoint and survives presets, A/B and restarts")
{
    const flubapptest::TempFolder temp;
    const auto curveFile = temp.file ("Test Headset ParametricEQ.txt");
    REQUIRE (curveFile.replaceWithText (kSyntheticCurve));

    {
        Foreground foreground;
        EngineController controller (headlessOptions (temp, true, &foreground));
        auto& host = controller.getHost();

        // No output open: nothing to attach it to.
        juce::String error;
        CHECK (! controller.importDeviceCorrection (curveFile, error));
        CHECK (error.contains ("No output device"));
        CHECK (host.getDeviceCorrection().curve.isEmpty());

        controller.simulateOutputDevice (kHeadset, 48000.0, 2);
        CHECK (! controller.getDeviceCorrection().hasCurve);
        juce::StringArray warnings;
        REQUIRE (controller.importDeviceCorrection (curveFile, error, &warnings));
        CHECK (warnings.isEmpty());
        const auto info = controller.getDeviceCorrection();
        CHECK (info.hasCurve && info.enabled && ! info.comparing);
        CHECK (info.endpoint == kHeadset);
        CHECK (info.name == "Test Headset ParametricEQ.txt");
        CHECK (info.numFilters == 10);
        CHECK (info.preampDb <= 0.0f);
        const auto applied = host.getDeviceCorrection();
        CHECK (applied.curve.numFilters == 10);
        CHECK (host.getMixEngine().getDeviceCorrection().getSettings() == applied);

        // Presets, A/B and the other strips' state never touch it.
        const auto strip = controller.findStrip ("Music");
        REQUIRE (strip >= 0);
        auto& presets = controller.getPresetManager();
        REQUIRE (! presets.getPresets().empty());
        for (int i = 0; i < 3; ++i)
            REQUIRE (controller.loadPreset (presets.getPresets()[static_cast<size_t> (i)], strip, error));
        controller.toggleAB (strip);
        controller.copyActiveToOtherBank (strip);
        controller.setActiveBank (Bank::A, strip);
        controller.setEnabled (false);
        controller.setEnabled (true);
        CHECK (host.getDeviceCorrection() == applied);

        // ... nor does an automatic profile, applied and restored.
        AutoProfileRule rule;
        rule.executable = "game.exe";
        rule.stripName = "Game";
        rule.presetId = presets.getPresets().back().id;
        rule.restoreOnExit = true;
        REQUIRE (controller.addAutoProfileRule (rule));
        foreground.executable = "game.exe";
        for (int i = 0; i < 3; ++i)
            controller.pollForegroundApp();
        CHECK (controller.getActiveAutoProfile() != nullptr);
        CHECK (host.getDeviceCorrection() == applied);
        foreground.executable = "explorer.exe";
        for (int i = 0; i < 3; ++i)
            controller.pollForegroundApp();
        CHECK (controller.getActiveAutoProfile() == nullptr);
        CHECK (host.getDeviceCorrection() == applied);

        // A structural rebuild (strip layout) keeps it on the new engine.
        controller.setStripLayout (AudioEngineHost::defaultStripLayout());
        CHECK (host.getMixEngine().getDeviceCorrection().getSettings() == applied);

        // Another endpoint has none: flat. The headset returns: its curve returns.
        controller.simulateOutputDevice (kSpeakers, 48000.0, 2);
        CHECK (! controller.getDeviceCorrection().hasCurve);
        CHECK (host.getDeviceCorrection().curve.isEmpty());
        CHECK (host.getMixEngine().getDeviceCorrection().getPreampDb() == 0.0f);
        controller.simulateOutputDevice (kHeadset, 48000.0, 2);
        CHECK (host.getDeviceCorrection() == applied);

        // A refused file changes nothing.
        CHECK (! controller.importDeviceCorrectionText ("GraphicEQ: 20 0; 20000 0", "graphic.txt", error));
        CHECK (error.contains ("GraphicEQ"));
        CHECK (host.getDeviceCorrection() == applied);
        CHECK (controller.getDeviceCorrection().name == "Test Headset ParametricEQ.txt");
        controller.shutdown();
    }

    // Restart: the stored curve is back for the headset, and only for it.
    EngineController restarted (headlessOptions (temp, true));
    CHECK (restarted.getHost().getDeviceCorrection().curve.isEmpty());
    restarted.simulateOutputDevice (kHeadset, 48000.0, 2);
    CHECK (restarted.getDeviceCorrection().hasCurve);
    CHECK (restarted.getHost().getDeviceCorrection().curve.numFilters == 10);
    CHECK (restarted.getSettings().getDeviceCorrections().size() == 1);
}

TEST_CASE ("App: device correction (E15): off, compare and remove; rendered through the engine before the limiter")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    controller.simulateOutputDevice (kHeadset, 48000.0, 2);
    controller.setEnabled (false); // strips pass their input: the output is the correction's
    const int music = controller.findStrip ("Music");
    REQUIRE (music >= 0);

    const double flat = renderedGainDb (controller, music, 200.0);
    std::cout << "    flat 200 Hz: " << flat << " dB\n";

    // A +6 dB bell at 3 kHz: its automatic preamp takes 6 dB off everything else.
    juce::String error;
    REQUIRE (controller.importDeviceCorrectionText ("Filter: ON PK Fc 3000 Hz Gain 6 dB Q 1\n", "bell.txt", error));
    CHECK_NEAR (controller.getDeviceCorrection().preampDb, -6.0, 0.01);
    CHECK_NEAR (controller.getDeviceCorrection().maxBoostDb, 6.0, 0.01);
    CHECK_NEAR (controller.getDeviceCorrection().maxBoostHz, 3000.0, 30.0);
    const double corrected = renderedGainDb (controller, music, 200.0);
    std::cout << "    with the bell's preamp 200 Hz: " << corrected << " dB\n";
    CHECK_NEAR (corrected - flat, controller.getHost().getDeviceCorrection().curve.responseDb (0, 200.0, 48000.0) - 6.0, 0.05);

    // Compare: filters off, the broadband gain kept (level-fair A/B): flat at -6 dB.
    controller.setDeviceCorrectionCompare (true);
    CHECK (controller.getDeviceCorrection().comparing);
    CHECK_NEAR (renderedGainDb (controller, music, 3000.0) - flat, -6.0, 0.05);
    controller.setDeviceCorrectionCompare (false);
    CHECK_NEAR (renderedGainDb (controller, music, 3000.0) - flat, 0.0, 0.05); // +6 bell -6 preamp

    // Off: flat again (persisted); on again; remove forgets it.
    controller.setDeviceCorrectionEnabled (false);
    CHECK (! controller.getDeviceCorrection().enabled);
    CHECK (! controller.getSettings().getDeviceCorrection (kHeadset)->enabled);
    CHECK_NEAR (renderedGainDb (controller, music, 200.0) - flat, 0.0, 0.05);
    controller.setDeviceCorrectionEnabled (true);
    CHECK_NEAR (controller.getDeviceCorrection().preampDb, -6.0, 0.01);
    controller.removeDeviceCorrection();
    CHECK (! controller.getDeviceCorrection().hasCurve);
    CHECK (! controller.getSettings().getDeviceCorrection (kHeadset).has_value());
    CHECK (controller.getHost().getDeviceCorrection().curve.isEmpty());
}

TEST_CASE ("App: Settings > Correction shows the output's curve and drives compare / on / remove")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};

    EngineController::DeviceCorrectionInfo none;
    CHECK (ui::SettingsDialog::describeDeviceCorrection (none) == "Open an output device to import a correction for it.");

    controller.simulateOutputDevice (kHeadset, 48000.0, 2);
    juce::String error;
    REQUIRE (controller.importDeviceCorrectionText ("Filter: ON PK Fc 3000 Hz Gain 6 dB Q 1\nFilter: ON PK Fc 100 Hz Gain -3 dB Q 1\n",
                                                    "bell.txt", error));
    CHECK (ui::SettingsDialog::describeDeviceCorrection (controller.getDeviceCorrection())
           == "bell.txt  -  2 filters  -  preamp -6.0 dB (max boost +6.0 dB at 3.0 kHz)");

    ui::SettingsDialog settings (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    settings.showPage (ui::SettingsDialog::Page::Correction);

    auto* compare = findChild<juce::TextButton> (settings, [] (juce::TextButton& b) { return b.getButtonText() == "Compare"; });
    auto* remove = findChild<juce::TextButton> (settings, [] (juce::TextButton& b) { return b.getButtonText() == "Remove"; });
    auto* onOff = findChild<juce::ToggleButton> (settings, [] (juce::ToggleButton& b) { return b.getButtonText() == "Correction on for this output"; });
    REQUIRE (compare != nullptr);
    REQUIRE (remove != nullptr);
    REQUIRE (onOff != nullptr);
    CHECK (compare->getParentComponent()->isVisible());
    CHECK (compare->isEnabled() && remove->isEnabled() && onOff->getToggleState());
    CHECK (onOff->getBottom() <= onOff->getParentComponent()->getHeight()); // fits the smallest dialog

    compare->triggerClick();
    flubapptest::pumpMessagesUntil ([&] { return controller.getDeviceCorrection().comparing; }, 2000);
    CHECK (controller.getDeviceCorrection().comparing);
    CHECK (ui::SettingsDialog::describeDeviceCorrection (controller.getDeviceCorrection()).endsWith ("comparing (filters off)"));
    compare->triggerClick();
    flubapptest::pumpMessagesUntil ([&] { return ! controller.getDeviceCorrection().comparing; }, 2000);

    onOff->triggerClick();
    flubapptest::pumpMessagesUntil ([&] { return ! controller.getDeviceCorrection().enabled; }, 2000);
    CHECK (! controller.getDeviceCorrection().enabled);
    CHECK (ui::SettingsDialog::describeDeviceCorrection (controller.getDeviceCorrection()) == "bell.txt  -  2 filters  -  off");
    CHECK (! compare->isEnabled());

    remove->triggerClick();
    flubapptest::pumpMessagesUntil ([&] { return ! controller.getDeviceCorrection().hasCurve; }, 2000);
    CHECK (! controller.getDeviceCorrection().hasCurve);
    CHECK (ui::SettingsDialog::describeDeviceCorrection (controller.getDeviceCorrection()) == "No correction for this output.");
    CHECK (! remove->isEnabled());
}
