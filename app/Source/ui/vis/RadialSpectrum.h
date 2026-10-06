// Flubsound Pro - radial spectrum (visualiser "radial-spectrum").
//
// The output spectrum wrapped around a ring that pulses with the music:
// log frequency runs from the top of the ring (kLowHz) clockwise down the
// right-hand side to the bottom (kHighHz), mirrored on the left so the
// figure is symmetric; the level pushes each of the kBins rays outwards
// from the inner ring. The rays are coloured along a synthwave gradient
// from the accent (lows) through violet to the other mode's colour
// (highs), with a glowing outline through their tips and a peak dot per
// ray that falls back slowly. A bass pulse (the 30 - 120 Hz level rising
// over its own 0.3 s average as it rises: kicks) swells the inner ring and the halo
// behind it; a ring of dots turns slowly, faster with louder music. The
// centre shows the momentary loudness (LUFS) while the strip plays.
//
// Data: the analyser's displayed post levels at the rays' frequencies
// (tilted +4.5 dB/octave around 1 kHz, like the spectrum), smoothed (fast
// rise, ~0.2 s fall), with the same automatic range as the 3D waterfall.
// No second analysis. Message thread only; paths and the halo image are
// allocated in the constructor / resized(), a frame allocates nothing.
#pragma once

#include "Visualiser.h"

#include <array>

namespace flub::app::ui::vis
{
class RadialSpectrum : public Visualiser
{
public:
    static constexpr int kBins = 84;   // rays per half
    static constexpr int kColourGroups = 14;
    static constexpr int kDots = 72;   // the turning ring
    static constexpr double kLowHz = 30.0, kHighHz = 16000.0;
    static constexpr float kTiltDbPerOctave = 4.5f;
    static constexpr float kRangeDb = 54.0f;
    static constexpr float kReleaseSeconds = 0.2f, kPeakFallDbPerSecond = 30.0f, kPeakHoldSeconds = 0.35f;

    RadialSpectrum();

    void reset() override;
    void advance (const FrameContext& frame) override;

    // ---- Pure mapping (tested) -------------------------------------------------------------
    static double binHz (int bin) noexcept;
    /** Angle (radians, clockwise from the top) of a frequency on the right
        half: 0 at kLowHz .. pi at kHighHz; the left half is its mirror (-angle). */
    static float angleFor (double hz) noexcept;
    /** The point at `angle` (clockwise from the top) and `radius` from `centre`. */
    static juce::Point<float> polar (juce::Point<float> centre, float angle, float radius) noexcept;
    /** Radius of a level 0..1 between the inner ring and the outer limit. */
    static float radiusFor (float level, float inner, float outer) noexcept;
    /** Level 0..1 of a (tilted) dB value for the range top `topDb`. */
    static float levelFor (float db, float topDb) noexcept;

    // ---- State (tests) ----------------------------------------------------------------------
    /** The smoothed level 0..1 of ray `bin`. */
    float getLevel (int bin) const noexcept { return levels[static_cast<size_t> (bin)]; }
    float getPulse() const noexcept { return pulse; }
    float getTopDb() const noexcept { return topDb; }
    juce::Point<float> getCentre() const noexcept { return centre; }
    float getInnerRadius() const noexcept;
    float getOuterRadius() const noexcept { return outerRadius; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void lookAndFeelChanged() override;
    void parentHierarchyChanged() override { lookAndFeelChanged(); }

private:
    void rebuildPaths();
    void renderHalo();
    /** Distance of the turning dot ring outside the outer limit. */
    float ringGap() const noexcept;

    std::array<double, kBins> hz {};
    std::array<float, kBins> tiltDb {}, levels {}, peaks {}, peakAge {};
    float topDb = -30.0f, pulse = 0.0f, bassFast = -66.0f, bassSlow = -66.0f, rotation = 0.0f, loudness = -160.0f;
    bool active = false, anyData = false;

    std::array<juce::Path, kColourGroups> rays;
    std::array<juce::Colour, kColourGroups> rayColours;
    juce::Path outline, peakDots, ringDots;
    juce::Image halo;
    juce::Colour accent, farColour;
    juce::Rectangle<float> well;
    juce::Point<float> centre;
    float outerRadius = 100.0f, baseInner = 30.0f;
};
} // namespace flub::app::ui::vis
