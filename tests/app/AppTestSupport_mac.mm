// Flubsound Pro - app tests: AppKit helpers for the OSD's display case
// (test_app_osd.cpp, docs/11 E56); the interface is in AppTestSupport_mac.h.
// Manual reference counting, like the JUCE and app .mm sources in this target.
#include "AppTestSupport_mac.h"

#import <AppKit/AppKit.h>

namespace flubapptest::macos
{
void requestActivation()
{
    [NSApplication sharedApplication];
    if ([NSApp activationPolicy] != NSApplicationActivationPolicyRegular)
        [NSApp setActivationPolicy: NSApplicationActivationPolicyRegular];
   #if defined(MAC_OS_VERSION_14_0)
    if (@available (macOS 14.0, *))
        [NSApp activate]; // cooperative activation (macOS 14)
   #endif
   #pragma clang diagnostic push
   #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps: YES]; // what JUCE calls on an input attempt
   #pragma clang diagnostic pop
}

void deactivate()
{
    [NSApp deactivate];
}

const void* windowOf (void* nsView)
{
    return nsView != nullptr ? (const void*) [(NSView*) nsView window] : nullptr;
}

void makeKeyAndFront (void* nsView)
{
    if (nsView == nullptr)
        return;
    NSWindow* window = [(NSView*) nsView window];
    if (window != nil)
        [window makeKeyAndOrderFront: nil];
}
} // namespace flubapptest::macos
