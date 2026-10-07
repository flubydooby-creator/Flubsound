// Flubsound Pro - app tests: AppKit helpers for the OSD's display case
// (test_app_osd.cpp, docs/11 E56), in AppTestSupport_mac.mm. A plain C++
// interface; macOS only. Message thread only.
#pragma once

namespace flubapptest::macos
{
/** Makes this console process a regular app (one that can be the active
    app and have a key window; the CI runner starts it as neither) and asks
    macOS to make it the active app. Pump messages, then check
    flub::app::ui::Osd::getNativeFocus(): macOS may refuse. */
void requestActivation();
/** [NSApp deactivate]: gives the active app back (after requestActivation). */
void deactivate();
/** The NSWindow holding an NSView (a JUCE peer's native handle). */
const void* windowOf (void* nsView);
/** makeKeyAndOrderFront on the NSWindow holding an NSView. */
void makeKeyAndFront (void* nsView);
} // namespace flubapptest::macos
