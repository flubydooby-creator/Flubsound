// App-level tests: docs/11 E47's route journal on Linux, against a real
// PipeWire server with WirePlumber and pipewire-pulse. A pw-play stream is
// moved to a Game sink by a child flub_app_tests process through the real
// pactl router; the child is killed with SIGKILL; the restart moves the
// stream back to the default output, and WirePlumber does not send the
// application's next stream to the Game sink again (its restore-stream entry
// is gone) nor pin it to the device that was the default at the time.
//
// Needs pactl, pw-play and pw-metadata, a PipeWire server with pipewire-pulse
// and WirePlumber, and a test server: one without ALSA or Bluetooth devices,
// because the test creates null sinks and changes the default output (a
// headless server as in platform/linux/README.md, "Testing against a
// headless PipeWire", and CI's pipewire job). Otherwise it prints "skipped".
// Every sink, file and media role it uses carries this process's pid.
#if defined(__linux__)

#include "AppTestSupport.h"

#include "engine/AppRouting.h"
#include "engine/AudioEngineHost.h"
#include "flub/io/Json.h"
#include "platform/PlatformBridge.h"
#include "settings/AppSettings.h"

#include <juce_core/juce_core.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace flub::app;

namespace
{
constexpr const char* kChildCase = "App: E47 Linux route journal child";
constexpr const char* kDirVariable = "FLUB_ROUTE_PW_DIR";
constexpr const char* kSinkVariable = "FLUB_ROUTE_PW_SINK";
constexpr const char* kExeVariable = "FLUB_ROUTE_PW_EXE";

std::string run (const std::string& command, int* exitCode = nullptr)
{
    std::string output;
    if (FILE* pipe = ::popen (command.c_str(), "r"))
    {
        char buffer[4096];
        size_t n = 0;
        while ((n = std::fread (buffer, 1, sizeof (buffer), pipe)) > 0)
            output.append (buffer, n);
        const int status = ::pclose (pipe);
        if (exitCode != nullptr)
            *exitCode = WIFEXITED (status) ? WEXITSTATUS (status) : -1;
    }
    else if (exitCode != nullptr)
    {
        *exitCode = -1;
    }
    return output;
}

std::string trimmed (std::string s)
{
    while (! s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::string unique (const std::string& suffix) { return "flubtest" + std::to_string (::getpid()) + "_" + suffix; }

bool haveTools()
{
    // A build without the platform services has no pactl router to test.
    const auto router = platform_bridge::createAppAudioRouter();
    if (router == nullptr)
    {
        std::cerr << "    (skipped: this build has no Linux per-app router)\n";
        return false;
    }
    if (! router->canMoveEndpoint())
    {
        // The pactl router is built in but needs pactl at run time (CI's app
        // job has none; its pipewire job runs this test).
        std::cerr << "    (skipped: the per-app router needs pactl, which is not installed)\n";
        return false;
    }
    for (const char* tool : { "pactl", "pw-play", "pw-metadata", "pw-cli" })
    {
        int code = 0;
        run (std::string ("command -v ") + tool + " >/dev/null 2>&1", &code);
        if (code != 0)
        {
            std::cerr << "    (skipped: " << tool << " is not installed)\n";
            return false;
        }
    }
    int code = 0;
    run ("LC_ALL=C pactl info >/dev/null 2>&1", &code);
    if (code != 0)
    {
        std::cerr << "    (skipped: no pipewire-pulse / PulseAudio server)\n";
        return false;
    }
    if (run ("LC_ALL=C pactl info 2>/dev/null").find ("PipeWire") == std::string::npos)
    {
        std::cerr << "    (skipped: the sound server is not PipeWire)\n";
        return false;
    }
    const auto nodes = run ("LC_ALL=C pw-cli ls Node 2>/dev/null");
    if (nodes.find ("\"alsa_") != std::string::npos || nodes.find ("\"bluez_") != std::string::npos)
    {
        std::cerr << "    (skipped: this server has real devices; the test changes the default output and needs a headless test server)\n";
        return false;
    }
    if (nodes.find ("WirePlumber") == std::string::npos && run ("LC_ALL=C pw-cli ls Client 2>/dev/null").find ("WirePlumber") == std::string::npos)
    {
        std::cerr << "    (skipped: WirePlumber is not running)\n";
        return false;
    }
    return true;
}

/** One sink-input as pactl lists it, found by its media.name (pw-play's
    file, as given on its command line). */
struct Stream
{
    bool found = false;
    std::string sink; // the sink's name
    uint32_t nodeId = 0;
};

Stream findStream (const std::string& mediaName)
{
    Stream stream;
    flub::json::Value sinks, inputs;
    std::string error;
    if (! flub::json::parse (run ("LC_ALL=C pactl --format=json list sinks 2>/dev/null"), sinks, error)
        || ! flub::json::parse (run ("LC_ALL=C pactl --format=json list sink-inputs 2>/dev/null"), inputs, error))
        return stream;
    std::map<long, std::string> sinkNames;
    for (const auto& sink : sinks.asArray())
        sinkNames[static_cast<long> (sink["index"].asNumber())] = sink["name"].asString();
    for (const auto& input : inputs.asArray())
    {
        const auto& props = input["properties"];
        if (props["media.name"].asString() != mediaName)
            continue;
        stream.found = true;
        stream.sink = sinkNames[static_cast<long> (input["sink"].asNumber())];
        stream.nodeId = static_cast<uint32_t> (std::strtoul (props["object.id"].asString().c_str(), nullptr, 10));
        break;
    }
    return stream;
}

/** A 16-bit mono 48 kHz WAV of silence ('seconds' long). */
void writeSilence (const juce::File& file, int seconds)
{
    const uint32_t dataBytes = static_cast<uint32_t> (seconds) * 48000u * 2u;
    juce::MemoryOutputStream out;
    out.write ("RIFF", 4);
    out.writeInt (static_cast<int> (36 + dataBytes));
    out.write ("WAVEfmt ", 8);
    out.writeInt (16);
    out.writeShort (1);          // PCM
    out.writeShort (1);          // mono
    out.writeInt (48000);        // rate
    out.writeInt (48000 * 2);    // bytes per second
    out.writeShort (2);          // block align
    out.writeShort (16);         // bits
    out.write ("data", 4);
    out.writeInt (static_cast<int> (dataBytes));
    out.writeRepeatedByte (0, dataBytes);
    file.replaceWithData (out.getData(), out.getDataSize());
}

/** Sinks and players the test made, and the default output it changed; all
    undone when it ends, whatever happened. */
struct Scene
{
    std::string previousDefault = trimmed (run ("LC_ALL=C pactl get-default-sink 2>/dev/null"));
    std::vector<std::string> modules;
    std::vector<std::unique_ptr<juce::ChildProcess>> players;

    ~Scene()
    {
        for (auto& p : players)
            p->kill();
        for (const auto& m : modules)
            run ("LC_ALL=C pactl unload-module " + m + " >/dev/null 2>&1");
        if (! previousDefault.empty())
            run ("LC_ALL=C pactl set-default-sink '" + previousDefault + "' >/dev/null 2>&1");
    }

    bool addSink (const std::string& name)
    {
        const auto id = trimmed (run ("LC_ALL=C pactl load-module module-null-sink sink_name=" + name + " 2>/dev/null"));
        if (id.empty())
            return false;
        modules.push_back (id);
        return true;
    }

    bool play (const juce::File& wav, const std::string& role)
    {
        auto player = std::make_unique<juce::ChildProcess>();
        if (! player->start (juce::StringArray { "pw-play", "--media-role", juce::String (role), wav.getFullPathName() }, 0))
            return false;
        players.push_back (std::move (player));
        return true;
    }
};

bool waitFor (const std::function<bool()>& done, int timeoutMs = 5000)
{
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (timeoutMs);
    while (! done())
    {
        if (juce::Time::getMillisecondCounter() > deadline)
            return false;
        juce::Thread::sleep (5);
    }
    return true;
}

void setVariable (const char* name, const juce::String& value)
{
    if (value.isEmpty())
        unsetenv (name);
    else
        setenv (name, value.toRawUTF8(), 1);
}
} // namespace

TEST_CASE ("App: E47 Linux route journal child (acts only as the child process of the pw-play / kill -9 test)")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable (kDirVariable, {});
    if (dir.isEmpty())
        return;

    // The real pactl router: the executable (pw-play's client, "pw-cat") to
    // the Game strip, whose endpoint is the test's sink. Settings are never
    // saved, so the restart starts without the route.
    const juce::File folder (dir);
    AppSettings settings (folder.getChildFile ("settings.xml"), false);
    settings.setStripEndpointId ("Game", juce::SystemStats::getEnvironmentVariable (kSinkVariable, {}));
    AudioEngineHost host;
    AppRouting routing (host, settings);
    routing.setMethod (AppRouting::Method::EndpointRouting);
    routing.setRoute (juce::SystemStats::getEnvironmentVariable (kExeVariable, {}), "Game");
    // What it sees, for the parent's failure report.
    routing.onChanged = [&routing]
    {
        for (const auto& app : routing.getApps())
            std::cerr << "child: " << app.processId << " " << app.executable << " strip " << app.strip << (app.routed ? " routed" : "") << " on "
                      << app.endpointId << (app.error.isNotEmpty() ? " error: " + app.error : juce::String()) << "\n";
    };
    routing.start();
    // The parent kills this process once the stream is on the Game sink; this is only a hang guard.
    flubapptest::pumpMessagesUntil ([] { return false; }, 10000);
}

TEST_CASE ("App: E47 Linux route journal: pw-play moved to the Game sink, kill -9, restart: back on the default output, and WirePlumber does not route it again (E47, E48 headless)")
{
    if (! haveTools())
        return;

    const flubapptest::TempFolder temp;
    const auto folder = temp.file ("pw");
    REQUIRE (folder.createDirectory().wasOk());
    Scene scene;
    const auto real = unique ("real"), game = unique ("game"), other = unique ("other"), role = unique ("role");
    REQUIRE (scene.addSink (real));
    REQUIRE (scene.addSink (game));
    REQUIRE (scene.addSink (other));
    run ("LC_ALL=C pactl set-default-sink " + real + " >/dev/null 2>&1");

    // A stream of pw-play on the default output (its media.name is the file).
    const auto wav = folder.getChildFile (juce::String (unique ("first")) + ".wav");
    writeSilence (wav, 10);
    const auto firstName = wav.getFullPathName().toStdString();
    REQUIRE (scene.play (wav, role));
    REQUIRE (waitFor ([&] { return findStream (firstName).sink == real; }));

    // The child routes it to the Game sink and is killed as soon as it did.
    juce::ChildProcess child;
    setVariable (kDirVariable, folder.getFullPathName());
    setVariable (kSinkVariable, juce::String (game));
    setVariable (kExeVariable, "pw-cat"); // pw-play is pw-cat; its client names the binary
    const bool started = child.start (juce::StringArray { juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName(), kChildCase },
                                      juce::ChildProcess::wantStdErr);
    setVariable (kDirVariable, {});
    setVariable (kSinkVariable, {});
    setVariable (kExeVariable, {});
    REQUIRE (started);
    const bool moved = waitFor ([&] { return findStream (firstName).sink == game; }, 10000);
    REQUIRE (child.kill());
    CHECK (child.waitForProcessToFinish (5000));
    if (! moved)
        std::cout << child.readAllProcessOutput() << "\n" << run ("LC_ALL=C pactl list short sink-inputs; LC_ALL=C pactl list short clients") << "\n";
    REQUIRE (moved);

    // What the kill left: the stream on the Game sink, and the journal naming it.
    const auto journalFile = folder.getChildFile ("route-journal.json");
    const auto entries = RouteJournal (journalFile).load();
    REQUIRE (entries.size() == 1);
    CHECK (entries[0].executable == "pw-cat");
    CHECK (entries[0].endpoint == juce::String (game));
    CHECK (findStream (firstName).sink == game);

    // And WirePlumber remembers the move (restore-stream): the application's
    // next stream goes to the Game sink too.
    const auto wavProbe = folder.getChildFile (juce::String (unique ("probe")) + ".wav");
    writeSilence (wavProbe, 10);
    const auto probeName = wavProbe.getFullPathName().toStdString();
    REQUIRE (scene.play (wavProbe, role));
    CHECK (waitFor ([&] { return findStream (probeName).sink == game; }));

    // The restart (the route was never saved) moves it back to the default
    // output and empties the journal.
    {
        AppSettings settings (folder.getChildFile ("settings.xml"), false);
        AudioEngineHost host;
        AppRouting routing (host, settings);
        CHECK (routing.getRoutes().empty());
        routing.start();
        // The probe is another process of the recorded executable: moved back as well.
        CHECK (flubapptest::pumpMessagesUntil ([&] { return findStream (firstName).sink == real && findStream (probeName).sink == real && ! journalFile.exists(); },
                                               5000));
        routing.shutdown();
    }
    const auto first = findStream (firstName);
    CHECK (first.sink == real);

    // No move target is left on the stream ("-1" or none): it follows the
    // default output again.
    REQUIRE (first.nodeId != 0);
    const auto metadata = run ("LC_ALL=C pw-metadata 2>/dev/null");
    for (const char* key : { "target.object", "target.node" })
    {
        const auto prefix = "id:" + std::to_string (first.nodeId) + " key:'" + key + "' value:'";
        const auto at = metadata.find (prefix);
        CHECK ((at == std::string::npos || metadata.compare (at + prefix.size(), 3, "-1'") == 0));
    }

    // WirePlumber's restore-stream entry for the application is gone: with
    // another default output, the application's next stream goes there - not
    // to the Game sink, and not to the device that was the default before.
    run ("LC_ALL=C pactl set-default-sink " + other + " >/dev/null 2>&1");
    const auto wav2 = folder.getChildFile (juce::String (unique ("second")) + ".wav");
    writeSilence (wav2, 10);
    const auto secondName = wav2.getFullPathName().toStdString();
    REQUIRE (scene.play (wav2, role));
    CHECK (waitFor ([&] { return findStream (secondName).found; }));
    CHECK (waitFor ([&] { return findStream (secondName).sink == other; }, 2000));
    CHECK (findStream (secondName).sink != game);
}

#endif
