#include "Osd.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <shellapi.h>
#endif

namespace flub::app::ui
{
namespace
{
constexpr const char* kEnabledKey = "osd.enabled";
constexpr const char* kEarconKey = "osd.earcon";

bool platformExclusiveFullscreen()
{
   #if JUCE_WINDOWS
    // QUNS_RUNNING_D3D_FULL_SCREEN: a Direct3D application in exclusive
    // fullscreen mode (borderless windowed games report QUNS_BUSY, where the
    // display shows normally).
    QUERY_USER_NOTIFICATION_STATE state {};
    return SUCCEEDED (SHQueryUserNotificationState (&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
   #else
    return false;
   #endif
}

/** "Boost 60%" -> 0.6 (Boost actions only). */
std::optional<float> boostLevel (HotkeyAction action, const juce::String& text)
{
    if (action != HotkeyAction::BoostUp && action != HotkeyAction::BoostDown)
        return std::nullopt;
    const auto percent = text.fromLastOccurrenceOf ("Boost ", false, false).upToFirstOccurrenceOf ("%", false, false);
    if (percent.isEmpty() || ! percent.containsOnly ("0123456789"))
        return std::nullopt;
    return juce::jlimit (0.0f, 1.0f, static_cast<float> (percent.getIntValue()) / 100.0f);
}

constexpr int kHeight = 64;
constexpr int kLevelHeight = 12; // extra height of the level bar
} // namespace

Osd::Osd (EngineController& c) : Osd (c, Environment {}) {}

Osd::Osd (EngineController& c, Environment environment) : controller (c), env (std::move (environment))
{
    // Never focus, never a click target: keyboard input stays with the game.
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setInterceptsMouseClicks (false, false);
    setOpaque (false);
    setAlwaysOnTop (true);
    setAccessible (false); // an announcement, not a control (screen readers get the tray / window state)
    setSize (300, kHeight);
    controller.addListener (this);
}

Osd::~Osd()
{
    stopTimer();
    controller.removeListener (this);
    if (earconRegistered)
        controller.getDeviceManager().removeAudioCallback (&earconVoice);
    if (isOnDesktop())
        removeFromDesktop(); // first: the peer's view lives in the panel
   #if JUCE_MAC
    osdpanel::destroy (macPanel);
   #endif
}

void Osd::playEarcon()
{
    if (env.playEarcon != nullptr)
    {
        env.playEarcon();
        return;
    }
    // Never into an output the device guard holds silent (a feedback loop).
    if (const auto safety = controller.getDeviceSafetyState(); safety.kind != DeviceSafetyState::Kind::None || safety.outputMuted)
        return;
    // Registered on the first earcon only: no cost for the default (off).
    if (! earconRegistered)
    {
        controller.getDeviceManager().addAudioCallback (&earconVoice);
        earconRegistered = true;
    }
    earconVoice.trigger();
}

// ---- earcon ------------------------------------------------------------------
void OsdEarconVoice::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr && device->getCurrentSampleRate() > 0.0)
        sampleRate.store (device->getCurrentSampleRate());
}

void OsdEarconVoice::audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                                       const juce::AudioIODeviceCallbackContext&) noexcept FLUB_NONBLOCKING
{
    // The device manager sums this into the output (or, were it the first
    // callback, uses it as the output): write every channel.
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill (outputs[ch], outputs[ch] + numSamples, 0.0f);

    if (const int r = requested.load (std::memory_order_acquire); r != played)
    {
        played = r;
        position = 0; // a new request restarts it
    }
    if (position < 0)
        return;

    const double rate = sampleRate.load();
    const int tone = static_cast<int> (kToneSeconds * rate);
    const int gap = static_cast<int> (kGapSeconds * rate);
    const double ramp = kRampSeconds * rate;
    const int total = 2 * tone + gap;
    for (int i = 0; i < numSamples; ++i, ++position)
    {
        if (position >= total)
        {
            position = -1;
            return;
        }
        const bool second = position >= tone + gap;
        if (! second && position >= tone)
            continue; // the gap
        const int local = second ? position - tone - gap : position;
        const double envelope = juce::jmin (1.0, local / ramp, (tone - local) / ramp);
        const double hz = second ? 1320.0 : 880.0;
        const auto sample = static_cast<float> (kPeak * envelope * std::sin (juce::MathConstants<double>::twoPi * hz * local / rate));
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                outputs[ch][i] += sample;
    }
}

int Osd::getDesktopStyleFlags() noexcept
{
    // windowIgnoresKeyPresses: macOS - JUCE's view refuses first responder and
    // its window key status; X11 - WM_TAKE_FOCUS is ignored (JUCE's tooltip
    // window has the same three flags).
    return juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresMouseClicks
         | juce::ComponentPeer::windowIgnoresKeyPresses;
}

unsigned long Osd::getNativeExStyle() const
{
   #if JUCE_WINDOWS
    if (auto* peer = getPeer())
        return static_cast<unsigned long> (GetWindowLongPtrW (static_cast<HWND> (peer->getNativeHandle()), GWL_EXSTYLE));
   #endif
    return 0;
}

OsdNativeWindowState Osd::getNativeWindowState() const
{
   #if JUCE_MAC
    return osdpanel::getState (macPanel);
   #elif JUCE_LINUX || JUCE_BSD
    OsdNativeWindowState state;
    if (auto* peer = getPeer())
    {
        state.inputRectangles = osdx11::countInputRectangles (peer->getNativeHandle());
        state.available = state.inputRectangles >= 0;
        state.ignoresMouseEvents = state.inputRectangles == 0;
    }
    return state;
   #else
    return {};
   #endif
}

OsdNativeFocus Osd::getNativeFocus()
{
   #if JUCE_MAC
    return osdpanel::getFocus();
   #else
    return {};
   #endif
}

double Osd::now() const
{
    return env.nowMs != nullptr ? env.nowMs() : juce::Time::getMillisecondCounterHiRes();
}

Osd::Outcome Osd::showFeedback (HotkeyAction action, const juce::String& feedback)
{
    const bool hasTitle = feedback.contains (": ");
    const auto head = hasTitle ? feedback.upToFirstOccurrenceOf (": ", false, false) : juce::String ("Flubsound");
    const auto body = hasTitle ? feedback.fromFirstOccurrenceOf (": ", false, false) : feedback;
    return show (head, body, boostLevel (action, body));
}

Osd::Outcome Osd::show (const juce::String& newTitle, const juce::String& newText, std::optional<float> newLevel)
{
    // docs/11 E55: nothing on screen while Tournament mode is on.
    if (controller.isTournamentActive())
    {
        hideNow();
        return Outcome::Tournament;
    }
    auto& settings = controller.getSettings().getPropertiesFile();
    if (! getEnabled (settings))
    {
        hideNow();
        return Outcome::Disabled;
    }

    const auto earcon = getEarcon (settings);
    const bool fullscreen = env.exclusiveFullscreen != nullptr ? env.exclusiveFullscreen() : platformExclusiveFullscreen();
    if (earcon == Earcon::Always || (earcon == Earcon::Fullscreen && fullscreen))
        playEarcon();
    if (fullscreen)
    {
        hideNow(); // a topmost window could take the game out of exclusive mode
        return Outcome::Fullscreen;
    }

    title = newTitle.toUpperCase();
    text = newText;
    level = newLevel;
    placeOnScreen();
    phase = Phase::Visible;
    shownAtMs = now();
    setOpacity (1.0f);
    if (env.addToDesktop)
    {
        if (! isOnDesktop())
        {
           #if JUCE_MAC
            // The peer goes into the OSD's own non-activating panel (OsdNative.h).
            if (macPanel == nullptr)
                macPanel = osdpanel::create();
            addToDesktop (getDesktopStyleFlags(), osdpanel::getContentView (macPanel));
            placeOnScreen(); // now the panel's frame, the component filling it
           #else
            addToDesktop (getDesktopStyleFlags());
           #endif
           #if JUCE_LINUX || JUCE_BSD
            // X11: an empty input shape, so clicks reach the window below (OsdNative.h).
            if (auto* peer = getPeer())
                osdx11::setEmptyInputShape (peer->getNativeHandle());
           #endif
           #if JUCE_WINDOWS
            if (auto* peer = getPeer())
            {
                auto hwnd = static_cast<HWND> (peer->getNativeHandle());
                SetWindowLongPtrW (hwnd, GWL_EXSTYLE, GetWindowLongPtrW (hwnd, GWL_EXSTYLE) | static_cast<LONG_PTR> (kWindowsExStyle));
                SetWindowPos (hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
           #endif
        }
        setVisible (true);
        toFront (false); // never activate
       #if JUCE_MAC
        osdpanel::orderFront (macPanel); // also while another app is active
       #endif
    }
    else
    {
        setVisible (true);
    }
    repaint();
    startTimerHz (30);
    return Outcome::Shown;
}

void Osd::update()
{
    if (phase == Phase::Hidden)
    {
        stopTimer();
        return;
    }
    const double elapsed = now() - shownAtMs;
    if (elapsed < kVisibleMs)
        return;
    if (elapsed >= kVisibleMs + kFadeMs)
    {
        hideNow();
        return;
    }
    phase = Phase::Fading;
    setOpacity (1.0f - static_cast<float> ((elapsed - kVisibleMs) / kFadeMs));
}

void Osd::hideNow()
{
    stopTimer();
    phase = Phase::Hidden;
    opacity = 0.0f;
    setVisible (false); // the window stays created (and hidden) for the next message
   #if JUCE_MAC
    osdpanel::orderOut (macPanel);
   #endif
}

void Osd::setOpacity (float newOpacity)
{
    opacity = juce::jlimit (0.0f, 1.0f, newOpacity);
    setAlpha (opacity);
}

void Osd::engineControllerChanged (EngineController::Change change)
{
    // Tournament mode switched on (by the user or an anti-cheat service)
    // takes a message on screen away at once.
    if (change == EngineController::Change::Settings && phase != Phase::Hidden && controller.isTournamentActive())
        hideNow();
}

void Osd::placeOnScreen()
{
    const auto titleWidth = juce::GlyphArrangement::getStringWidth (Theme::caption (12.0f), title);
    const auto textWidth = juce::GlyphArrangement::getStringWidth (Theme::font (20.0f, true), text);
    const int width = juce::jlimit (240, 560, juce::roundToInt (juce::jmax (titleWidth, textWidth)) + 48);
    const int height = kHeight + (level.has_value() ? kLevelHeight : 0);

    juce::Rectangle<int> area (0, 0, 1280, 720);
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        area = display->userBounds.getLargestIntegerWithin();
    const juce::Rectangle<int> bounds (area.getCentreX() - width / 2, area.getY() + area.getHeight() / 10, width, height);
   #if JUCE_MAC
    if (macPanel != nullptr && isOnDesktop())
    {
        // The panel takes the screen position, in points (JUCE's logical
        // pixels times the app's UI scale); the component fills its content.
        const auto points = bounds.toDouble() * static_cast<double> (getDesktopScaleFactor());
        osdpanel::setFrame (macPanel, points.getX(), points.getY(), points.getWidth(), points.getHeight());
        setBounds (bounds.withZeroOrigin());
        return;
    }
   #endif
    setBounds (bounds);
}

void Osd::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (Palette::tooltip.withAlpha (0.92f));
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (Palette::borderStrong);
    g.drawRoundedRectangle (r, 10.0f, 1.0f);

    auto content = getLocalBounds().reduced (20, 10);
    g.setColour (Palette::muted);
    g.setFont (Theme::caption (12.0f));
    g.drawText (title, content.removeFromTop (16), juce::Justification::centred, false);
    g.setColour (Palette::text);
    g.setFont (Theme::font (20.0f, true));
    g.drawText (text, content.removeFromTop (28), juce::Justification::centred, true);

    if (level.has_value())
    {
        auto bar = content.removeFromTop (kLevelHeight).withSizeKeepingCentre (content.getWidth(), 6).toFloat();
        g.setColour (Palette::track);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (Palette::teal);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * *level), 3.0f);
    }
}

// ---- settings ----------------------------------------------------------------
bool Osd::getEnabled (juce::PropertiesFile& settings)
{
    return settings.getBoolValue (kEnabledKey, true);
}

void Osd::setEnabled (juce::PropertiesFile& settings, bool enabled)
{
    settings.setValue (kEnabledKey, enabled);
}

Osd::Earcon Osd::getEarcon (juce::PropertiesFile& settings)
{
    return parseEarcon (settings.getValue (kEarconKey, "off")).value_or (Earcon::Off);
}

void Osd::setEarcon (juce::PropertiesFile& settings, Earcon earcon)
{
    settings.setValue (kEarconKey, getEarconName (earcon));
}

juce::String Osd::getEarconName (Earcon earcon)
{
    switch (earcon)
    {
        case Earcon::Off: return "off";
        case Earcon::Fullscreen: return "fullscreen";
        case Earcon::Always: return "always";
    }
    return "off";
}

std::optional<Osd::Earcon> Osd::parseEarcon (const juce::String& name)
{
    for (const auto earcon : { Earcon::Off, Earcon::Fullscreen, Earcon::Always })
        if (name.trim().equalsIgnoreCase (getEarconName (earcon)))
            return earcon;
    return std::nullopt;
}
} // namespace flub::app::ui
