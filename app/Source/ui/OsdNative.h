// Flubsound Pro - what the on-screen display (docs/11 E56, ui/Osd.h) needs from
// the window system beyond JUCE's peer flags. A plain C++ interface (no AppKit
// or Xlib types), so Osd.cpp stays plain C++. Message thread only.
//
// macOS (OsdNative_mac.mm). JUCE gives a top-level component an NSWindow of
// its own class that can become the key window, and makes its view the first
// responder, unless the peer has windowIgnoresKeyPresses; and a regular app's
// NSWindow does not show in another app's fullscreen Space. So the OSD's peer
// is attached (Component::addToDesktop's nativeWindowToAttachTo) to the
// content view of a panel of its own:
// * an NSPanel, NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel,
//   whose canBecomeKeyWindow and canBecomeMainWindow are NO (and
//   becomesKeyOnlyIfNeeded YES): showing it never activates Flubsound and
//   never takes the key window from the game;
// * click-through (ignoresMouseEvents);
// * on every Space and over fullscreen apps: collection behaviour
//   CanJoinAllSpaces | FullScreenAuxiliary | Stationary | IgnoresCycle, at
//   NSPopUpMenuWindowLevel (the level JUCE gives a temporary always-on-top
//   window);
// * not hidden while another app is active (hidesOnDeactivate NO; NSPanel's
//   default is YES), and ordered in with orderFrontRegardless, since
//   Flubsound is never the active app while a game runs.
// The panel is an opaque, retained NSPanel pointer.
//
// X11 (OsdNative_linux.cpp). JUCE's windowIgnoresMouseClicks only leaves the
// mouse events out of the window's event mask: the X server still gives a
// click inside the window to it (where nothing handles it), not to the window
// below. An empty input shape (the SHAPE extension 1.1, ShapeInput; libXext,
// which JUCE loads at run time too) makes it click-through.
#pragma once

namespace flub::app::ui
{
/** What the OSD's native window reports: macOS - its NSPanel; X11 - its input
    shape (available, ignoresMouseEvents and inputRectangles only). Elsewhere,
    and before the first message, available is false and the rest default. */
struct OsdNativeWindowState
{
    bool available = false;
    bool isPanel = false;              // an NSPanel
    bool nonactivating = false;        // NSWindowStyleMaskNonactivatingPanel
    bool canBecomeKey = true;          // -canBecomeKeyWindow
    bool canBecomeMain = true;         // -canBecomeMainWindow
    bool isKey = false;                // -isKeyWindow
    bool viewIsFirstResponder = false; // a view (the JUCE peer's) is the panel's first responder
    bool ignoresMouseEvents = false;   // click-through (X11: an empty input shape)
    bool joinsAllSpaces = false;       // NSWindowCollectionBehaviorCanJoinAllSpaces
    bool fullScreenAuxiliary = false;  // NSWindowCollectionBehaviorFullScreenAuxiliary
    bool hidesOnDeactivate = true;
    bool visible = false;              // -isVisible
    long level = 0;                    // -level
    int inputRectangles = -1;          // X11: rectangles of the input shape (-1 unknown)
};

/** Whether Flubsound is the active app and which window is key (macOS:
    [NSApp isActive], [NSApp keyWindow]); empty elsewhere. */
struct OsdNativeFocus
{
    bool appActive = false;
    const void* keyWindow = nullptr;

    bool operator== (const OsdNativeFocus&) const = default;
};

#if defined(__APPLE__)
namespace osdpanel
{
    /** The panel's level: NSPopUpMenuWindowLevel (kCGPopUpMenuWindowLevel,
        101 in CGWindowLevel.h; a function call in the SDK, so the test compares
        the panel's -level with this value). */
    constexpr long kWindowLevel = 101;

    /** A new, hidden panel (retained; destroy() releases it). */
    void* create();
    /** Orders the panel out and releases it. Remove the peer attached to it first. */
    void destroy (void* panel);
    /** The NSView to attach the JUCE peer to. */
    void* getContentView (void* panel);
    /** Places the panel in screen points, origin at the top left of the main
        display (JUCE's convention; Cocoa's origin is at the bottom left). */
    void setFrame (void* panel, double x, double y, double width, double height);
    /** orderFrontRegardless: shown in front of its level, the key and main
        windows and the active app unchanged. */
    void orderFront (void* panel);
    void orderOut (void* panel);
    OsdNativeWindowState getState (const void* panel);
    OsdNativeFocus getFocus();
} // namespace osdpanel
#elif ! defined(_WIN32)
namespace osdx11
{
    /** Gives the X window (a JUCE peer's native handle) an empty input shape,
        through JUCE's own display connection. False without X or without
        SHAPE 1.1 on the server. */
    bool setEmptyInputShape (void* nativeHandle);
    /** The number of rectangles in the window's input shape: 0 when
        click-through, 1 for an ordinary window; -1 when it cannot be read. */
    int countInputRectangles (void* nativeHandle);
} // namespace osdx11
#endif
} // namespace flub::app::ui
