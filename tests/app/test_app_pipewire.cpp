// Flubsound Pro - the native PipeWire node against a real PipeWire server
// (docs/11 E48; app/Source/platform/pipewire).
//
// These need a running PipeWire server ($PIPEWIRE_REMOTE, else
// $XDG_RUNTIME_DIR/pipewire-0) and a build with libpipewire
// (FLUB_HAS_PIPEWIRE). Without either they print "skipped" and pass, so CI,
// which starts no server, never fails for lack of one. A headless server is
// enough: pipewire, wireplumber and pipewire-pulse on a private
// $XDG_RUNTIME_DIR and session bus (platform/linux/README.md, "Testing
// against a headless PipeWire"). Every sink and node the tests create has a
// name with this process's pid; nothing is played to a real output: the
// tests' outputs go to sinks they created themselves, or write silence. The
// device-type test is skipped when the desktop already has Flubsound's own
// sinks (a real setup the test must not play into), and the app-level case
// (EngineController with a device, which opens the first-run default and
// plays to the default output until it is moved) runs on a test server only:
// one without ALSA or Bluetooth devices. Waits are hang guards (5 s), not
// timing assertions.
#if defined(__linux__) && defined(FLUB_HAS_PIPEWIRE) && FLUB_HAS_PIPEWIRE

#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "engine/EngineController.h"
#include "platform/PlatformServices.h"
#include "platform/pipewire/PipeWireDeviceType.h"
#include "platform/pipewire/PipeWireLibrary.h"
#include "platform/pipewire/PipeWireNative.h"
#include "ui/RoutingPanel.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <cmath>
#include <iostream>
#include <thread>

using namespace flub::app;
using namespace flub::platform;
using namespace flubtest;

namespace
{
constexpr double kPi = 3.14159265358979323846;

bool serverAvailable()
{
    static int available = -1;
    if (available < 0)
    {
        pipewire::Session probe;
        std::string error;
        available = probe.connect ("flub_app_tests probe", error) ? 1 : 0;
        if (available == 0)
            std::cerr << "    (no PipeWire server: " << error << ")\n";
    }
    if (available == 0)
        std::cerr << "    (skipped: needs a running PipeWire server)\n";
    return available == 1;
}

std::string unique (const std::string& suffix) { return "flubtest" + std::to_string (::getpid()) + "_" + suffix; }

bool waitUntil (const std::function<bool()>& done, int timeoutMs = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
    while (! done())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    return true;
}

/** The node names a fresh session sees. */
std::vector<std::string> nodeNames()
{
    pipewire::Session session;
    std::string error;
    std::vector<std::string> names;
    if (! session.connect ("flub_app_tests lister", error))
        return names;
    session.lock();
    for (const auto& [id, node] : session.graph().nodes)
        names.push_back (node.name);
    session.unlock();
    return names;
}

bool hasNode (const std::vector<std::string>& names, const std::string& name) { return std::find (names.begin(), names.end(), name) != names.end(); }

/** A server without real devices (a headless test server: the dummy driver,
    null sinks), where a test may open the default output. */
bool isTestServer()
{
    for (const auto& name : nodeNames())
        if (name.rfind ("alsa_", 0) == 0 || name.rfind ("bluez_", 0) == 0)
        {
            std::cerr << "    (skipped: this server has real devices, e.g. " << name << "; the test needs a headless test server)\n";
            return false;
        }
    return true;
}

/** Records what AppRouting's worker asks the router to link. */
class InputRecorder final : public AppAudioRouter
{
public:
    bool isSupported() const override { return true; }
    std::vector<AudioSessionInfo> enumerateSessions() override { return {}; }
    bool setAppEndpoint (uint32_t, const std::string&, std::string& error) override
    {
        error = "test";
        return false;
    }
    void openSystemRoutingSettings() override {}
    bool connectEndpointInputs (const std::vector<EndpointInput>& inputs, std::string& status) override
    {
        const std::lock_guard<std::mutex> guard (mutex);
        last = inputs;
        ++calls;
        status.clear();
        return true;
    }
    std::vector<EndpointInput> lastInputs() const
    {
        const std::lock_guard<std::mutex> guard (mutex);
        return last;
    }
    int callCount() const
    {
        const std::lock_guard<std::mutex> guard (mutex);
        return calls;
    }

private:
    mutable std::mutex mutex;
    std::vector<EndpointInput> last;
    int calls = 0;
};

/** A node callback for the tests: a 997 Hz tone at -20 dBFS on every output
    (or silence), the RMS of each input over the last cycles, and the data
    thread's own allocation and lock counts between cycle 20 and cycle 120. */
class TestCallback final : public NativeAudioNode::Callback
{
public:
    explicit TestCallback (bool tone) : playTone (tone) {}

    void nodeStarting (double rate, int maxBlock) override
    {
        sampleRate = rate;
        maxBlockFrames = maxBlock;
        started = true;
    }

    void nodeProcess (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numFrames,
                      uint64_t timeNs) noexcept FLUB_NONBLOCKING override
    {
        lastTimeNs.store (timeNs, std::memory_order_relaxed);
        const auto cycle = calls.load (std::memory_order_relaxed);
        if (cycle == 20)
        {
            allocationsAt20 = flubtest::allocationCount();
            locksAt20 = flubapptest::lockCount();
        }
        if (cycle == 120)
        {
            dataThreadAllocations.store (flubtest::allocationCount() - allocationsAt20);
            dataThreadLocks.store (flubapptest::lockCount() - locksAt20);
        }

        for (int c = 0; c < std::min (numInputs, kChannels); ++c)
        {
            double sum = 0.0;
            for (int n = 0; n < numFrames; ++n)
                sum += static_cast<double> (inputs[c][n]) * static_cast<double> (inputs[c][n]);
            const double rms = numFrames > 0 ? std::sqrt (sum / numFrames) : 0.0;
            inputRms[static_cast<size_t> (c)].store (static_cast<float> (rms), std::memory_order_relaxed);
        }
        for (int n = 0; n < numFrames; ++n)
        {
            const float value = playTone ? 0.1f * static_cast<float> (std::sin (phase)) : 0.0f;
            phase += 2.0 * kPi * 997.0 / 48000.0;
            if (phase > 2.0 * kPi)
                phase -= 2.0 * kPi;
            for (int o = 0; o < numOutputs; ++o)
                outputs[o][n] = value;
        }
        largestBlock.store (std::max (largestBlock.load (std::memory_order_relaxed), numFrames), std::memory_order_relaxed);
        inputsSeen.store (numInputs, std::memory_order_relaxed);
        outputsSeen.store (numOutputs, std::memory_order_relaxed);
        calls.fetch_add (1, std::memory_order_relaxed);
    }

    void nodeStopped() override { stopped = true; }

    void nodeError (const std::string& message) override
    {
        const std::lock_guard<std::mutex> guard (errorMutex);
        errors.push_back (message);
    }

    std::vector<std::string> errorMessages() const
    {
        const std::lock_guard<std::mutex> guard (errorMutex);
        return errors;
    }

    float rms (int channel) const { return inputRms[static_cast<size_t> (channel)].load(); }

    static constexpr int kChannels = 16;
    const bool playTone;
    double sampleRate = 0.0;
    int maxBlockFrames = 0;
    bool started = false, stopped = false;
    double phase = 0.0;
    int64_t allocationsAt20 = 0, locksAt20 = 0;
    std::atomic<int64_t> dataThreadAllocations { -1 }, dataThreadLocks { -1 };
    std::atomic<int> calls { 0 }, largestBlock { 0 }, inputsSeen { 0 }, outputsSeen { 0 };
    std::atomic<uint64_t> lastTimeNs { 0 };
    std::array<std::atomic<float>, kChannels> inputRms {};
    mutable std::mutex errorMutex;
    std::vector<std::string> errors;
};

NativeAudioNodeConfig testConfig (const std::string& prefix, std::vector<NativeAudioNodeConfig::Strip> strips, const std::string& outputTarget)
{
    NativeAudioNodeConfig config;
    config.strips = std::move (strips);
    config.outputTarget = outputTarget;
    config.nodeName = unique (prefix + "_engine");
    config.nodeDescription = "Flubsound test " + prefix;
    config.maxBlockFrames = 256;
    return config;
}
} // namespace

TEST_CASE ("App: native PipeWire node - sinks from the app, links by registry, audio through, node.latency in place, gone after stop (E48)")
{
    if (! serverAvailable())
        return;

    // B: the node under test, Game 7.1 and Music strips; its output plays
    // into its own "out" sink's twin that C reads (never a real device).
    const auto bGame = unique ("b_game"), bMusic = unique ("b_music"), cOut = unique ("c_out"), aIn = unique ("a_in");
    auto b = NativeAudioNode::create();
    auto a = NativeAudioNode::create();
    auto c = NativeAudioNode::create();
    REQUIRE (b->isSupported());
    TestCallback bCallback (false), aCallback (true), cCallback (false);

    // C only provides a sink for B's output and measures what arrives there.
    auto cConfig = testConfig ("c", { { "Out", cOut, "Flubsound test C", { "FL", "FR" } } }, "");
    cConfig.outputChannels = 2;
    std::string error;
    REQUIRE (c->start (cConfig, cCallback, error));

    auto bConfig = testConfig ("b", { { "Game", bGame, "Flubsound test Game", { "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR" } },
                                      { "Music", bMusic, "Flubsound test Music", { "FL", "FR" } } },
                               cOut);
    REQUIRE (b->start (bConfig, bCallback, error));
    CHECK (bCallback.started);
    CHECK (bCallback.maxBlockFrames == 256);
    CHECK (b->isRunning());

    // A plays a tone into B's Game sink (A's own strip sink stays silent).
    REQUIRE (a->start (testConfig ("a", { { "In", aIn, "Flubsound test A", { "FL", "FR" } } }, bGame), aCallback, error));

    // B made both of its sinks, linked all 10 strip ports and its 2 outputs.
    CHECK (waitUntil ([&] {
        const auto s = b->getStatus();
        return s.inputLinksMade == 10 && s.inputLinksWanted == 10 && s.outputLinksMade == 2;
    }));
    auto status = b->getStatus();
    CHECK (status.running);
    CHECK (status.nodeId != 0);
    CHECK ((status.createdSinks == std::vector<std::string> { bGame, bMusic }));
    CHECK (status.outputSink == cOut);
    CHECK (status.message.empty());
    if (! status.message.empty())
        std::cout << "    status: " << pipewire::describeLinks (status) << "\n";

    // The tone reaches B's Game FL / FR only, and B's (silent) output
    // reaches C.
    CHECK (waitUntil ([&] { return bCallback.rms (0) > 0.05f && bCallback.rms (1) > 0.05f; }));
    CHECK_NEAR (bCallback.rms (0), 0.1 / std::sqrt (2.0), 0.01); // -20 dBFS peak sine, through the sink unchanged
    CHECK (bCallback.rms (2) < 1.0e-6f);                         // FC
    CHECK (bCallback.rms (8) < 1.0e-6f);                         // Music FL
    CHECK (bCallback.inputsSeen.load() == 10);
    CHECK (bCallback.outputsSeen.load() == 2);
    CHECK (bCallback.largestBlock.load() <= 256);
    CHECK (waitUntil ([&] { return c->getStatus().inputLinksMade == 2; }));

    // The quantum: 256/48000 asked on Balanced (another client may ask for
    // less; the graph follows the smallest), 128 locked after setLatency,
    // without a re-open (the cycle count keeps growing).
    CHECK (waitUntil ([&] { return b->getStatus().quantumFrames != 0; }));
    CHECK_LE (b->getStatus().quantumFrames, 256);
    CHECK (b->getStatus().sampleRate != 0);
    const auto nodeBefore = b->getStatus().nodeId;
    CHECK (b->setLatency (NativeAudioNodeConfig::Latency::LowLatency));
    CHECK (waitUntil ([&] { return b->getStatus().quantumFrames != 0 && b->getStatus().quantumFrames <= 128; }));
    CHECK (b->getStatus().nodeId == nodeBefore);
    std::cout << "    quantum after Low Latency: " << b->getStatus().quantumFrames << "/" << b->getStatus().sampleRate << "\n";

    // The data thread (libpipewire's process path, the cycle runner and the
    // callback) allocates nothing and takes no lock over 100 cycles.
    CHECK (waitUntil ([&] { return bCallback.dataThreadAllocations.load() >= 0; }));
    CHECK (bCallback.dataThreadAllocations.load() == 0);
    if (flubapptest::lockCountingAvailable())
        CHECK (bCallback.dataThreadLocks.load() == 0);

    // Moving the output: back to "the default" (which here refuses nothing
    // but may be absent) and then to C again.
    CHECK (b->setOutputTarget (bGame)); // its own sink: refused as a feedback loop
    CHECK (waitUntil ([&] { return b->getStatus().outputLinksMade == 0 && ! b->getStatus().message.empty(); }));
    CHECK (b->getStatus().message.find ("feedback loop") != std::string::npos);
    CHECK (b->setOutputTarget (cOut));
    CHECK (waitUntil ([&] { return b->getStatus().outputLinksMade == 2; }));

    a->stop();
    b->stop();
    c->stop();
    CHECK (bCallback.stopped);
    CHECK (! b->isRunning());
    const auto calls = bCallback.calls.load();
    std::this_thread::sleep_for (std::chrono::milliseconds (30));
    CHECK (bCallback.calls.load() == calls); // no process() after stop()

    // Everything B made went with it (not lingering): what a crash leaves too.
    const auto names = nodeNames();
    CHECK (! hasNode (names, bGame));
    CHECK (! hasNode (names, bMusic));
    CHECK (! hasNode (names, bConfig.nodeName));
    CHECK (! hasNode (names, cOut));

    // Starting again works (the device type re-opens this way).
    TestCallback again (false);
    REQUIRE (b->start (bConfig, again, error));
    CHECK (waitUntil ([&] { return b->getStatus().inputLinksMade == 10; }));
    b->stop();
}

TEST_CASE ("App: native PipeWire node - existing sinks are used, not duplicated; a missing sink is named in the status (E48)")
{
    if (! serverAvailable())
        return;

    const auto shared = unique ("shared"), missing = unique ("missing");
    auto owner = NativeAudioNode::create();
    auto user = NativeAudioNode::create();
    TestCallback ownerCallback (false), userCallback (false);
    std::string error;
    REQUIRE (owner->start (testConfig ("owner", { { "Shared", shared, "Flubsound test shared", { "FL", "FR" } } }, ""), ownerCallback, error));

    auto config = testConfig ("user", { { "Shared", shared, "x", { "FL", "FR" } }, { "Missing", missing, "x", { "FL", "FR" } } }, "");
    config.createSinks = false;
    REQUIRE (user->start (config, userCallback, error));
    CHECK (waitUntil ([&] { return user->getStatus().inputLinksMade == 2; }));
    const auto status = user->getStatus();
    CHECK (status.createdSinks.empty());
    CHECK (status.inputLinksWanted == 2);
    CHECK (status.message.find ("PipeWire has no sink '" + missing + "' for the Missing strip") != std::string::npos);
    user->stop();
    owner->stop();
}

TEST_CASE ("App: PipeWire registry linker - JUCE-path monitor links follow the input map without pw-dump, and go when unmapped (E48)")
{
    if (! serverAvailable())
        return;

    // A node of this process with 4 inputs stands in for JUCE's JACK input
    // client; its own strips read nothing (their sinks do not exist).
    const auto sink = unique ("linker_sink");
    auto provider = NativeAudioNode::create();
    auto engine = NativeAudioNode::create();
    TestCallback providerCallback (false), engineCallback (false);
    std::string error;
    REQUIRE (provider->start (testConfig ("provider", { { "P", sink, "Flubsound test linker", { "FL", "FR" } } }, ""), providerCallback, error));
    auto engineConfig = testConfig ("jackish", { { "Q", unique ("none"), "x", { "FL", "FR", "RL", "RR" } } }, "");
    engineConfig.createSinks = false;
    REQUIRE (engine->start (engineConfig, engineCallback, error));

    pipewire::RegistryLinker linker;
    REQUIRE (linker.connect (error));
    std::string status;
    // The sink's monitors -> the engine's inputs 3 and 4 (first channel 2).
    const std::vector<AppAudioRouter::EndpointInput> mapped { { sink, 2 } };
    CHECK (waitUntil ([&] { return linker.update (mapped, status) && linker.ownedLinks() == 2; }));
    CHECK (status.empty());
    CHECK (waitUntil ([&] { return engine->getStatus().cycles > 0; }));

    // A missing sink is named; unmapping removes the linker's links.
    const std::vector<AppAudioRouter::EndpointInput> missing { { unique ("nope"), 0 } };
    CHECK (! linker.update (missing, status));
    CHECK (status.find ("PipeWire has no sink '" + unique ("nope") + "'") != std::string::npos);
    CHECK (linker.ownedLinks() == 0);
    CHECK (linker.update (mapped, status));
    CHECK (waitUntil ([&] { return linker.ownedLinks() == 2; }));
    CHECK (linker.update ({ { sink, -1 } }, status));
    CHECK (linker.ownedLinks() == 0);

    engine->stop();
    provider->stop();
}

TEST_CASE ("App: the PipeWire device type runs AudioEngineHost in the node: 14 named inputs, the Game tone on FL / FR, no re-open for Low Latency (E48)")
{
    if (! serverAvailable())
        return;
    if (hasNode (nodeNames(), "flubsound_game"))
    {
        std::cerr << "    (skipped: this desktop already has Flubsound's sinks; the test must not play into them)\n";
        return;
    }

    AudioEngineHost host;
    std::array<int, AudioEngineHost::kMaxStrips> map {};
    map.fill (-1);
    map[0] = 0; // Game 0-7, Music 8-9, Chat 10-11, System 12-13
    map[1] = 8;
    map[2] = 10;
    map[3] = 12;
    host.setDeviceInputMap (map);
    auto& manager = host.getDeviceManager();
    CHECK (pipewire::addDeviceType (manager));
    CHECK (! pipewire::addDeviceType (manager)); // once
    CHECK (manager.getAvailableDeviceTypes().size() >= 2); // JUCE's own types stay, first

    // An output sink for the engine (C reads it), so nothing reaches a real device.
    const auto out = unique ("engine_out");
    auto sinkNode = NativeAudioNode::create();
    TestCallback sinkCallback (false);
    std::string error;
    REQUIRE (sinkNode->start (testConfig ("sink", { { "Out", out, "Flubsound test engine output", { "FL", "FR" } } }, ""), sinkCallback, error));

    juce::XmlElement state ("DEVICESETUP");
    state.setAttribute ("deviceType", pipewire::kDeviceTypeName);
    state.setAttribute ("audioOutputDeviceName", pipewire::kDeviceName);
    state.setAttribute ("audioInputDeviceName", pipewire::kInputDeviceName);
    state.setAttribute ("audioDeviceRate", 48000.0);
    state.setAttribute ("audioDeviceBufferSize", 256);
    const auto openError = host.openDevice (&state, 14, 2);
    CHECK (openError.isEmpty());
    auto* device = manager.getCurrentAudioDevice();
    REQUIRE (device != nullptr);
    CHECK (device->getTypeName() == pipewire::kDeviceTypeName);
    CHECK (! AudioEngineHost::isLoopbackPair (pipewire::kInputDeviceName, pipewire::kDeviceName)); // not muted as a feedback loop
    CHECK (pipewire::setDeviceOutputTarget (device, out));
    const auto names = device->getInputChannelNames();
    REQUIRE (names.size() == 14);
    CHECK (names[0] == "Game:FL");
    CHECK (names[13] == "System:FR");
    CHECK (AudioEngineHost::positionFromChannelName (names[3]) == SpeakerPosition::LFE);
    CHECK (AudioEngineHost::positionFromChannelName (names[5]) == SpeakerPosition::RR);

    CHECK (waitUntil ([&] {
        const auto s = pipewire::getDeviceStatus (device);
        return s.inputLinksMade == 14 && s.outputLinksMade == 2;
    }));
    CHECK (pipewire::getDeviceStatus (device).outputSink == out);

    // A tone into flubsound_game (created by the device) reaches the Game
    // strip's FL and FR.
    auto player = NativeAudioNode::create();
    TestCallback playerCallback (true);
    REQUIRE (player->start (testConfig ("player", { { "Src", unique ("player_in"), "x", { "FL", "FR" } } }, "flubsound_game"), playerCallback, error));
    const bool reached = waitUntil ([&] { return (host.getMixEngine().chain (0).meters().activeChannelMask.load() & 3u) == 3u; });
    CHECK (reached);
    if (! reached)
        std::cout << "    player: " << pipewire::describeLinks (player->getStatus()) << "\n    device: " << pipewire::describeLinks (pipewire::getDeviceStatus (device))
                  << "\n    mask 0x" << std::hex << host.getMixEngine().chain (0).meters().activeChannelMask.load() << std::dec << "\n";
    const auto engineStatus = host.getStatus();
    CHECK (engineStatus.running);
    CHECK (engineStatus.deviceTypeName == pipewire::kDeviceTypeName);
    CHECK (engineStatus.numInputChannels == 14);
    CHECK (engineStatus.callbacks > 0);
    CHECK (engineStatus.inputChannelMap == InputChannelMap::ChannelNames);
    CHECK (waitUntil ([&] { return sinkCallback.rms (0) > 1.0e-4f; })); // processed audio arrives at the output sink

    // Low Latency: node.latency changes in place, the device stays open.
    const auto callbacksBefore = host.getStatus().callbacks;
    CHECK (pipewire::setDeviceLatency (device, NativeAudioNodeConfig::Latency::LowLatency));
    CHECK (waitUntil ([&] { const auto q = pipewire::getDeviceStatus (device).quantumFrames; return q != 0 && q <= 128; }));
    CHECK (manager.getCurrentAudioDevice() == device);
    CHECK (host.getStatus().callbacks > callbacksBefore);

    player->stop();
    host.closeDevice();
    sinkNode->stop();
    CHECK (! hasNode (nodeNames(), "flubsound_game")); // the device's sinks went with it
}

TEST_CASE ("App: the running app offers the PipeWire device and plays through it: first-run default, every strip mapped, Low Latency without a re-open, the quantum in the latency total, the routing panel's link line (E48)")
{
    if (! serverAvailable() || ! isTestServer())
        return;
    if (hasNode (nodeNames(), "flubsound_game"))
    {
        std::cerr << "    (skipped: this server already has Flubsound's sinks)\n";
        return;
    }

    // Stands in for the headset: the engine is moved here once it runs.
    const auto out = unique ("app_out");
    auto sinkNode = NativeAudioNode::create();
    TestCallback sinkCallback (false);
    std::string error;
    REQUIRE (sinkNode->start (testConfig ("appsink", { { "Out", out, "Flubsound test app output", { "FL", "FR" } } }, ""), sinkCallback, error));

    // A fresh install: nothing saved, a real device session.
    const flubapptest::TempFolder temp;
    EngineController::Options o;
    o.openAudioDevice = true;
    o.restoreState = false;
    o.enableAppRouting = false; // started below with a recording router
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<ForegroundApp>(); };
    o.antiCheatServices = [] { return std::vector<std::string>(); };
    o.outputEndpoints = [] { return std::vector<OutputEndpointIdentity>(); };
    o.endpointVolumeReader = [] (const std::string&) { return EndpointVolume {}; };
    EngineController controller (o);
    auto& host = controller.getHost();
    auto& manager = controller.getDeviceManager();

    // Offered after JUCE's own types, and the first-run choice when a
    // PipeWire server answers.
    bool offered = false;
    for (auto* type : manager.getAvailableDeviceTypes())
        offered = offered || type->getTypeName() == pipewire::kDeviceTypeName;
    CHECK (offered);
    CHECK (manager.getAvailableDeviceTypes().getFirst()->getTypeName() != pipewire::kDeviceTypeName);
    auto* device = manager.getCurrentAudioDevice();
    REQUIRE (device != nullptr);
    CHECK (device->getTypeName() == pipewire::kDeviceTypeName);
    CHECK (host.isNativeNodeDevice());
    CHECK (host.createDeviceStateXml() == nullptr); // not chosen: nothing is persisted until the user picks a device
    CHECK (pipewire::setDeviceOutputTarget (device, out));

    // Every strip reads its own sink: Game 0-7, Music 8-9, Chat 10-11, System 12-13.
    CHECK (device->getActiveInputChannels().countNumberOfSetBits() == 14);
    const auto map = host.getDeviceInputMap();
    CHECK (map[0] == 0);
    CHECK (map[1] == 8);
    CHECK (map[2] == 10);
    CHECK (map[3] == 12);

    // The router is given nothing to link: the node links its own sinks.
    auto recorder = std::make_unique<InputRecorder>();
    auto* recorded = recorder.get();
    controller.getRouting().setRouter (std::move (recorder), false);
    controller.getRouting().start();

    // Tones into flubsound_game and flubsound_chat reach those strips, and
    // processed audio arrives at the output sink.
    auto gamePlayer = NativeAudioNode::create();
    auto chatPlayer = NativeAudioNode::create();
    TestCallback gameTone (true), chatTone (true);
    REQUIRE (gamePlayer->start (testConfig ("gameplayer", { { "Src", unique ("gp_in"), "x", { "FL", "FR" } } }, "flubsound_game"), gameTone, error));
    REQUIRE (chatPlayer->start (testConfig ("chatplayer", { { "Src", unique ("cp_in"), "x", { "FL", "FR" } } }, "flubsound_chat"), chatTone, error));
    const auto stripHears = [&] (int strip) { return (host.getMixEngine().chain (strip).meters().activeChannelMask.load() & 3u) == 3u; };
    CHECK (flubapptest::pumpMessagesUntil ([&] { return stripHears (0) && stripHears (2); }, 5000));
    CHECK (! stripHears (1)); // Music's sink is silent
    CHECK (flubapptest::pumpMessagesUntil ([&] { return sinkCallback.rms (0) > 1.0e-4f; }, 5000));
    CHECK (flubapptest::pumpMessagesUntil ([&] { return recorded->callCount() > 0; }, 5000));
    const auto inputs = recorded->lastInputs();
    CHECK (inputs.size() == 4);
    CHECK (std::all_of (inputs.begin(), inputs.end(), [] (const AppAudioRouter::EndpointInput& i) { return i.firstInputChannel < 0; }));

    // The routing panel's line.
    ui::RoutingPanel panel (controller);
    panel.setSize (340, 900);
    CHECK (flubapptest::pumpMessagesUntil ([&] {
        const auto s = host.getNativeNodeStatus();
        return s.inputLinksMade == 14 && s.outputLinksMade == 2 && s.quantumFrames != 0;
    }, 5000));
    panel.refreshLinkStatus();
    CHECK (panel.getLinkStatus().startsWith ("PipeWire: Linked 14 of 14 input channels, output to " + juce::String (out) + " (2 of 2), quantum "));
    CHECK (! panel.isLinkStatusWarning());
    std::cout << "    panel: " << panel.getLinkStatus() << "\n";

    // Low Latency: node.latency in place, the same device keeps running, and
    // the header's total carries the graph quantum (E42).
    const auto callbacksBefore = host.getStatus().callbacks;
    controller.setLatencyProfile (flub::param::LatencyProfileValue::LowLatency);
    CHECK (manager.getCurrentAudioDevice() == device);
    CHECK (flubapptest::pumpMessagesUntil ([&] { const auto q = host.getNativeNodeStatus().quantumFrames; return q != 0 && q <= 128; }, 5000));
    CHECK (host.getStatus().callbacks > callbacksBefore);
    CHECK (flubapptest::pumpMessagesUntil ([&] {
        const auto info = host.getLatencyInfo();
        return info.graphQuantumMs > 0.0 && info.graphQuantumMs <= 128.0 / 44100.0 * 1000.0 + 1.0e-6;
    }, 3000));
    const auto info = host.getLatencyInfo();
    CHECK (info.deviceInputSamples == 0);  // the node adds no device latency of its own
    CHECK (info.deviceOutputSamples == 0);
    CHECK (std::abs (info.totalMs - info.engineMs - info.graphQuantumMs) < 1.0e-9);
    std::cout << "    latency total " << info.totalMs << " ms = engine " << info.engineMs << " + graph quantum " << info.graphQuantumMs << "\n";
    panel.refreshLinkStatus();
    CHECK (panel.getLinkStatus().contains ("quantum 128/"));

    gamePlayer->stop();
    chatPlayer->stop();
    controller.shutdown();
    sinkNode->stop();
    CHECK (! hasNode (nodeNames(), "flubsound_game")); // the device's sinks went with it
}

// ---------------------------------------------------------------------------
// R1.2: libpipewire at run time, the device's xruns and time stamps, and the
// device coming back by itself.
// ---------------------------------------------------------------------------
namespace
{
/** Opens the "PipeWire" device in 'host' as a saved choice, every strip mapped
    (Game 0-7, Music 8-9, Chat 10-11, System 12-13). */
juce::String openNodeDevice (AudioEngineHost& host)
{
    std::array<int, AudioEngineHost::kMaxStrips> map {};
    map.fill (-1);
    map[0] = 0;
    map[1] = 8;
    map[2] = 10;
    map[3] = 12;
    host.setDeviceInputMap (map);
    pipewire::addDeviceType (host.getDeviceManager());
    juce::XmlElement state ("DEVICESETUP");
    state.setAttribute ("deviceType", pipewire::kDeviceTypeName);
    state.setAttribute ("audioOutputDeviceName", pipewire::kDeviceName);
    state.setAttribute ("audioInputDeviceName", pipewire::kInputDeviceName);
    state.setAttribute ("audioDeviceRate", 48000.0);
    state.setAttribute ("audioDeviceBufferSize", 256);
    return host.openDevice (&state, 14, 2);
}
} // namespace

TEST_CASE ("App: libpipewire is opened at run time - a missing library is reported and offers no PipeWire type, the installed one resolves every entry point (E48, R1.2)")
{
    // What a system without PipeWire's client library sees (PulseAudio only,
    // no sound server): a soname that does not exist.
    const auto missing = pipewire::probeLibrary ("libpipewire-0.3.so.flubtest-missing");
    CHECK (! missing.loaded);
    CHECK (missing.error.find ("is not installed") != std::string::npos);
    CHECK (missing.version.empty());

    // This machine (CI installs the library with libpipewire-0.3-dev).
    const auto& installed = pipewire::library();
    std::cout << "    " << pipewire::kLibraryName << ": " << (installed.loaded ? "loaded, version " + installed.version : installed.error) << "\n";
    juce::AudioDeviceManager manager;
    CHECK (pipewire::addDeviceType (manager) == installed.loaded); // no "PipeWire" entry without the library
    CHECK (NativeAudioNode::create()->isSupported() == installed.loaded);
    if (! installed.loaded)
    {
        CHECK (! installed.error.empty());
        CHECK (NativeAudioNode::create()->unsupportedReason() == installed.error);
        return;
    }
    CHECK (installed.error.empty());
    CHECK (! installed.version.empty());
    CHECK (pipewire::probeLibrary (pipewire::kLibraryName).loaded); // every entry point resolves
}

TEST_CASE ("App: the PipeWire device reports its xruns and the driver's time - callbacks at the graph's cadence, none late, no xruns while idle (E48, R1.2)")
{
    if (! serverAvailable() || ! isTestServer())
        return;
    if (hasNode (nodeNames(), "flubsound_game"))
    {
        std::cerr << "    (skipped: this server already has Flubsound's sinks)\n";
        return;
    }

    // The output plays into a sink this test makes, never a device.
    const auto out = unique ("xrun_out");
    auto sinkNode = NativeAudioNode::create();
    TestCallback sinkCallback (false);
    std::string error;
    REQUIRE (sinkNode->start (testConfig ("xrunsink", { { "Out", out, "Flubsound test xrun output", { "FL", "FR" } } }, ""), sinkCallback, error));

    AudioEngineHost host;
    CHECK (openNodeDevice (host).isEmpty());
    auto* device = host.getDeviceManager().getCurrentAudioDevice();
    REQUIRE (device != nullptr);
    CHECK (pipewire::setDeviceOutputTarget (device, out));
    CHECK (device->getXRunCount() >= 0); // counted (JUCE's "not supported" is -1)

    // Past the first cycles, then 0.6 s measured (the 2 s rule): one callback
    // per block of min (quantum, 256) frames, stamped by the driver's clock.
    CHECK (waitUntil ([&] { return pipewire::getDeviceStatus (device).quantumFrames != 0 && host.getStatus().callbacks > 20; }));
    const auto before = host.getStatus();
    const auto xrunsBefore = device->getXRunCount();
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for (std::chrono::milliseconds (600));
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    const auto after = host.getStatus();
    const auto node = pipewire::getDeviceStatus (device);
    const auto xrunsAfter = device->getXRunCount();
    REQUIRE (node.sampleRate != 0);
    const double block = std::min (256.0, static_cast<double> (node.quantumFrames));
    const double expected = seconds * node.sampleRate / block;
    const auto callbacks = static_cast<double> (after.callbacks - before.callbacks);
    const auto timing = after.callbackTiming.since (before.callbackTiming);
    std::cout << "    " << seconds << " s on the node: " << callbacks << " callbacks (expected " << expected << ", quantum " << node.quantumFrames << "/"
              << node.sampleRate << "), xruns " << xrunsBefore << " -> " << xrunsAfter << ", late intervals " << timing.late << ", mean interval "
              << timing.interval.meanNs() / 1.0e6 << " ms\n";
    CHECK (after.xruns == xrunsAfter); // the engine status carries the device's count
    CHECK (xrunsAfter == xrunsBefore); // none while idle
    CHECK_NEAR (callbacks, expected, expected * 0.1);
    CHECK (timing.late == 0);
    CHECK_NEAR (timing.interval.meanNs(), 1.0e9 * block / node.sampleRate, 0.1e9 * block / node.sampleRate);

    host.closeDevice();
    sinkNode->stop();
}

TEST_CASE ("App: the PipeWire device follows the default output as it is plugged in and unplugged, without a re-open (E48, R1.2)")
{
    if (! serverAvailable() || ! isTestServer())
        return;
    if (hasNode (nodeNames(), "flubsound_game"))
    {
        std::cerr << "    (skipped: this server already has Flubsound's sinks)\n";
        return;
    }
    if (std::system ("command -v pw-metadata > /dev/null 2>&1") != 0)
    {
        std::cerr << "    (skipped: needs pw-metadata)\n";
        return;
    }

    // Stand in for the speakers (they stay) and a USB headset (it comes and
    // goes). Without a second real sink nothing would be left to play to:
    // WirePlumber's fallback null sink exists only while there is no sink.
    const auto speakersSink = unique ("hotplug_speakers"), headsetSink = unique ("hotplug_headset");
    auto speakers = NativeAudioNode::create();
    auto headset = NativeAudioNode::create();
    TestCallback speakersCallback (false), headsetCallback (false);
    std::string error;
    REQUIRE (speakers->start (testConfig ("hpspeakers", { { "Out", speakersSink, "Flubsound test speakers", { "FL", "FR" } } }, ""), speakersCallback, error));
    REQUIRE (headset->start (testConfig ("hpheadset", { { "Out", headsetSink, "Flubsound test headset", { "FL", "FR" } } }, ""), headsetCallback, error));

    AudioEngineHost host;
    CHECK (openNodeDevice (host).isEmpty()); // no output target: the default output
    auto* device = host.getDeviceManager().getCurrentAudioDevice();
    REQUIRE (device != nullptr);

    // Plugged in and made the default (what the desktop's sound settings
    // write): the node's outputs move there.
    const auto sinkValue = "'{ \"name\": \"" + headsetSink + "\" }' Spa:String:JSON > /dev/null 2>&1";
    CHECK (std::system (("pw-metadata -n default 0 default.configured.audio.sink " + sinkValue).c_str()) == 0);
    CHECK (std::system (("pw-metadata -n default 0 default.audio.sink " + sinkValue).c_str()) == 0);
    CHECK (waitUntil ([&] {
        const auto s = pipewire::getDeviceStatus (device);
        return s.outputSink == headsetSink && s.outputLinksMade == 2;
    }));

    // Unplugged: the outputs go to what is left (the speakers), by themselves.
    headset->stop();
    CHECK (waitUntil ([&] {
        const auto s = pipewire::getDeviceStatus (device);
        return s.outputSink == speakersSink && s.outputLinksMade == 2;
    }));
    std::cout << "    unplugged: output to " << pipewire::getDeviceStatus (device).outputSink << "\n";
    CHECK (host.getDeviceManager().getCurrentAudioDevice() == device); // the same device, never re-opened
    CHECK (host.getStatus().running);

    if (std::system ("pw-metadata -n default -d 0 default.configured.audio.sink > /dev/null 2>&1") != 0)
        std::cerr << "    (could not clear default.configured.audio.sink)\n";
    host.closeDevice();
    speakers->stop();
}

TEST_CASE ("App: the PipeWire device comes back by itself when the server removes its node - the error reaches the host, which re-opens it (E48, R1.2)")
{
    if (! serverAvailable() || ! isTestServer())
        return;
    if (hasNode (nodeNames(), "flubsound_game"))
    {
        std::cerr << "    (skipped: this server already has Flubsound's sinks)\n";
        return;
    }
    if (std::system ("command -v pw-cli > /dev/null 2>&1") != 0)
    {
        std::cerr << "    (skipped: needs pw-cli)\n";
        return;
    }

    juce::StringArray errors; // outlives the host
    AudioEngineHost host;
    AudioEngineHost::RecoveryTiming fast;
    fast.settleMs = 100;
    fast.firstRetryMs = 100;
    fast.maxRetryMs = 400;
    host.setRecoveryTiming (fast);
    host.onDeviceError = [&errors] (const juce::String& message) { errors.add (message); };
    CHECK (openNodeDevice (host).isEmpty()); // plays to the test server's default (null) sink
    auto* device = host.getDeviceManager().getCurrentAudioDevice();
    REQUIRE (device != nullptr);
    CHECK (waitUntil ([&] { return pipewire::getDeviceStatus (device).nodeId != 0 && host.getStatus().callbacks > 10; }));
    const auto oldNode = pipewire::getDeviceStatus (device).nodeId;

    // What a patchbay's "destroy" or a crashed session manager does to it.
    const auto destroyedAt = juce::Time::getMillisecondCounterHiRes();
    CHECK (std::system (("pw-cli destroy " + std::to_string (oldNode) + " > /dev/null 2>&1").c_str()) == 0);

    // The host hears of it (the device banner) and re-opens the device by
    // itself (the E51 recovery): a new node, callbacks again.
    const bool heard = flubapptest::pumpMessagesUntil ([&] { return ! errors.isEmpty(); }, 3000);
    const auto heardAt = juce::Time::getMillisecondCounterHiRes();
    CHECK (heard);
    std::cout << "    error: " << errors.joinIntoString (" | ") << "\n";
    if (! heard)
        std::cout << "    device lastError '" << device->getLastError() << "', playing " << device->isPlaying() << ", host lastDeviceError '"
                  << host.getLastDeviceError() << "', current device " << (host.getDeviceManager().getCurrentAudioDevice() == device) << "\n";
    CHECK (errors.joinIntoString (" | ").contains ("the Flubsound node")); // "removed ..." (or "stopped ...: <error>")
    const auto mark = host.getStatus().callbacks;
    const bool back = flubapptest::pumpMessagesUntil (
        [&]
        {
            auto* now = host.getDeviceManager().getCurrentAudioDevice();
            if (now == nullptr || now->getTypeName() != pipewire::kDeviceTypeName)
                return false;
            const auto s = pipewire::getDeviceStatus (now);
            return s.nodeId != 0 && s.nodeId != oldNode && host.getStatus().callbacks > mark + 10;
        },
        3000);
    CHECK (back);
    std::cout << "    re-opened: node " << oldNode << " -> " << pipewire::getDeviceStatus (host.getDeviceManager().getCurrentAudioDevice()).nodeId
              << ", the error " << juce::roundToInt (heardAt - destroyedAt) << " ms and callbacks again "
              << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - destroyedAt) << " ms after the destroy, recovery attempts "
              << host.getRecoveryAttempts() << "\n";

    host.closeDevice();
    CHECK (! hasNode (nodeNames(), "flubsound_game")); // the device's sinks went with it
}

#endif
