// Flubsound Pro - app tests that put a real window on a display (the OSD's
// style flags, the Settings dialog open on a page): whether there is a
// display, and Xlib errors made harmless while a window lives on Xvfb.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>

#if JUCE_LINUX || JUCE_BSD
    #include <dlfcn.h>
#endif

namespace flubapptest
{
/** Xlib's default error handler exits the process. JUCE replaces it in a
    GUI app (JUCEApplication), not in this console test runner, and on a
    window-manager-less Xvfb JUCE queries atoms no WM created (BadAtom on
    _NET_WM_STATE), which the app ignores. Do the same while this lives
    (construct it after haveDisplay(), which loads Xlib). */
struct TolerateXErrors
{
   #if JUCE_LINUX || JUCE_BSD
    using Handler = int (*) (void*, void*);
    using Setter = Handler (*) (Handler);

    TolerateXErrors()
    {
        if (auto* x11 = dlopen ("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD))
            if ((setter = reinterpret_cast<Setter> (dlsym (x11, "XSetErrorHandler"))) != nullptr)
                previous = setter (&ignore);
    }
    ~TolerateXErrors()
    {
        if (setter != nullptr)
            setter (previous);
    }
    static int ignore (void*, void*) { return 0; }

    Setter setter = nullptr;
    Handler previous = nullptr;
   #endif
};

/** A display to put windows on (Xvfb or a desktop session). */
inline bool haveDisplay()
{
   #if JUCE_LINUX || JUCE_BSD
    if (std::getenv ("DISPLAY") == nullptr)
        return false;
   #endif
    return juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr;
}
} // namespace flubapptest
