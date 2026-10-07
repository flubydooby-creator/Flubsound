// Flubsound Pro - the device types the app offers and what Settings > Audio
// says about them (R1.2; docs/08 D10 and D2, docs/06 §6.11).
//
// * The note above the device selector: ASIO (single-client drivers) and
//   WASAPI exclusive mode give the output to Flubsound alone, the native
//   PipeWire node links itself; shared types have none. A stand-in "ASIO"
//   type is installed when this build has no ASIO (the default: the
//   Steinberg SDK is not in the repository).
// * ASIO is offered exactly when the build has the SDK (-DFLUB_ASIO=ON);
//   CI's `asio` job builds with the SDK and runs these cases.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "platform/pipewire/PipeWireDeviceType.h"
#include "ui/SettingsDialog.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <iostream>
#include <utility>

using namespace flub::app;
using namespace flubtest;

namespace
{
/** A silent device of a NamedDeviceType. */
class NamedDevice final : public juce::AudioIODevice
{
public:
    explicit NamedDevice (const juce::String& ofType) : juce::AudioIODevice ("Stand-in device", ofType) {}

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 256 }; }
    int getDefaultBufferSize() override { return 256; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger& outputs, double, int) override
    {
        activeOutputs = outputs;
        opened = true;
        return {};
    }
    void close() override { opened = false; }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* callback) override
    {
        if (callback != nullptr)
            callback->audioDeviceAboutToStart (this);
        startedWith = callback;
    }
    void stop() override
    {
        if (auto* callback = std::exchange (startedWith, nullptr))
            callback->audioDeviceStopped();
    }
    bool isPlaying() override { return startedWith != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return 256; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

    /** The callback the device manager started this device with. */
    juce::AudioIODeviceCallback* startedWith = nullptr;

private:
    juce::BigInteger activeOutputs;
    bool opened = false;
};

/** Counts the device errors a device manager hands its callbacks. */
struct ErrorListener final : juce::AudioIODeviceCallback
{
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOutputs; ++c)
            if (outputs[c] != nullptr)
                std::fill (outputs[c], outputs[c] + numSamples, 0.0f);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
    void audioDeviceError (const juce::String& message) override { errors.add (message); }

    juce::StringArray errors;
};

/** A device type of any name with one device: stands in for a backend this
    build or machine lacks. */
class NamedDeviceType final : public juce::AudioIODeviceType
{
public:
    explicit NamedDeviceType (const juce::String& name) : juce::AudioIODeviceType (name) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames) const override { return wantInputNames ? juce::StringArray() : juce::StringArray ("Stand-in device"); }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override { return device != nullptr && ! asInput ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice (const juce::String&, const juce::String&) override { return new NamedDevice (getTypeName()); }
};

bool hasType (juce::AudioDeviceManager& manager, const juce::String& name)
{
    for (auto* type : manager.getAvailableDeviceTypes()) // creates JUCE's own on the first call
        if (type->getTypeName() == name)
            return true;
    return false;
}

EngineController::Options headless (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    return o;
}

ui::HotkeyHooks noHotkeys()
{
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    return hooks;
}

} // namespace

TEST_CASE ("App: Settings > Audio notes what the device type means for other apps: ASIO and exclusive mode serve Flubsound alone, PipeWire links itself (R1.2, docs/08 D10)")
{
    using ui::SettingsDialog;
    const auto asio = SettingsDialog::describeDeviceTypeNote ("ASIO");
    CHECK (asio.contains ("one application at a time"));
    CHECK (asio.contains ("Flubsound's virtual devices"));
    CHECK (SettingsDialog::describeDeviceTypeNote ("Windows Audio (Exclusive Mode)").contains ("to Flubsound alone"));
    CHECK (SettingsDialog::describeDeviceTypeNote (flub::platform::pipewire::kDeviceTypeName).contains ("linking everything itself"));
    for (const auto* shared : { "Windows Audio", "Windows Audio (Low Latency Mode)", "CoreAudio", "ALSA", "JACK", "" })
        CHECK (SettingsDialog::describeDeviceTypeNote (shared).isEmpty());

    // The page shows the current type's note, and none for a shared type.
    // Stand-in types only: the test never opens a real driver (an ASIO
    // build's own ASIO type could take over the machine's interface).
    const flubapptest::TempFolder temp;
    EngineController controller (headless (temp));
    auto& manager = controller.getDeviceManager();
    const bool realAsio = hasType (manager, "ASIO"); // JUCE's own types are created here
    if (! realAsio)                                  // a build without the SDK (the default)
        manager.addAudioDeviceType (std::make_unique<NamedDeviceType> ("ASIO"));
    if (! hasType (manager, flub::platform::pipewire::kDeviceTypeName)) // only openDevice() adds the real one
        manager.addAudioDeviceType (std::make_unique<NamedDeviceType> (flub::platform::pipewire::kDeviceTypeName));
    manager.addAudioDeviceType (std::make_unique<NamedDeviceType> ("Stand-in shared type"));

    ui::SettingsDialog settings (controller, noHotkeys(), [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
    settings.showPage (ui::SettingsDialog::Page::Audio);
    const auto showsNoteFor = [&] (const juce::String& type)
    {
        // Nothing open while switching: JUCE pauses 1.5 s when it switches away from an open device.
        manager.closeAudioDevice();
        manager.setCurrentAudioDeviceType (type, true);
        CHECK (manager.getCurrentAudioDeviceType() == type);
        const auto shown = settings.getAudioDeviceTypeNote();
        manager.closeAudioDevice();
        return shown;
    };
    if (! realAsio)
    {
        CHECK (showsNoteFor ("ASIO") == asio);
        CHECK (settings.createComponentSnapshot (settings.getLocalBounds(), true, 1.0f).isValid()); // the note's box paints
    }
    else
    {
        std::cout << "    (ASIO is built in: the page is checked with the stand-in types; no real ASIO driver is opened)\n";
    }
    CHECK (showsNoteFor (flub::platform::pipewire::kDeviceTypeName) == SettingsDialog::describeDeviceTypeNote ("PipeWire"));
    CHECK (showsNoteFor ("Stand-in shared type").isEmpty());
}

TEST_CASE ("App: JUCE 9.0.2's AudioDeviceManager drops a device's audioDeviceError - why the PipeWire device reports to the host directly (R1.2)")
{
    // The manager starts a device with a wrapper (CallbackMaxSizeEnforcer)
    // that forwards the audio, start and stop but not audioDeviceError, so
    // an error a device reports to the callback it was started with never
    // reaches the manager's callbacks (AudioEngineHost's device-error banner
    // and E51 recovery). pipewire::setDeviceErrorTarget works around it. If
    // a JUCE update forwards the error, this fails: then the PipeWire device
    // may report through its callback again, and every backend's errors
    // start the recovery.
    juce::AudioDeviceManager manager;
    manager.addAudioDeviceType (std::make_unique<NamedDeviceType> ("Stand-in error type"));
    manager.setCurrentAudioDeviceType ("Stand-in error type", true);
    ErrorListener listener;
    manager.addAudioCallback (&listener);
    auto* device = dynamic_cast<NamedDevice*> (manager.getCurrentAudioDevice());
    REQUIRE (device != nullptr);
    REQUIRE (device->startedWith != nullptr);
    device->startedWith->audioDeviceError ("the device went away");
    CHECK (listener.errors.isEmpty()); // dropped by JUCE 9.0.2
    manager.removeAudioCallback (&listener);
    manager.closeAudioDevice();
}

TEST_CASE ("App: the ASIO device type is offered exactly when the build has the Steinberg SDK (FLUB_ASIO, R1.2)")
{
    juce::AudioDeviceManager manager;
    const bool offered = hasType (manager, "ASIO");
#if JUCE_WINDOWS && JUCE_ASIO
    CHECK (offered); // -DFLUB_ASIO=ON: JUCE's ASIO type (it lists the installed drivers; none on CI)
    std::cout << "    ASIO built in; drivers here: " << manager.getAvailableDeviceTypes().size() << " types, ASIO lists "
              << [&manager]
                 {
                     for (auto* type : manager.getAvailableDeviceTypes())
                         if (type->getTypeName() == "ASIO")
                         {
                             type->scanForDevices();
                             return type->getDeviceNames (false).size();
                         }
                     return -1;
                 }()
              << " driver(s)\n";
#else
    CHECK (! offered); // the default build: no SDK, no ASIO type
#endif
}
