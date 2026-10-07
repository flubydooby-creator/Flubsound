// App-level tests: the experimental "Neural voice cleanup" switch (docs/03 §16,
// Settings > Processing > Voice chat). Off by default; on, the Chat strip's
// neural slot runs the voice cleanup model with safety frames that cover one
// device buffer (960 samples at 48 kHz / 480), only the Chat strip's latency
// grows, no parameter changes, the setting persists, and the Settings page's
// switch and status line follow it. The safety frames are resolved by the host
// for every engine it builds (the first engine after a device start is right),
// the status names what to change when the profile does not allow the model,
// and a layout change carries the model with the Chat strip in one engine
// build. The devices are fakes (no audio thread is driven: the newest engine
// carries the model as soon as the switch is applied).
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "ui/SettingsDialog.h"

#include "flub/engine/Parameters.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>
#include <iostream>
#include <utility>
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
    explicit FakeDevice (double rate, int blockSize = kBlock)
        : juce::AudioIODevice ("Fake Neural Device", "Fake"), sampleRate (rate), block (blockSize)
    {
    }

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
    juce::Array<double> getAvailableSampleRates() override { return { sampleRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { block }; }
    int getDefaultBufferSize() override { return block; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return block; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger (0x3); }
    juce::BigInteger getActiveInputChannels() const override { return juce::BigInteger (0x3); }
    int getOutputLatencyInSamples() override { return block; }
    int getInputLatencyInSamples() override { return block; }

private:
    double sampleRate;
    int block;
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
        CHECK (status.blockSize == kBlock);
        CHECK (EngineController::chatNeuralLatencyForBuffer (kBlock) == 960);
        CHECK (host.getMixEngine().getStripLatencySamples (chat) == chatBefore + 960);
        CHECK (host.getMixEngine().getStripLatencySamples (game) == gameBefore); // strips in no sync group are not padded
        CHECK (snapshot (controller.getParams (chat)) == paramsBefore);         // no parameter, preset or bank change
        CHECK (controller.describeChatNeuralCleanup().startsWith ("On: 20 ms added to the Chat strip"));

        // Low Latency's budget (one 10 ms frame) leaves it out, with the reason and the fix.
        controller.setLatencyProfile (flub::param::LatencyProfileValue::LowLatency);
        host.reconfigure();
        CHECK (controller.getChatNeuralCleanupStatus().state == flub::NeuralSlotState::Ineligible);
        const auto lowText = controller.describeChatNeuralCleanup();
        std::cerr << "    Low Latency, 480: " << lowText << "\n";
        CHECK (lowText.contains ("Low Latency allows at most 10 ms"));
        CHECK (lowText.contains ("the model needs 20 ms"));
        CHECK (lowText.contains ("Choose Balanced or Quality"));
        controller.setLatencyProfile (flub::param::LatencyProfileValue::Balanced);
        host.reconfigure();
        CHECK (controller.getChatNeuralCleanupStatus().state == flub::NeuralSlotState::Active);

        // The Settings page's switch shows it and turns it off; the live status is a
        // fixed-height line of its own, so its changing text never moves the rows below.
        ui::HotkeyHooks hooks;
        hooks.isSupported = [] { return false; };
        hooks.getFailures = [] { return juce::StringArray(); };
        hooks.reRegister = [] {};
        ui::SettingsDialog settings (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
        settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
        settings.showPage (ui::SettingsDialog::Page::Processing);
        auto* toggle = findChild<juce::ToggleButton> (settings, [] (juce::ToggleButton& b) { return b.getTitle() == "Neural voice cleanup (experimental)"; });
        auto* statusLine = findChild<juce::Label> (settings, [] (juce::Label& l) { return l.getTitle() == "Neural voice cleanup status"; });
        auto* palette = findChild<juce::ComboBox> (settings, [] (juce::ComboBox& b) { return b.getTitle() == "Meter colours"; });
        REQUIRE (toggle != nullptr);
        REQUIRE (statusLine != nullptr);
        REQUIRE (palette != nullptr);
        CHECK (toggle->getToggleState());
        CHECK (statusLine->getText().startsWith ("Status: On: 20 ms added to the Chat strip"));
        const auto paletteOn = palette->getBounds();
        const auto statusOn = statusLine->getBounds();
        toggle->triggerClick();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return ! controller.getChatNeuralCleanup(); }, 2000));
        CHECK (! host.hasNeuralModel (chat));
        CHECK (host.getMixEngine().chain (chat).getNeuralStatus().state == flub::NeuralSlotState::Empty);
        CHECK (host.getMixEngine().getStripLatencySamples (chat) == chatBefore);
        CHECK (statusLine->getText() == "Status: Off.");
        CHECK (palette->getBounds() == paletteOn); // "Off." and the long "On: ..." line: the same layout
        CHECK (statusLine->getBounds() == statusOn);
        toggle->triggerClick();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getChatNeuralCleanup(); }, 2000));
        CHECK (palette->getBounds() == paletteOn);
        controller.getSettings().save();
        host.audioDeviceStopped();
    }

    // A restarted app has it on; at 44.1 kHz it stays out of the chain and says why.
    EngineController controller (options (temp));
    CHECK (controller.getChatNeuralCleanup());
    FakeDevice slow (44100.0, 441);
    controller.getHost().audioDeviceAboutToStart (&slow);
    const int chat = controller.findStrip ("Chat");
    CHECK (controller.getHost().hasNeuralModel (chat));
    CHECK (controller.getChatNeuralCleanupStatus().state == flub::NeuralSlotState::SampleRateMismatch);
    CHECK (controller.describeChatNeuralCleanup().contains ("48 kHz"));
    controller.getHost().audioDeviceStopped();
}

TEST_CASE ("App: Neural voice cleanup - the first engine after a device start has the buffer's safety frames (480: 960 samples; 512: 1200, Quality only), and the status names the fix")
{
    const flubapptest::TempFolder temp;
    {
        EngineController controller (options (temp));
        controller.setChatNeuralCleanup (true); // before any device: built for the host's nominal 512-sample buffer
        controller.getSettings().save();
    }

    // Persisted on: the controller installs it before the device starts. The engine
    // the device start builds already has the 480-sample buffer's two safety frames
    // (it used to start Ineligible at 1 200 samples and be swapped again 0.5 s later).
    EngineController controller (options (temp));
    auto& host = controller.getHost();
    const int chat = controller.findStrip ("Chat");
    REQUIRE (chat >= 0);
    FakeDevice device (kRate, 480);
    const auto builds = host.getStructureGeneration();
    host.audioDeviceAboutToStart (&device);
    auto st = controller.getChatNeuralCleanupStatus();
    CHECK (host.getStructureGeneration() - builds == 1u); // the device start's own engine, nothing more
    CHECK (st.state == flub::NeuralSlotState::Active);
    CHECK (st.latencySamples == 960);
    host.audioDeviceStopped();

    // 512-sample buffers: three safety frames, 1 200 samples (25 ms), more than
    // Balanced's 20 ms. The status says so and names Quality or a 480-sample buffer.
    FakeDevice longer (kRate, 512);
    host.audioDeviceAboutToStart (&longer);
    st = controller.getChatNeuralCleanupStatus();
    CHECK (st.state == flub::NeuralSlotState::Ineligible);
    CHECK (st.latencySamples == 1200);
    CHECK (st.blockSize == 512);
    CHECK (EngineController::chatNeuralLatencyForBuffer (512) == 1200);
    const auto balancedText = controller.describeChatNeuralCleanup();
    std::cerr << "    Balanced, 512: " << balancedText << "\n";
    CHECK (balancedText.contains ("Balanced allows at most 20 ms"));
    CHECK (balancedText.contains ("with this 512-sample buffer the model needs 25 ms"));
    CHECK (balancedText.contains ("Choose Quality"));
    CHECK (balancedText.contains ("a buffer of 480 samples or less"));
    CHECK (! balancedText.contains ("Choose Balanced"));

    controller.setLatencyProfile (flub::param::LatencyProfileValue::LowLatency);
    host.reconfigure();
    const auto lowText = controller.describeChatNeuralCleanup();
    std::cerr << "    Low Latency, 512: " << lowText << "\n";
    CHECK (lowText.contains ("Low Latency allows at most 10 ms"));
    CHECK (lowText.contains ("Choose Quality"));
    CHECK (lowText.contains ("or Balanced with a buffer of 480 samples or less"));

    controller.setLatencyProfile (flub::param::LatencyProfileValue::Quality);
    host.reconfigure();
    st = controller.getChatNeuralCleanupStatus();
    CHECK (st.state == flub::NeuralSlotState::Active);
    CHECK (st.latencySamples == 1200);
    CHECK (controller.describeChatNeuralCleanup().startsWith ("On: 25 ms added to the Chat strip"));
    controller.setLatencyProfile (flub::param::LatencyProfileValue::Balanced);
    host.audioDeviceStopped();
}

TEST_CASE ("App: Neural voice cleanup - a layout change carries the model with the Chat strip in the layout's own engine build")
{
    const flubapptest::TempFolder temp;
    EngineController controller (options (temp));
    auto& host = controller.getHost();
    FakeDevice device (kRate);
    host.audioDeviceAboutToStart (&device);
    const auto original = host.getStripLayout();
    REQUIRE (original.size() >= 3);
    const int chat = controller.findStrip ("Chat");
    REQUIRE (chat >= 1);
    auto moved = original; // the Chat strip one place up: its old index belongs to another strip
    std::swap (moved[static_cast<size_t> (chat)], moved[static_cast<size_t> (chat - 1)]);
    const std::string displaced = moved[static_cast<size_t> (chat)].name;

    auto builds = host.getStructureGeneration();
    controller.setStripLayout (moved);
    const auto buildsOff = host.getStructureGeneration() - builds;
    controller.setStripLayout (original);

    controller.setChatNeuralCleanup (true);
    REQUIRE (host.hasNeuralModel (chat));
    builds = host.getStructureGeneration();
    controller.setStripLayout (moved);
    const auto buildsOn = host.getStructureGeneration() - builds;
    std::cerr << "    layout change: " << buildsOff << " engine build(s) with the switch off, " << buildsOn << " with it on\n";
    CHECK (buildsOn == buildsOff); // no extra build: no engine ran the model on the displaced strip
    const int newChat = controller.findStrip ("Chat");
    CHECK (newChat == chat - 1);
    CHECK (host.hasNeuralModel (newChat));
    CHECK (! host.hasNeuralModel (chat)); // now the displaced strip
    CHECK (host.getMixEngine().chain (newChat).getNeuralStatus().state == flub::NeuralSlotState::Active);
    CHECK (host.getMixEngine().chain (chat).getNeuralStatus().state == flub::NeuralSlotState::Empty);
    CHECK (controller.findStrip (displaced) == chat);
    CHECK (controller.getChatNeuralCleanupStatus().state == flub::NeuralSlotState::Active);

    // A layout without a Chat strip drops the model; it comes back with the strip.
    auto noChat = original;
    noChat.erase (noChat.begin() + chat);
    controller.setStripLayout (noChat);
    for (int i = 0; i < static_cast<int> (noChat.size()); ++i)
        CHECK (! host.hasNeuralModel (i));
    CHECK (controller.describeChatNeuralCleanup().contains ("no Chat strip"));
    controller.setStripLayout (original);
    CHECK (host.hasNeuralModel (chat));
    CHECK (controller.getChatNeuralCleanupStatus().state == flub::NeuralSlotState::Active);
    host.audioDeviceStopped();
}
