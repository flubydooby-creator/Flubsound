// App-level tests: the on-screen display of hotkey actions (ui/Osd.h, docs/11
// E56). Headless (a fake clock, no native window): the message, its 1.2 s
// hold and 0.3 s fade, the earcon option in exclusive fullscreen (no window
// there), the switch in the settings file, and Tournament mode (docs/11
// E55): nothing shown, and a message on screen goes at once when it comes
// on. With a display (Xvfb on Linux CI, the desktop on Windows / macOS): the
// real window's style flags (on Windows its WS_EX_NOACTIVATE |
// WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW extended styles; on macOS its
// non-activating NSPanel and on X11 its empty input shape, ui/OsdNative.h),
// its place at the top centre, and that a focused window keeps the keyboard
// focus while the display comes and goes (macOS: the test process is made the
// active app with the focus holder's window key, as a game would be, and both
// must stay so).
#include "AppTestSupport.h"
#include "DisplayTestSupport.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "ui/Osd.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#if JUCE_MAC
    #include "AppTestSupport_mac.h"
#endif

using namespace flub::app;
using flub::app::ui::Osd;
using flubapptest::haveDisplay;
using flubapptest::TolerateXErrors;

namespace
{
EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.antiCheatServices = [] { return std::vector<std::string>(); };
    return o;
}

struct FakeEnvironment
{
    double clockMs = 1000.0;
    bool fullscreen = false;
    int earcons = 0;

    Osd::Environment make (bool desktop = false)
    {
        Osd::Environment env;
        env.nowMs = [this] { return clockMs; };
        env.exclusiveFullscreen = [this] { return fullscreen; };
        env.playEarcon = [this] { ++earcons; };
        env.addToDesktop = desktop;
        return env;
    }
};
} // namespace

TEST_CASE ("App: the OSD shows a hotkey's feedback for 1.2 s and fades out over 0.3 s (E56)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    FakeEnvironment fake;
    Osd osd (controller, fake.make());

    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 60%") == Osd::Outcome::Shown);
    CHECK (osd.isShowingMessage());
    CHECK (osd.getTitle() == "GAME");
    CHECK (osd.getText() == "Boost 60%");
    REQUIRE (osd.getLevel().has_value());
    CHECK_NEAR (*osd.getLevel(), 0.6, 1e-6);
    CHECK (osd.getOpacity() == 1.0f);
    CHECK (fake.earcons == 0); // the earcon is off by default

    fake.clockMs += 1190.0;
    osd.update();
    CHECK (osd.getOpacity() == 1.0f);
    fake.clockMs += 160.0; // 1350 ms: half-way through the fade
    osd.update();
    CHECK_NEAR (osd.getOpacity(), 0.5, 0.01);
    CHECK (osd.isShowingMessage());
    fake.clockMs += 160.0; // 1510 ms
    osd.update();
    CHECK (! osd.isShowingMessage());
    CHECK (! osd.isVisible());

    // A message without a strip, and one without a level.
    CHECK (osd.showFeedback (HotkeyAction::ToggleEnable, "Flubsound disabled") == Osd::Outcome::Shown);
    CHECK (osd.getTitle() == "FLUBSOUND");
    CHECK (osd.getText() == "Flubsound disabled");
    CHECK (! osd.getLevel().has_value());
    // A new message restarts the hold.
    fake.clockMs += 1000.0;
    CHECK (osd.showFeedback (HotkeyAction::ToggleNight, "Game: Night listening on") == Osd::Outcome::Shown);
    fake.clockMs += 1000.0;
    osd.update();
    CHECK (osd.getOpacity() == 1.0f);
    CHECK (osd.getText() == "Night listening on");

    // Switched off in the settings: nothing shown (the app shows a tray bubble instead).
    auto& settings = controller.getSettings().getPropertiesFile();
    CHECK (Osd::getEnabled (settings));
    Osd::setEnabled (settings, false);
    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 70%") == Osd::Outcome::Disabled);
    CHECK (! osd.isShowingMessage());
    Osd::setEnabled (settings, true);
}

TEST_CASE ("App: the OSD shows no window in exclusive fullscreen; the earcon option sounds there or always (E56)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    FakeEnvironment fake;
    Osd osd (controller, fake.make());
    auto& settings = controller.getSettings().getPropertiesFile();

    CHECK (Osd::getEarcon (settings) == Osd::Earcon::Off);
    fake.fullscreen = true;
    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 60%") == Osd::Outcome::Fullscreen);
    CHECK (! osd.isShowingMessage()); // never a topmost window over an exclusive-fullscreen game
    CHECK (fake.earcons == 0);

    Osd::setEarcon (settings, Osd::Earcon::Fullscreen);
    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 70%") == Osd::Outcome::Fullscreen);
    CHECK (fake.earcons == 1);
    fake.fullscreen = false;
    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 80%") == Osd::Outcome::Shown);
    CHECK (fake.earcons == 1); // shown: no earcon with "fullscreen"

    Osd::setEarcon (settings, Osd::Earcon::Always);
    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 90%") == Osd::Outcome::Shown);
    CHECK (fake.earcons == 2);

    CHECK (Osd::parseEarcon ("FullScreen") == Osd::Earcon::Fullscreen);
    CHECK (! Osd::parseEarcon ("sometimes").has_value());
    CHECK (Osd::getEarconName (Osd::Earcon::Always) == "always");
}

TEST_CASE ("App: the OSD is hidden in Tournament mode and goes at once when it comes on (E56, E55)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    FakeEnvironment fake;
    Osd osd (controller, fake.make());
    Osd::setEarcon (controller.getSettings().getPropertiesFile(), Osd::Earcon::Always);

    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 60%") == Osd::Outcome::Shown);
    CHECK (fake.earcons == 1);
    controller.setTournamentMode (true);
    REQUIRE (controller.isTournamentActive());
    CHECK (! osd.isShowingMessage()); // taken away at once, not after the fade
    CHECK (! osd.isVisible());

    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 70%") == Osd::Outcome::Tournament);
    CHECK (! osd.isShowingMessage());
    CHECK (fake.earcons == 1); // no earcon either

    controller.setTournamentMode (false);
    CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 80%") == Osd::Outcome::Shown);
}

TEST_CASE ("App: the OSD window is non-activating and click-through, and a focused window keeps the keyboard focus (E56, needs a display)")
{
    if (! haveDisplay())
        return; // plain Linux run without X: the Xvfb run (and Windows / macOS) cover it

    [[maybe_unused]] const TolerateXErrors tolerateXErrors; // empty off X11
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));

    // Style flags without a window.
    const int flags = Osd::getDesktopStyleFlags();
    CHECK ((flags & juce::ComponentPeer::windowIsTemporary) != 0);         // X11: override-redirect, no WM focus
    CHECK ((flags & juce::ComponentPeer::windowIgnoresMouseClicks) != 0);  // click-through
    CHECK ((flags & juce::ComponentPeer::windowIgnoresKeyPresses) != 0);   // macOS: never key, never first responder
    CHECK ((flags & juce::ComponentPeer::windowAppearsOnTaskbar) == 0);    // Windows: WS_EX_TOOLWINDOW
    CHECK (Osd::kWindowsExStyle == (0x08000000ul | 0x00000020ul | 0x00000080ul));

    // The window with the keyboard focus (the game, in real life).
    juce::Component focusHolder;
    focusHolder.setWantsKeyboardFocus (true);
    focusHolder.setBounds (40, 40, 320, 200);
    focusHolder.addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
    focusHolder.setVisible (true);
    focusHolder.toFront (true);
    focusHolder.grabKeyboardFocus();
    // The window maps asynchronously; ask again until the X server agrees.
    flubapptest::pumpMessagesUntil ([&]
                                    {
                                        auto* peer = focusHolder.getPeer();
                                        if (peer != nullptr && ! peer->isFocused())
                                            peer->grabFocus();
                                        return peer != nullptr && peer->isFocused();
                                    },
                                    1500);
    focusHolder.grabKeyboardFocus();
    REQUIRE (focusHolder.getPeer() != nullptr);
   #if JUCE_LINUX || JUCE_BSD || JUCE_MAC
    // Xvfb: XSetInputFocus always works. macOS: without a JUCEApplication
    // (this console runner) JUCE tracks the focused peer itself, and
    // grabFocus() makes it the focus holder's.
    REQUIRE (focusHolder.getPeer()->isFocused());
   #else
    if (! focusHolder.getPeer()->isFocused())
    {
        // A desktop may refuse the foreground to a background test process:
        // nothing to compare against there.
        std::cout << "    (skipped: the desktop did not give the focus holder the foreground)\n";
        focusHolder.removeFromDesktop();
        return;
    }
   #endif
    REQUIRE (focusHolder.hasKeyboardFocus (false));
   #if JUCE_MAC
    // A game is the active app with its window key. The runner starts this
    // console process as neither (no key window at all), where the OSD could
    // take neither; so make it the active app with the focus holder's window
    // key first. macOS may refuse: then say so, and the comparison below only
    // shows that the OSD changes neither.
    auto* holderView = focusHolder.getPeer()->getNativeHandle();
    const void* holderWindow = flubapptest::macos::windowOf (holderView);
    REQUIRE (holderWindow != nullptr);
    flubapptest::macos::requestActivation();
    const bool macKeyHeld = flubapptest::pumpMessagesUntil ([&]
                                                            {
                                                                const auto focusNow = Osd::getNativeFocus();
                                                                if (focusNow.appActive && focusNow.keyWindow != holderWindow)
                                                                    flubapptest::macos::makeKeyAndFront (holderView);
                                                                return focusNow.appActive && focusNow.keyWindow == holderWindow;
                                                            },
                                                            800);
    if (! macKeyHeld)
        std::cout << "    (macOS: this process did not become the active app with the focus holder's window key;"
                     " the active app / key window comparison is weak here)\n";
    REQUIRE (focusHolder.getPeer()->isFocused());
    REQUIRE (focusHolder.hasKeyboardFocus (false));
   #endif
    // macOS: whether this process is the active app and which window is key
    // (empty elsewhere); the OSD must change neither.
    const auto nativeFocus = Osd::getNativeFocus();

    FakeEnvironment fake;
    {
        Osd osd (controller, fake.make (true));
        CHECK (osd.showFeedback (HotkeyAction::BoostUp, "Game: Boost 60%") == Osd::Outcome::Shown);
        REQUIRE (osd.isOnDesktop());
        auto* peer = osd.getPeer();
        REQUIRE (peer != nullptr);
        CHECK ((peer->getStyleFlags() & juce::ComponentPeer::windowIsTemporary) != 0);
        CHECK ((peer->getStyleFlags() & juce::ComponentPeer::windowIgnoresMouseClicks) != 0);
        CHECK ((peer->getStyleFlags() & juce::ComponentPeer::windowIgnoresKeyPresses) != 0);
        CHECK (osd.isAlwaysOnTop());
        CHECK (! osd.getWantsKeyboardFocus());
       #if JUCE_WINDOWS
        CHECK ((osd.getNativeExStyle() & Osd::kWindowsExStyle) == Osd::kWindowsExStyle);
       #else
        CHECK (osd.getNativeExStyle() == 0ul);
       #endif

        // At the top centre of the primary display (macOS: the panel's
        // frame, set in Cocoa's bottom-left coordinates, read back by JUCE).
        const auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        REQUIRE (primary != nullptr);
        const auto area = primary->userBounds.getLargestIntegerWithin();
        const auto onScreen = osd.getScreenBounds();
        CHECK (std::abs (onScreen.getCentreX() - area.getCentreX()) <= 1);
        CHECK (std::abs (onScreen.getY() - (area.getY() + area.getHeight() / 10)) <= 1);
        CHECK (std::abs (onScreen.getHeight() - osd.getHeight()) <= 1);

        // Let the window system map it and deliver any focus events.
        const auto until = juce::Time::getMillisecondCounter() + 250;
        flubapptest::pumpMessagesUntil ([until] { return juce::Time::getMillisecondCounter() > until; });
        CHECK (osd.isShowing());
        CHECK (focusHolder.getPeer()->isFocused());
        CHECK (! peer->isFocused());
        CHECK (focusHolder.hasKeyboardFocus (false));
        CHECK (juce::Component::getCurrentlyFocusedComponent() == &focusHolder);
        CHECK (Osd::getNativeFocus() == nativeFocus);

        const auto native = osd.getNativeWindowState();
       #if JUCE_MAC
        // The OSD's own panel (ui/OsdNative.h): a non-activating NSPanel that
        // cannot become key or main, whose view is not the first responder,
        // click-through, on every Space and over fullscreen apps, and not
        // hidden while another app is active.
        REQUIRE (native.available);
        CHECK (native.isPanel);
        CHECK (native.nonactivating);
        CHECK (! native.canBecomeKey);
        CHECK (! native.canBecomeMain);
        CHECK (! native.isKey);
        CHECK (! native.viewIsFirstResponder);
        CHECK (native.ignoresMouseEvents);
        CHECK (native.joinsAllSpaces);
        CHECK (native.fullScreenAuxiliary);
        CHECK (! native.hidesOnDeactivate);
        CHECK (native.visible);
        CHECK (native.level == flub::app::ui::osdpanel::kWindowLevel);
        if (macKeyHeld)
        {
            CHECK (Osd::getNativeFocus().appActive);
            CHECK (Osd::getNativeFocus().keyWindow == holderWindow); // the "game" keeps the key window
        }
        const char* keyName = nativeFocus.keyWindow == nullptr      ? "none"
                              : nativeFocus.keyWindow == holderWindow ? "the focus holder's"
                                                                      : "another";
        std::cout << "    [E56] macOS: OSD panel level " << native.level << ", key " << (native.isKey ? "yes" : "no")
                  << "; this app active " << (nativeFocus.appActive ? "yes" : "no") << ", key window " << keyName
                  << ", unchanged by the OSD\n";
       #elif JUCE_LINUX || JUCE_BSD
        // X11: an empty input shape, so a click inside the display reaches
        // the window below; an ordinary JUCE window (the focus holder) has
        // its one rectangle, as the OSD had before.
        const int ordinaryRectangles = flub::app::ui::osdx11::countInputRectangles (focusHolder.getPeer()->getNativeHandle());
        std::cout << "    [E56] X11 input shape: OSD " << native.inputRectangles << " rectangles, an ordinary window "
                  << ordinaryRectangles << "\n";
        REQUIRE (native.available);
        CHECK (native.inputRectangles == 0);
        CHECK (native.ignoresMouseEvents);
        CHECK (ordinaryRectangles == 1);
        // A failed query (here BadWindow: a window that is gone) reads as
        // unknown, not as an empty shape.
        void* goneHandle = nullptr;
        {
            juce::Component gone;
            gone.setBounds (40, 300, 120, 60);
            gone.addToDesktop (0);
            REQUIRE (gone.getPeer() != nullptr);
            goneHandle = gone.getPeer()->getNativeHandle();
            gone.removeFromDesktop(); // XDestroyWindow, then XSync
        }
        const int goneRectangles = flub::app::ui::osdx11::countInputRectangles (goneHandle);
        std::cout << "    [E56] X11 input shape of a destroyed window: " << goneRectangles << " (-1: unknown)\n";
        CHECK (goneRectangles == -1);
       #else
        CHECK (! native.available);
       #endif

        // A second message and the fade to hidden: still no focus change.
        CHECK (osd.showFeedback (HotkeyAction::BoostDown, "Game: Boost 50%") == Osd::Outcome::Shown);
        fake.clockMs += Osd::kVisibleMs + Osd::kFadeMs + 1;
        osd.update();
        CHECK (! osd.isVisible());
        const auto later = juce::Time::getMillisecondCounter() + 100;
        flubapptest::pumpMessagesUntil ([later] { return juce::Time::getMillisecondCounter() > later; });
        CHECK (focusHolder.getPeer()->isFocused());
        CHECK (! peer->isFocused());
        CHECK (juce::Component::getCurrentlyFocusedComponent() == &focusHolder);
        CHECK (Osd::getNativeFocus() == nativeFocus);
       #if JUCE_MAC
        CHECK (! osd.getNativeWindowState().visible); // ordered out with the message
        if (macKeyHeld)
        {
            CHECK (Osd::getNativeFocus().keyWindow == holderWindow);
        }
       #endif
    }
    focusHolder.removeFromDesktop();
   #if JUCE_MAC
    flubapptest::macos::deactivate();
   #endif
}

TEST_CASE ("App: the OSD earcon is two short blips at -24 dBFS on the app's output, rendered without allocating or locking (E56)")
{
    flub::app::ui::OsdEarconVoice voice;
    // No device started: the default 48 kHz.
    constexpr int kBlock = 256, kChannels = 2;
    std::vector<float> left (kBlock), right (kBlock);
    float* outs[kChannels] = { left.data(), right.data() };
    const juce::AudioIODeviceCallbackContext context {};

    const auto renderBlock = [&]
    {
        std::fill (left.begin(), left.end(), 0.5f); // stale data the callback must overwrite
        std::fill (right.begin(), right.end(), 0.5f);
        flubapptest::RealtimeProbe probe;
        voice.audioDeviceIOCallbackWithContext (nullptr, 0, outs, kChannels, kBlock, context);
        CHECK (probe.allocations() == 0);
        CHECK (probe.locks() == 0);
    };

    renderBlock(); // idle: silence
    CHECK (*std::max_element (left.begin(), left.end()) == 0.0f);

    voice.trigger();
    float peak = 0.0f;
    int lastSound = -1;
    for (int b = 0; b < 40; ++b) // 213 ms
    {
        renderBlock();
        for (int i = 0; i < kBlock; ++i)
        {
            CHECK (left[(size_t) i] == right[(size_t) i]);
            peak = std::max (peak, std::abs (left[(size_t) i]));
            if (left[(size_t) i] != 0.0f)
                lastSound = b * kBlock + i;
        }
    }
    CHECK_NEAR (peak, flub::app::ui::OsdEarconVoice::kPeak, 0.002);
    CHECK_LE (peak, flub::app::ui::OsdEarconVoice::kPeak);
    // 60 + 20 + 60 ms at 48 kHz: the last sample is before 140 ms.
    CHECK_GE (lastSound, 6500);
    CHECK_LE (lastSound, 6720);
    // Summed after the master limiter's -1 dBTP ceiling: still below full scale.
    CHECK_LE (0.891f + flub::app::ui::OsdEarconVoice::kPeak, 0.96f);
}
