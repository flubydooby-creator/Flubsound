#include "BrainView.h"

#include "VisCommon.h"
#include "VisualiserWindow.h"

#include "../Theme.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flub::app::ui::vis
{
namespace
{
using brain::Vec3;

constexpr Vec3 kEye { 0.0f, 0.42f, 3.1f }, kTarget { 0.0f, -0.17f, 0.0f };
constexpr float kTanHalfFov = 0.3249197f; // tan 18 degrees: the mockup's 36-degree field of view
constexpr float kSceneHalfWidth = 1.42f;  // the ear rings' outer edges and a margin (view units)

// Base colours (0..1; the mockup's, the cloud a little brighter for the app's darker well) and point diameters
// (view units, the mockup's).
constexpr float kCortex[3] = { 0.16f, 0.2f, 0.34f };
constexpr float kCerebellumBase[3] = { 0.19f, 0.18f, 0.33f };
constexpr float kStem[3] = { 0.24f, 0.22f, 0.34f };
constexpr float kHeschlBase[3] = { 0.17f, 0.17f, 0.27f };
constexpr float kNucleus[3] = { 0.26f, 0.28f, 0.4f };
constexpr float kPutamen[3] = { 0.24f, 0.22f, 0.37f };
constexpr float kCaudate[3] = { 0.27f, 0.19f, 0.33f };
constexpr float kLimbic[3] = { 0.32f, 0.17f, 0.28f };
constexpr float kCortexRegion[3] = { 0.16f, 0.16f, 0.26f };
constexpr float kTract[3] = { 0.09f, 0.11f, 0.18f };
constexpr float kRing[3] = { 0.30f, 0.34f, 0.44f };
constexpr float kCortexSize = 0.032f, kHeschlSize = 0.036f, kRegionSize = 0.022f;
constexpr int kProfileLast = 64; // the dot profile's last entry (BrainView::profile has 65)

const float* baseColour (brain::PointKind kind, bool region) noexcept
{
    switch (kind)
    {
        case brain::PointKind::Cortex: return region ? kCortexRegion : kCortex;
        case brain::PointKind::Cerebellum: return kCerebellumBase;
        case brain::PointKind::Brainstem: return kStem;
        case brain::PointKind::Heschl: return kHeschlBase;
        case brain::PointKind::Nucleus: return kNucleus;
        case brain::PointKind::Putamen: return kPutamen;
        case brain::PointKind::Caudate: return kCaudate;
        case brain::PointKind::Limbic: return kLimbic;
    }
    return kCortex;
}

// Where R, G and B sit in a pixel read as one 32-bit word (JUCE's ARGB layout: 0xAARRGGBB on these platforms).
#if JUCE_BIG_ENDIAN
constexpr int kShiftR = 8 * (3 - juce::PixelARGB::indexR), kShiftG = 8 * (3 - juce::PixelARGB::indexG), kShiftB = 8 * (3 - juce::PixelARGB::indexB),
              kShiftA = 8 * (3 - juce::PixelARGB::indexA);
#else
constexpr int kShiftR = 8 * juce::PixelARGB::indexR, kShiftG = 8 * juce::PixelARGB::indexG, kShiftB = 8 * juce::PixelARGB::indexB,
              kShiftA = 8 * juce::PixelARGB::indexA;
#endif

/** One pixel of the frame buffer (opaque) += colour (0..255 each), hue-preserving: a channel over 255 scales
    the whole pixel down, and its grey part never passes 80 % of its brightest channel, so crowded light keeps
    a hue (no white-out). Integer arithmetic on one 32-bit load and store: the hot loop of every point, glow
    and tract. */
inline void addPixel (uint32_t* p, int r, int g, int b) noexcept
{
    const uint32_t v = *p;
    int R = static_cast<int> ((v >> kShiftR) & 0xffu) + r, G = static_cast<int> ((v >> kShiftG) & 0xffu) + g, B = static_cast<int> ((v >> kShiftB) & 0xffu) + b;
    const int hi = std::max (R, std::max (G, B));
    if (hi > 204) // only bright pixels can saturate or turn white
    {
        if (hi > 255)
        {
            R = R * 255 / hi;
            G = G * 255 / hi;
            B = B * 255 / hi;
        }
        const int grey = std::min (R, std::min (G, B)), limit = (std::min (hi, 255) * 205) >> 8;
        if (grey > limit)
        {
            R -= grey - limit;
            G -= grey - limit;
            B -= grey - limit;
        }
    }
    *p = (0xffu << kShiftA) | (static_cast<uint32_t> (R) << kShiftR) | (static_cast<uint32_t> (G) << kShiftG) | (static_cast<uint32_t> (B) << kShiftB);
}

/** The pixels i of a row whose centres X = i + 0.5 lie on one side of a line through (xp, row) with normal
    (nx, ny), ryp = the row's y minus the point's: positive side (X - xp) nx + ryp ny >= 0, or the strictly
    negative side. Both neighbours of a joint call this with the same numbers, so they split it exactly. */
inline void sideOfLine (float nx, float ny, float xp, float ryp, bool positive, int& from, int& to) noexcept
{
    const float offset = ryp * ny;
    if (std::abs (nx) < 1.0e-6f)
    {
        if ((offset >= 0.0f) != positive)
            to = from - 1; // the whole row is on the other side
        return;
    }
    const float boundary = xp - offset / nx;
    const bool above = (nx > 0.0f) == positive; // the kept side is X >= boundary (or X > boundary)
    if (above)
        from = std::max (from, positive ? static_cast<int> (std::ceil (boundary - 0.5f)) : static_cast<int> (std::floor (boundary - 0.5f)) + 1);
    else
        to = std::min (to, positive ? static_cast<int> (std::floor (boundary - 0.5f)) : static_cast<int> (std::ceil (boundary - 0.5f)) - 1);
}

juce::String sideName (int side)
{
    return side == brain::kRight ? "right" : "left";
}
} // namespace

// =============================================================================
// Construction and model
// =============================================================================
BrainView::BrainView() : anatomy (brain::BrainAnatomy::get()), activity (anatomy)
{
    setOpaque (false);
    setInterceptsMouseClicks (true, false);
    setRepaintsOnMouseActivity (false);
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);

    // The mockup's dot: alpha 1 at the centre, 0.6 at 30 % of the radius, 0 at the edge (by squared distance).
    for (size_t i = 0; i < profile.size(); ++i)
    {
        const float rho = std::sqrt (static_cast<float> (i) / static_cast<float> (profile.size() - 1));
        profile[i] = rho < 0.3f ? 1.0f - 0.4f * rho / 0.3f : 0.6f * (1.0f - rho) / 0.7f;
        profileFixed[i] = juce::roundToInt (profile[i] * 256.0f);
    }
    // Each spiral point's band (from its place, inverting Greenwood's map).
    for (int i = 0; i < brain::BrainAnatomy::kSpiralPoints; ++i)
    {
        const double place = brain::BrainAnatomy::spiralPlace (i);
        const double hz = 165.4 * (std::pow (10.0, 2.1 * (1.0 - place)) - 0.88);
        const double t = (std::log2 (std::max (hz, 1.0)) - std::log2 (brain::kLowHz)) / (std::log2 (brain::kHighHz) - std::log2 (brain::kLowHz));
        const double band = std::clamp (t, 0.0, 1.0) * (brain::kBands - 1);
        const int b0 = std::min (brain::kBands - 2, static_cast<int> (std::floor (band)));
        spiralBand[static_cast<size_t> (i)] = b0;
        spiralFrac[static_cast<size_t> (i)] = static_cast<float> (band - b0);
        spiralRgb[static_cast<size_t> (i)] = brain::frequencyRgb (std::max (hz, brain::kLowHz));
    }
    for (int b = 0; b < brain::kBands; ++b)
        bandRgb[static_cast<size_t> (b)] = brain::frequencyRgb (brain::bandHz (b));
    // Scratch for one polyline (a tract, a ring or a spiral; the spiral is the longest).
    constexpr auto kLongest = static_cast<size_t> (std::max ({ brain::BrainAnatomy::kSpiralPoints, brain::BrainAnatomy::kSegments + 1,
                                                                brain::BrainAnatomy::kRingPoints }));
    polyColours.resize (3 * kLongest);
    polyLevels.resize (kLongest);
    screen.resize (3 * kLongest);
    lookAndFeelChanged();
}

BrainView::~BrainView() = default;

void BrainView::setSampleRate (double sampleRate)
{
    listener.setSampleRate (sampleRate);
}

void BrainView::reset()
{
    listener.reset();
    activity.reset();
    clock = 0.0;
    dirty = true;
    repaint();
}

void BrainView::pushPost (const float* mid, const float* side, int numSamples)
{
    listener.push (mid, side, numSamples);
}

void BrainView::advance (const FrameContext& frame)
{
    const double dt = juce::jlimit (0.0, 0.25, frame.dtSeconds);
    if (frame.sampleRate > 0.0)
        listener.setSampleRate (frame.sampleRate);
    clock += dt;
    listener.advance (clock, dt, activity);
    // The display runs the analysis latency behind, so the cochlea and the canal (0 - 20 ms) see every hop.
    activity.update (clock - listener.getLatency(), dt);
    if (turning && ! pressed)
        angle = std::fmod (angle - kTurnRadiansPerSecond * static_cast<float> (dt), 2.0f * juce::MathConstants<float>::pi);
    // A new picture at most kMaxFramesPerSecond: every frame at 60 Hz, every second one at 120 / 144 Hz.
    sinceRender += dt;
    if (sinceRender < 0.9 / kMaxFramesPerSecond)
        return;
    sinceRender = 0.0;
    // A still, dark brain looks the same as the last picture: no new render (the overlay is still repainted, so the
    // status pills fade). A drag, a resize, a theme change or resetView sets `dirty` itself.
    const bool still = ! turning && ! pressed && activity.isDark();
    if (! (still && lastRenderStill))
        dirty = true;
    if (isVisible())
        repaint();
}

// =============================================================================
// View control
// =============================================================================
void BrainView::setTurning (bool shouldTurn)
{
    turning = shouldTurn;
    repaint();
}

void BrainView::resetView()
{
    angle = kStartAngle;
    tilt = kStartTilt;
    dirty = true;
    repaint();
}

void BrainView::setView (float newAngle, float newTilt)
{
    angle = newAngle;
    tilt = juce::jlimit (kMinTilt, kMaxTilt, newTilt);
    dirty = true;
    repaint();
}

void BrainView::addMenuItems (juce::PopupMenu& menu)
{
    juce::Component::SafePointer<BrainView> safe (this);
    menu.addItem (turning ? "Stop turning" : "Turn", true, false,
                  [safe]
                  {
                      if (safe != nullptr)
                          safe->setTurning (! safe->isTurning());
                  });
    menu.addItem ("Reset view", true, false,
                  [safe]
                  {
                      if (safe != nullptr)
                          safe->resetView();
                  });
}

void BrainView::showMenu()
{
    juce::PopupMenu menu;
    addMenuItems (menu);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

// =============================================================================
// Mouse
// =============================================================================
void BrainView::mouseMove (const juce::MouseEvent& e)
{
    const int n = findNodeAt (e.position);
    hoverPosition = e.position;
    if (n != hovered)
    {
        hovered = n;
        repaint();
    }
}

void BrainView::mouseExit (const juce::MouseEvent&)
{
    if (hovered >= 0)
    {
        hovered = -1;
        repaint();
    }
}

void BrainView::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        // In the visualiser window its own menu (with these items) answers through its mouse listener.
        if (findParentComponentOfClass<VisualiserWindow>() == nullptr)
            showMenu();
        return;
    }
    pressed = true;
    dragged = false;
    pressPosition = e.position;
    pressAngle = angle;
    pressTilt = tilt;
}

void BrainView::mouseDrag (const juce::MouseEvent& e)
{
    if (! pressed)
        return;
    const auto d = e.position - pressPosition;
    if (! dragged && d.getDistanceFromOrigin() < 4.0f)
        return;
    dragged = true;
    angle = pressAngle - d.x * 0.01f; // the near side follows the hand
    tilt = juce::jlimit (kMinTilt, kMaxTilt, pressTilt + d.y * 0.01f);
    dirty = true;
    repaint();
}

void BrainView::mouseUp (const juce::MouseEvent& e)
{
    if (! pressed)
        return;
    pressed = false;
    if (! dragged && ! e.mods.isPopupMenu())
        setTurning (! turning); // a click stops or restarts the turning
}

void BrainView::mouseDoubleClick (const juce::MouseEvent&)
{
    // The two clicks toggled the turning twice (unchanged); the visualiser window's listener toggles full screen.
}

juce::MouseCursor BrainView::getMouseCursor()
{
    if (auto* parent = getParentComponent(); parent != nullptr && parent->getMouseCursor() == juce::MouseCursor::NoCursor)
        return juce::MouseCursor::NoCursor;
    return juce::MouseCursor::DraggingHandCursor;
}

juce::String BrainView::getTooltip()
{
    if (hovered >= 0)
        return describeNode (hovered);
    return getDescription();
}

juce::String BrainView::describeNode (int index)
{
    const auto& n = brain::BrainAnatomy::get().getNode (index);
    juce::String text (n.name);
    if (n.station != brain::Station::Vta)
        text << " (" << sideName (n.side) << ")";
    text << ": " << n.role;
    if (n.heuristic)
        text << " Lit by a detected musical feature (a heuristic of this view, not a measurement).";
    return text;
}

// =============================================================================
// Projection
// =============================================================================
bool BrainView::Projection::project (Vec3 p, float& x, float& y, float& depth) const noexcept
{
    const float xc = m[0] * p.x + m[1] * p.y + m[2] * p.z + t[0];
    const float yc = m[3] * p.x + m[4] * p.y + m[5] * p.z + t[1];
    const float zc = m[6] * p.x + m[7] * p.y + m[8] * p.z + t[2];
    if (zc < 0.2f)
        return false;
    // The view's axes (x right, y up, z front) are a left-handed frame (MNI's right, anterior, superior swapped to
    // right, superior, anterior): screen x runs against the camera's right so the picture is the brain, not its mirror
    // image (facing it, its right side is on the viewer's left).
    const float inv = focal / zc;
    x = cx - xc * inv;
    y = cy - yc * inv;
    depth = zc;
    return true;
}

void BrainView::updateProjection() noexcept
{
    // The root turns about y (angle), then tilts about x: R = Rx (tilt) Ry (angle), as the mockup's Euler XYZ.
    const float ca = std::cos (angle), sa = std::sin (angle), ct = std::cos (tilt), st = std::sin (tilt);
    const float r[9] = { ca, 0.0f, sa, st * sa, ct, -st * ca, -ct * sa, st, ct * ca };
    // Camera basis: right, up, forward (looking from kEye at kTarget).
    Vec3 f = kTarget - kEye;
    f = f * (1.0f / brain::length (f));
    Vec3 right { -f.z, 0.0f, f.x }; // f x (0, 1, 0)
    right = right * (1.0f / brain::length (right));
    const Vec3 up { right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x };
    const Vec3 basis[3] = { right, up, f };
    for (int i = 0; i < 3; ++i)
    {
        const auto& c = basis[i];
        for (int j = 0; j < 3; ++j)
            projection.m[static_cast<size_t> (3 * i + j)] = c.x * r[j] + c.y * r[3 + j] + c.z * r[6 + j];
        projection.t[static_cast<size_t> (i)] = -(c.x * kEye.x + c.y * kEye.y + c.z * kEye.z);
    }
    const float s = renderScale;
    const float w = plot.getWidth() * s, h = plot.getHeight() * s;
    const float distance = brain::length (kTarget - kEye);
    projection.focal = std::max (1.0f, std::min (h * 0.5f / kTanHalfFov, w * 0.5f * distance / kSceneHalfWidth));
    projection.cx = (plot.getCentreX() - well.getX()) * s;
    projection.cy = (plot.getCentreY() - well.getY()) * s;
    projection.pointScale = projection.focal * kTanHalfFov * 0.5f; // a point of size d has the radius d x this / depth
}

bool BrainView::projectPoint (Vec3 p, juce::Point<float>& at, float& depth) const noexcept
{
    float x = 0.0f, y = 0.0f;
    if (! projection.project (p, x, y, depth))
        return false;
    at = { well.getX() + x / renderScale, well.getY() + y / renderScale };
    return true;
}

juce::Point<float> BrainView::getNodeScreenPosition (int index) const noexcept
{
    return nodeScreen[static_cast<size_t> (index)];
}

int BrainView::findNodeAt (juce::Point<float> position) const noexcept
{
    int best = -1;
    float bestDistance = 1.0e9f;
    for (int n = 0; n < brain::kNumNodes; ++n)
    {
        const auto& node = anatomy.getNode (n);
        float d = 0.0f;
        if (node.kind == brain::NodeKind::Heschl)
        {
            // Distance to the gyrus's axis.
            const auto& ends = heschlEnds[static_cast<size_t> (node.side)];
            const juce::Line<float> axis (ends[0], ends[1]);
            juce::Point<float> nearest;
            d = axis.getDistanceFromPoint (position, nearest);
        }
        else
        {
            d = position.getDistanceFrom (nodeScreen[static_cast<size_t> (n)]);
        }
        const float reach = std::max (9.0f, nodeRadius[static_cast<size_t> (n)]);
        if (d <= reach && d / reach < bestDistance)
        {
            bestDistance = d / reach;
            best = n;
        }
    }
    return best;
}

// =============================================================================
// Rasterising
// =============================================================================
void BrainView::splat (Target& t, float x, float y, float radius, float r, float g, float b) const noexcept
{
    if (radius < 0.75f)
    {
        // Too small for its profile: the same energy, spread bilinearly.
        const float energy = std::min (1.0f, juce::MathConstants<float>::pi * radius * radius * 0.29f);
        const float fx = x - 0.5f, fy = y - 0.5f;
        const int x0 = static_cast<int> (std::floor (fx)), y0 = static_cast<int> (std::floor (fy));
        if (x0 < 0 || y0 < 0 || x0 + 1 >= t.width || y0 + 1 >= t.height)
            return;
        const float ax = fx - static_cast<float> (x0), ay = fy - static_cast<float> (y0);
        const float w[4] = { (1.0f - ax) * (1.0f - ay), ax * (1.0f - ay), (1.0f - ax) * ay, ax * ay };
        for (int k = 0; k < 4; ++k)
        {
            const float e = w[k] * energy;
            addPixel (t.data + (y0 + k / 2) * t.stride + (x0 + k % 2), static_cast<int> (r * e + 0.5f), static_cast<int> (g * e + 0.5f),
                      static_cast<int> (b * e + 0.5f));
        }
        t.minX = std::min (t.minX, x0);
        t.minY = std::min (t.minY, y0);
        t.maxX = std::max (t.maxX, x0 + 1);
        t.maxY = std::max (t.maxY, y0 + 1);
        return;
    }
    // The profile is under 0.09 beyond 90 % of the radius: those pixels are left out.
    const float reach = radius * 0.9f;
    const int yA = std::max (0, static_cast<int> (y - reach)), yB = std::min (t.height - 1, static_cast<int> (y + reach));
    const int xA = std::max (0, static_cast<int> (x - reach)), xB = std::min (t.width - 1, static_cast<int> (x + reach));
    if (yA > yB || xA > xB)
        return;
    t.minX = std::min (t.minX, xA);
    t.minY = std::min (t.minY, yA);
    t.maxX = std::max (t.maxX, xB);
    t.maxY = std::max (t.maxY, yB);
    const float inv = 1.0f / (radius * radius);
    const auto lutScale = static_cast<float> (profile.size() - 1);
    // 8.8 fixed point colour times 0..256 weights: >> 16 gives 0..255.
    const int cr = static_cast<int> (r * 256.0f), cg = static_cast<int> (g * 256.0f), cb = static_cast<int> (b * 256.0f);
    for (int py = yA; py <= yB; ++py)
    {
        const float dy = static_cast<float> (py) + 0.5f - y;
        const float dy2 = dy * dy * inv;
        if (dy2 >= 0.81f)
            continue;
        const float half = radius * std::sqrt (0.81f - dy2);
        const int from = std::max (xA, static_cast<int> (x - half)), to = std::min (xB, static_cast<int> (x + half));
        uint32_t* px = t.data + py * t.stride + from;
        // d2 (x + 1) = d2 (x) + (2 dx + 1) inv: two additions per pixel, the LUT index clamped (its last entry is 0).
        const float dx0 = static_cast<float> (from) + 0.5f - x;
        float index = (dx0 * dx0 * inv + dy2) * lutScale, step = (2.0f * dx0 + 1.0f) * inv * lutScale;
        const float stepStep = 2.0f * inv * lutScale;
        for (int i = from; i <= to; ++i, ++px, index += step, step += stepStep)
        {
            const int w = profileFixed[static_cast<size_t> (std::min (static_cast<int> (index), kProfileLast))];
            addPixel (px, (cr * w) >> 16, (cg * w) >> 16, (cb * w) >> 16);
        }
    }
}

void BrainView::segment (Target& t, const float* a, const float* b, float hw, const float* c0, const float* c1, float glow, const float* startNormal,
                         const float* endNormal) const noexcept
{
    const float x0 = a[0], y0 = a[1], x1 = b[0], y1 = b[1];
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    if (len2 < 1.0e-4f)
        return;
    const float len = std::sqrt (len2);
    const float nx = -dy / len, ny = dx / len;
    const float reach = glow > 0.0f ? hw * 2.0f + 1.5f : hw + 1.0f;
    const int yA = std::max (0, static_cast<int> (std::floor (std::min (y0, y1) - reach)));
    const int yB = std::min (t.height - 1, static_cast<int> (std::ceil (std::max (y0, y1) + reach)));
    const int xA = std::max (0, static_cast<int> (std::floor (std::min (x0, x1) - reach)));
    const int xB = std::min (t.width - 1, static_cast<int> (std::ceil (std::max (x0, x1) + reach)));
    if (yA > yB || xA > xB)
        return;
    t.minX = std::min (t.minX, xA);
    t.minY = std::min (t.minY, yA);
    t.maxX = std::max (t.maxX, xB);
    t.maxY = std::max (t.maxY, yB);
    const float invLen2 = 1.0f / len2, invReach = 1.0f / reach;
    const float dc[3] = { c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2] };
    for (int py = yA; py <= yB; ++py)
    {
        const float ry = static_cast<float> (py) + 0.5f - y0, ry1 = static_cast<float> (py) + 0.5f - y1;
        int from = xA, to = xB;
        // Within `reach` of the line, and between the bisectors of the joints (each joint is split exactly).
        if (std::abs (nx) > 1.0e-6f)
        {
            const float centre = x0 - ry * ny / nx, half = reach / std::abs (nx);
            from = std::max (from, static_cast<int> (std::floor (centre - half - 0.5f)));
            to = std::min (to, static_cast<int> (std::ceil (centre + half - 0.5f)));
        }
        else if (std::abs (ry * ny) >= reach)
        {
            continue;
        }
        sideOfLine (startNormal[0], startNormal[1], x0, ry, true, from, to);
        sideOfLine (endNormal[0], endNormal[1], x1, ry1, false, from, to);
        if (from > to)
            continue;
        uint32_t* px = t.data + py * t.stride + from;
        const float rx = static_cast<float> (from) + 0.5f - x0;
        float across = rx * nx + ry * ny, along = (rx * dx + ry * dy) * invLen2;
        const float alongStep = dx * invLen2;
        for (int i = from; i <= to; ++i, ++px, across += nx, along += alongStep)
        {
            const float d = std::abs (across);
            if (d >= reach)
                continue;
            float w = std::clamp (hw + 0.5f - d, 0.0f, 1.0f);
            if (glow > 0.0f)
            {
                const float q = 1.0f - d * invReach;
                w += glow * q * q;
            }
            if (w <= 0.004f)
                continue;
            const float u = std::clamp (along, 0.0f, 1.0f);
            addPixel (px, static_cast<int> (w * (c0[0] + dc[0] * u)), static_cast<int> (w * (c0[1] + dc[1] * u)), static_cast<int> (w * (c0[2] + dc[2] * u)));
        }
    }
}

void BrainView::drawPolyline (Target& t, const Vec3* points, int count, float radius, const float* colours, const float* levels, bool closed) noexcept
{
    for (int i = 0; i < count; ++i)
    {
        float x = 0.0f, y = 0.0f, depth = 0.0f;
        if (! projection.project (points[i], x, y, depth))
            depth = -1.0f;
        screen[static_cast<size_t> (3 * i)] = x;
        screen[static_cast<size_t> (3 * i + 1)] = y;
        screen[static_cast<size_t> (3 * i + 2)] = depth;
    }
    // Unit direction of each segment; a joint's boundary is the mean of the directions meeting there.
    const int segments = closed ? count : count - 1;
    const auto direction = [this, count] (int i, float* out)
    {
        const float* a = &screen[static_cast<size_t> (3 * (((i % count) + count) % count))];
        const float* b = &screen[static_cast<size_t> (3 * (((i + 1) % count + count) % count))];
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        const float len = std::sqrt (dx * dx + dy * dy);
        out[0] = len > 1.0e-6f ? dx / len : 1.0f;
        out[1] = len > 1.0e-6f ? dy / len : 0.0f;
    };
    float previous[2] = {}, current[2] = {}, next[2] = {};
    direction (closed ? -1 : 0, previous);
    direction (0, current);
    for (int i = 0; i < segments; ++i)
    {
        const int j = (i + 1) % count;
        direction (closed || i + 1 < segments ? i + 1 : i, next);
        const float startNormal[2] = { previous[0] + current[0], previous[1] + current[1] };
        const float endNormal[2] = { current[0] + next[0], current[1] + next[1] };
        const float* a = &screen[static_cast<size_t> (3 * i)];
        const float* b = &screen[static_cast<size_t> (3 * j)];
        if (a[2] > 0.0f && b[2] > 0.0f)
        {
            const float hw = std::max (0.55f, radius * projection.focal * 2.0f / (a[2] + b[2]));
            const float glow = 0.5f * std::max (levels[i], levels[j]);
            segment (t, a, b, hw, colours + 3 * i, colours + 3 * j, glow > 0.02f ? glow : 0.0f, startNormal, endNormal);
        }
        previous[0] = current[0];
        previous[1] = current[1];
        current[0] = next[0];
        current[1] = next[1];
    }
}

void BrainView::renderScene() noexcept
{
    dirty = false;
    if (! canvas.isValid())
        return;
    ++renders;
    lastRenderStill = ! turning && ! pressed && activity.isDark();
    const auto started = juce::Time::getHighResolutionTicks();
    Target t;
    t.data = pixels.data();
    t.stride = pixelsWidth;
    t.width = pixelsWidth;
    t.height = pixelsHeight;
    t.minX = t.width;
    t.minY = t.height;

    // Clear what the last frame drew (a row of the background colour, copied).
    const auto cleared = lastDrawn.getIntersection ({ 0, 0, t.width, t.height });
    for (int y = cleared.getY(); y < cleared.getBottom(); ++y)
        std::memcpy (pixels.data() + static_cast<size_t> (y * pixelsWidth + cleared.getX()), backgroundRow.data(),
                     static_cast<size_t> (cleared.getWidth()) * sizeof (uint32_t));
    updateProjection();

    // ---- Tracts: the base colour, lit by what runs along them --------------------------------
    const auto& tracts = anatomy.getTracts();
    const auto& tractPoints = anatomy.getTractPoints();
    constexpr int kPoints = brain::BrainAnatomy::kSegments + 1;
    for (size_t ti = 0; ti < tracts.size(); ++ti)
    {
        const auto& tr = tracts[ti];
        for (int j = 0; j < kPoints; ++j)
        {
            const auto& gl = activity.segment (static_cast<int> (ti), j);
            const float lit = std::min (1.0f, gl.level);
            const auto k = static_cast<size_t> (j);
            polyLevels[k] = lit;
            polyColours[3 * k] = 255.0f * (kTract[0] * (1.0f - lit) + gl.r * lit);
            polyColours[3 * k + 1] = 255.0f * (kTract[1] * (1.0f - lit) + gl.g * lit);
            polyColours[3 * k + 2] = 255.0f * (kTract[2] * (1.0f - lit) + gl.b * lit);
        }
        drawPolyline (t, &tractPoints[static_cast<size_t> (tr.firstPoint)], kPoints, tr.radius, polyColours.data(), polyLevels.data(), false);
    }

    // ---- Ears and cochleas -------------------------------------------------------------------------
    for (int side = brain::kLeft; side <= brain::kRight; ++side)
    {
        const float lit = std::min (1.0f, activity.ear (side));
        const auto& ring = anatomy.getRing (side);
        for (size_t k = 0; k < ring.size(); ++k)
        {
            polyLevels[k] = 0.12f * lit;
            polyColours[3 * k] = 255.0f * (kRing[0] + 0.08f * lit);
            polyColours[3 * k + 1] = 255.0f * (kRing[1] + 0.09f * lit);
            polyColours[3 * k + 2] = 255.0f * (kRing[2] + 0.11f * lit);
        }
        drawPolyline (t, ring.data(), static_cast<int> (ring.size()), 0.012f, polyColours.data(), polyLevels.data(), true);

        const auto& spiral = anatomy.getSpiral (side);
        for (size_t k = 0; k < spiral.size(); ++k)
        {
            const int b = spiralBand[k];
            const float f = spiralFrac[k];
            const float v = std::min (1.0f, activity.cochlea (side, b) * (1.0f - f) + activity.cochlea (side, b + 1) * f);
            const auto& rgb = spiralRgb[k];
            polyLevels[k] = 0.45f * v; // a smaller glow: the spiral's turns are close
            polyColours[3 * k] = 255.0f * (kTract[0] * (1.0f - v) + rgb[0] * v);
            polyColours[3 * k + 1] = 255.0f * (kTract[1] * (1.0f - v) + rgb[1] * v);
            polyColours[3 * k + 2] = 255.0f * (kTract[2] * (1.0f - v) + rgb[2] * v);
        }
        drawPolyline (t, spiral.data(), static_cast<int> (spiral.size()), 0.008f, polyColours.data(), polyLevels.data(), false);
    }

    // ---- Points ------------------------------------------------------------------------------------
    for (int side = brain::kLeft; side <= brain::kRight; ++side)
        for (int b = 0; b < brain::kBands; ++b)
            heschlLevel[static_cast<size_t> (side)][static_cast<size_t> (b)] = activity.heschl (side, b);
    for (const auto& pt : anatomy.getPoints())
    {
        float x = 0.0f, y = 0.0f, depth = 0.0f;
        if (! projection.project (pt.p, x, y, depth))
            continue;
        float size = kCortexSize, lit = 0.0f;
        const float* base = baseColour (pt.kind, pt.node >= 0);
        float hr = 0.0f, hg = 0.0f, hb = 0.0f;
        if (pt.kind == brain::PointKind::Heschl)
        {
            const float band = pt.tonotopy * static_cast<float> (brain::kBands - 1);
            const int b0 = std::min (brain::kBands - 2, static_cast<int> (band));
            const float f = band - static_cast<float> (b0);
            const auto& levels = heschlLevel[static_cast<size_t> (pt.side)];
            lit = levels[static_cast<size_t> (b0)] * (1.0f - f) + levels[static_cast<size_t> (b0 + 1)] * f;
            const auto& c0 = bandRgb[static_cast<size_t> (b0)];
            const auto& c1 = bandRgb[static_cast<size_t> (b0 + 1)];
            hr = c0[0] + (c1[0] - c0[0]) * f;
            hg = c0[1] + (c1[1] - c0[1]) * f;
            hb = c0[2] + (c1[2] - c0[2]) * f;
            size = kHeschlSize + 0.03f * std::min (lit, 1.3f);
        }
        else if (pt.node >= 0)
        {
            const auto& gl = activity.glow (pt.node);
            lit = gl.level;
            hr = gl.r;
            hg = gl.g;
            hb = gl.b;
            size = (pt.kind == brain::PointKind::Cortex || pt.kind == brain::PointKind::Cerebellum ? 0.026f : kRegionSize) + 0.05f * std::min (lit, 1.37f);
        }
        const float c = std::min (1.0f, lit);
        const float r = 255.0f * (base[0] * (1.0f - c) + hr * c * 0.95f);
        const float g = 255.0f * (base[1] * (1.0f - c) + hg * c * 0.95f);
        const float b = 255.0f * (base[2] * (1.0f - c) + hb * c * 0.95f);
        splat (t, x, y, std::min (24.0f, size * projection.pointScale / depth), r, g, b);
    }

    // ---- Landing spots: a soft glow that grows with the intensity -----------------------------------
    for (int n = 0; n < brain::kNumNodes; ++n)
    {
        const auto& node = anatomy.getNode (n);
        float x = 0.0f, y = 0.0f, depth = 1.0f;
        if (! projection.project (node.position, x, y, depth))
            continue;
        nodeScreen[static_cast<size_t> (n)] = { well.getX() + x / renderScale, well.getY() + y / renderScale };
        nodeRadius[static_cast<size_t> (n)] = node.extent * projection.focal / depth / renderScale;
        const auto& gl = activity.glow (n);
        const bool glowing = node.kind == brain::NodeKind::Nucleus || node.kind == brain::NodeKind::Cortex;
        if (! glowing || gl.level < 0.02f)
            continue;
        const float lit = std::min (gl.level, 1.37f);
        const float radius = (std::min (node.extent, 0.07f) + 0.05f * lit) * projection.focal / depth;
        const float a = 255.0f * 0.32f * std::min (1.0f, lit);
        splat (t, x, y, std::min (90.0f, radius), gl.r * a, gl.g * a, gl.b * a);
    }
    for (int side = brain::kLeft; side <= brain::kRight; ++side)
        for (int end = 0; end < 2; ++end)
        {
            float x = 0.0f, y = 0.0f, depth = 1.0f;
            projection.project (anatomy.heschlPoint (side, static_cast<float> (end)), x, y, depth);
            heschlEnds[static_cast<size_t> (side)][static_cast<size_t> (end)] = { well.getX() + x / renderScale, well.getY() + y / renderScale };
        }

    lastDrawn = t.maxX >= t.minX ? juce::Rectangle<int>::leftTopRightBottom (t.minX, t.minY, t.maxX + 1, t.maxY + 1) : juce::Rectangle<int>();

    pendingUpload = pendingUpload.getUnion (cleared.getUnion (lastDrawn));

    lastRenderMs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - started) * 1000.0;
}

void BrainView::uploadScene()
{
    // Into the image: only what changed (what was cleared and what was drawn). A native image: a window that draws on
    // the GPU (Direct2D) takes its pixels as they are (no format conversion). After a write JUCE marks every GPU page of
    // the image outdated and re-uploads the whole image when it is next drawn, so the rectangle saves the CPU-side copy;
    // the BitmapData itself makes JUCE allocate one small releaser object on Windows.
    const auto changed = pendingUpload.getIntersection (canvas.getBounds());
    pendingUpload = {};
    if (changed.isEmpty())
        return;
    juce::Image::BitmapData out (canvas, changed.getX(), changed.getY(), changed.getWidth(), changed.getHeight(), juce::Image::BitmapData::writeOnly);
    const auto bytes = static_cast<size_t> (changed.getWidth()) * sizeof (uint32_t);
    for (int y = 0; y < changed.getHeight(); ++y)
    {
        const auto* from = pixels.data() + static_cast<size_t> ((changed.getY() + y) * pixelsWidth + changed.getX());
        if (out.pixelFormat == juce::Image::ARGB && out.pixelStride == 4)
            std::memcpy (out.getLinePointer (y), from, bytes);
        else
            for (int x = 0; x < changed.getWidth(); ++x)
                out.setPixelColour (x, y, juce::Colour (from[x]));
    }
}

// =============================================================================
// Layout and painting
// =============================================================================
void BrainView::resized()
{
    well = getLocalBounds().toFloat();
    const bool roomy = well.getHeight() >= 260.0f;
    plot = well.withTrimmedTop (roomy ? 26.0f : 4.0f).withTrimmedBottom (roomy ? 40.0f : 18.0f).reduced (8.0f, 0.0f);
    const float display = juce::jlimit (1.0f, 2.0f, juce::Component::getApproximateScaleFactorForComponent (this));
    const double logicalPixels = static_cast<double> (well.getWidth()) * well.getHeight();
    renderScale = logicalPixels > 0.0 ? static_cast<float> (std::min (static_cast<double> (display), std::sqrt (kMaxPixels / logicalPixels))) : 1.0f;
    renderScale = std::max (0.25f, renderScale);
    const int w = std::max (1, juce::roundToInt (well.getWidth() * renderScale)), h = std::max (1, juce::roundToInt (well.getHeight() * renderScale));
    if (well.isEmpty())
    {
        canvas = {};
        return;
    }
    if (! canvas.isValid() || canvas.getWidth() != w || canvas.getHeight() != h)
    {
        // The frame is drawn in `pixels` and copied into a native image (opaque ARGB).
        canvas = juce::Image (juce::Image::ARGB, w, h, false, juce::NativeImageType());
        pixels.assign (static_cast<size_t> (w) * static_cast<size_t> (h), 0u);
        pixelsWidth = w;
        pixelsHeight = h;
        fillBackgroundRow();
    }
    lastDrawn = { 0, 0, w, h }; // clear everything once
    dirty = true;
    renderLegend();
}

void BrainView::lookAndFeelChanged()
{
    background = { Palette::well.getRed(), Palette::well.getGreen(), Palette::well.getBlue() };
    fillBackgroundRow();
    renderLegend();
    lastDrawn = canvas.isValid() ? canvas.getBounds() : juce::Rectangle<int>();
    dirty = true;
    repaint();
}

void BrainView::fillBackgroundRow()
{
    if (pixelsWidth <= 0)
        return;
    const juce::PixelARGB px (255, background[0], background[1], background[2]);
    backgroundRow.assign (static_cast<size_t> (pixelsWidth), px.getNativeARGB());
}

void BrainView::paint (juce::Graphics& g)
{
    if (dirty)
        renderScene();
    uploadScene();
    if (canvas.isValid())
    {
        // The opaque canvas without its four corners, then the corners rounded like a plot well (a rectangle
        // clip keeps the blit a straight copy; a rounded clip path would make it an edge-table blend).
        const auto area = well.toNearestInt();
        const int r = juce::roundToInt (kWellRadius);
        const juce::Rectangle<int> corners[4] = { { area.getX(), area.getY(), r, r },
                                                  { area.getRight() - r, area.getY(), r, r },
                                                  { area.getX(), area.getBottom() - r, r, r },
                                                  { area.getRight() - r, area.getBottom() - r, r, r } };
        {
            juce::Graphics::ScopedSaveState state (g);
            for (const auto& c : corners)
                g.excludeClipRegion (c);
            if (renderScale == 1.0f)
                g.drawImageAt (canvas, area.getX(), area.getY());
            else
                g.drawImage (canvas, well, juce::RectanglePlacement::stretchToFit);
        }
        g.setColour (Palette::well);
        for (const auto& c : corners)
        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (c);
            g.fillRoundedRectangle (well, kWellRadius);
        }
    }
    else
    {
        drawWell (g, well);
    }
    paintOverlay (g);
}

void BrainView::renderLegend()
{
    const bool roomy = well.getHeight() >= 260.0f;
    legendArea = well.reduced (12.0f, 0.0f).withTrimmedTop (well.getHeight() - (roomy ? 38.0f : 17.0f)).withTrimmedBottom (1.0f);
    if (legendArea.isEmpty())
    {
        legendImage = {};
        return;
    }
    const float scale = juce::jlimit (1.0f, 3.0f, juce::Component::getApproximateScaleFactorForComponent (this));
    legendImage = juce::Image (juce::Image::ARGB, juce::roundToInt (legendArea.getWidth() * scale), juce::roundToInt (legendArea.getHeight() * scale), true);
    juce::Graphics g (legendImage);
    g.addTransform (juce::AffineTransform::translation (-legendArea.getX(), -legendArea.getY()).scaled (scale));
    auto bottom = legendArea;
    const auto colourOf = [] (const std::array<float, 3>& c) { return juce::Colour::fromFloatRGBA (c[0], c[1], c[2], 1.0f); };
    if (roomy)
    {
        auto legend = bottom.removeFromTop (17.0f).withTrimmedRight (200.0f); // the hint sits at the right
        // The signal's colour: low (red) to high (cyan) notes.
        const auto bar = legend.removeFromLeft (34.0f).withSizeKeepingCentre (30.0f, 4.0f);
        juce::ColourGradient grad (colourOf (brain::frequencyRgb (60.0)), bar.getX(), 0.0f, colourOf (brain::frequencyRgb (12000.0)), bar.getRight(), 0.0f, false);
        grad.addColour (0.5, colourOf (brain::frequencyRgb (800.0)));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (bar, 2.0f);
        g.setFont (Theme::font (11.0f));
        g.setColour (Palette::muted);
        const juce::String lowHigh ("low to high note");
        const float lw = juce::GlyphArrangement::getStringWidth (Theme::font (11.0f), lowHigh) + 14.0f;
        g.drawText (lowHigh, legend.removeFromLeft (lw), juce::Justification::centredLeft, false);
        drawLegend (g, legend,
                    { { colourOf (brain::BrainActivity::kBeatRgb), "Beat (movement areas)" },
                      { colourOf (brain::BrainActivity::kChordRgb), "Surprise chord (frontal)" },
                      { colourOf (brain::BrainActivity::kDopamineRgb), "Dopamine (build-up, drop)" },
                      { Palette::text.withAlpha (0.6f), "Brighter, bigger spot = more intense" } });
    }
    // The caption: the longest wording that fits (only the hearing pathway is slowed 20 times; the beat, chord and
    // reward responses run at roughly the mockup's delays, not to scale).
    static const char* const captions[] = {
        "A model from published research, driven by the music playing now - not a scan of your brain. The trip from ear to "
        "cortex (about 15 ms) is shown 20 times slower; the beat, chord and reward responses are not to scale.",
        "A model from published research driven by the music, not a scan of your brain. Ear to cortex (about 15 ms) shown "
        "20 times slower; beat, chord and reward not to scale.",
        "A research-based model driven by the music, not a brain scan. Ear to cortex shown 20x slower; beat, chord, reward "
        "not to scale.",
    };
    const auto captionFont = Theme::font (roomy ? 11.5f : 10.5f);
    juce::String caption;
    for (const char* text : captions)
    {
        caption = juce::String (text);
        if (juce::GlyphArrangement::getStringWidth (captionFont, caption) * 0.9f <= bottom.getWidth())
            break;
    }
    g.setFont (captionFont);
    g.setColour (Palette::text.withAlpha (0.72f));
    g.drawFittedText (caption, bottom.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
}

void BrainView::paintOverlay (juce::Graphics& g)
{
    const bool roomy = well.getHeight() >= 260.0f;
    const auto now = activity.getNow();
    // In the visualiser window's full screen its header floats over the view's top: the pills go under it.
    const auto* window = findParentComponentOfClass<VisualiserWindow>();
    const float headerGap = window != nullptr && window->isFullScreenMode() ? 44.0f : 0.0f;
    auto top = well.reduced (12.0f, 0.0f).withTrimmedTop (headerGap).withHeight (roomy ? 26.0f : 0.0f).withTrimmedTop (5.0f);

    // Status: what was detected just now.
    if (roomy)
    {
        float x = top.getX();
        const auto pill = [&g, &x, &top] (const juce::String& text, juce::Colour colour, float alpha)
        {
            const auto font = Theme::font (11.0f, true);
            const float w = juce::GlyphArrangement::getStringWidth (font, text) + 16.0f;
            const auto r = juce::Rectangle<float> (x, top.getY(), w, 17.0f);
            g.setColour (colour.withAlpha (0.18f + 0.5f * alpha));
            g.fillRoundedRectangle (r, 8.5f);
            g.setColour (Palette::text.withAlpha (0.45f + 0.55f * alpha));
            g.setFont (font);
            g.drawText (text, r, juce::Justification::centred, false);
            x += w + 6.0f;
        };
        const auto fade = [now] (double when, double seconds) { return static_cast<float> (std::clamp (1.0 - (now - when) / seconds, 0.0, 1.0)); };
        const auto beat = juce::Colour::fromFloatRGBA (brain::BrainActivity::kBeatRgb[0], brain::BrainActivity::kBeatRgb[1], brain::BrainActivity::kBeatRgb[2], 1.0f);
        const auto chordColour = juce::Colour::fromFloatRGBA (brain::BrainActivity::kChordRgb[0], brain::BrainActivity::kChordRgb[1], brain::BrainActivity::kChordRgb[2], 1.0f);
        const auto dopamine = juce::Colour::fromFloatRGBA (brain::BrainActivity::kDopamineRgb[0], brain::BrainActivity::kDopamineRgb[1], brain::BrainActivity::kDopamineRgb[2], 1.0f);
        if (const float a = fade (activity.getLastBeatTime(), 0.4); a > 0.0f)
            pill ("Beat", beat, a);
        const auto& chord = listener.getTracker().getChord();
        if (chord.isChord())
        {
            char name[32] = {};
            music::formatChord (chord, music::keyPrefersFlats (listener.getMusic().key.getKey()), name, sizeof (name));
            const bool surprise = listener.getLastChordSurprise() == 1 && now - activity.getLastSurpriseTime() < 2.0;
            pill (juce::String ("Chord ") + juce::String (name) + (surprise ? juce::String (" (outside the key)") : juce::String()), chordColour,
                  surprise ? fade (activity.getLastSurpriseTime(), 2.0) : 0.0f);
        }
        if (listener.isBuildUp())
            pill ("Build-up", dopamine, 0.6f);
        if (const float a = fade (activity.getLastDropTime(), 2.5); a > 0.0f)
            pill ("Drop", dopamine, a);

        // The hint at the right of the legend row (clear of the window's header and buttons).
        g.setFont (labelFont());
        g.setColour (labelColour());
        g.drawText (turning ? "Drag to turn, click to stop turning" : "Drag to turn, click to turn again", legendArea.withHeight (17.0f),
                    juce::Justification::centredRight, false);
    }

    // Legend and caption at the bottom (static: drawn once into legendImage).
    if (legendImage.isValid())
        g.drawImage (legendImage, legendArea, juce::RectanglePlacement::stretchToFit);

    // The hovered landing spot's name.
    if (hovered >= 0)
    {
        const auto& node = anatomy.getNode (hovered);
        juce::String name (node.name);
        if (node.station != brain::Station::Vta)
            name << " (" << sideName (node.side) << ")";
        if (node.heuristic)
            name << juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 heuristic"));
        const auto font = Theme::font (12.0f, true);
        const float w = juce::GlyphArrangement::getStringWidth (font, name) + 18.0f;
        auto box = juce::Rectangle<float> (w, 22.0f).withPosition (hoverPosition.translated (14.0f, -30.0f));
        box = box.constrainedWithin (well.reduced (4.0f));
        g.setColour (Palette::tooltip.withAlpha (0.92f));
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (box, 6.0f, 1.0f);
        g.setColour (Palette::text);
        g.setFont (font);
        g.drawText (name, box, juce::Justification::centred, false);
        const auto at = getNodeScreenPosition (hovered);
        if (node.kind != brain::NodeKind::Heschl)
        {
            g.setColour (Palette::text.withAlpha (0.6f));
            g.drawEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (at), 1.2f);
        }
    }
}
} // namespace flub::app::ui::vis
