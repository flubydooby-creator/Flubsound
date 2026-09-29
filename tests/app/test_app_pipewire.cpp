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
// sinks (a real setup the test must not play into). Waits are hang guards
// (5 s), not timing assertions.
#if defined(__linux__) && defined(FLUB_HAS_PIPEWIRE) && FLUB_HAS_PIPEWIRE

#include "AppTestSupport.h"

#include "engine/AudioEngineHost.h"
#include "platform/PlatformServices.h"
#include "platform/pipewire/PipeWireDeviceType.h"
#include "platform/pipewire/PipeWireNative.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
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

    void nodeProcess (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numFrames) noexcept FLUB_NONBLOCKING override
    {
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
    std::array<std::atomic<float>, kChannels> inputRms {};
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

#endif
