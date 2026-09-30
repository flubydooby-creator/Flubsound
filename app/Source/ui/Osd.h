// Flubsound Pro - the on-screen display of hotkey and `ctl` actions (docs/11
// E56): "GAME / Boost 60%" with a level bar where the action has a level,
// shown for kVisibleMs (1.2 s) at the top centre of the primary display and
// then faded out over kFadeMs.
//
// It must never disturb a game:
// * it never takes keyboard focus or activation: a JUCE top-level window
//   with windowIsTemporary | windowIgnoresMouseClicks (X11: an
//   override-redirect window the window manager does not focus), shown with
//   toFront (false), and on Windows WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
//   WS_EX_TOOLWINDOW (click-through, no taskbar button; JUCE adds
//   WS_EX_LAYERED for the translucent window) and HWND_TOPMOST;
// * while a game runs in exclusive fullscreen (Windows:
//   SHQueryUserNotificationState == QUNS_RUNNING_D3D_FULL_SCREEN) the window
//   is not shown at all - a topmost window can knock the game out of
//   exclusive mode - and the earcon option is the feedback there;
// * it is off while Tournament mode is on (docs/11 E55): nothing is shown,
//   and a display already on screen goes at once when Tournament mode
//   comes on.
// Settings (the settings file; no parameter or preset): osd.enabled (default
// on) and osd.earcon (off / fullscreen / always, default off). The earcon
// (OsdEarconVoice) is two short blips on Flubsound's own output device, a
// second juce::AudioDeviceManager callback registered on the first earcon,
// never the system sound (which could land in a game or stream mix); not
// while the device guard holds the output silent. `flubsound-cli ctl Osd
// on|off` and `ctl Earcon ...` set them (shell/RemoteControl.cpp).
//
// Message thread only. Headless (tests): Environment::addToDesktop = false
// keeps the component off the desktop, and a fake clock drives the fade.
#pragma once

#include "engine/EngineController.h"
#include "settings/AppSettings.h"

#include "flub/common/Realtime.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <optional>

namespace flub::app::ui
{
/** The earcon: 880 Hz then 1320 Hz, 60 ms each with 5 ms ramps and 20 ms
    apart, at kPeak (-24 dBFS), on every output channel. It is ADDED to the
    device output by the AudioDeviceManager after Flubsound's own callback,
    i.e. after the master limiter: with the programme at the -1 dBTP ceiling
    the sum peaks at 0.954 (-0.4 dBFS), below full scale. trigger() is for the
    message thread; the audio callback allocates nothing and takes no lock. */
class OsdEarconVoice final : public juce::AudioIODeviceCallback
{
public:
    void trigger() noexcept { requested.fetch_add (1, std::memory_order_release); }

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) noexcept
        FLUB_NONBLOCKING override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override {}

    static constexpr float kPeak = 0.063f; // -24 dBFS
    static constexpr double kToneSeconds = 0.06, kGapSeconds = 0.02, kRampSeconds = 0.005;

private:
    std::atomic<int> requested { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    int played = 0;    // audio thread: the last request started
    int position = -1; // audio thread: samples into the earcon, -1 idle
};

class Osd final : public juce::Component,
                  private juce::Timer,
                  private EngineController::Listener
{
public:
    enum class Earcon
    {
        Off,
        Fullscreen, // only while a game runs in exclusive fullscreen
        Always
    };

    /** Where a message went. */
    enum class Outcome
    {
        Shown,      // on screen (and the earcon with Always)
        Fullscreen, // exclusive fullscreen: no window; the earcon unless Off
        Disabled,   // osd.enabled is off
        Tournament  // Tournament mode is on
    };

    struct Environment
    {
        std::function<double()> nowMs;              // default: Time::getMillisecondCounterHiRes
        std::function<bool()> exclusiveFullscreen;  // default: the Windows query (false elsewhere)
        std::function<void()> playEarcon;           // default: OsdEarconVoice on the app's device
        bool addToDesktop = true;                   // false: never a native window (headless tests)
    };

    explicit Osd (EngineController& controller);
    Osd (EngineController& controller, Environment environment);
    ~Osd() override;

    /** The feedback of a hotkey / `ctl` action ("Game: Boost 60%"): the part
        before ": " is the title, the rest the text; Boost actions carry their
        level. */
    Outcome showFeedback (HotkeyAction action, const juce::String& feedback);
    /** Feedback of a `ctl` action that is not a hotkey action (level: 0..1). */
    Outcome show (const juce::String& title, const juce::String& text, std::optional<float> level = {});

    /** A message is on screen (fading counts). */
    bool isShowingMessage() const noexcept { return phase != Phase::Hidden; }
    /** 1 while fully visible, falling to 0 over the fade. */
    float getOpacity() const noexcept { return opacity; }
    const juce::String& getTitle() const noexcept { return title; }
    const juce::String& getText() const noexcept { return text; }
    std::optional<float> getLevel() const noexcept { return level; }

    /** Advances the fade from the clock (the timer calls it). */
    void update();

    static constexpr int kVisibleMs = 1200;
    static constexpr int kFadeMs = 300;

    /** The ComponentPeer style flags of the window. */
    static int getDesktopStyleFlags() noexcept;
    /** The extended window styles set on Windows (WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW). */
    static constexpr unsigned long kWindowsExStyle = 0x08000000ul | 0x00000020ul | 0x00000080ul;
    /** The window's GWL_EXSTYLE on Windows; 0 elsewhere or without a window. */
    unsigned long getNativeExStyle() const;

    // ---- settings (the settings file) ----
    static bool getEnabled (juce::PropertiesFile& settings);
    static void setEnabled (juce::PropertiesFile& settings, bool enabled);
    static Earcon getEarcon (juce::PropertiesFile& settings);
    static void setEarcon (juce::PropertiesFile& settings, Earcon earcon);
    static juce::String getEarconName (Earcon earcon); // "off", "fullscreen", "always"
    static std::optional<Earcon> parseEarcon (const juce::String& name);

    void paint (juce::Graphics& g) override;

private:
    enum class Phase
    {
        Hidden,
        Visible,
        Fading
    };

    void timerCallback() override { update(); }
    void engineControllerChanged (EngineController::Change change) override;
    void hideNow();
    void placeOnScreen();
    void setOpacity (float newOpacity);
    double now() const;

    void playEarcon();

    EngineController& controller;
    Environment env;
    OsdEarconVoice earconVoice;
    bool earconRegistered = false;
    Phase phase = Phase::Hidden;
    double shownAtMs = 0.0;
    float opacity = 0.0f;
    juce::String title, text;
    std::optional<float> level;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Osd)
};
} // namespace flub::app::ui
