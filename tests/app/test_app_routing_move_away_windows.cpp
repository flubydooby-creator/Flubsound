// App-level check on a real Windows PC (opt-in): docs/11 E47 / R4.5. The
// real router (PlatformServices_win.cpp: the undocumented per-app device API)
// moves the output of a test process - a child of this binary that plays
// silence on the default output - to a likely silent device (the automatic
// choice of AppRouting::chooseSilentEndpoint) and back. It reads the per-app
// device back after each step, starts a second process of the same
// executable while the first is moved (Windows keeps the device per
// executable file: it reads the moved device and its new stream must play
// there), and leaves the device as it found it, also when a check fails (a
// scope guard puts it back through a child, then stops the children). It
// prints whether an open stream follows either change. Nothing else is
// touched: the moved executable is this test binary.
//
// It changes a per-app setting for the duration, so it runs only with
// FLUB_TEST_REAL_APP_MOVE=1 (and prints what it saw); otherwise both cases
// return at once. Dump HKCU\Software\Microsoft\Internet Explorer\LowRegistry\
// Audio\PolicyConfig\PropertyStore before and after to compare.
#include "AppTestSupport.h"

#include "engine/AppRouting.h"
#include "platform/PlatformBridge.h"
#include "platform/PlatformServices.h"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#if JUCE_WINDOWS
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <audioclient.h>
    #include <mmdeviceapi.h>
#endif

using flub::app::AppRouting;

namespace
{
constexpr const char* kChildVariable = "FLUB_SILENCE_CHILD_SECONDS";

#if JUCE_WINDOWS
constexpr const char* kChildCase = "App: R4.5 Windows silence child";

template <typename T>
void release (T*& p)
{
    if (p != nullptr)
        p->Release();
    p = nullptr;
}

/** Plays silence on the default output (console role) for `seconds`, as an
    app that follows the default device does. */
bool playSilence (int seconds)
{
    // JUCE may already have made this thread an STA: COM is usable either way.
    const HRESULT com = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    if (FAILED (com) && com != RPC_E_CHANGED_MODE)
        return false;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* format = nullptr;
    bool ok = SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator),
                                           reinterpret_cast<void**> (&enumerator)))
              && SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device))
              && SUCCEEDED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (&client)))
              && SUCCEEDED (client->GetMixFormat (&format))
              && SUCCEEDED (client->Initialize (AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, format, nullptr)) // 200 ms
              && SUCCEEDED (client->GetService (__uuidof (IAudioRenderClient), reinterpret_cast<void**> (&render)));
    UINT32 frames = 0;
    ok = ok && SUCCEEDED (client->GetBufferSize (&frames));
    if (ok)
    {
        BYTE* data = nullptr;
        if (SUCCEEDED (render->GetBuffer (frames, &data)))
            render->ReleaseBuffer (frames, AUDCLNT_BUFFERFLAGS_SILENT);
        ok = SUCCEEDED (client->Start());
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG> (seconds) * 1000u;
        while (ok && GetTickCount64() < deadline)
        {
            Sleep (20);
            UINT32 padding = 0;
            if (SUCCEEDED (client->GetCurrentPadding (&padding)) && padding < frames
                && SUCCEEDED (render->GetBuffer (frames - padding, &data)))
                render->ReleaseBuffer (frames - padding, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        client->Stop();
    }
    if (format != nullptr)
        CoTaskMemFree (format);
    release (render);
    release (client);
    release (device);
    release (enumerator);
    if (SUCCEEDED (com))
        CoUninitialize();
    return ok;
}

void setChildVariable (const juce::String& value)
{
    _putenv_s (kChildVariable, value.toRawUTF8());
}

juce::String describe (const flub::platform::AudioSessionInfo& s)
{
    juce::StringArray active;
    for (const auto& e : s.activeEndpointIds)
        active.add (juce::String (e));
    return "pid " + juce::String (s.processId) + " current " + juce::String (s.currentEndpointId) + " active [" + active.joinIntoString (", ")
           + "]";
}
#endif
} // namespace

TEST_CASE ("App: R4.5 Windows silence child (acts only as the child process of the real per-app device test)")
{
    const auto seconds = juce::SystemStats::getEnvironmentVariable (kChildVariable, {}).getIntValue();
    if (seconds <= 0)
        return;
#if JUCE_WINDOWS
    CHECK (playSilence (seconds));
#endif
}

TEST_CASE ("App: R4.5 Windows: the real per-app device move of a test process and back (opt-in: FLUB_TEST_REAL_APP_MOVE=1)")
{
    if (juce::SystemStats::getEnvironmentVariable ("FLUB_TEST_REAL_APP_MOVE", {}) != "1")
        return;
#if JUCE_WINDOWS
    auto router = flub::app::platform_bridge::createAppAudioRouter();
    REQUIRE (router != nullptr);
    REQUIRE (router->canMoveAppOutput());

    const auto endpoints = router->listOutputEndpoints();
    REQUIRE (! endpoints.empty());
    for (const auto& e : endpoints)
        std::printf ("  endpoint %s  %s%s\n", e.id.c_str(), e.name.c_str(), &e == &endpoints.front() ? "  (default)" : "");
    // As if Flubsound played to the default device (the headset on the owner's PC).
    const auto target = AppRouting::chooseSilentEndpoint (endpoints, endpoints.front().id, {}, {}, {}, {});
    std::printf ("  silent device: %s %s\n", target.endpoint.id.c_str(), target.endpoint.name.c_str());
    REQUIRE (! target.endpoint.id.empty());

    const auto ownPath = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    const auto ownPid = static_cast<uint32_t> (GetCurrentProcessId());
    const auto findChild = [&] (uint32_t except, flub::platform::AudioSessionInfo& found)
    {
        for (const auto& s : router->enumerateSessions())
            if (s.processId != ownPid && s.processId != except && juce::File (juce::String (s.executablePath)) == juce::File (ownPath))
            {
                found = s;
                return true;
            }
        return false;
    };
    const auto startChild = [&] (juce::ChildProcess& child)
    {
        setChildVariable ("12");
        const bool started = child.start (juce::StringArray { ownPath, kChildCase }, 0);
        setChildVariable ({});
        return started;
    };
    const auto waitFor = [] (const std::function<bool()>& done, int ms)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (ms);
        while (! done() && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep (20);
        return done();
    };

    // Puts the device back and stops the children however the case ends (a
    // failed REQUIRE throws): the test binary must not stay moved. Windows
    // sets the device per executable file, through a running process of it
    // (the router refuses this very process): the children, before they stop.
    struct Cleanup
    {
        explicit Cleanup (flub::platform::AppAudioRouter& r) : router (r) {}
        ~Cleanup()
        {
            if (moved)
            {
                std::string error;
                const bool restored = (firstPid != 0 && router.setAppEndpoint (firstPid, before, error))
                                      || (secondPid != 0 && router.setAppEndpoint (secondPid, before, error));
                std::printf ("  cleanup: per-app device %s ('%s')%s%s\n", restored ? "put back" : "NOT put back", before.c_str(),
                             restored ? "" : ": ", restored ? "" : error.c_str());
            }
            first.kill();
            second.kill();
        }
        Cleanup (const Cleanup&) = delete;
        Cleanup& operator= (const Cleanup&) = delete;

        flub::platform::AppAudioRouter& router;
        juce::ChildProcess first, second;
        uint32_t firstPid = 0, secondPid = 0;
        bool moved = false;
        std::string before;
    } cleanup (*router);

    REQUIRE (startChild (cleanup.first));
    flub::platform::AudioSessionInfo a;
    REQUIRE (waitFor ([&] { return findChild (0, a) && a.isActive; }, 5000));
    cleanup.firstPid = a.processId;
    std::printf ("  child A before: %s\n", describe (a).toRawUTF8());

    std::string error;
    REQUIRE (router->getAppEndpoint (a.processId, cleanup.before, error));
    std::printf ("  child A per-app device before: '%s'\n", cleanup.before.c_str());

    // The move.
    cleanup.moved = true; // (from here on the cleanup puts it back, also when the call half-failed)
    REQUIRE (router->setAppEndpoint (a.processId, target.endpoint.id, error));
    std::string now;
    REQUIRE (router->getAppEndpoint (a.processId, now, error));
    CHECK (juce::String (now).equalsIgnoreCase (juce::String (target.endpoint.id)));
    const bool followed = waitFor ([&]
    {
        flub::platform::AudioSessionInfo s;
        return findChild (0, s) && AppRouting::playsToEndpoint (s, target.endpoint.id);
    }, 500); // (a stream that follows does so at once; the waits keep the case near 2 s)
    findChild (0, a);
    std::printf ("  child A after the move (%s): %s\n", followed ? "its stream followed" : "its stream stayed", describe (a).toRawUTF8());

    // A second process of the same executable: it reads the moved device, and
    // the stream it opens (on the default device, as an app that follows the
    // default does) plays there.
    REQUIRE (startChild (cleanup.second));
    flub::platform::AudioSessionInfo b;
    REQUIRE (waitFor ([&] { return findChild (a.processId, b) && b.isActive; }, 5000));
    cleanup.secondPid = b.processId;
    std::string secondDevice;
    REQUIRE (router->getAppEndpoint (b.processId, secondDevice, error));
    CHECK (juce::String (secondDevice).equalsIgnoreCase (juce::String (target.endpoint.id)));
    const bool secondOnTarget = waitFor ([&] { return findChild (a.processId, b) && AppRouting::playsToEndpoint (b, target.endpoint.id); }, 2000);
    std::printf ("  child B (same executable): per-app device '%s', %s\n", secondDevice.c_str(), describe (b).toRawUTF8());
    CHECK (secondOnTarget);

    // Put back as found, through the first process.
    REQUIRE (router->setAppEndpoint (a.processId, cleanup.before, error));
    cleanup.moved = false;
    std::string after;
    REQUIRE (router->getAppEndpoint (a.processId, after, error));
    CHECK (juce::String (after).equalsIgnoreCase (juce::String (cleanup.before)));
    REQUIRE (router->getAppEndpoint (b.processId, after, error));
    CHECK (juce::String (after).equalsIgnoreCase (juce::String (cleanup.before)));
    std::printf ("  per-app device after putting it back: '%s'\n", after.c_str());
    // A stream that is open stays where it is (the put-back direction, as AppRouting's watch expects).
    flub::platform::AudioSessionInfo bAfter;
    const bool bFollowedBack = waitFor ([&]
    {
        return findChild (a.processId, bAfter) && ! AppRouting::playsToEndpoint (bAfter, target.endpoint.id);
    }, 500);
    std::printf ("  child B after the put-back (%s): %s\n", bFollowedBack ? "its stream followed" : "its stream stayed on the silent device",
                 describe (bAfter).toRawUTF8());
#endif
}
