// App-level test: the experimental "Neural voice cleanup" switch (docs/03 §16,
// Settings > Processing > Voice chat). Off by default; on, the Chat strip's
// neural slot runs the voice cleanup model with safety frames from the device
// buffer (960 samples at 48 kHz / 480), only the Chat strip's latency grows, no
// parameter changes, the setting persists, and the Settings page's switch and
// status line follow it. The device is a fake (no audio thread is driven: the
// newest engine carries the model as soon as the switch is applied).
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "ui/SettingsDialog.h"

#include "flub/engine/Parameters.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>
#include <vector>

using namespace flub::app;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 480;

EngineController::Options options (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = true;
    return o;
}

class FakeDevice final : public juce::AudioIODevice
{
public:
    explicit FakeDevice (double rate) : juce::AudioIODevice ("Fake Neural Device", "Fake"), sampleRate (rate) {}

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
    juce::Array<double> getAvailableSampleRates() override { return { sampleRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger (0x3); }
    juce::BigInteger getActiveInputChannels() const override { return juce::BigInteger (0x3); }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }

private:
    double sampleRate;
};

template <typename T>
T* findChild (juce::Component& root, const std::function<bool (T&)>& match)
{
    for (auto* child : root.getChildren())
    {
        if (auto* c = dynamic_cast<T*> (child); c != nullptr && match (*c))
            return c;
        if (auto* found = findChild<T> (*child, match))
            return found;
    }
    return nullptr;
}

std::vector<float> snapshot (flub::param::ParameterStore& store)
{
    std::vector<float> v (static_cast<size_t> (flub::param::kNumParams));
    store.snapshot (v.data());
    return v;
}
} // namespace

TEST_CASE ("App: Neural voice cleanup (experimental) - off by default; on, the Chat strip runs the model (+960 samples at 48 kHz / 480), nothing else changes, it persists")
{
    const flubapptest::TempFolder temp;
    FakeDevice device (kRate);
    {
        EngineController controller (options (temp));
        auto& host = controller.getHost();
        host.audioDeviceAboutToStart (&device); // message thread: configures synchronously
        const int chat = controller.findStrip ("Chat");
        const int game = controller.findStrip ("Game");
        REQUIRE (chat >= 0);
        REQUIRE (game >= 0);
        CHECK (! controller.getChatNeuralCleanup());
        CHECK (host.getMixEngine().chain (chat).getNeuralStatus().state == flub::NeuralSlotState::Empty);
        CHECK (controller.describeChatNeuralCleanup() == "Off.");
        const int chatBefore = host.getMixEngine().getStripLatencySamples (chat);
        const int gameBefore = host.getMixEngine().getStripLatencySamples (game);
        const auto paramsBefore = snapshot (controller.getParams (chat));

        controller.setChatNeuralCleanup (true);
        CHECK (controller.getChatNeuralCleanup());
        CHECK (host.hasNeuralModel (chat));
        CHECK (! host.hasNeuralModel (game));
        const auto status = controller.getChatNeuralCleanupStatus();
        CHECK (status.state == flub::NeuralSlotState::Active); // Balanced is the default profile
        CHECK (status.latencySamples == 960);
        CHECK (host.getMixEngine().getStripLatencySamples (chat) == chatBefore + 960);
        CHECK (host.getMixEngine().getStripLatencySamples (game) == gameBefore); // strips in no sync group are not padded
        CHECK (snapshot (controller.getParams (chat)) == paramsBefore);         // no parameter, preset or bank change
        CHECK (controller.describeChatNeuralCleanup().startsWith ("On: 20 ms added to the Chat strip"));

        // Low Latency's budget (one 10 ms frame) leaves it out, with the reason.
        controller.setLatencyProfile (flub::param::LatencyProfileValue::LowLatency);
        host.reconfigure();
        CHECK (controller.getChatNeuralCleanupStatus().state == flub::NeuralSlotState::Ineligible);
        CHECK (controller.describeChatNeuralCleanup().contains ("Balanced or Quality"));
        controller.setLatencyProfile (flub::param::LatencyProfileValue::Balanced);
        host.reconfigure();
        CHECK (controller.getChatNeuralCleanupStatus().state == flub::NeuralSlotState::Active);

        // The Settings page's switch shows it and turns it off.
        ui::HotkeyHooks hooks;
        hooks.isSupported = [] { return false; };
        hooks.getFailures = [] { return juce::StringArray(); };
        hooks.reRegister = [] {};
        ui::SettingsDialog settings (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
        settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
        settings.showPage (ui::SettingsDialog::Page::Processing);
        auto* toggle = findChild<juce::ToggleButton> (settings, [] (juce::ToggleButton& b) { return b.getTitle() == "Neural voice cleanup (experimental)"; });
        REQUIRE (toggle != nullptr);
        CHECK (toggle->getToggleState());
        toggle->triggerClick();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return ! controller.getChatNeuralCleanup(); }, 2000));
        CHECK (! host.hasNeuralModel (chat));
        CHECK (host.getMixEngine().chain (chat).getNeuralStatus().state == flub::NeuralSlotState::Empty);
        CHECK (host.getMixEngine().getStripLatencySamples (chat) == chatBefore);
        toggle->triggerClick();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getChatNeuralCleanup(); }, 2000));
        controller.getSettings().save();
        host.audioDeviceStopped();
    }

    // A restarted app has it on; at 44.1 kHz it stays out of the chain and says why.
    EngineController controller (options (temp));
    CHECK (controller.getChatNeuralCleanup());
    FakeDevice slow (44100.0);
    controller.getHost().audioDeviceAboutToStart (&slow);
    const int chat = controller.findStrip ("Chat");
    CHECK (controller.getHost().hasNeuralModel (chat));
    CHECK (controller.getChatNeuralCleanupStatus().state == flub::NeuralSlotState::SampleRateMismatch);
    CHECK (controller.describeChatNeuralCleanup().contains ("48 kHz"));
    controller.getHost().audioDeviceStopped();
}
