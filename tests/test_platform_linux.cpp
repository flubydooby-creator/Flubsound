// Tests for the Linux platform services (headless: no audio server, no
// display needed). The implementation files are compiled into this test TU
// directly so the pactl JSON parsing and argument sanitising - which live in
// an unnamed namespace - can be tested without widening the app's interface.
// On other operating systems this file compiles to nothing.
#if defined(__linux__)

#include "TestFramework.h"

#include "../app/Source/platform/PlatformServices_common.cpp"
#include "../app/Source/platform/PlatformServices_linux.cpp"

#include <thread>

using namespace flub::platform;
using namespace flubtest;

namespace
{
KeyChord chord (uint32_t modifiers, uint32_t keyCode)
{
    KeyChord c;
    c.modifiers = modifiers;
    c.keyCode = keyCode;
    return c;
}

constexpr uint32_t kF1 = 0x70;

// Trimmed real-world `pactl --format=json list sinks` output (PipeWire 1.0).
const char* const kSinksJson = R"([
  {"index":48,"state":"RUNNING","name":"alsa_output.pci-0000_00_1f.3.analog-stereo","description":"Built-in Audio Analog Stereo"},
  {"index":61,"state":"IDLE","name":"flubsound_game","description":"Flubsound Game"}
])";

// Trimmed `pactl --format=json list sink-inputs` output: two streams from one
// game (one corked), a browser, a stream without pid (a loopback module) and
// one with a string sink index.
const char* const kSinkInputsJson = R"([
  {"index":101,"driver":"PipeWire","sink":61,"corked":true,
   "properties":{"application.name":"Counter-Strike 2","application.process.id":"4242","application.process.binary":"cs2"}},
  {"index":102,"driver":"PipeWire","sink":48,"corked":false,
   "properties":{"application.name":"Counter-Strike 2","application.process.id":"4242","application.process.binary":"cs2"}},
  {"index":103,"driver":"PipeWire","sink":48,"corked":false,
   "properties":{"application.name":"Firefox","pipewire.sec.pid":"777","application.process.binary":"firefox"}},
  {"index":104,"driver":"PipeWire","sink":48,"corked":false,
   "properties":{"media.name":"loopback-1-12"}},
  {"index":105,"driver":"PipeWire","sink":"61","corked":false,
   "properties":{"application.process.id":"9001","application.process.binary":"spotify"}}
])";
} // namespace

TEST_CASE ("Platform: KeyChord::toString uses Linux modifier names and F-key numbering")
{
    CHECK (chord (KeyChord::Ctrl | KeyChord::Shift, 'M').toString() == "Ctrl+Shift+M");
    CHECK (chord (KeyChord::Super | KeyChord::Alt, kF1 + 12).toString() == "Alt+Super+F13");
    CHECK (chord (KeyChord::None, kF1).toString() == "F1");
    CHECK (chord (KeyChord::None, kF1 + 23).toString() == "F24");
    CHECK (chord (KeyChord::Ctrl, '7').toString() == "Ctrl+7");
    CHECK (chord (KeyChord::None, 0).toString().empty());
    CHECK (chord (KeyChord::Ctrl, 0x20).toString() == "Ctrl+Key 0x20");
}

TEST_CASE ("Platform: chord validation rejects shortcuts that would swallow typing")
{
    CHECK (! detail::isValidChord (chord (KeyChord::None, 'A')));
    CHECK (! detail::isValidChord (chord (KeyChord::Shift, 'A')));
    CHECK (detail::isValidChord (chord (KeyChord::Ctrl, 'A')));
    CHECK (detail::isValidChord (chord (KeyChord::Alt | KeyChord::Shift, '9')));
    CHECK (detail::isValidChord (chord (KeyChord::None, kF1 + 12)));
    CHECK (! detail::isValidChord (chord (KeyChord::Ctrl, 0x20)));
    CHECK (! detail::isValidChord (chord (KeyChord::Ctrl, kF1 + 24)));
    CHECK (! detail::isValidChord (chord (1u << 7, 'A')));

    std::string reason;
    CHECK (! detail::isValidChord (chord (KeyChord::None, 'Q'), &reason));
    CHECK (! reason.empty());
}

TEST_CASE ("Platform: pactl argument whitelist blocks shell and option injection")
{
    CHECK (pactl::isSafeToken ("flubsound_game"));
    CHECK (pactl::isSafeToken ("alsa_output.pci-0000_00_1f.3.analog-stereo"));
    CHECK (pactl::isSafeToken ("bluez_output.AA_BB_CC_DD_EE_FF.1"));
    CHECK (pactl::isSafeToken ("@DEFAULT_SINK@"));
    CHECK (pactl::isSafeToken ("42"));

    CHECK (! pactl::isSafeToken (""));
    CHECK (! pactl::isSafeToken ("-h"));
    CHECK (! pactl::isSafeToken ("--server=evil"));
    CHECK (! pactl::isSafeToken ("a b"));
    CHECK (! pactl::isSafeToken ("x;rm -rf ~"));
    CHECK (! pactl::isSafeToken ("x'y"));
    CHECK (! pactl::isSafeToken ("$(id)"));
    CHECK (! pactl::isSafeToken ("`id`"));
    CHECK (! pactl::isSafeToken ("a\nb"));
    CHECK (! pactl::isSafeToken ("a|b"));
    CHECK (! pactl::isSafeToken (std::string (256, 'a')));
    CHECK (pactl::shellQuote ("flubsound_game") == "'flubsound_game'");
}

TEST_CASE ("Platform: pactl JSON is turned into one session per process")
{
    std::string error;
    std::map<uint32_t, std::string> sinkNames;
    REQUIRE (pactl::parseSinks (kSinksJson, sinkNames, error));
    CHECK (sinkNames.size() == 2);
    CHECK (sinkNames[61] == "flubsound_game");

    std::vector<pactl::SinkInput> inputs;
    REQUIRE (pactl::parseSinkInputs (kSinkInputsJson, inputs, error));
    REQUIRE (inputs.size() == 5);
    CHECK (inputs[0].index == 101 && inputs[0].processId == 4242 && inputs[0].corked);
    CHECK (inputs[2].processId == 777); // pipewire.sec.pid fallback
    CHECK (inputs[3].processId == 0);
    CHECK (inputs[4].sinkIndex == 61); // sink index given as a string

    // Pretend we are the Spotify process: our own streams must be hidden.
    const auto sessions = pactl::toSessions (inputs, sinkNames, 9001);
    REQUIRE (sessions.size() == 2);

    CHECK (sessions[0].processId == 4242);
    CHECK (sessions[0].executableName == "cs2");
    CHECK (sessions[0].displayName == "Counter-Strike 2");
    CHECK (sessions[0].isActive);
    // The playing stream (on the hardware sink) wins over the corked one.
    CHECK (sessions[0].currentEndpointId == "alsa_output.pci-0000_00_1f.3.analog-stereo");

    CHECK (sessions[1].processId == 777);
    CHECK (sessions[1].displayName == "Firefox");

    // Unknown sink index falls back to the numeric id.
    const auto withoutNames = pactl::toSessions (inputs, {}, 1);
    REQUIRE (withoutNames.size() == 3);
    CHECK (withoutNames[2].currentEndpointId == "61");
    CHECK (withoutNames[2].displayName == "spotify"); // no application.name -> binary
}

TEST_CASE ("Platform: malformed pactl output is reported, not crashed on")
{
    std::string error;
    std::vector<pactl::SinkInput> inputs;
    CHECK (! pactl::parseSinkInputs ("pactl: unrecognized option '--format=json'", inputs, error));
    CHECK (! error.empty());

    error.clear();
    CHECK (! pactl::parseSinkInputs (R"({"index":1})", inputs, error));
    CHECK (! error.empty());

    // Entries with junk ids are skipped, valid ones kept.
    inputs.clear();
    CHECK (pactl::parseSinkInputs (R"([{"index":-1},{"index":"x"},{"index":1.5},{"index":7,"properties":{"application.process.id":"12abc"}}])",
                                   inputs,
                                   error));
    REQUIRE (inputs.size() == 1);
    CHECK (inputs[0].index == 7);
    CHECK (inputs[0].processId == 0);
}

TEST_CASE ("Platform: unsupported Linux services report themselves as such")
{
    auto hotkeys = GlobalHotkeys::create();
    REQUIRE (hotkeys != nullptr);
    CHECK (! hotkeys->isSupported());
    CHECK (! hotkeys->registerHotkey (1, chord (KeyChord::Ctrl, 'G'), [] {}));
    hotkeys->unregisterAll();

    auto capture = ProcessLoopbackCapture::create();
    REQUIRE (capture != nullptr);
    CHECK (! capture->isSupported());
    std::string error;
    CHECK (! capture->start (1, true, 48000.0, 2, [] (const float*, int, int) {}, error));
    CHECK (! error.empty());
    CHECK (! capture->isRunning());

    auto router = AppAudioRouter::create();
    REQUIRE (router != nullptr);
    error.clear();
    CHECK (! router->setAppEndpoint (0, "flubsound_game", error));
    CHECK (! error.empty());
    error.clear();
    CHECK (! router->setAppEndpoint (static_cast<uint32_t> (::getpid()), "flubsound_game", error));
    CHECK (! error.empty());
    error.clear();
    CHECK (! router->setAppEndpoint (4242, "x; touch /tmp/pwned", error));
    CHECK (! error.empty());

    // Must not crash without a sound server; never lists our own process.
    for (const auto& s : router->enumerateSessions())
        CHECK (s.processId != static_cast<uint32_t> (::getpid()));
}

TEST_CASE ("Platform: SystemTuning promote/revert restores the thread's policy")
{
    CHECK (SystemTuning::disablePowerThrottling());
    SystemTuning::revertAudioThread (nullptr); // no-op

    bool restored = true;
    std::thread worker (
        [&restored]
        {
            int before = -1, after = -1;
            sched_param p {};
            pthread_getschedparam (pthread_self(), &before, &p);

            // Succeeds only with RT rights (root/CAP_SYS_NICE/rtprio limit).
            if (void* handle = SystemTuning::promoteAudioThread())
            {
                int during = -1;
                pthread_getschedparam (pthread_self(), &during, &p);
                restored = (during & ~SCHED_RESET_ON_FORK) == SCHED_FIFO;
                SystemTuning::revertAudioThread (handle);
            }

            pthread_getschedparam (pthread_self(), &after, &p);
            restored = restored && (after == before);
        });
    worker.join();
    CHECK (restored);
}

#endif // __linux__
