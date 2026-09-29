// App-level tests: the OS output volume the loudness contour follows (docs/11
// E32), platform::AudioEndpoints::queryOutputVolume. Linux runs against a
// fake pactl: a shell script put first on PATH that answers from files the
// test writes and logs its arguments, so the real popen / parse path runs
// without a sound server. The Windows (IAudioEndpointVolume) and macOS (Core
// Audio) paths are compiled on their CI jobs but not run here.
#include "AppTestSupport.h"

#if defined(__linux__) && FLUB_HAS_PLATFORM_SERVICES

    #include "platform/PlatformServices.h"

    #include <sys/stat.h>
    #include <unistd.h>

    #include <cstdlib>
    #include <filesystem>
    #include <fstream>
    #include <sstream>
    #include <string>

namespace
{
namespace fs = std::filesystem;

/** A fake `pactl` first on PATH for its lifetime. `volume` / `mute` are what
    get-sink-volume / get-sink-mute print; exit codes likewise. */
class FakePactl
{
public:
    FakePactl()
    {
        dir = fs::temp_directory_path() / ("flub-fake-pactl-" + std::to_string (::getpid()));
        fs::create_directories (dir);
        std::ofstream script (dir / "pactl");
        script << "#!/bin/sh\n"
               << "echo \"$@\" >> '" << (dir / "calls").string() << "'\n"
               << "case \"$1\" in\n"
               << "  get-sink-volume) cat '" << (dir / "volume").string() << "'; exit $(cat '" << (dir / "volume.exit").string() << "') ;;\n"
               << "  get-sink-mute) cat '" << (dir / "mute").string() << "'; exit 0 ;;\n"
               << "esac\n"
               << "echo 'No valid command specified.'; exit 1\n";
        script.close();
        ::chmod ((dir / "pactl").c_str(), 0755);
        const char* path = std::getenv ("PATH");
        savedPath = path != nullptr ? path : "";
        ::setenv ("PATH", (dir.string() + ":" + savedPath).c_str(), 1);
        answer ("", "Mute: no\n");
    }

    ~FakePactl()
    {
        ::setenv ("PATH", savedPath.c_str(), 1);
        std::error_code ignored;
        fs::remove_all (dir, ignored);
    }

    void answer (const std::string& volume, const std::string& mute, int volumeExit = 0)
    {
        std::ofstream (dir / "volume") << volume;
        std::ofstream (dir / "volume.exit") << volumeExit;
        std::ofstream (dir / "mute") << mute;
        std::error_code ignored;
        fs::remove (dir / "calls", ignored);
    }

    std::string calls() const
    {
        std::ifstream in (dir / "calls");
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

private:
    fs::path dir;
    std::string savedPath;
};

using flub::platform::AudioEndpoints;
using flub::platform::EndpointVolume;
} // namespace

TEST_CASE ("App: the Linux endpoint volume is read from the default sink through pactl (fake pactl), per channel, with mute (E32)")
{
    FakePactl pactl;
    pactl.answer ("Volume: front-left: 32768 /  50% / -18.06 dB,   front-right: 32768 /  50% / -18.06 dB\n        balance 0.00\n",
                  "Mute: no\n");
    auto v = AudioEndpoints::queryOutputVolume ("pipewire"); // JUCE's ALSA device: the default sink
    CHECK (v.known);
    CHECK_NEAR (v.volumeDb, -18.06, 1.0e-4);
    CHECK (! v.muted);
    CHECK (pactl.calls() == "get-sink-volume @DEFAULT_SINK@\nget-sink-mute @DEFAULT_SINK@\n");

    // A PipeWire / Pulse sink name is used as is; unbalanced channels read as their mean.
    pactl.answer ("Volume: front-left: 26214 /  40% / -23.88 dB,   front-right: 39321 /  60% / -13.31 dB\n        balance 0.33\n",
                  "Mute: yes\n");
    v = AudioEndpoints::queryOutputVolume ("alsa_output.usb-Headset-00.analog-stereo");
    CHECK (v.known);
    CHECK_NEAR (v.volumeDb, -18.595, 1.0e-4);
    CHECK (v.muted);
    CHECK (pactl.calls() == "get-sink-volume alsa_output.usb-Headset-00.analog-stereo\nget-sink-mute alsa_output.usb-Headset-00.analog-stereo\n");

    // 0 %: -inf dB reads as the silent floor. Mono sinks have one channel.
    pactl.answer ("Volume: mono: 0 /   0% / -inf dB\n        balance 0.00\n", "Mute: no\n");
    v = AudioEndpoints::queryOutputVolume ("");
    CHECK (v.known);
    CHECK (v.volumeDb == EndpointVolume::kSilentDb);

    // Over-amplified (> 100 %) reads above 0 dB.
    pactl.answer ("Volume: front-left: 98304 / 150% / 10.57 dB,   front-right: 98304 / 150% / 10.57 dB\n", "Mute: no\n");
    v = AudioEndpoints::queryOutputVolume ("default");
    CHECK_NEAR (v.volumeDb, 10.57, 1.0e-4);
}

TEST_CASE ("App: the Linux endpoint volume reports failures instead of a volume (fake pactl) (E32)")
{
    FakePactl pactl;
    // No sound server / no such sink: pactl's message, not a number.
    pactl.answer ("Connection failure: Connection refused\n", "", 1);
    auto v = AudioEndpoints::queryOutputVolume ("");
    CHECK (! v.known);
    CHECK (v.error.find ("Connection refused") != std::string::npos);
    // An old pactl without get-sink-volume.
    pactl.answer ("", "", 1);
    v = AudioEndpoints::queryOutputVolume ("");
    CHECK (! v.known);
    // Output that has no dB value.
    pactl.answer ("Volume: front-left: 32768 /  50%\n", "Mute: no\n");
    v = AudioEndpoints::queryOutputVolume ("");
    CHECK (! v.known);
    CHECK (v.error.find ("Unexpected pactl output") != std::string::npos);
    // A sink name that is not a safe token never reaches the shell: the default sink is read instead.
    pactl.answer ("Volume: front-left: 32768 /  50% / -18.06 dB\n", "Mute: no\n");
    v = AudioEndpoints::queryOutputVolume ("x.y';touch /tmp/flub-pwned;'");
    CHECK (v.known);
    CHECK (pactl.calls().find ("@DEFAULT_SINK@") != std::string::npos);
    CHECK (! fs::exists ("/tmp/flub-pwned"));
}

#endif
