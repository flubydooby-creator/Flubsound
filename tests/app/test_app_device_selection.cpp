// App-level tests: docs/11 E51 device selection and recovery in
// AudioEngineHost (DEVICE SELECTION AND RECOVERY).
//   * Endpoint identities: Windows' "2- " instance number, JUCE's " (2)",
//     vendor / product ids from adapter device paths, identity matching.
//   * selectOutput(): chosen, recognised after a re-plug into another USB
//     port or a rename, the default unless virtual or looping, the first safe
//     output, none; the safe speaker profile only on an unplanned fallback to
//     something that is not headphones.
//   * Through a fake JUCE device type and a fake platform::AudioDeviceWatcher:
//     explicit selection at start (the saved headset missing, the default
//     CABLE Input), hot-plug (the Turtle Beach dongle comes back on another
//     port under a new name and id), following the default when nothing was
//     chosen, sleep / resume, an output busy in exclusive mode retried with
//     backoff, the identity saved with the device state, and the safe
//     speaker profile's trim on the audio thread (no allocation, no lock).
#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"

#include "flub/engine/Parameters.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace flub::app;
using flub::platform::AudioDeviceEvent;
using flub::platform::AudioDeviceWatcher;
using flub::platform::EndpointFormFactor;
using flub::platform::EndpointTransport;
using flub::platform::OutputEndpointIdentity;

namespace
{
constexpr int kBlock = 256;

const juce::String kHeadset ("Headset Earphone (Stealth 700 Gen 2)");
const juce::String kHeadsetPort2 ("Headset Earphone (2- Stealth 700 Gen 2)");
const juce::String kSpeakers ("Speakers (Realtek(R) Audio)");
const juce::String kHeadphonesJack ("Headphones (Realtek(R) Audio)");
const juce::String kCableIn ("CABLE Input (VB-Audio Virtual Cable)");
const juce::String kCableOut ("CABLE Output (VB-Audio Virtual Cable)");
const std::string kStealthHw ("USB\\VID_10F5&PID_0210&MI_00");

OutputEndpointIdentity endpoint (const juce::String& name, const std::string& id, const std::string& hardwareId = {},
                                 EndpointFormFactor form = EndpointFormFactor::Unknown,
                                 EndpointTransport transport = EndpointTransport::Unknown, bool isDefault = false)
{
    OutputEndpointIdentity e;
    e.id = id;
    e.name = name.toStdString();
    e.hardwareId = hardwareId;
    e.formFactor = form;
    e.transport = transport;
    e.isDefault = isDefault;
    return e;
}

OutputEndpointIdentity stealth (const juce::String& name, const std::string& id)
{
    return endpoint (name, id, kStealthHw, EndpointFormFactor::Headset, EndpointTransport::Usb);
}
OutputEndpointIdentity speakers() { return endpoint (kSpeakers, "{0.0.0.00000000}.{spk}", "HDAUDIO\\FUNC_01&VEN_10EC", EndpointFormFactor::Speakers, EndpointTransport::Analog); }
OutputEndpointIdentity cableIn() { return endpoint (kCableIn, "{0.0.0.00000000}.{cable}", {}, EndpointFormFactor::Speakers, EndpointTransport::Virtual); }

// ---- Fakes -----------------------------------------------------------------------
class SelectionDevice final : public juce::AudioIODevice
{
public:
    SelectionDevice (const juce::String& outputName, const juce::String& inputName, bool refuseOpen, int& openCount)
        : juce::AudioIODevice (outputName.isNotEmpty() ? outputName : inputName, "SelectionBackend"), hasInput (inputName.isNotEmpty()),
          hasOutput (outputName.isNotEmpty()), refuse (refuseOpen), opens (openCount)
    {
    }

    ~SelectionDevice() override { stop(); }

    juce::StringArray getOutputChannelNames() override { return hasOutput ? juce::StringArray { "Left", "Right" } : juce::StringArray(); }
    juce::StringArray getInputChannelNames() override { return hasInput ? juce::StringArray { "Left", "Right" } : juce::StringArray(); }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double, int) override
    {
        if (refuse)
            return "The device is being used by another application (exclusive mode)";
        ++opens;
        activeIns = hasInput ? ins : juce::BigInteger();
        activeOuts = hasOutput ? outs : juce::BigInteger();
        opened = true;
        return {};
    }
    void close() override
    {
        stop();
        opened = false;
    }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* cb) override
    {
        if (cb != nullptr && callback == nullptr)
        {
            cb->audioDeviceAboutToStart (this);
            callback = cb;
        }
    }
    void stop() override
    {
        if (auto* cb = std::exchange (callback, nullptr))
            cb->audioDeviceStopped();
    }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOuts; }
    juce::BigInteger getActiveInputChannels() const override { return activeIns; }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }

    /** The backend's audio thread: the active channels only, as a real backend. */
    void process (const float* const* ins, float* const* outs)
    {
        if (callback != nullptr)
            callback->audioDeviceIOCallbackWithContext (ins, activeIns.countNumberOfSetBits(), outs, activeOuts.countNumberOfSetBits(), kBlock,
                                                        juce::AudioIODeviceCallbackContext {});
    }

private:
    juce::AudioIODeviceCallback* callback = nullptr;
    juce::BigInteger activeIns, activeOuts;
    bool hasInput, hasOutput, refuse, opened = false;
    int& opens;
};

class SelectionBackend final : public juce::AudioIODeviceType
{
public:
    SelectionBackend() : juce::AudioIODeviceType ("SelectionBackend") {}

    juce::StringArray outputs, inputs;
    int defaultOutput = 0, defaultInput = 0;
    std::set<juce::String> busy; // outputs that refuse to open (held in exclusive mode)
    int opens = 0;               // successful device opens

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames) const override { return wantInputNames ? inputs : outputs; }
    int getDefaultDeviceIndex (bool forInput) const override { return forInput ? defaultInput : defaultOutput; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
    {
        return device != nullptr ? (asInput ? inputs : outputs).indexOf (device->getName()) : -1;
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String& inputName) override
    {
        if ((outputName.isNotEmpty() && ! outputs.contains (outputName)) || (inputName.isNotEmpty() && ! inputs.contains (inputName)))
            return nullptr;
        return new SelectionDevice (outputName, inputName, busy.count (outputName) > 0, opens);
    }

    /** The OS reported a device list change: JUCE's own path. */
    void devicesChanged() { callDeviceChangeListeners(); }
};

class FakeWatcher final : public AudioDeviceWatcher
{
public:
    std::vector<OutputEndpointIdentity> endpoints;
    int listCalls = 0;

    bool isSupported() const override { return true; }
    std::vector<OutputEndpointIdentity> listOutputs() override
    {
        ++listCalls;
        return endpoints;
    }
    bool start (Listener l) override
    {
        listener = std::move (l);
        return true;
    }
    void stop() override { listener = nullptr; }

    /** The OS's notification thread reports an event. */
    void fire (AudioDeviceEvent::Kind kind, const std::string& id = {})
    {
        AudioDeviceEvent e;
        e.kind = kind;
        e.endpointId = id;
        std::thread t ([this, e] {
            if (listener)
                listener (e);
        });
        t.join();
    }

private:
    Listener listener;
};

/** A host on the fake backend and watcher, with the device input feeding
    the Music strip (the cable setup). */
struct Rig
{
    AudioEngineHost host;
    SelectionBackend* backend = nullptr;
    FakeWatcher* watcher = nullptr;
    std::array<std::vector<float>, 2> in { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    std::array<std::vector<float>, 2> out { std::vector<float> (kBlock), std::vector<float> (kBlock) };
    int64_t sample = 0;

    Rig()
    {
        auto b = std::make_unique<SelectionBackend>();
        backend = b.get();
        host.getDeviceManager().addAudioDeviceType (std::move (b));
        auto w = std::make_unique<FakeWatcher>();
        watcher = w.get();
        host.setDeviceWatcher (std::move (w));
        AudioEngineHost::RecoveryTiming timing;
        timing.settleMs = 40;
        timing.firstRetryMs = 40;
        timing.maxRetryMs = 160;
        timing.maxAttempts = 4;
        host.setRecoveryTiming (timing);
        host.setDeviceInputRouting (1, 0);
    }

    ~Rig() { host.closeDevice(); }

    juce::String open (const juce::String& savedOutput, const juce::String& savedInput = kCableOut, const juce::String& endpointId = {},
                       const juce::String& hardwareId = {})
    {
        if (savedOutput.isEmpty())
            return host.openDevice (nullptr, 2, 2);
        juce::XmlElement saved ("DEVICESETUP");
        saved.setAttribute ("deviceType", "SelectionBackend");
        saved.setAttribute ("audioOutputDeviceName", savedOutput);
        saved.setAttribute ("audioInputDeviceName", savedInput);
        if (endpointId.isNotEmpty())
            saved.setAttribute ("flubOutputEndpointId", endpointId);
        if (hardwareId.isNotEmpty())
            saved.setAttribute ("flubOutputHardwareId", hardwareId);
        return host.openDevice (&saved, 2, 2);
    }

    SelectionDevice* device() { return dynamic_cast<SelectionDevice*> (host.getDeviceManager().getCurrentAudioDevice()); }
    juce::String output() { return device() != nullptr ? device()->getName() : juce::String(); }

    /** Callbacks on a fresh thread (the backend's audio thread); returns the output peak. */
    float run (int blocks)
    {
        float peak = 0.0f;
        std::thread t ([&]
        {
            for (int b = 0; b < blocks; ++b)
            {
                for (size_t c = 0; c < 2; ++c)
                    for (int i = 0; i < kBlock; ++i)
                        in[c][static_cast<size_t> (i)] =
                            0.25f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * static_cast<double> (sample + i) / 48000.0));
                sample += kBlock;
                const std::array<const float*, 2> ins { in[0].data(), in[1].data() };
                const std::array<float*, 2> outs { out[0].data(), out[1].data() };
                if (auto* d = device())
                    d->process (ins.data(), outs.data());
                if (b >= blocks / 2)
                    for (const auto& ch : out)
                        for (const float v : ch)
                            peak = std::max (peak, std::abs (v));
            }
        });
        t.join();
        return peak;
    }

    bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 1500) { return flubapptest::pumpMessagesUntil (done, timeoutMs); }
};

bool profileBypasses (AudioEngineHost& host)
{
    auto& engine = host.getMixEngine();
    for (int s = 0; s < engine.getNumStrips(); ++s)
        if (! engine.chain (s).isAuditionBypassed (flub::param::VirtualizerOn) || engine.chain (s).isAuditionBypassed (flub::param::BassOn)
            || engine.chain (s).getSafeSpeakerBassCapDb() != AudioEngineHost::kSafeSpeakerBassCapDb) // bass capped, not bypassed
            return false;
    return true;
}
} // namespace

TEST_CASE ("App: E51 endpoint identities: instance numbers, hardware ids from device paths, matching")
{
    using W = AudioDeviceWatcher;
    CHECK (W::withoutInstanceNumber ("Speakers (2- Stealth 700 Gen 2)") == "Speakers (Stealth 700 Gen 2)");
    CHECK (W::withoutInstanceNumber ("Headset Earphone (12- Arctis Nova 7)") == "Headset Earphone (Arctis Nova 7)");
    CHECK (W::withoutInstanceNumber ("Speakers (Stealth 700 Gen 2) (2)") == "Speakers (Stealth 700 Gen 2)");
    CHECK (W::withoutInstanceNumber ("Speakers (Realtek(R) Audio)") == "Speakers (Realtek(R) Audio)");
    CHECK (W::withoutInstanceNumber ("LG ULTRAGEAR (NVIDIA High Definition Audio)") == "LG ULTRAGEAR (NVIDIA High Definition Audio)");
    CHECK (W::withoutInstanceNumber ("Speakers (2-channel USB)") == "Speakers (2-channel USB)"); // no space after the dash: part of the name

    CHECK (W::hardwareIdFromDevicePath ("{2}.\\\\?\\usb#vid_10f5&pid_0210&mi_00#7&2b8e41a&0&0000#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\global")
           == "USB\\VID_10F5&PID_0210&MI_00");
    CHECK (W::hardwareIdFromDevicePath ("USB\\VID_1038&PID_12AD\\5&1b2c3d&0&3") == "USB\\VID_1038&PID_12AD");
    // Bluetooth: the device address, not the service GUID's 00805f9b34fb.
    CHECK (W::hardwareIdFromDevicePath ("{2}.\\\\?\\bthenum#{0000110b-0000-1000-8000-00805f9b34fb}_vid&0002054c_pid&0e45#8&2b3e4a4b&0&a4c1389f2b77_c00000000#{6994ad04-93ef-11d0-a3cc-00a0c9223196}")
           == "BTHENUM\\A4C1389F2B77");
    CHECK (W::hardwareIdFromDevicePath ("{2}.\\\\?\\hdaudio#func_01&ven_10ec&dev_0897#4&1a2b&0&0001#{6994ad04-93ef-11d0-a3cc-00a0c9223196}").empty());
    CHECK (W::hardwareIdFromDevicePath ("").empty());

    const auto remembered = stealth (kHeadset, "{0.0.0.00000000}.{port1}");
    CHECK (W::matchIdentity (remembered, stealth (kHeadset, "{0.0.0.00000000}.{port1}")) == W::Match::Exact);
    CHECK (W::matchIdentity (remembered, stealth ("Renamed by the user", "{0.0.0.00000000}.{port1}")) == W::Match::Exact);
    CHECK (W::matchIdentity (remembered, stealth (kHeadsetPort2, "{0.0.0.00000000}.{port2}")) == W::Match::Hardware);
    CHECK (W::matchIdentity (remembered, endpoint (kHeadsetPort2, "{x}")) == W::Match::Name);
    // Another model under the same name is not the headset.
    CHECK (W::matchIdentity (remembered, endpoint (kHeadset, "{y}", "USB\\VID_1038&PID_12AD")) == W::Match::None);
    CHECK (W::matchIdentity (remembered, speakers()) == W::Match::None);

    const std::vector<OutputEndpointIdentity> list { speakers(), endpoint (kHeadsetPort2, "{x}"), stealth (kHeadsetPort2, "{port2}") };
    W::Match how = W::Match::None;
    CHECK (W::findEndpoint (list, remembered, &how) == 2);
    CHECK (how == W::Match::Hardware);

    CHECK (AudioEngineHost::isVirtualOutput (kCableIn));
    CHECK (AudioEngineHost::isVirtualOutput ("Voicemeeter Input (VB-Audio Voicemeeter VAIO)"));
    CHECK (AudioEngineHost::isVirtualOutput ("Flubsound Game (Flubsound Virtual Audio)"));
    const auto odd = endpoint ("Line 1 (Odd Audio Router)", "{odd}", {}, EndpointFormFactor::Speakers, EndpointTransport::Virtual);
    CHECK (AudioEngineHost::isVirtualOutput ("Line 1 (Odd Audio Router)", &odd));
    CHECK (! AudioEngineHost::isVirtualOutput (kSpeakers));
    CHECK (! AudioEngineHost::isVirtualOutput (kHeadset));
}

TEST_CASE ("App: E51 selectOutput: chosen, recognised on another port or renamed, safe fallback, never the loop")
{
    using R = AudioEngineHost::OutputReason;
    const AudioEngineHost::OutputChoice choice { kHeadset, "{0.0.0.00000000}.{port1}", juce::String (kStealthHw) };
    const juce::StringArray all { kCableIn, kSpeakers, kHeadset };
    const std::vector<OutputEndpointIdentity> ids { cableIn(), speakers(), stealth (kHeadset, "{0.0.0.00000000}.{port1}") };

    auto s = AudioEngineHost::selectOutput (choice, all, 0, ids, kCableOut, true);
    CHECK (s.deviceName == kHeadset);
    CHECK (s.reason == R::Chosen);
    CHECK (! s.fallback);
    CHECK (! s.safeSpeakerProfile);

    // The dongle re-plugged into another USB port: a new endpoint id and "2- ".
    const juce::StringArray moved { kCableIn, kSpeakers, kHeadsetPort2 };
    const std::vector<OutputEndpointIdentity> movedIds { cableIn(), speakers(), stealth (kHeadsetPort2, "{0.0.0.00000000}.{port2}") };
    s = AudioEngineHost::selectOutput (choice, moved, 0, movedIds, kCableOut, true);
    CHECK (s.deviceName == kHeadsetPort2);
    CHECK (s.reason == R::Recognised);
    CHECK (! s.fallback);
    // ... also from settings that saved only the name (no platform identities).
    s = AudioEngineHost::selectOutput ({ kHeadset, {}, {} }, moved, 0, {}, kCableOut, true);
    CHECK (s.deviceName == kHeadsetPort2);
    CHECK (s.reason == R::Recognised);

    // Renamed by the user in Sound settings: the same endpoint id.
    const juce::StringArray renamed { kCableIn, kSpeakers, "Game headset" };
    s = AudioEngineHost::selectOutput (choice, renamed, 0, { cableIn(), speakers(), stealth ("Game headset", "{0.0.0.00000000}.{port1}") },
                                       kCableOut, true);
    CHECK (s.deviceName == "Game headset");
    CHECK (s.reason == R::Recognised);

    // Unplugged: the default is CABLE Input, the input's loopback partner and
    // virtual. Before E51 JUCE opened it (the guard muted it); now the
    // speakers, with the safe speaker profile.
    const juce::StringArray gone { kCableIn, kSpeakers };
    s = AudioEngineHost::selectOutput (choice, gone, 0, { cableIn(), speakers() }, kCableOut, true);
    CHECK (s.deviceName == kSpeakers);
    CHECK (s.reason == R::FirstSafe);
    CHECK (s.fallback);
    CHECK (s.safeSpeakerProfile);
    // A fallback to headphones keeps the headphone voicing.
    s = AudioEngineHost::selectOutput (choice, { kCableIn, kHeadphonesJack }, 0,
                                       { cableIn(), endpoint (kHeadphonesJack, "{hp}", {}, EndpointFormFactor::Headphones) }, kCableOut, true);
    CHECK (s.deviceName == kHeadphonesJack);
    CHECK (! s.safeSpeakerProfile);
    // ... and by name when the platform does not say.
    s = AudioEngineHost::selectOutput (choice, { kCableIn, kHeadphonesJack }, 0, {}, kCableOut, true);
    CHECK (! s.safeSpeakerProfile);
    s = AudioEngineHost::selectOutput (choice, gone, 0, {}, kCableOut, true);
    CHECK (s.safeSpeakerProfile);
    // A default that is safe is taken.
    s = AudioEngineHost::selectOutput (choice, { kSpeakers, kCableIn }, 0, {}, kCableOut, true);
    CHECK (s.reason == R::SystemDefault);
    // Nothing safe: none (the current device stays, the guard keeps a loop silent).
    s = AudioEngineHost::selectOutput (choice, { kCableIn }, 0, {}, kCableOut, true);
    CHECK (s.reason == R::NoSafeOutput);
    CHECK (s.deviceName.isEmpty());
    CHECK (s.fallback);

    // Another model under the headset's name is not the headset.
    s = AudioEngineHost::selectOutput (choice, { kCableIn, kSpeakers, kHeadset }, 0,
                                       { cableIn(), speakers(), endpoint (kHeadset, "{other}", "USB\\VID_1038&PID_12AD") }, kCableOut, true);
    CHECK (s.deviceName == kSpeakers);
    CHECK (s.fallback);

    // Busy (exclusive mode): the chosen output is there but failed to open.
    s = AudioEngineHost::selectOutput (choice, all, 0, ids, kCableOut, true, { kHeadset });
    CHECK (s.deviceName == kSpeakers);
    CHECK (s.fallback);
    CHECK (s.chosenUnavailable);

    // Nothing chosen: follow the default, but not into a virtual device.
    s = AudioEngineHost::selectOutput ({}, { kCableIn, kSpeakers }, 0, {}, kCableOut, true);
    CHECK (s.deviceName == kSpeakers);
    CHECK (s.reason == R::FirstSafe);
    CHECK (! s.fallback);
    CHECK (! s.safeSpeakerProfile);
    s = AudioEngineHost::selectOutput ({}, { kSpeakers, kHeadset }, 1, {}, {}, false);
    CHECK (s.deviceName == kHeadset);
    CHECK (s.reason == R::SystemDefault);
    // A chosen virtual output is the user's choice.
    s = AudioEngineHost::selectOutput ({ kCableIn, {}, {} }, { kCableIn, kSpeakers }, 1, {}, {}, false);
    CHECK (s.deviceName == kCableIn);
    CHECK (s.reason == R::Chosen);
}

TEST_CASE ("App: E51 explicit selection at start: the saved headset is missing and the default is CABLE Input")
{
    Rig rig;
    rig.backend->outputs = { kCableIn, kSpeakers };
    rig.backend->inputs = { kCableOut };
    rig.backend->defaultOutput = 0; // the cable setup: the system default output IS CABLE Input
    rig.watcher->endpoints = { cableIn(), speakers() };

    // Before E51 (initialise (..., true)): JUCE opened the default, CABLE
    // Input, and the loopback guard held the output silent.
    CHECK (rig.open (kHeadset, kCableOut, "{0.0.0.00000000}.{port1}", juce::String (kStealthHw)).isEmpty());
    CHECK (rig.output() == kSpeakers);
    REQUIRE (rig.device() != nullptr);
    CHECK (rig.device()->getActiveInputChannels().countNumberOfSetBits() == 2); // CABLE Output still feeds the Music strip
    CHECK (rig.host.getDeviceManager().getAudioDeviceSetup().inputDeviceName == kCableOut);
    CHECK (! rig.host.isOutputMutedByGuard());
    const auto sel = rig.host.getOutputSelection();
    CHECK (sel.fallback);
    CHECK (sel.reason == AudioEngineHost::OutputReason::FirstSafe);
    CHECK (rig.host.isSafeSpeakerProfileActive());
    CHECK (profileBypasses (rig.host));

    const auto state = rig.host.getDeviceSafetyState();
    CHECK (state.kind == DeviceSafetyState::Kind::None);
    CHECK (state.outputFallback);
    CHECK (state.safeSpeakerProfile);
    CHECK (state.chosenOutputName == kHeadset);
    CHECK (state.fallbackOutputName == kSpeakers);
    CHECK (state.fallbackMessage.contains ("is not connected"));
    CHECK (state.fallbackMessage.contains ("safe speaker profile"));

    // Audible, and the choice is still the headset (not the fallback).
    CHECK (rig.run (60) > 0.01f);
    CHECK (rig.host.getChosenOutput().deviceName == kHeadset);
    auto xml = rig.host.createDeviceStateXml();
    REQUIRE (xml != nullptr);
    CHECK (xml->getStringAttribute ("audioOutputDeviceName") == kHeadset);
    CHECK (xml->getStringAttribute ("flubOutputEndpointId") == "{0.0.0.00000000}.{port1}");
    CHECK (xml->getStringAttribute ("flubOutputHardwareId") == juce::String (kStealthHw));
}

TEST_CASE ("App: E51 hot-plug: the dongle leaves, then comes back on another USB port under a new name and id")
{
    Rig rig;
    rig.backend->outputs = { kCableIn, kSpeakers, kHeadset };
    rig.backend->inputs = { kCableOut };
    rig.backend->defaultOutput = 0;
    rig.watcher->endpoints = { cableIn(), speakers(), stealth (kHeadset, "{0.0.0.00000000}.{port1}") };
    CHECK (rig.open (kHeadset).isEmpty());
    CHECK (rig.output() == kHeadset);
    CHECK (! rig.host.getOutputSelection().fallback);
    // Settings from before E51 saved the name only: the identity is filled in.
    CHECK (rig.host.getChosenOutput().endpointId == "{0.0.0.00000000}.{port1}");
    CHECK (rig.host.getChosenOutput().hardwareId == juce::String (kStealthHw));
    const float before = rig.run (80);
    CHECK (before > 0.01f);

    // Unplugged: JUCE falls back to the default (CABLE Input, muted by the
    // guard), then the host's selection plays on the speakers.
    rig.backend->outputs = { kCableIn, kSpeakers };
    rig.watcher->endpoints = { cableIn(), speakers() };
    rig.backend->devicesChanged();
    rig.watcher->fire (AudioDeviceEvent::Kind::DeviceRemoved, "{0.0.0.00000000}.{port1}");
    CHECK (rig.pumpUntil ([&] { return rig.output() == kSpeakers && rig.host.getDeviceEventsHandled() >= 1; }));
    CHECK (rig.host.isSafeSpeakerProfileActive());
    CHECK (rig.host.getDeviceSafetyState().outputFallback);
    CHECK (rig.run (80) > 0.01f);

    // Plugged into another port: Windows names it "2- ..." with a new id.
    const int opensBefore = rig.backend->opens;
    rig.backend->outputs = { kCableIn, kSpeakers, kHeadsetPort2 };
    rig.watcher->endpoints = { cableIn(), speakers(), stealth (kHeadsetPort2, "{0.0.0.00000000}.{port2}") };
    rig.watcher->fire (AudioDeviceEvent::Kind::DeviceAdded, "{0.0.0.00000000}.{port2}");
    CHECK (rig.pumpUntil ([&] { return rig.output() == kHeadsetPort2; }));
    CHECK (rig.backend->opens == opensBefore + 1);
    CHECK (rig.host.getOutputSelection().reason == AudioEngineHost::OutputReason::Recognised);
    CHECK (! rig.host.getOutputSelection().fallback);
    CHECK (! rig.host.isSafeSpeakerProfileActive());
    CHECK (! profileBypasses (rig.host));
    CHECK (! rig.host.getDeviceSafetyState().outputFallback);
    // The choice is unchanged: the original port is recognised too.
    CHECK (rig.host.getChosenOutput().deviceName == kHeadset);
    std::cerr << "    re-plug on another port: output \"" << rig.output() << "\", device opens for the return " << (rig.backend->opens - opensBefore)
              << "\n";
}

TEST_CASE ("App: E51 the user's choice in Settings wins; with nothing chosen the output follows the default, never a cable")
{
    Rig rig;
    rig.backend->outputs = { kSpeakers, kHeadphonesJack, kCableIn };
    rig.backend->inputs = { kCableOut };
    rig.backend->defaultOutput = 0;
    rig.watcher->endpoints = { speakers(), endpoint (kHeadphonesJack, "{hp}", {}, EndpointFormFactor::Headphones), cableIn() };
    CHECK (rig.open ({}).isEmpty());
    CHECK (rig.output() == kSpeakers);
    CHECK (rig.host.getChosenOutput().deviceName.isEmpty());
    CHECK (rig.host.createDeviceStateXml() == nullptr);

    // The default moves to the headphones: followed.
    rig.backend->defaultOutput = 1;
    rig.watcher->fire (AudioDeviceEvent::Kind::DefaultOutputChanged, "{hp}");
    CHECK (rig.pumpUntil ([&] { return rig.output() == kHeadphonesJack; }));
    CHECK (! rig.host.getOutputSelection().fallback);
    // ... to CABLE Input (the cable setup): not followed into the loop.
    rig.backend->defaultOutput = 2;
    const auto handled = rig.host.getDeviceEventsHandled();
    rig.watcher->fire (AudioDeviceEvent::Kind::DefaultOutputChanged, "{0.0.0.00000000}.{cable}");
    CHECK (rig.pumpUntil ([&] { return rig.host.getDeviceEventsHandled() > handled; }));
    CHECK (rig.output() == kSpeakers);

    // The user picks the headphones in Settings > Audio: that is the choice now.
    auto setup = rig.host.getDeviceManager().getAudioDeviceSetup();
    setup.outputDeviceName = kHeadphonesJack;
    CHECK (rig.host.getDeviceManager().setAudioDeviceSetup (setup, true).isEmpty());
    CHECK (rig.pumpUntil ([&] { return rig.host.getChosenOutput().deviceName == kHeadphonesJack; }));
    CHECK (rig.host.getChosenOutput().endpointId == "{hp}");
    CHECK (rig.output() == kHeadphonesJack);
    rig.backend->defaultOutput = 0;
    rig.watcher->fire (AudioDeviceEvent::Kind::DefaultOutputChanged, "{0.0.0.00000000}.{spk}");
    CHECK (rig.pumpUntil ([&] { return rig.host.getDeviceEventsHandled() > handled + 1; }));
    CHECK (rig.output() == kHeadphonesJack); // a choice is not moved by the default
}

TEST_CASE ("App: E51 sleep / resume: a device that stopped calling back is re-opened, a running one is left alone")
{
    Rig rig;
    rig.backend->outputs = { kSpeakers, kHeadset };
    rig.backend->inputs = { kCableOut };
    rig.watcher->endpoints = { speakers(), stealth (kHeadset, "{0.0.0.00000000}.{port1}") };
    CHECK (rig.open (kHeadset).isEmpty());
    CHECK (rig.run (20) > 0.0f);

    // Resume, and the device keeps calling back: nothing is re-opened.
    int opens = rig.backend->opens;
    rig.watcher->fire (AudioDeviceEvent::Kind::Suspending);
    rig.watcher->fire (AudioDeviceEvent::Kind::Resumed);
    CHECK (rig.pumpUntil ([&] { return rig.host.isRecoveryPending(); }));
    rig.run (4);
    CHECK (rig.pumpUntil ([&] { return ! rig.host.isRecoveryPending(); }));
    CHECK (rig.backend->opens == opens);

    // Resume, and the device is dead (no callback since): re-opened after the
    // settle time, then recovered once callbacks run again.
    opens = rig.backend->opens;
    rig.watcher->fire (AudioDeviceEvent::Kind::Suspending);
    rig.watcher->fire (AudioDeviceEvent::Kind::Resumed);
    rig.watcher->fire (AudioDeviceEvent::Kind::Resumed); // Windows sends both resume notifications
    CHECK (rig.pumpUntil ([&] { return rig.backend->opens > opens; }));
    CHECK (rig.output() == kHeadset);
    CHECK (rig.host.getRecoveryAttempts() == 1);
    rig.run (4);
    CHECK (rig.pumpUntil ([&] { return ! rig.host.isRecoveryPending(); }));
    CHECK (rig.backend->opens == opens + 1);
    CHECK (rig.host.getRecoveryAttempts() == 0);
    CHECK (rig.run (40) > 0.01f);

    // While suspended, device churn is ignored (the resume check re-selects).
    const auto handled = rig.host.getDeviceEventsHandled();
    opens = rig.backend->opens;
    rig.watcher->fire (AudioDeviceEvent::Kind::Suspending);
    rig.watcher->fire (AudioDeviceEvent::Kind::DeviceStateChanged, "{0.0.0.00000000}.{port1}");
    CHECK (rig.pumpUntil ([&] { return rig.host.getDeviceEventsHandled() >= handled + 2; }));
    CHECK (rig.output() == kHeadset);
    CHECK (rig.backend->opens == opens);
}

TEST_CASE ("App: E51 an output busy in exclusive mode: safe fallback, retried with backoff, back when it is free")
{
    Rig rig;
    rig.backend->outputs = { kSpeakers, kHeadset };
    rig.backend->inputs = { kCableOut };
    rig.backend->busy = { kHeadset };
    rig.watcher->endpoints = { speakers(), stealth (kHeadset, "{0.0.0.00000000}.{port1}") };
    CHECK (rig.open (kHeadset).isEmpty());
    CHECK (rig.output() == kSpeakers);
    CHECK (rig.host.getOutputSelection().chosenUnavailable);
    CHECK (rig.host.isSafeSpeakerProfileActive());
    CHECK (rig.host.getDeviceSafetyState().fallbackMessage.contains ("could not be opened"));
    CHECK (rig.host.isRecoveryPending());

    // Still busy at the first retries: the speakers keep playing.
    CHECK (rig.pumpUntil ([&] { return rig.host.getRecoveryAttempts() >= 2; }));
    CHECK (rig.output() == kSpeakers);
    // Freed: the next retry opens it.
    rig.backend->busy.clear();
    CHECK (rig.pumpUntil ([&] { return rig.output() == kHeadset; }));
    const int attemptsWhenBack = rig.host.getRecoveryAttempts();
    CHECK (! rig.host.isSafeSpeakerProfileActive());
    CHECK (! rig.host.getDeviceSafetyState().outputFallback);
    rig.run (4);
    CHECK (rig.pumpUntil ([&] { return ! rig.host.isRecoveryPending(); }));
    std::cerr << "    busy headset: back on retry " << attemptsWhenBack << "\n";
}

TEST_CASE ("App: E51 the safe speaker profile trims the output by 6 dB on the audio thread, allocation- and lock-free")
{
    Rig rig;
    rig.backend->outputs = { kSpeakers, kHeadset };
    rig.backend->inputs = { kCableOut };
    rig.watcher->endpoints = { speakers(), stealth (kHeadset, "{0.0.0.00000000}.{port1}") };
    CHECK (rig.open (kHeadset).isEmpty());
    // Music strip, virtualiser and bass engine already off: the trim alone.
    auto& params = rig.host.getMixEngine().params (1);
    params.set (flub::param::VirtualizerOn, 0.0f);
    params.set (flub::param::BassOn, 0.0f);
    const float full = rig.run (200);
    CHECK (full > 0.01f);

    // Unplugged: the speakers play with the profile.
    rig.backend->outputs = { kSpeakers };
    rig.watcher->endpoints = { speakers() };
    rig.backend->devicesChanged();
    CHECK (rig.pumpUntil ([&] { return rig.output() == kSpeakers && rig.host.isSafeSpeakerProfileActive(); }));
    const float trimmed = rig.run (200);
    const double db = 20.0 * std::log10 (static_cast<double> (trimmed) / static_cast<double> (full));
    std::cerr << "    safe speaker profile: output peak " << full << " -> " << trimmed << " (" << db << " dB)\n";
    CHECK_NEAR (db, static_cast<double> (AudioEngineHost::kSafeSpeakerTrimDb), 0.3);

    // The callback while the trim ramps back up and after: no allocation, no
    // lock (one audio thread; its first callback promotes it, then the probe).
    const std::array<const float*, 2> ins { rig.in[0].data(), rig.in[1].data() };
    const std::array<float*, 2> outs { rig.out[0].data(), rig.out[1].data() };
    const auto callback = [&] { rig.host.audioDeviceIOCallbackWithContext (ins.data(), 2, outs.data(), 2, kBlock, juce::AudioIODeviceCallbackContext {}); };
    int64_t allocations = -1, locks = -1;
    std::thread audio ([&]
    {
        callback();
        flubapptest::RealtimeProbe probe;
        for (int b = 0; b < 8; ++b)
            callback();
        allocations = probe.allocations();
        locks = probe.locks();
    });
    audio.join();
    CHECK (allocations == 0);
    if (flubapptest::lockCountingAvailable())
        CHECK (locks == 0);
}
