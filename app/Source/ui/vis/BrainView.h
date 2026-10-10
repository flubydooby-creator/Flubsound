// Flubsound Pro - "Brain": how the music playing now travels through a model of
// the human brain (visualiser "brain", main view and visualiser window;
// docs/06 §6.4.2, docs/12 §8).
//
// A rotating 3D point-cloud brain (two folded hemispheres, cerebellum,
// brainstem) with the ears, the cochlea spirals and the nerve tracts that
// connect every station (BrainAnatomy). What BrainListener hears in the post
// tap lights it (BrainActivity): each band enters the cochlea at its place
// and runs up the auditory pathway, mostly to the opposite hemisphere, to
// Heschl's gyrus at its tonotopic place, shown 20 times slower than real;
// beats run through the cerebellum, thalamus, premotor cortex, SMA and
// putamen, chord changes to the inferior frontal gyrus, build-ups and drops
// release dopamine to the caudate and the nucleus accumbens. A signal lights
// the tract itself as a bright segment that runs along it and fades; landing
// spots grow brighter and bigger with the level that reaches them and go
// dark in silence. It is a model from published research driven by the
// music, not a scan of the listener (the caption says so).
//
// Rendering: the app's own CPU rendering (no OpenGL): the points, tracts and
// glows are projected (perspective, the mockup's camera) and splatted
// additively into a preallocated 32-bit frame buffer (integer arithmetic, a
// hue-preserving saturation: a crowded spot reaches its hue at full brightness,
// never white), the points in a fixed top-to-bottom order (cache friendly);
// only the changed rectangle is copied into a native image (BitmapData; a
// Direct2D window then re-uploads the whole image to the GPU when it next
// draws it, so the rectangle saves the CPU-side copy). The view's axes are a
// left-handed frame (x right, y up, z front), so the projection mirrors screen
// x: the picture is the brain itself, not its mirror image. The legend and
// caption are drawn once per size into an image; the status pills and hover
// label are drawn with Graphics. A new picture is rendered in paint() when one
// is due, at most kMaxFramesPerSecond (every second frame on a 144 Hz display;
// the model itself advances every frame), at the display's scale up to
// kMaxPixels (beyond that rendered smaller and scaled up); a still (not turning,
// not dragged), dark brain is not rendered again (the overlay still is).
//
// Mouse: drag turns the brain, a click stops or restarts the turning, the
// right-click menu has Stop turning / Turn and Reset view (in the visualiser
// window these join its own menu, and a double-click toggles full screen);
// hovering a landing spot names it (a label in the view and the tooltip).
//
// Message thread only. Buffers, images and model are allocated in the
// constructor, setSampleRate and resized(); pushPost(), advance() and
// renderScene() allocate nothing. On Windows uploadScene()'s BitmapData on the
// Direct2D image makes JUCE allocate one small releaser object per frame.
#pragma once

#include "BrainActivity.h"
#include "BrainListener.h"
#include "Visualiser.h"

#include <array>
#include <vector>

namespace flub::app::ui::vis
{
class BrainView : public Visualiser, public juce::TooltipClient
{
public:
    /** Turning speed; the angle decreases, so the face moves to the viewer's right first (the mockup's direction). */
    static constexpr float kTurnRadiansPerSecond = 0.16f;
    /** The mockup's pose: the face to the viewer's right, the right hemisphere nearer. */
    static constexpr float kStartAngle = -0.5f, kStartTilt = 0.15f;
    static constexpr float kMinTilt = -0.6f, kMaxTilt = 0.9f;
    static constexpr double kMaxPixels = 2.3e6; // the rendered image at most (scaled up beyond)
    static constexpr double kMaxFramesPerSecond = 75.0; // pictures rendered at most (the model runs every frame)

    BrainView();
    ~BrainView() override;

    void setSampleRate (double sampleRate) override;
    void reset() override;
    void pushPost (const float* mid, const float* side, int numSamples) override;
    void advance (const FrameContext& frame) override;
    void addMenuItems (juce::PopupMenu& menu) override;

    // ---- View ---------------------------------------------------------------------------------
    bool isTurning() const noexcept { return turning; }
    void setTurning (bool shouldTurn);
    /** Back to the starting angle and tilt. */
    void resetView();
    /** Turns the brain to an angle and tilt (tests; the turning, if on, goes on from there). */
    void setView (float newAngle, float newTilt);
    float getAngle() const noexcept { return angle; }
    float getTilt() const noexcept { return tilt; }

    // ---- Model (tests) --------------------------------------------------------------------------
    const brain::BrainActivity& getActivity() const noexcept { return activity; }
    const brain::BrainListener& getListener() const noexcept { return listener; }
    /** Display time (seconds since the view started). */
    double getClock() const noexcept { return clock; }

    // ---- Rendering --------------------------------------------------------------------------------
    /** Renders the scene into the frame buffer now (paint() does this once per new frame); allocation-free. */
    void renderScene() noexcept;
    /** Copies what renderScene changed into the image paint() draws (paint() calls it). */
    void uploadScene();
    const juce::Image& getCanvas() const noexcept { return canvas; }
    /** Milliseconds the last renderScene took. */
    double getLastRenderMs() const noexcept { return lastRenderMs; }
    /** Scenes rendered so far (a still, dark view stops rendering new ones). */
    int64_t getRenderCount() const noexcept { return renders; }
    /** Where a point of the model (view units) lands with the last render's camera: component coordinates and its
        depth (larger = further away); false when it is behind the camera. */
    bool projectPoint (brain::Vec3 p, juce::Point<float>& at, float& depth) const noexcept;
    /** Where a node was drawn in the last render (component coordinates). */
    juce::Point<float> getNodeScreenPosition (int node) const noexcept;
    /** The landing spot under a point (component coordinates) in the last render, or -1. */
    int findNodeAt (juce::Point<float> position) const noexcept;
    int getHoveredNode() const noexcept { return hovered; }
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }
    /** What the hover label and tooltip say for a node. */
    static juce::String describeNode (int node);

    juce::String getTooltip() override;
    /** The dragging hand, hidden while the owner hides its own (the visualiser window's idle full screen). */
    juce::MouseCursor getMouseCursor() override;
    void paint (juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

private:
    struct Projection
    {
        std::array<float, 9> m {};
        std::array<float, 3> t {};
        float cx = 0.0f, cy = 0.0f, focal = 1.0f, pointScale = 1.0f;
        /** Screen x, y (image pixels) and depth; false behind the camera. */
        bool project (brain::Vec3 p, float& x, float& y, float& depth) const noexcept;
    };
    struct Target
    {
        uint32_t* data = nullptr; // the frame buffer: one 32-bit ARGB word per pixel
        int stride = 0, width = 0, height = 0;
        int minX = 0, minY = 0, maxX = -1, maxY = -1; // what was drawn (for the next clear)
    };

    void updateProjection() noexcept;
    void splat (Target& t, float x, float y, float radius, float r, float g, float b) const noexcept;
    /** A thick anti-aliased line from a to b (screen x, y), colours c0 -> c1 (0..255), with a glow; the joints
        with the neighbouring segments are cut along the given boundary normals (unnormalised directions). */
    void segment (Target& t, const float* a, const float* b, float halfWidth, const float* c0, const float* c1, float glow, const float* startNormal,
                  const float* endNormal) const noexcept;
    void drawPolyline (Target& t, const brain::Vec3* points, int count, float radius, const float* colours, const float* levels, bool closed) noexcept;
    void fillBackgroundRow();
    void renderLegend();
    void showMenu();
    void paintOverlay (juce::Graphics& g);

    const brain::BrainAnatomy& anatomy;
    brain::BrainListener listener;
    brain::BrainActivity activity;
    double clock = 0.0;
    float angle = kStartAngle, tilt = kStartTilt;
    bool turning = true;

    // Mouse.
    bool pressed = false, dragged = false;
    juce::Point<float> pressPosition;
    float pressAngle = 0.0f, pressTilt = 0.0f;
    int hovered = -1;
    juce::Point<float> hoverPosition;

    // Rendering.
    juce::Image canvas;
    juce::Image legendImage; // the legend and the caption (static), drawn once per size / theme
    juce::Rectangle<float> legendArea;
    float renderScale = 1.0f;
    juce::Rectangle<float> well, plot;
    juce::Rectangle<int> lastDrawn;
    bool dirty = true;
    bool lastRenderStill = false; // the last picture showed a still, dark brain
    int64_t renders = 0;
    double lastRenderMs = 0.0;
    Projection projection;
    std::array<uint8_t, 3> background {};
    std::vector<uint32_t> backgroundRow; // one row of the background (the clear copies it)
    std::vector<uint32_t> pixels;        // the frame buffer (JUCE's ARGB layout), copied into canvas
    int pixelsWidth = 0, pixelsHeight = 0;
    juce::Rectangle<int> pendingUpload; // changed since the last upload
    double sinceRender = 1.0;
    std::vector<float> polyColours, polyLevels, screen; // scratch for one polyline, per point: RGB, level, x / y / depth
    std::array<juce::Point<float>, brain::kNumNodes> nodeScreen {};
    std::array<float, brain::kNumNodes> nodeRadius {};
    std::array<std::array<juce::Point<float>, 2>, 2> heschlEnds {};
    std::array<int, brain::BrainAnatomy::kSpiralPoints> spiralBand {};
    std::array<float, brain::BrainAnatomy::kSpiralPoints> spiralFrac {};
    std::array<std::array<float, 3>, brain::BrainAnatomy::kSpiralPoints> spiralRgb {};
    std::array<std::array<float, 3>, brain::kBands> bandRgb {};
    std::array<std::array<float, brain::kBands>, 2> heschlLevel {}; // this frame's, per side and band
    std::array<float, 65> profile {};
    std::array<int, 65> profileFixed {}; // the same, 0..256
};
} // namespace flub::app::ui::vis
