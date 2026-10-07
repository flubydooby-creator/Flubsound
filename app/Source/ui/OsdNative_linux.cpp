// Flubsound Pro - the on-screen display's X11 input shape (docs/11 E56; why, in
// OsdNative.h). Its own translation unit: the Xlib headers JUCE includes for
// JUCE_GUI_BASICS_INCLUDE_XHEADERS define macros (None, Bool, Status, ...)
// that the rest of the app should not see.
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_gui_basics/juce_gui_basics.h>

#include "OsdNative.h"

#include <dlfcn.h>

namespace flub::app::ui::osdx11
{
namespace
{
// <X11/extensions/shape.h> (the SHAPE extension, version 1.1 for ShapeInput).
constexpr int kShapeSet = 0;
constexpr int kShapeInput = 2;

/** The SHAPE calls, from libXext at run time (JUCE loads the X libraries
    the same way; the app does not link them). */
struct ShapeFunctions
{
    using QueryExtension = Bool (*) (::Display*, int*, int*);
    using QueryVersion = Status (*) (::Display*, int*, int*);
    using CombineRectangles = void (*) (::Display*, ::Window, int, int, int, XRectangle*, int, int, int);
    using GetRectangles = XRectangle* (*) (::Display*, ::Window, int, int*, int*);

    ShapeFunctions()
    {
        library = dlopen ("libXext.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (library == nullptr)
            return;
        queryExtension = reinterpret_cast<QueryExtension> (dlsym (library, "XShapeQueryExtension"));
        queryVersion = reinterpret_cast<QueryVersion> (dlsym (library, "XShapeQueryVersion"));
        combineRectangles = reinterpret_cast<CombineRectangles> (dlsym (library, "XShapeCombineRectangles"));
        getRectangles = reinterpret_cast<GetRectangles> (dlsym (library, "XShapeGetRectangles"));
    }

    ~ShapeFunctions()
    {
        if (library != nullptr)
            dlclose (library);
    }

    ShapeFunctions (const ShapeFunctions&) = delete;
    ShapeFunctions& operator= (const ShapeFunctions&) = delete;

    bool isComplete() const noexcept
    {
        return queryExtension != nullptr && queryVersion != nullptr && combineRectangles != nullptr && getRectangles != nullptr;
    }

    void* library = nullptr;
    QueryExtension queryExtension = nullptr;
    QueryVersion queryVersion = nullptr;
    CombineRectangles combineRectangles = nullptr;
    GetRectangles getRectangles = nullptr;
};

const ShapeFunctions& shapeFunctions()
{
    static const ShapeFunctions functions;
    return functions;
}

/** JUCE's own connection (so a request follows the window's creation in
    order), when the server has SHAPE 1.1; nullptr otherwise. Call with the
    display locked. */
::Display* displayWithInputShapes()
{
    const auto& shape = shapeFunctions();
    auto* windowSystem = juce::XWindowSystem::getInstance();
    auto* display = windowSystem != nullptr ? windowSystem->getDisplay() : nullptr;
    if (display == nullptr || ! shape.isComplete())
        return nullptr;

    int eventBase = 0, errorBase = 0, major = 0, minor = 0;
    if (! shape.queryExtension (display, &eventBase, &errorBase) || shape.queryVersion (display, &major, &minor) == 0)
        return nullptr;
    return (major > 1 || (major == 1 && minor >= 1)) ? display : nullptr;
}

::Window toWindow (void* nativeHandle)
{
    return static_cast<::Window> (reinterpret_cast<juce::pointer_sized_uint> (nativeHandle));
}
} // namespace

bool setEmptyInputShape (void* nativeHandle)
{
    if (nativeHandle == nullptr)
        return false;
    const juce::XWindowSystemUtilities::ScopedXLock lock;
    auto* display = displayWithInputShapes();
    if (display == nullptr)
        return false;
    // No rectangles, ShapeSet: the input region is empty.
    shapeFunctions().combineRectangles (display, toWindow (nativeHandle), kShapeInput, 0, 0, nullptr, 0, kShapeSet, Unsorted);
    juce::X11Symbols::getInstance()->xFlush (display);
    return true;
}

int countInputRectangles (void* nativeHandle)
{
    if (nativeHandle == nullptr)
        return -1;
    const juce::XWindowSystemUtilities::ScopedXLock lock;
    auto* display = displayWithInputShapes();
    if (display == nullptr)
        return -1;
    int count = 0, ordering = 0;
    auto* rectangles = shapeFunctions().getRectangles (display, toWindow (nativeHandle), kShapeInput, &count, &ordering);
    if (rectangles != nullptr)
        juce::X11Symbols::getInstance()->xFree (rectangles);
    return count;
}
} // namespace flub::app::ui::osdx11
