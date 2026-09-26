// Flubsound Pro - per-application routing model (executable -> strip).
//
// The user maps executables ("cs2.exe", "Spotify") to strips ("Game",
// "Music", ...). The map is persisted in AppSettings and applied with one of
// two methods (resolved per OS by getEffectiveMethod()):
//
//   EndpointRouting  flub::platform::AppAudioRouter::setAppEndpoint moves the
//                    app's audio session to the strip's virtual endpoint
//                    ("Flubsound Game" ...), whose capture side feeds the
//                    engine (device input path or a capture). Windows:
//                    persisted-default-endpoint API; Linux: move sink-inputs.
//                    Endpoints are restored to the system default on shutdown.
//   ProcessCapture   flub::platform::ProcessLoopbackCapture captures the app's
//                    process tree directly into the strip through a
//                    DriftCompensatedFifo (AudioEngineHost::startProcessCapture).
//                    macOS (process taps) and Windows 10 20348+.
//
// Running sessions are enumerated every 2 s while routes exist (or while the
// UI asked for live updates), so apps that start later are picked up.
// Message thread only.
#pragma once

#include "settings/AppSettings.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace flub::app
{
class AudioEngineHost;

class AppRouting final : private juce::Timer
{
public:
    using Method = AppSettings::RoutingMethod;

    struct AppState
    {
        uint32_t processId = 0;
        juce::String executable, displayName, endpointId;
        bool isActive = false;     // currently producing audio
        int strip = -1;            // mapped strip index, -1 = unmapped
        bool routed = false;       // endpoint routing applied by us
        int captureId = -1;        // >= 0 while a capture runs
        juce::String error;        // last routing / capture error
    };

    AppRouting (AudioEngineHost& host, AppSettings& settings);
    ~AppRouting() override;

    /** Restores endpoints, stops captures and timers. Idempotent. */
    void shutdown();

    // ---- Capabilities ------------------------------------------------------------
    bool isEndpointRoutingSupported() const noexcept;
    bool isCaptureSupported() const noexcept { return captureSupported; }
    /** True if sessions can be enumerated at all (router object available). */
    bool canEnumerateApps() const noexcept { return router != nullptr; }

    Method getMethod() const noexcept { return method; }
    void setMethod (Method newMethod);
    /** Automatic -> EndpointRouting if supported, else ProcessCapture if
        supported, else Disabled. */
    Method getEffectiveMethod() const noexcept;

    // ---- Routes ----------------------------------------------------------------------
    const std::vector<AppRoute>& getRoutes() const noexcept { return routes; }
    /** Maps an executable to a strip (by name); an empty strip name removes it. */
    void setRoute (const juce::String& executable, const juce::String& stripName);
    void removeRoute (const juce::String& executable);
    juce::String getStripNameForExecutable (const juce::String& executable) const;

    /** Case-insensitive, ignores a trailing ".exe" and any directory part. */
    static bool executablesMatch (const juce::String& a, const juce::String& b);

    // ---- Live state ------------------------------------------------------------------
    const std::vector<AppState>& getApps() const noexcept { return apps; }
    /** Enumerates sessions now and applies the routes. */
    void refresh();
    /** Keep enumerating every 2 s even without routes (e.g. while a routing
        page is visible). */
    void setLiveUpdates (bool shouldUpdate);
    void openSystemRoutingSettings();

    /** Called after refresh() when the app list or routing state changed. */
    std::function<void()> onChanged;

private:
    void timerCallback() override;
    void updateTimer();
    void persistRoutes();
    int stripIndexForName (const juce::String& stripName) const;
    void restoreAllEndpoints();
    void stopAllCaptures();

    AudioEngineHost& host;
    AppSettings& settings;
    std::unique_ptr<flub::platform::AppAudioRouter> router;
    bool captureSupported = false, liveUpdates = false, isShutDown = false;
    Method method = Method::Automatic;

    std::vector<AppRoute> routes;
    std::vector<AppState> apps;
    std::map<uint32_t, juce::String> routedEndpoints; // pid -> endpoint we assigned
    std::map<uint32_t, int> captures;                 // pid -> capture id
    std::map<uint32_t, int> captureFailures;          // pid -> failed attempts

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppRouting)
};
} // namespace flub::app
