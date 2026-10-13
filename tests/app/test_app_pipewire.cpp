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
#include "CallbackStallAttribution.h"

#include "engine/AudioEngineHost.h"
#include "engine/EngineController.h"
#include "platform/PlatformServices.h"
#include "platform/pipewire/PipeWireDeviceType.h"
#include "platform/pipewire/PipeWireLibrary.h"
#include "platform/pipewire/PipeWireNative.h"
#include "ui/RoutingPanel.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <fcntl.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
    #include <cpuid.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <mutex>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace flub::app;
using namespace flub::platform;
using namespace flubtest;
namespace stalls = flubapptest::stalls;

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
    // PipeWire server answers and plays audio (R1.2: an output sink that is
    // not Flubsound's; the test's own sink is one).
    std::string notPlaying;
    CHECK (pipewire::serverPlaysAudio (notPlaying));
    if (! notPlaying.empty())
        std::cout << "    first start: " << notPlaying << "\n";
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
// R1.2: libpipewire at run time, the device's xruns and time stamps (each
// idle xrun attributed, with the runner's steal time read around it), and
// the device coming back by itself.
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

/** A second device-manager callback that reads the steady clock (on Linux
    CLOCK_MONOTONIC, the clock of the driver's clock.nsec) in each device
    callback, right after the engine's, next to the driver time the device
    stamped it with and the device's xrun count. The stamps alone cannot show
    that callbacks ran on time: they are the driver's cycle start plus the
    block's offset (R1.2 review). It also reads the data thread's CPU time
    and its voluntary / involuntary context switches (getrusage
    RUSAGE_THREAD), so a callback that ran late can be told apart: the
    engine's own work (CPU time), a wait of its own (a voluntary switch more
    than the cycle's wait for its trigger) or the kernel's preemption. And it
    notes the CPU the thread ran on (sched_getcpu, for the runner's steal on
    that CPU) and, at its first call, the thread's id (for RunnerSampler). It
    records every callback from the moment it is added until 'recording'
    goes false (at most kCapacity), so an xrun at the edge of a measured
    window still has the callbacks around it. Preallocated; writes silence. */
class WallClockRecorder final : public juce::AudioIODeviceCallback
{
public:
    explicit WallClockRecorder (const juce::AudioIODevice& deviceToRead)
        : wallNs (kCapacity, 0), stampNs (kCapacity, 0), cpuNs (kCapacity, 0), voluntary (kCapacity, 0), involuntary (kCapacity, 0),
          xruns (kCapacity, 0), cpus (kCapacity, -1), device (deviceToRead)
    {
    }

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override
    {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
        for (int c = 0; c < numOutputs; ++c)
            if (outputs[c] != nullptr)
                std::fill (outputs[c], outputs[c] + numSamples, 0.0f);
        if (tid.load (std::memory_order_relaxed) == 0)
            tid.store (static_cast<long> (::syscall (SYS_gettid)), std::memory_order_release);
        if (! recording.load (std::memory_order_acquire))
            return;
        const auto i = count.load (std::memory_order_relaxed);
        if (i >= kCapacity)
            return;
        timespec cpu {};
        ::clock_gettime (CLOCK_THREAD_CPUTIME_ID, &cpu);
        rusage usage {};
        ::getrusage (RUSAGE_THREAD, &usage);
        wallNs[i] = static_cast<int64_t> (now);
        stampNs[i] = context.hostTimeNs != nullptr ? static_cast<int64_t> (*context.hostTimeNs) : 0;
        cpuNs[i] = static_cast<int64_t> (cpu.tv_sec) * 1000000000 + static_cast<int64_t> (cpu.tv_nsec);
        voluntary[i] = usage.ru_nvcsw;
        involuntary[i] = usage.ru_nivcsw;
        cpus[i] = ::sched_getcpu();
        // The node counts a cycle's xrun after that cycle's callbacks
        // (XrunCounter::cycleDone), so this is the count up to the previous
        // cycle: an xrun of callback i's cycle shows at callback i + 1.
        xruns[i] = device.getXRunCount();
        count.store (i + 1, std::memory_order_release);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    /** Callbacks 0 .. n - 1 as recorded (after 'recording' went false). */
    std::vector<stalls::Callback> callbacks (size_t n) const
    {
        std::vector<stalls::Callback> out (n);
        for (size_t i = 0; i < n; ++i)
        {
            out[i].wallNs = wallNs[i];
            out[i].stampNs = stampNs[i];
            out[i].cpuNs = cpuNs[i];
            out[i].voluntary = voluntary[i];
            out[i].involuntary = involuntary[i];
            out[i].xruns = xruns[i];
            out[i].cpu = cpus[i];
        }
        return out;
    }

    static constexpr size_t kCapacity = 8192;
    std::atomic<bool> recording { true };
    std::atomic<size_t> count { 0 };
    std::atomic<long> tid { 0 }; // the data thread's id, from its first callback
    std::vector<int64_t> wallNs, stampNs, cpuNs;
    std::vector<long> voluntary, involuntary;
    std::vector<int> xruns, cpus;

private:
    const juce::AudioIODevice& device;
};

int64_t threadCpuNs()
{
    timespec cpu {};
    ::clock_gettime (CLOCK_THREAD_CPUTIME_ID, &cpu);
    return static_cast<int64_t> (cpu.tv_sec) * 1000000000 + static_cast<int64_t> (cpu.tv_nsec);
}

/** The runner's counters about every millisecond, on a thread of its own at
    the lowest priority (SCHED_IDLE: it never preempts the data thread, and
    reading /proc is I/O the data thread must not do): the steal time per
    CPU and in all (/proc/stat, in USER_HZ ticks), the data thread's
    run-queue wait (/proc/self/task/<tid>/schedstat, ns) and, for
    information, the interrupts and softirqs per CPU (/proc/interrupts,
    /proc/softirqs). Preallocated. */
class RunnerSampler
{
public:
    RunnerSampler()
        : numCpus (std::clamp (static_cast<int> (::sysconf (_SC_NPROCESSORS_CONF)), 1, 1024)), wall (kCapacity, 0), total (kCapacity, 0),
          perCpu (kCapacity * static_cast<size_t> (numCpus), 0), irqs (kCapacity * static_cast<size_t> (numCpus), 0),
          softirqs (kCapacity * static_cast<size_t> (numCpus), 0), wait (kCapacity, -1), buffer (size_t { 1 } << 18, '\0'),
          columnCpu (static_cast<size_t> (numCpus) + 64, -1)
    {
    }
    ~RunnerSampler() { stop(); }

    void start (long dataThreadTid)
    {
        std::snprintf (schedstatPath, sizeof (schedstatPath), "/proc/self/task/%ld/schedstat", dataThreadTid);
        running.store (true, std::memory_order_release);
        thread = std::thread ([this] { run(); });
    }

    void stop()
    {
        running.store (false, std::memory_order_release);
        if (thread.joinable())
            thread.join();
    }

    size_t count() const { return n.load (std::memory_order_acquire); }

    /** The samples so far, for the attribution. */
    stalls::RunnerSamples samples (stalls::StealInCpuTime counted, int64_t tickNs) const
    {
        stalls::RunnerSamples s;
        s.numCpus = numCpus;
        s.tickNs = tickNs;
        s.stealInCpuTime = counted;
        const auto m = count();
        std::vector<int64_t> steal (static_cast<size_t> (numCpus)), irqCounts (steal.size()), softirqCounts (steal.size());
        for (size_t i = 0; i < m; ++i)
        {
            const auto offset = static_cast<std::ptrdiff_t> (i * static_cast<size_t> (numCpus));
            std::copy_n (perCpu.begin() + offset, numCpus, steal.begin());
            std::copy_n (irqs.begin() + offset, numCpus, irqCounts.begin());
            std::copy_n (softirqs.begin() + offset, numCpus, softirqCounts.begin());
            s.add (wall[i], total[i], steal, wait[i], irqCounts, softirqCounts);
        }
        return s;
    }

    const int numCpus;

private:
    static constexpr size_t kCapacity = 8192;

    void run()
    {
        sched_param idle {};
        ::pthread_setschedparam (::pthread_self(), SCHED_IDLE, &idle);
        while (running.load (std::memory_order_acquire))
        {
            const auto i = n.load (std::memory_order_relaxed);
            if (i >= kCapacity)
                break;
            // Stamped first: the counters read after it hold everything up to it.
            wall[i] = std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
            const auto offset = i * static_cast<size_t> (numCpus);
            if (readStat (total[i], perCpu.data() + offset))
            {
                wait[i] = readWait();
                readPerCpuTable ("/proc/interrupts", irqs.data() + offset);
                readPerCpuTable ("/proc/softirqs", softirqs.data() + offset);
                n.store (i + 1, std::memory_order_release);
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    }

    /** The whole file into 'buffer', zero-terminated. */
    bool readAll (const char* path)
    {
        const int fd = ::open (path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        size_t used = 0;
        while (used + 1 < buffer.size())
        {
            const auto got = ::read (fd, buffer.data() + used, buffer.size() - 1 - used);
            if (got <= 0)
                break;
            used += static_cast<size_t> (got);
        }
        ::close (fd);
        buffer[used] = '\0';
        return used > 0;
    }

    /** Per-CPU sums of a table with a "CPU0 CPU1 ..." header and one row per
        interrupt or softirq ("label: count count ..."; ERR and MIS hold
        one total, not per-CPU counts). */
    void readPerCpuTable (const char* path, int64_t* sums)
    {
        std::fill (sums, sums + numCpus, int64_t { 0 });
        if (! readAll (path))
            return;
        const char* p = buffer.data();
        const char* lineEnd = std::strchr (p, '\n');
        if (lineEnd == nullptr)
            return;
        size_t columns = 0;
        while (p < lineEnd && columns < columnCpu.size())
        {
            while (p < lineEnd && *p == ' ')
                ++p;
            if (std::strncmp (p, "CPU", 3) != 0)
                break;
            char* end = nullptr;
            columnCpu[columns++] = static_cast<int> (std::strtol (p + 3, &end, 10));
            p = end;
        }
        p = lineEnd + 1;
        while (*p != '\0')
        {
            lineEnd = std::strchr (p, '\n');
            if (lineEnd == nullptr)
                lineEnd = p + std::strlen (p);
            const char* colon = std::strchr (p, ':');
            if (colon != nullptr && colon < lineEnd)
            {
                const char* label = p;
                while (*label == ' ')
                    ++label;
                const bool oneTotal = std::strncmp (label, "ERR:", 4) == 0 || std::strncmp (label, "MIS:", 4) == 0;
                const char* q = colon + 1;
                for (size_t column = 0; ! oneTotal && column < columns; ++column)
                {
                    char* end = nullptr;
                    const auto value = std::strtoll (q, &end, 10);
                    if (end == q || end > lineEnd)
                        break;
                    if (columnCpu[column] >= 0 && columnCpu[column] < numCpus)
                        sums[columnCpu[column]] += value;
                    q = end;
                }
            }
            if (*lineEnd == '\0')
                break;
            p = lineEnd + 1;
        }
    }

    /** /proc/stat's "cpu" lines: the 8th value is steal. */
    bool readStat (int64_t& all, int64_t* cpus)
    {
        if (! readAll ("/proc/stat"))
            return false;
        std::fill (cpus, cpus + numCpus, int64_t { 0 });
        bool sawTotal = false;
        const char* p = buffer.data();
        while (std::strncmp (p, "cpu", 3) == 0)
        {
            p += 3;
            long index = -1;
            char* end = nullptr;
            if (*p != ' ')
            {
                index = std::strtol (p, &end, 10);
                p = end;
            }
            long long values[8] = {};
            for (auto& value : values)
            {
                value = std::strtoll (p, &end, 10);
                if (end == p)
                    break;
                p = end;
            }
            if (index < 0)
            {
                all = values[7];
                sawTotal = true;
            }
            else if (index < numCpus)
                cpus[index] = values[7];
            const char* next = std::strchr (p, '\n');
            if (next == nullptr)
                break;
            p = next + 1;
        }
        return sawTotal;
    }

    /** schedstat: time on the CPU, time waiting on a run queue, slices. */
    int64_t readWait() const
    {
        const int fd = ::open (schedstatPath, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return -1;
        char text[128];
        const auto got = ::read (fd, text, sizeof (text) - 1);
        ::close (fd);
        if (got <= 0)
            return -1;
        text[got] = '\0';
        long long onCpu = 0, waited = 0;
        return std::sscanf (text, "%lld %lld", &onCpu, &waited) == 2 ? waited : -1;
    }

    std::vector<int64_t> wall, total, perCpu, irqs, softirqs, wait;
    std::vector<char> buffer;
    std::vector<int> columnCpu; // the CPU of each column of a per-CPU table
    char schedstatPath[64] {};
    std::atomic<bool> running { false };
    std::atomic<size_t> n { 0 };
    std::thread thread;
};

std::string readTextFile (const std::string& path)
{
    std::ifstream in (path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/** Hyper-V's run time per vCPU as tools/scripts/hv-vp-runtime.py writes it
    to $FLUB_VP_RUNTIME_LOG (CI's pipewire job starts it as root): the
    samples from 'fromNs' on, into 's'. Returns what was read, or why not. */
std::string readVpRuntime (stalls::RunnerSamples& s, int64_t fromNs)
{
    const char* path = std::getenv ("FLUB_VP_RUNTIME_LOG");
    if (path == nullptr || *path == '\0')
        return "not sampled (no FLUB_VP_RUNTIME_LOG)";
    std::ifstream in (path, std::ios::binary);
    int32_t numCpus = 0;
    if (! in.read (reinterpret_cast<char*> (&numCpus), sizeof (numCpus)) || numCpus <= 0 || numCpus > 1024)
        return std::string ("not sampled (no header in ") + path + ")";
    std::vector<int32_t> ids (static_cast<size_t> (numCpus));
    if (! in.read (reinterpret_cast<char*> (ids.data()), static_cast<std::streamsize> (ids.size() * sizeof (int32_t))))
        return "not sampled (a short header)";
    const auto header = static_cast<std::streamoff> (sizeof (int32_t) * (ids.size() + 1));
    const auto record = static_cast<std::streamoff> (sizeof (int64_t) * (ids.size() + 1));
    in.seekg (0, std::ios::end);
    const auto records = (static_cast<std::streamoff> (in.tellg()) - header) / record; // a record being written is left out
    const auto tail = std::min<std::streamoff> (records, 20000);                      // about the last 20 s
    in.seekg (header + (records - tail) * record);
    std::vector<char> bytes (static_cast<size_t> (tail * record));
    if (tail <= 0 || ! in.read (bytes.data(), static_cast<std::streamsize> (bytes.size())))
        return "not sampled (no records)";
    std::vector<int64_t> runNs (static_cast<size_t> (s.numCpus), -1);
    size_t added = 0;
    for (std::streamoff i = 0; i < tail; ++i)
    {
        const char* p = bytes.data() + i * record;
        int64_t t = 0;
        std::memcpy (&t, p, sizeof (t));
        if (t < fromNs)
            continue;
        for (size_t c = 0; c < ids.size(); ++c)
        {
            uint64_t units = 0; // 100 ns
            std::memcpy (&units, p + sizeof (int64_t) * (c + 1), sizeof (units));
            if (ids[c] >= 0 && ids[c] < s.numCpus)
                runNs[static_cast<size_t> (ids[c])] = static_cast<int64_t> (units) * 100;
        }
        s.addVp (t, runNs);
        ++added;
    }
    std::ostringstream text;
    text << added << " samples of " << numCpus << " vCPUs";
    if (added > 1)
        text << ", every " << static_cast<double> (s.vpWallNs.back() - s.vpWallNs.front()) / 1.0e6 / static_cast<double> (added - 1) << " ms";
    return text.str();
}

/** What the guest can tell about the hypervisor and its own accounting. */
struct KernelFacts
{
    std::string hypervisor = "none";   // CPUID's hypervisor vendor ("Microsoft Hv": Hyper-V, Azure)
    stalls::StealInCpuTime stealInCpuTime = stalls::StealInCpuTime::Unknown;
    std::string accounting;            // why
    int64_t tickNs = 10000000;         // /proc/stat's unit
    std::string cycleCounter;          // the hardware cycle counter for the data thread
};

KernelFacts readKernelFacts (long dataThreadTid)
{
    KernelFacts f;
#if defined(__x86_64__) || defined(__i386__)
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid (1, &a, &b, &c, &d) != 0 && (c & (1u << 31)) != 0)
    {
        __cpuid (0x40000000, a, b, c, d);
        char vendor[13] {};
        std::memcpy (vendor, &b, 4);
        std::memcpy (vendor + 4, &c, 4);
        std::memcpy (vendor + 8, &d, 4);
        f.hypervisor = vendor;
    }
#else
    f.hypervisor = "unknown";
#endif
    if (const auto ticks = ::sysconf (_SC_CLK_TCK); ticks > 0)
        f.tickNs = 1000000000 / static_cast<int64_t> (ticks);
    utsname name {};
    ::uname (&name);
    const auto cmdline = readTextFile ("/proc/cmdline");
    const auto config = readTextFile (std::string ("/boot/config-") + name.release);
    if (cmdline.find ("no-steal-acc") != std::string::npos)
    {
        f.stealInCpuTime = stalls::StealInCpuTime::Yes;
        f.accounting = "booted with no-steal-acc";
    }
    else if (config.find ("CONFIG_PARAVIRT_TIME_ACCOUNTING=y") != std::string::npos)
    {
        f.stealInCpuTime = stalls::StealInCpuTime::No;
        f.accounting = "CONFIG_PARAVIRT_TIME_ACCOUNTING=y";
    }
    else if (! config.empty())
    {
        f.stealInCpuTime = stalls::StealInCpuTime::Yes;
        f.accounting = "CONFIG_PARAVIRT_TIME_ACCOUNTING not set";
    }
    else
        f.accounting = std::string ("no /boot/config-") + name.release;

    // A per-thread hardware cycle counter would count only while the vCPU
    // runs: the probe says whether this runner has one for the tests.
    perf_event_attr attr {};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof (attr);
    attr.config = PERF_COUNT_HW_CPU_CYCLES;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    const auto fd = ::syscall (__NR_perf_event_open, &attr, static_cast<pid_t> (dataThreadTid), -1, -1, 0UL);
    if (fd >= 0)
    {
        ::close (static_cast<int> (fd));
        f.cycleCounter = "available";
    }
    else
    {
        const int error = errno;
        auto paranoid = readTextFile ("/proc/sys/kernel/perf_event_paranoid");
        paranoid.erase (std::remove (paranoid.begin(), paranoid.end(), '\n'), paranoid.end());
        f.cycleCounter = std::string ("not available (") + std::strerror (error) + ", perf_event_paranoid " + paranoid + ")";
    }
    return f;
}

/** Engine slowness for the attribution to catch, in a device callback that
    runs after the engine's and before the recorder's (the same cycle): 4 ms
    of the data thread's CPU time in one cycle, a 4 ms sleep in another.
    Added after the recorder (JUCE runs the callbacks after the first in
    reverse order); before it, run 38357673366 had the work in the cycle
    after the one the recorder closed, so neither cycle finished late and
    the sleep, crossing the next trigger, replaced that cycle's wait. */
class WorkInjector final : public juce::AudioIODeviceCallback
{
public:
    explicit WorkInjector (const WallClockRecorder& recorderToFollow) : recorder (recorderToFollow) {}

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOutputs; ++c)
            if (outputs[c] != nullptr)
                std::fill (outputs[c], outputs[c] + numSamples, 0.0f);
        // The recorder runs next in this cycle: this is the index it records it at.
        const auto i = static_cast<long long> (recorder.count.load (std::memory_order_acquire));
        if (i == workAt.load (std::memory_order_relaxed))
        {
            const auto start = threadCpuNs();
            while (threadCpuNs() - start < kInjectedNs)
            {
            }
            worked.store (i, std::memory_order_release);
        }
        if (i == sleepAt.load (std::memory_order_relaxed))
        {
            std::this_thread::sleep_for (std::chrono::nanoseconds (kInjectedNs));
            slept.store (i, std::memory_order_release);
        }
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    static constexpr int64_t kInjectedNs = 4000000;
    std::atomic<long long> workAt { -1 }, sleepAt { -1 }, worked { -1 }, slept { -1 };

private:
    const WallClockRecorder& recorder;
};

/** One idle window on the PipeWire device, measured and judged. */
struct IdleRun
{
    bool ran = false; // false: skipped (no server, a server with real devices, Flubsound's sinks present)
    stalls::Window window;
    stalls::Attribution attribution;
    std::vector<std::string> failed; // the xrun case's verdict: what it failed
    int xruns = 0;                   // the device's count over the window
    long long worked = -1, slept = -1; // the injected cycles (recorder indices)
    std::string lines;               // the measurement lines the CI job prints
};

/** Opens the device on a sink of its own, records 0.6 s of idle callbacks
    (the 2 s rule) with the runner sampled throughout and judges every
    event: the PipeWire xrun case, and with 'inject' the same window with
    engine slowness injected. */
IdleRun runIdleWindow (bool inject)
{
    IdleRun run;
    if (! serverAvailable() || ! isTestServer())
        return run;
    if (hasNode (nodeNames(), "flubsound_game"))
    {
        std::cerr << "    (skipped: this server already has Flubsound's sinks)\n";
        return run;
    }
    run.ran = true;

    // The output plays into a sink this test makes, never a device.
    const auto out = unique (inject ? "inject_out" : "xrun_out");
    auto sinkNode = NativeAudioNode::create();
    TestCallback sinkCallback (false);
    std::string error;
    REQUIRE (sinkNode->start (testConfig (inject ? "injectsink" : "xrunsink", { { "Out", out, "Flubsound test xrun output", { "FL", "FR" } } }, ""), sinkCallback,
                              error));

    AudioEngineHost host;
    CHECK (openNodeDevice (host).isEmpty());
    auto* device = host.getDeviceManager().getCurrentAudioDevice();
    REQUIRE (device != nullptr);
    CHECK (pipewire::setDeviceOutputTarget (device, out));
    CHECK (device->getXRunCount() >= 0); // counted (JUCE's "not supported" is -1)
    WallClockRecorder wall (*device);
    WorkInjector injector (wall);
    struct Detach // also when a REQUIRE throws: the device must not call them once they are gone
    {
        juce::AudioDeviceManager& manager;
        std::vector<juce::AudioIODeviceCallback*> callbacks;
        ~Detach()
        {
            for (auto* callback : callbacks)
                manager.removeAudioCallback (callback);
        }
    } detach { host.getDeviceManager(), { &injector, &wall } };
    // JUCE runs the first callback (the engine's), then the others from the
    // last added back: the injector, added last, runs before the recorder,
    // so its work and waits fall in the cycle the recorder closes.
    host.getDeviceManager().addAudioCallback (&wall);
    if (inject)
        host.getDeviceManager().addAudioCallback (&injector);
    REQUIRE (waitUntil ([&] { return wall.tid.load (std::memory_order_acquire) != 0; }));
    const auto tid = wall.tid.load (std::memory_order_acquire);
    const auto facts = readKernelFacts (tid);
    RunnerSampler sampler;
    sampler.start (tid);

    // Past the first cycles (watching for a start-up xrun: the node does not
    // judge its first XrunCounter::kSettleCycles), then 0.6 s measured (the
    // 2 s rule): one callback per block of min (quantum, 256) frames. The
    // recorder and the sampler run throughout; the window is the recorder's
    // callbacks first .. last.
    long long firstXrunNear = -1; // the engine's callback count when the first xrun was seen
    CHECK (waitUntil ([&] {
        if (firstXrunNear < 0 && device->getXRunCount() > 0)
            firstXrunNear = static_cast<long long> (host.getStatus().callbacks);
        return pipewire::getDeviceStatus (device).quantumFrames != 0 && host.getStatus().callbacks > 20
               && wall.count.load (std::memory_order_acquire) > 4 && sampler.count() > 2;
    }));
    const auto first = wall.count.load (std::memory_order_acquire);
    if (inject)
    {
        injector.workAt.store (static_cast<long long> (first) + 60, std::memory_order_relaxed);   // 0.16 s into the window
        injector.sleepAt.store (static_cast<long long> (first) + 150, std::memory_order_relaxed); // 0.4 s
    }
    const auto before = host.getStatus();
    const auto xrunsBefore = device->getXRunCount();
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for (std::chrono::milliseconds (600));
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    const auto after = host.getStatus();
    const auto node = pipewire::getDeviceStatus (device);
    const auto xrunsAfter = device->getXRunCount();
    const auto last = wall.count.load (std::memory_order_acquire);
    // A few callbacks more: an xrun counted at the window's end reaches the
    // recorder one callback later, and its stall may follow it.
    CHECK (waitUntil ([&] { return wall.count.load (std::memory_order_acquire) >= last + 3; }));
    wall.recording.store (false, std::memory_order_release);
    const auto recorded = wall.count.load (std::memory_order_acquire);
    REQUIRE (node.sampleRate != 0);
    const double block = std::min (256.0, static_cast<double> (node.quantumFrames));
    const double periodNs = 1.0e9 * block / node.sampleRate;
    // The kernel adds a CPU's steal to /proc/stat at its next tick or when it
    // leaves idle: the last cycles' steal is read up to 2 periods + 4 ms on.
    const auto lagNs = static_cast<int64_t> (2.0 * periodNs) + 4000000;
    std::this_thread::sleep_for (std::chrono::nanoseconds (lagNs + 3000000));
    sampler.stop();

    const double expected = seconds * node.sampleRate / block;
    const auto callbacks = static_cast<double> (after.callbacks - before.callbacks);
    const auto timing = after.callbackTiming.since (before.callbackTiming);
    std::ostringstream lines;
    lines << "    " << seconds << " s on the node: " << callbacks << " callbacks (expected " << expected << ", quantum " << node.quantumFrames << "/"
          << node.sampleRate << "), xruns " << xrunsBefore << " -> " << xrunsAfter << " (start-up: "
          << (firstXrunNear < 0 ? std::string ("none") : "first seen near callback " + std::to_string (firstXrunNear)) << "), late intervals "
          << timing.late << ", mean interval " << timing.interval.meanNs() / 1.0e6 << " ms (driver clock)\n";
    CHECK (after.xruns == xrunsAfter); // the engine status carries the device's count
    CHECK_NEAR (callbacks, expected, expected * 0.1);
    // E45 takes the interval from the host time when the device gives one,
    // so on this device the histogram is the driver's cadence (it cannot
    // show a late wake-up; the xrun count does). The mean follows from the
    // stamps; the wall-clock checks are the independent ones.
    CHECK_NEAR (timing.interval.meanNs(), periodNs, 0.1 * periodNs);

    REQUIRE (first > 0);
    REQUIRE (last > first + 10);
    REQUIRE (recorded >= last + 3);
    auto& w = run.window;
    w.callbacks = wall.callbacks (recorded);
    w.first = first;
    w.last = last;
    w.recorded = recorded;
    w.periodNs = periodNs;
    w.unsplit = static_cast<double> (node.quantumFrames) <= block; // no quantum split: the stamps are cycle starts
    auto samples = sampler.samples (facts.stealInCpuTime, facts.tickNs);
    const auto vpRuntime = readVpRuntime (samples, w.callbacks.front().wallNs - 10000000);
    w.runner = stalls::runnerPerCycle (w.callbacks, samples, first, recorded, lagNs);
    run.attribution = stalls::attribute (w);
    const auto& a = run.attribution;
    run.xruns = xrunsAfter - xrunsBefore;
    run.failed = stalls::judge (a, w, run.xruns, timing.late, timing.overBudget);
    run.worked = injector.worked.load (std::memory_order_acquire);
    run.slept = injector.slept.load (std::memory_order_acquire);

    // Wall clock: the callbacks really ran at the graph's cadence, and each
    // ran (up to the end of the engine's work, where the recorder reads the
    // clock) before its cycle's deadline: within one quantum of the driver's
    // cycle start (no quantum split here: block = quantum <= 256). The data
    // thread's CPU time from one recorder call to the next is a cycle's own
    // work (the engine's callback and PipeWire's for the node), whatever the
    // scheduler did meanwhile, and whatever the hypervisor took that the
    // kernel charged to it (steal, below).
    const auto ms = [] (int64_t ns) { return static_cast<double> (ns) / 1.0e6; };
    lines << "    wall clock: " << last - first << " callbacks, mean interval " << a.wallMeanNs / 1.0e6 << " ms, max " << ms (a.maxIntervalNs) << " ms, "
          << a.wideIntervals << " over 1.5 periods; done after the cycle start: median " << ms (a.medianDelayNs) << " ms, max " << ms (a.maxDelayNs)
          << " ms (period " << periodNs / 1.0e6 << " ms)\n";
    lines << "    own work: the data thread's CPU time per cycle median " << ms (a.medianCpuNs) << " ms, max " << ms (a.maxCpuNs) << " ms; preempted in "
          << a.preemptedCycles << " of " << a.cycles << " cycles; voluntary switches per cycle (median) " << a.usualWaits << "; " << a.cpuOverPeriod
          << " cycles at or over the period (" << a.cpuOverRunner << " explained by steal)" << a.overs << "\n";

    // The runner over the window: steal per CPU and in all, the data
    // thread's CPUs and run-queue wait, the sampler's cadence.
    lines << "    runner: hypervisor " << facts.hypervisor << ", " << samples.numCpus << " CPUs; steal in the window (/proc/stat, " << ms (facts.tickNs)
          << " ms ticks): ";
    const auto s0 = stalls::sampleAtOrBefore (samples, w.callbacks[first].wallNs);
    const auto s1 = stalls::sampleAtOrAfter (samples, w.callbacks[last - 1].wallNs + lagNs);
    if (s0 < samples.size() && s1 < samples.size())
    {
        lines << samples.stealTotal[s1] - samples.stealTotal[s0] << " in all (per CPU";
        for (int c = 0; c < samples.numCpus; ++c)
            lines << " " << samples.steal (s1, c) - samples.steal (s0, c);
        lines << "), " << samples.stealTotal[s1] << " since boot";
        if (samples.waitNs[s0] >= 0 && samples.waitNs[s1] >= 0)
            lines << "; the data thread waited " << ms (samples.waitNs[s1] - samples.waitNs[s0]) << " ms on the run queue";
    }
    else
        lines << "not bracketed by the samples";
    int64_t maxWaitNs = -1;
    std::vector<int> threadCpus;
    std::vector<int64_t> irqCounts, softirqCounts; // per cycle, on the thread's CPU
    for (size_t k = first; k < last; ++k)
    {
        if (const auto* r = stalls::runnerOf (w, k))
        {
            maxWaitNs = std::max (maxWaitNs, r->waitNs);
            if (r->irqs >= 0)
            {
                irqCounts.push_back (r->irqs);
                softirqCounts.push_back (r->softirqs);
            }
        }
        if (w.callbacks[k].cpu >= 0 && std::find (threadCpus.begin(), threadCpus.end(), w.callbacks[k].cpu) == threadCpus.end())
            threadCpus.push_back (w.callbacks[k].cpu);
    }
    std::sort (threadCpus.begin(), threadCpus.end());
    lines << " (at most " << ms (maxWaitNs) << " ms in a cycle) on CPUs";
    for (const auto c : threadCpus)
        lines << " " << c;
    std::sort (irqCounts.begin(), irqCounts.end());
    std::sort (softirqCounts.begin(), softirqCounts.end());
    if (! irqCounts.empty())
        lines << "; interrupts / softirqs per cycle on its CPU: median " << irqCounts[irqCounts.size() / 2] << " / " << softirqCounts[softirqCounts.size() / 2]
              << ", max " << irqCounts.back() << " / " << softirqCounts.back();
    // Hyper-V: how much of the data thread's CPU time its vCPU did not run.
    int vpCycles = 0;
    int64_t hypervisorTotalNs = 0, hypervisorMaxNs = 0;
    size_t hypervisorMaxAt = 0;
    for (size_t k = first; k < last; ++k)
        if (const auto* r = stalls::runnerOf (w, k); r != nullptr && r->vpRunNs >= 0)
        {
            ++vpCycles;
            hypervisorTotalNs += r->hypervisorNs;
            if (r->hypervisorNs > hypervisorMaxNs)
            {
                hypervisorMaxNs = r->hypervisorNs;
                hypervisorMaxAt = k;
            }
        }
    lines << "; Hyper-V vCPU run time: " << vpRuntime;
    // Each vCPU's run time over the window's wall time: a halted (idle) vCPU
    // does not run, so this is how busy each one was, not 100 %.
    const auto vpFirst = static_cast<size_t> (std::lower_bound (samples.vpWallNs.begin(), samples.vpWallNs.end(), w.callbacks[first].wallNs) - samples.vpWallNs.begin());
    const auto vpEnd = static_cast<size_t> (std::upper_bound (samples.vpWallNs.begin(), samples.vpWallNs.end(), w.callbacks[last - 1].wallNs) - samples.vpWallNs.begin());
    if (vpEnd >= vpFirst + 2)
    {
        const auto vpLast = vpEnd - 1;
        const auto spanNs = static_cast<double> (samples.vpWallNs[vpLast] - samples.vpWallNs[vpFirst]);
        lines << " (busy over the window:";
        for (int c = 0; c < samples.numCpus; ++c)
            if (samples.vpRun (vpFirst, c) >= 0 && samples.vpRun (vpLast, c) >= 0)
                lines << " CPU " << c << " " << 100.0 * static_cast<double> (samples.vpRun (vpLast, c) - samples.vpRun (vpFirst, c)) / spanNs << " %";
        lines << ")";
    }
    if (vpCycles > 0)
    {
        lines << ", " << vpCycles << " cycles bounded: the hypervisor's share of the thread's CPU time at least " << ms (hypervisorTotalNs) << " ms in all, "
              << ms (hypervisorMaxNs) << " ms at most in a cycle";
        if (hypervisorMaxNs > 0)
            lines << " (callback " << hypervisorMaxAt - first << ", CPU " << ms (stalls::cycleCpu (w, hypervisorMaxAt)) << " ms)";
    }
    lines << "; reported steal in the thread's CPU time: " << stalls::describe (facts.stealInCpuTime) << " (" << facts.accounting << "); "
          << samples.size() << " samples";
    if (samples.size() > 1)
    {
        int64_t maxGap = 0;
        for (size_t i = 1; i < samples.size(); ++i)
            maxGap = std::max (maxGap, samples.wallNs[i] - samples.wallNs[i - 1]);
        lines << ", every " << ms (samples.wallNs.back() - samples.wallNs.front()) / static_cast<double> (samples.size() - 1) << " ms (max gap " << ms (maxGap)
              << " ms)";
    }
    lines << "; hardware cycle counter: " << facts.cycleCounter << "\n";

    lines << "    stall attribution: " << run.xruns << " xruns, " << timing.late << " late intervals and " << a.finishesSeen
          << " callbacks finished after their cycle in the window (" << a.xrunsSeen << ", " << a.lateSeen << " and " << a.finishesSeen << " located; "
          << a.xrunsStalled << ", " << a.lateStalled << " and " << a.finishesStalled << " a scheduling stall); the engine's callback at most "
          << ms (static_cast<int64_t> (timing.duration.maxNs)) << " ms, " << timing.overBudget << " over the period"
          << (a.xrunsSeen + a.lateSeen + a.finishesSeen == 0 ? std::string ("; none, as expected") : a.events) << "\n";
    run.lines = lines.str();
    CHECK_NEAR (a.wallMeanNs, periodNs, 0.1 * periodNs);
    if (w.unsplit)
        CHECK (a.medianDelayNs >= 0); // the stamps are on the same clock, not in the future

    host.getDeviceManager().removeAudioCallback (&wall);
    if (inject)
        host.getDeviceManager().removeAudioCallback (&injector);
    host.closeDevice();
    sinkNode->stop();
    return run;
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

TEST_CASE ("App: the PipeWire device reports its xruns and the driver's time - callbacks at the graph's cadence, none late, no xruns while idle, a runner's scheduling stall attributed (E48, R1.2)")
{
    // No xruns, no late intervals (driver clock) and every callback finished
    // within its cycle while idle: that stays the expectation. But a shared
    // CI runner can keep the node's data thread off the CPU for a period or
    // more: run 37634884865 woke it too late for one cycle (an xrun and a
    // late interval; 5.05 ms on the wall clock between two callbacks, each
    // done within 0.95 ms of its cycle's start), run 37640705514 held one
    // callback 2.5 ms or more (done 2.87 ms after its cycle's start; an
    // xrun). So each such event in the window must be a scheduling stall at
    // a callback around it: a cycle whose own work fit the period and that
    // waited for nothing but its trigger (a voluntary switch more than usual
    // is a lock or a wait in the engine), and that still ran late: more than
    // 1.5 periods after the callback before it, or finished after its
    // cycle's end while the kernel preempted it or the hypervisor took time
    // from it. A cycle's own work is its CPU time less the steal seen on its
    // CPU while the kernel charges steal to the running thread: run
    // 38030655885 had one cycle of 3.98 ms CPU time (median 0.62 ms), which
    // only steal could excuse (stalls::judge). One without such a stall, one
    // the recorder did not locate, more than 2 of a kind in 0.6 s or a cycle
    // whose own work reached the period fails (CallbackStallAttribution.h;
    // the rules on recorded numbers: test_app_stall_attribution.cpp).
    const auto run = runIdleWindow (false);
    if (! run.ran)
        return;
    std::cout << run.lines;
    for (const auto& why : run.failed)
        std::cout << "    not the runner's: " << why << "\n";
    CHECK (run.failed.empty());
}

TEST_CASE ("App: the PipeWire xrun attribution blames the engine for its own work or wait - 4 ms of CPU time or a 4 ms sleep injected into an idle callback is no scheduling stall (E48, R1.2)")
{
    // The same window with engine slowness the attribution must not excuse:
    // 4 ms of the data thread's CPU time in one cycle (no steal: real work)
    // and a 4 ms sleep in another (a wait of its own). Both finish after
    // their cycle and make xruns, and the xrun case's verdict on the window
    // must fail.
    const auto run = runIdleWindow (true);
    if (! run.ran)
        return;
    const auto& w = run.window;
    const auto& a = run.attribution;
    REQUIRE (run.worked > 0);
    REQUIRE (run.slept > 0);
    const auto worked = static_cast<size_t> (run.worked), slept = static_cast<size_t> (run.slept);
    REQUIRE (worked > w.first);
    REQUIRE (slept < w.last);
    const auto ms = [] (int64_t ns) { return static_cast<double> (ns) / 1.0e6; };
    const bool engineWork = std::find (a.engineCycles.begin(), a.engineCycles.end(), worked) != a.engineCycles.end();
    std::cout << "    injected work: 4 ms of CPU time at callback " << worked - w.first << " (CPU " << ms (stalls::cycleCpu (w, worked)) << " ms, done "
              << ms (stalls::doneAfter (w, worked)) << " ms after its cycle's start" << stalls::describeRunner (w, worked) << ": "
              << (engineWork ? "the engine's own work" : "NOT the engine's") << "), a 4 ms sleep at callback " << slept - w.first << " ("
              << stalls::waitsOf (w, slept) << " waits against " << a.usualWaits << " usual, done " << ms (stalls::doneAfter (w, slept))
              << " ms after its cycle's start: " << (stalls::isStall (w, slept, a.usualWaits) ? "a scheduling stall" : "a wait of its own") << "); "
              << run.xruns << " xruns, " << a.finishesStalled << " of " << a.finishesSeen << " late finishes and " << a.xrunsStalled << " of "
              << a.xrunsSeen << " xruns excused; the xrun case would " << (run.failed.empty() ? "pass" : "fail: " + run.failed.front()) << "\n";

    CHECK (stalls::cycleCpu (w, worked) >= WorkInjector::kInjectedNs);
    CHECK (stalls::finishedLate (w, worked));
    const bool stealDuringWork = stalls::explainedNs (w, worked) > 0;
    if (stealDuringWork)
        std::cout << "    (steal on its CPU during the injected work: nothing to conclude for it)\n";
    else
    {
        CHECK (engineWork);
        CHECK (! stalls::isStall (w, worked, a.usualWaits));
    }
    CHECK (stalls::waitsOf (w, slept) > a.usualWaits);
    CHECK (stalls::finishedLate (w, slept));
    CHECK (! stalls::isStall (w, slept, a.usualWaits));
    const int notExcused = stealDuringWork ? 1 : 2; // the injected finishes
    CHECK (a.finishesSeen >= 2);
    CHECK (a.finishesStalled <= a.finishesSeen - notExcused);
    CHECK (run.xruns >= 1);
    CHECK (! run.failed.empty());
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

TEST_CASE ("App: the PipeWire device drops its node's error when it is re-opened before the error is handled - no stale device error after a buffer change (E48, R1.2)")
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
    auto& manager = host.getDeviceManager();
    auto* device = manager.getCurrentAudioDevice();
    REQUIRE (device != nullptr);
    CHECK (waitUntil ([&] { return pipewire::getDeviceStatus (device).nodeId != 0 && host.getStatus().callbacks > 10; }));
    const auto oldNode = pipewire::getDeviceStatus (device).nodeId;

    // The node goes away; its error waits for the message thread, which this
    // test does not run yet (waitUntil only sleeps).
    CHECK (std::system (("pw-cli destroy " + std::to_string (oldNode) + " > /dev/null 2>&1").c_str()) == 0);
    CHECK (waitUntil ([&] { return pipewire::getPendingDeviceError (device).isNotEmpty(); }));
    const auto pending = pipewire::getPendingDeviceError (device);

    // Before the message thread handles it, JUCE re-opens the same device
    // object for a new buffer size (setAudioDeviceSetup: open() on the open
    // device, which closes it first): a new run with a new node. The old
    // run's error must not reach the host now (a spurious device banner and
    // recovery check).
    auto setup = manager.getAudioDeviceSetup();
    setup.bufferSize = 128;
    CHECK (manager.setAudioDeviceSetup (setup, false).isEmpty());
    CHECK (manager.getCurrentAudioDevice() == device); // the same object
    CHECK (pipewire::getPendingDeviceError (device).isEmpty());
    const auto mark = host.getStatus().callbacks;
    flubapptest::pumpMessagesUntil ([&] { return ! errors.isEmpty(); }, 400);
    std::cout << "    stale error: '" << pending << "' pending before the re-open; after it, " << errors.size() << " reached the host, node " << oldNode
              << " -> " << pipewire::getDeviceStatus (device).nodeId << ", callbacks +" << (host.getStatus().callbacks - mark) << "\n";
    CHECK (errors.isEmpty());
    CHECK (host.getLastDeviceError().isEmpty());
    CHECK (pipewire::getDeviceStatus (device).nodeId != 0);
    CHECK (pipewire::getDeviceStatus (device).nodeId != oldNode);
    CHECK (host.getStatus().callbacks > mark + 10); // the new run plays
    CHECK (device->getCurrentBufferSizeSamples() == 128);

    host.closeDevice();
    CHECK (! hasNode (nodeNames(), "flubsound_game"));
}

#endif
