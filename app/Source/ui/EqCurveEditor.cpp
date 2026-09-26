#include "EqCurveEditor.h"

#include "ParameterBinding.h"
#include "Theme.h"

#include <cmath>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
constexpr float kNodeRadius = 7.5f;
constexpr float kHitRadius = 12.0f;

// Frequencies of the dynamic EQ's internal mode bands 4..7 (mirrors
// configureModeBands() in core/src/engine/ProcessingChain.cpp, which the
// core does not expose).
constexpr std::array<float, 4> kGamingModeBandHz { 3200.0f, 260.0f, 90.0f, 2000.0f };
constexpr std::array<float, 4> kMusicModeBandHz { 3500.0f, 12000.0f, 120.0f, 1000.0f };

const char* typeName (EqBandType t)
{
    switch (t)
    {
        case EqBandType::Bell: return "Bell";
        case EqBandType::LowShelf: return "Low Shelf";
        case EqBandType::HighShelf: return "High Shelf";
        case EqBandType::LowCut: return "Low Cut";
        case EqBandType::HighCut: return "High Cut";
        case EqBandType::Notch: return "Notch";
        case EqBandType::BandPass: return "Band Pass";
    }
    return "";
}
} // namespace

EqCurveEditor::EqCurveEditor (SpectrumAnalyzer& g, StoreProvider s)
    : geometry (g), storeProvider (std::move (s))
{
    setOpaque (false);
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
    setTitle ("Parametric EQ curve");
    setDescription ("Drag a numbered node to change its frequency and gain, use the mouse wheel for Q, "
                    "double-click to reset a band and right-click for type, slope and enable.");
    setHelpText (getDescription());
    dynFreqs.fill (1000.0f);
}

juce::Colour EqCurveEditor::bandColour (int band)
{
    static const juce::Colour colours[] = { juce::Colour (0xfff87171), juce::Colour (0xfffb923c), juce::Colour (0xfffbbf24),
                                            juce::Colour (0xffa3e635), juce::Colour (0xff34d399), juce::Colour (0xff22d3ee),
                                            juce::Colour (0xff60a5fa), juce::Colour (0xff818cf8), juce::Colour (0xffc084fc),
                                            juce::Colour (0xfff472b6) };
    return colours[juce::jlimit (0, 9, band)];
}

bool EqCurveEditor::typeHasGain (EqBandType type) noexcept
{
    return type == EqBandType::Bell || type == EqBandType::LowShelf || type == EqBandType::HighShelf;
}

void EqCurveEditor::setSampleRate (double newSampleRate)
{
    if (newSampleRate > 0.0 && std::abs (newSampleRate - sampleRate) > 0.5)
    {
        sampleRate = newSampleRate;
        rebuildCurve();
        repaint();
    }
}

void EqCurveEditor::setRangeDb (float newRange)
{
    rangeDb = juce::jlimit (3.0f, 24.0f, newRange);
    rebuildCurve();
    repaint();
}

void EqCurveEditor::selectBand (int band)
{
    band = juce::jlimit (-1, kBands - 1, band);
    if (band == selected)
        return;
    selected = band;
    rebuildCurve();
    repaint();
    if (onBandSelected != nullptr)
        onBandSelected (selected);
}

// =============================================================================
// Model
// =============================================================================
void EqCurveEditor::refresh (bool force)
{
    auto* store = storeProvider != nullptr ? storeProvider() : nullptr;
    if (store == nullptr)
        return;
    if (! force && store == lastStore && store->version() == lastVersion)
        return;
    lastStore = store;
    lastVersion = store->version();

    for (int b = 0; b < kBands; ++b)
    {
        auto& p = bands[static_cast<size_t> (b)];
        p.enabled = store->get (eq (b, EqFieldOn)) >= 0.5f;
        p.type = static_cast<EqBandType> (juce::jlimit (0, 6, static_cast<int> (std::lround (store->get (eq (b, EqFieldType))))));
        p.frequency = store->get (eq (b, EqFieldFreq));
        p.gainDb = store->get (eq (b, EqFieldGain));
        p.q = store->get (eq (b, EqFieldQ));
        p.slopeDbPerOct = 12 * (static_cast<int> (std::lround (store->get (eq (b, EqFieldSlope)))) + 1);
    }
    outputGainDb = store->get (EqOutputGainDb);
    eqEnabled = store->get (EqOn) >= 0.5f;

    // User dynamic bands: their frequency comes from the store.
    for (int b = 0; b < kDynEqBands; ++b)
        dynFreqs[static_cast<size_t> (b)] = store->get (dyn (b, DynFieldFreq));

    rebuildCurve();
    repaint();
}

void EqCurveEditor::setDynamicEqState (const std::array<float, kNumDynMarkers>& gainsDb, ModeValue mode)
{
    const auto& modeHz = mode == ModeValue::Gaming ? kGamingModeBandHz : kMusicModeBandHz;
    bool changed = false;
    for (size_t b = 0; b < 4; ++b)
    {
        changed = changed || dynFreqs[b + 4] != modeHz[b];
        dynFreqs[b + 4] = modeHz[b];
    }
    for (size_t b = 0; b < gainsDb.size(); ++b)
    {
        const float g = std::isfinite (gainsDb[b]) ? gainsDb[b] : 0.0f;
        changed = changed || std::abs (g - dynGains[b]) > 0.05f;
    }
    if (changed)
    {
        for (size_t b = 0; b < gainsDb.size(); ++b)
            dynGains[b] = std::isfinite (gainsDb[b]) ? gainsDb[b] : 0.0f;
        repaint();
    }
}

void EqCurveEditor::set (int band, EqField field, float value)
{
    if (auto* store = storeProvider != nullptr ? storeProvider() : nullptr)
    {
        store->set (eq (band, field), value);
        refresh (true);
    }
}

void EqCurveEditor::resetBand (int band)
{
    const auto& table = layout();
    for (const auto field : { EqFieldOn, EqFieldType, EqFieldFreq, EqFieldGain, EqFieldQ, EqFieldSlope })
        set (band, field, table[static_cast<size_t> (eq (band, field))].defaultValue);
}

// =============================================================================
// Geometry
// =============================================================================
float EqCurveEditor::yForGain (float db) const noexcept
{
    const auto plot = geometry.getPlotArea();
    const float t = juce::jlimit (-1.08f, 1.08f, db / rangeDb);
    return plot.getCentreY() - t * plot.getHeight() * 0.5f * 0.92f;
}

float EqCurveEditor::gainForY (float y) const noexcept
{
    const auto plot = geometry.getPlotArea();
    return (plot.getCentreY() - y) / (plot.getHeight() * 0.5f * 0.92f) * rangeDb;
}

juce::Point<float> EqCurveEditor::nodePosition (int band) const
{
    const auto& p = bands[static_cast<size_t> (band)];
    return { geometry.xForFrequency (p.frequency), yForGain (typeHasGain (p.type) ? p.gainDb : 0.0f) };
}

int EqCurveEditor::nodeAt (juce::Point<float> pos) const
{
    int best = -1;
    float bestDistance = kHitRadius;
    // Selected band first so overlapping nodes stay grabbable.
    for (int i = -1; i < kBands; ++i)
    {
        const int b = i < 0 ? selected : i;
        if (b < 0)
            continue;
        const float d = nodePosition (b).getDistanceFrom (pos);
        if (d < bestDistance)
        {
            bestDistance = d - (b == selected ? 3.0f : 0.0f);
            best = b;
        }
    }
    return best;
}

void EqCurveEditor::resized()
{
    rebuildCurve();
}

void EqCurveEditor::rebuildCurve()
{
    curve.clear();
    curveFill.clear();
    selectedFill.clear();

    const auto plot = geometry.getPlotArea();
    if (plot.isEmpty())
        return;

    const float zeroY = yForGain (0.0f);
    const float step = 2.0f;
    bool first = true;
    for (float x = plot.getX(); x <= plot.getRight() + 0.01f; x += step)
    {
        const double hz = geometry.frequencyForX (x);
        const auto db = static_cast<float> (flub::ParametricEq::responseDb (bands.data(), kBands, hz, sampleRate)) + outputGainDb;
        const float y = juce::jlimit (plot.getY() - 2.0f, plot.getBottom() + 2.0f, yForGain (db));
        if (first)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
        first = false;
    }

    curveFill = curve;
    curveFill.lineTo (plot.getRight(), zeroY);
    curveFill.lineTo (plot.getX(), zeroY);
    curveFill.closeSubPath();

    if (selected >= 0 && bands[static_cast<size_t> (selected)].enabled)
    {
        const auto& band = bands[static_cast<size_t> (selected)];
        first = true;
        for (float x = plot.getX(); x <= plot.getRight() + 0.01f; x += step)
        {
            const auto db = static_cast<float> (flub::ParametricEq::responseDb (&band, 1, geometry.frequencyForX (x), sampleRate));
            const float y = juce::jlimit (plot.getY(), plot.getBottom(), yForGain (db));
            if (first)
                selectedFill.startNewSubPath (x, zeroY);
            selectedFill.lineTo (x, y);
            first = false;
        }
        selectedFill.lineTo (plot.getRight(), zeroY);
        selectedFill.closeSubPath();
    }
}

// =============================================================================
// Painting
// =============================================================================
juce::String EqCurveEditor::describeBand (int band) const
{
    const auto& p = bands[static_cast<size_t> (band)];
    juce::String s;
    s << juce::String (band + 1) << "  " << typeName (p.type) << "   " << ParamFormat::toText (eq (band, EqFieldFreq), p.frequency);
    if (typeHasGain (p.type))
        s << "   " << ParamFormat::toText (eq (band, EqFieldGain), p.gainDb);
    if (p.type == EqBandType::LowCut || p.type == EqBandType::HighCut)
        s << "   " << p.slopeDbPerOct << " dB/oct";
    else
        s << "   Q " << juce::String (p.q, 2);
    if (! p.enabled)
        s << "   (off)";
    return s;
}

void EqCurveEditor::drawBubble (juce::Graphics& g, int band) const
{
    const auto text = describeBand (band);
    const auto font = Theme::font (12.0f);
    const float w = juce::GlyphArrangement::getStringWidth (font, text) + 20.0f, h = 24.0f;
    const auto node = nodePosition (band);
    const auto plot = geometry.getPlotArea();

    auto r = juce::Rectangle<float> (w, h).withCentre ({ node.x, node.y - 26.0f });
    if (r.getY() < plot.getY() + 2.0f)
        r.setY (node.y + 16.0f);
    r = r.constrainedWithin (plot.reduced (2.0f));

    g.setColour (juce::Colour (0xf01f2530));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (bandColour (band).withAlpha (0.7f));
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    g.setColour (Palette::text);
    g.setFont (font);
    g.drawText (text, r, juce::Justification::centred, false);
}

void EqCurveEditor::paint (juce::Graphics& g)
{
    const auto plot = geometry.getPlotArea();
    if (plot.isEmpty())
        return;

    const auto accent = Theme::accent (*this);
    const float zeroY = yForGain (0.0f);
    const float alpha = eqEnabled ? 1.0f : 0.4f;

    // ---- Right axis: EQ gain ----
    g.setFont (Theme::font (10.5f));
    for (const float frac : { 1.0f, 0.5f, 0.0f, -0.5f, -1.0f })
    {
        const float db = frac * rangeDb;
        const float y = yForGain (db);
        g.setColour (frac == 0.0f ? Palette::muted : Palette::faint);
        const auto label = (db > 0.0f ? "+" : "") + juce::String (juce::roundToInt (db));
        g.drawText (label, juce::Rectangle<float> (plot.getRight() + 5.0f, y - 7.0f, SpectrumAnalyzer::kRightInset - 7.0f, 14.0f),
                    juce::Justification::centredLeft);
    }
    g.setColour (Palette::muted.withAlpha (0.35f));
    g.drawHorizontalLine (juce::roundToInt (zeroY), plot.getX(), plot.getRight());

    g.saveState();
    g.reduceClipRegion (plot.expanded (0.0f, 2.0f).getSmallestIntegerContainer());

    // ---- Selected band contribution ----
    if (! selectedFill.isEmpty())
    {
        g.setColour (bandColour (selected).withAlpha (0.16f * alpha));
        g.fillPath (selectedFill);
    }

    // ---- Combined curve ----
    g.setColour (accent.withAlpha (0.10f * alpha));
    g.fillPath (curveFill);
    g.setColour (Palette::text.withAlpha (0.10f * alpha));
    g.strokePath (curve, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (Palette::text.withAlpha (0.92f * alpha));
    g.strokePath (curve, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // ---- Dynamic EQ ghost markers ----
    for (size_t b = 0; b < dynGains.size(); ++b)
    {
        const float gain = dynGains[b];
        if (std::abs (gain) < 0.1f)
            continue;
        const float x = geometry.xForFrequency (dynFreqs[b]);
        const float y = yForGain (gain);
        const float strength = juce::jlimit (0.35f, 1.0f, std::abs (gain) / 4.0f);
        g.setColour (Palette::dynamicEq.withAlpha (0.55f * strength));
        g.drawLine (x, zeroY, x, y, 2.0f);
        juce::Path diamond;
        diamond.addPolygon ({ x, y }, 4, 5.0f, juce::MathConstants<float>::pi * 0.25f);
        g.setColour (Palette::dynamicEq.withAlpha (0.9f * strength));
        g.fillPath (diamond);
    }

    // ---- Nodes ----
    for (int b = 0; b < kBands; ++b)
    {
        const auto& p = bands[static_cast<size_t> (b)];
        const auto pos = nodePosition (b);
        const auto colour = bandColour (b);
        const bool isSelected = b == selected;
        const float r = kNodeRadius + (isSelected ? 1.5f : 0.0f) + (b == hovered && ! isSelected ? 1.0f : 0.0f);
        const auto circle = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos);

        if (isSelected)
        {
            g.setColour (colour.withAlpha (0.25f));
            g.fillEllipse (circle.expanded (5.0f));
        }
        if (p.enabled)
        {
            g.setColour (colour.withAlpha (alpha));
            g.fillEllipse (circle);
            g.setColour (Palette::background.withAlpha (0.85f));
            g.drawEllipse (circle.reduced (0.5f), 1.2f);
        }
        else
        {
            g.setColour (Palette::well.withAlpha (0.9f));
            g.fillEllipse (circle);
            g.setColour (colour.withAlpha (0.55f));
            g.drawEllipse (circle.reduced (0.75f), 1.5f);
        }
        g.setColour (p.enabled ? Palette::background : colour.withAlpha (0.8f));
        g.setFont (Theme::font (9.5f, true));
        g.drawText (juce::String (b + 1), circle.translated (0.0f, 0.5f), juce::Justification::centred, false);
    }

    const int bubbleBand = dragging >= 0 ? dragging : hovered;
    if (bubbleBand >= 0)
        drawBubble (g, bubbleBand);

    g.restoreState();

    if (hasKeyboardFocus (false))
    {
        g.setColour (accent.withAlpha (0.45f));
        g.drawRoundedRectangle (plot.expanded (1.0f), 6.0f, 1.0f);
    }
}

// =============================================================================
// Interaction
// =============================================================================
void EqCurveEditor::mouseMove (const juce::MouseEvent& e)
{
    const int b = nodeAt (e.position);
    if (b != hovered)
    {
        hovered = b;
        setMouseCursor (b >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void EqCurveEditor::mouseExit (const juce::MouseEvent&)
{
    if (hovered >= 0)
    {
        hovered = -1;
        repaint();
    }
}

void EqCurveEditor::mouseDown (const juce::MouseEvent& e)
{
    const int b = nodeAt (e.position);
    if (e.mods.isPopupMenu())
    {
        if (b >= 0)
        {
            selectBand (b);
            showBandMenu (b);
        }
        return;
    }

    selectBand (b);
    dragging = b;
    if (b >= 0)
    {
        const auto& p = bands[static_cast<size_t> (b)];
        dragStartFreq = p.frequency;
        dragStartGain = p.gainDb;
        dragStartPos = e.position;
        if (! p.enabled)
            set (b, EqFieldOn, 1.0f); // grabbing a disabled node switches it on
    }
}

void EqCurveEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging < 0)
        return;

    const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
    const auto delta = (e.position - dragStartPos) * fine;
    const float startX = geometry.xForFrequency (dragStartFreq);
    const auto hz = static_cast<float> (geometry.frequencyForX (startX + delta.x));
    set (dragging, EqFieldFreq, juce::jlimit (20.0f, 20000.0f, hz));

    if (typeHasGain (bands[static_cast<size_t> (dragging)].type))
    {
        const float startY = yForGain (dragStartGain);
        set (dragging, EqFieldGain, juce::jlimit (-24.0f, 24.0f, gainForY (startY + delta.y)));
    }
}

void EqCurveEditor::mouseUp (const juce::MouseEvent&)
{
    dragging = -1;
    repaint();
}

void EqCurveEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int b = nodeAt (e.position);
    if (b >= 0)
        resetBand (b);
}

void EqCurveEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    int b = nodeAt (e.position);
    if (b < 0)
        b = selected;
    if (b < 0)
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }
    const float delta = (std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX) * (wheel.isReversed ? -1.0f : 1.0f);
    const float q = bands[static_cast<size_t> (b)].q * std::pow (2.0f, delta * 1.6f);
    set (b, EqFieldQ, juce::jlimit (0.1f, 18.0f, q));
    hovered = b;
}

bool EqCurveEditor::keyPressed (const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::tabKey || code == ']' || code == '[')
    {
        const int dir = (code == '[' || key.getModifiers().isShiftDown()) ? -1 : 1;
        if (code == juce::KeyPress::tabKey && ((dir > 0 && selected == kBands - 1) || (dir < 0 && selected == 0)))
            return false; // let focus traversal continue
        selectBand (selected < 0 ? 0 : (selected + dir + kBands) % kBands);
        return true;
    }
    if (selected < 0)
        return false;

    const auto& p = bands[static_cast<size_t> (selected)];
    const bool fine = key.getModifiers().isShiftDown();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const float octaves = (fine ? 1.0f / 48.0f : 1.0f / 12.0f) * (code == juce::KeyPress::rightKey ? 1.0f : -1.0f);
        set (selected, EqFieldFreq, juce::jlimit (20.0f, 20000.0f, p.frequency * std::pow (2.0f, octaves)));
        return true;
    }
    if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) && typeHasGain (p.type))
    {
        const float step = (fine ? 0.1f : 0.5f) * (code == juce::KeyPress::upKey ? 1.0f : -1.0f);
        set (selected, EqFieldGain, juce::jlimit (-24.0f, 24.0f, p.gainDb + step));
        return true;
    }
    if (code == '+' || code == '=' || code == '-')
    {
        set (selected, EqFieldQ, juce::jlimit (0.1f, 18.0f, p.q * std::pow (2.0f, code == '-' ? -0.25f : 0.25f)));
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        set (selected, EqFieldOn, p.enabled ? 0.0f : 1.0f);
        return true;
    }
    if (code == juce::KeyPress::escapeKey)
    {
        selectBand (-1);
        return true;
    }
    return false;
}

void EqCurveEditor::showBandMenu (int band)
{
    const auto& p = bands[static_cast<size_t> (band)];
    const auto& table = layout();

    juce::PopupMenu types, slopes;
    const auto& typeInfo = table[static_cast<size_t> (eq (band, EqFieldType))];
    for (size_t t = 0; t < typeInfo.choices.size(); ++t)
        types.addItem (100 + static_cast<int> (t), juce::String (typeInfo.choices[t]), true, static_cast<int> (p.type) == static_cast<int> (t));
    const auto& slopeInfo = table[static_cast<size_t> (eq (band, EqFieldSlope))];
    const bool isCut = p.type == EqBandType::LowCut || p.type == EqBandType::HighCut;
    for (size_t s = 0; s < slopeInfo.choices.size(); ++s)
        slopes.addItem (200 + static_cast<int> (s), juce::String (slopeInfo.choices[s]), isCut,
                        p.slopeDbPerOct == 12 * (static_cast<int> (s) + 1));

    juce::PopupMenu menu;
    menu.addSectionHeader ("Band " + juce::String (band + 1));
    menu.addItem (1, "Enabled", true, p.enabled);
    menu.addSubMenu ("Type", types);
    menu.addSubMenu ("Slope", slopes, isCut);
    menu.addSeparator();
    menu.addItem (2, "Reset band");
    menu.addItem (3, "Reset all bands (flat)");

    juce::Component::SafePointer<EqCurveEditor> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe, band] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            auto& self = *safe;
                            if (result == 1)
                                self.set (band, EqFieldOn, self.bands[static_cast<size_t> (band)].enabled ? 0.0f : 1.0f);
                            else if (result == 2)
                                self.resetBand (band);
                            else if (result == 3)
                                for (int b = 0; b < kBands; ++b)
                                    self.resetBand (b);
                            else if (result >= 200)
                                self.set (band, EqFieldSlope, static_cast<float> (result - 200));
                            else if (result >= 100)
                                self.set (band, EqFieldType, static_cast<float> (result - 100));
                        });
}
} // namespace flub::app::ui
