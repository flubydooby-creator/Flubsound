// Flubsound Pro - the on-screen display's non-activating NSPanel on macOS
// (docs/11 E56). The interface, and why the OSD needs a panel of its own, are
// in OsdNative.h; ui/Osd.cpp attaches its JUCE peer to the panel's content view.
#include "OsdNative.h"

#import <AppKit/AppKit.h>

#if __has_feature(objc_arc)
 #error "OsdNative_mac.mm uses manual reference counting, like the JUCE sources compiled into the same target"
#endif

/*  Whatever AppKit's defaults for a borderless panel are, this one never
    becomes the key or the main window. */
@interface FlubOsdPanel : NSPanel
@end

@implementation FlubOsdPanel
- (BOOL) canBecomeKeyWindow  { return NO; }
- (BOOL) canBecomeMainWindow { return NO; }
@end

namespace flub::app::ui::osdpanel
{
namespace
{
NSPanel* asPanel (const void* panel)
{
    return (NSPanel*) const_cast<void*> (panel);
}
} // namespace

void* create()
{
    FlubOsdPanel* panel = [[FlubOsdPanel alloc] initWithContentRect: NSMakeRect (0, 0, 300, 64)
                                                         styleMask: NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                                           backing: NSBackingStoreBuffered
                                                             defer: YES];
    if (panel == nil)
        return nullptr;

    [panel setReleasedWhenClosed: NO]; // destroy() releases it
    [panel setFloatingPanel: YES];
    [panel setBecomesKeyOnlyIfNeeded: YES];
    [panel setWorksWhenModal: YES];   // shown while a modal dialog of Flubsound's is open
    [panel setHidesOnDeactivate: NO]; // NSPanel's default YES would hide it whenever a game is active
    [panel setLevel: NSPopUpMenuWindowLevel];
    [panel setCollectionBehavior: NSWindowCollectionBehaviorCanJoinAllSpaces
                                   | NSWindowCollectionBehaviorFullScreenAuxiliary
                                   | NSWindowCollectionBehaviorStationary
                                   | NSWindowCollectionBehaviorIgnoresCycle];
    [panel setIgnoresMouseEvents: YES]; // click-through
    [panel setOpaque: NO];
    [panel setBackgroundColor: [NSColor clearColor]]; // the OSD paints its own rounded box
    [panel setHasShadow: NO];
    [panel setExcludedFromWindowsMenu: YES];
    [panel setMovable: NO];
    [panel setRestorable: NO];
    [panel setAnimationBehavior: NSWindowAnimationBehaviorNone];
    [panel setColorSpace: [NSColorSpace sRGBColorSpace]]; // as JUCE's own windows
    return (void*) panel;
}

void destroy (void* panel)
{
    if (panel == nullptr)
        return;
    auto* p = asPanel (panel);
    [p orderOut: nil];
    [p close];
    [p release];
}

void* getContentView (void* panel)
{
    return panel != nullptr ? (void*) [asPanel (panel) contentView] : nullptr;
}

void setFrame (void* panel, double x, double y, double width, double height)
{
    if (panel == nullptr)
        return;
    // JUCE's flippedScreenRect: Cocoa's y runs up from the bottom of the main
    // display (the first screen).
    NSArray<NSScreen*>* screens = [NSScreen screens];
    const CGFloat mainHeight = [screens count] > 0 ? [[screens objectAtIndex: 0] frame].size.height : (CGFloat) 0;
    [asPanel (panel) setFrame: NSMakeRect ((CGFloat) x, mainHeight - (CGFloat) (y + height), (CGFloat) width, (CGFloat) height)
                      display: NO];
}

void orderFront (void* panel)
{
    if (panel != nullptr)
        [asPanel (panel) orderFrontRegardless];
}

void orderOut (void* panel)
{
    if (panel != nullptr)
        [asPanel (panel) orderOut: nil];
}

OsdNativeWindowState getState (const void* panel)
{
    OsdNativeWindowState state;
    if (panel == nullptr)
        return state;

    auto* p = asPanel (panel);
    const NSWindowCollectionBehavior behaviour = [p collectionBehavior];
    state.available = true;
    state.isPanel = static_cast<bool> ([p isKindOfClass: [NSPanel class]]);
    state.nonactivating = ([p styleMask] & NSWindowStyleMaskNonactivatingPanel) != 0;
    state.canBecomeKey = static_cast<bool> ([p canBecomeKeyWindow]);
    state.canBecomeMain = static_cast<bool> ([p canBecomeMainWindow]);
    state.isKey = static_cast<bool> ([p isKeyWindow]);
    state.viewIsFirstResponder = static_cast<bool> ([[p firstResponder] isKindOfClass: [NSView class]]);
    state.ignoresMouseEvents = static_cast<bool> ([p ignoresMouseEvents]);
    state.joinsAllSpaces = (behaviour & NSWindowCollectionBehaviorCanJoinAllSpaces) != 0;
    state.fullScreenAuxiliary = (behaviour & NSWindowCollectionBehaviorFullScreenAuxiliary) != 0;
    state.hidesOnDeactivate = static_cast<bool> ([p hidesOnDeactivate]);
    state.visible = static_cast<bool> ([p isVisible]);
    state.level = static_cast<long> ([p level]);
    return state;
}

OsdNativeFocus getFocus()
{
    OsdNativeFocus focus;
    focus.appActive = static_cast<bool> ([NSApp isActive]);
    focus.keyWindow = (const void*) [NSApp keyWindow];
    return focus;
}
} // namespace flub::app::ui::osdpanel
