#include "ParamGrid.h"

#include "Theme.h"
#include "Widgets.h"

namespace flub::app::ui
{
using flub::param::Unit;

namespace
{
constexpr int kRowHeight = 80;
constexpr int kGap = 6;

/** "eq.3.freq" -> 3, "dyneq.1.q" -> 1, anything else -> -1. */
int bandOfKey (const std::string& key, juce::String& prefix)
{
    const juce::String k (key);
    for (const char* p : { "eq.", "dyneq." })
    {
        if (k.startsWith (p))
        {
            const auto rest = k.fromFirstOccurrenceOf (p, false, false);
            const auto number = rest.upToFirstOccurrenceOf (".", false, false);
            if (number.isNotEmpty() && number.containsOnly ("0123456789"))
            {
                prefix = p;
                return number.getIntValue();
            }
        }
    }
    return -1;
}

/** "Band 4 Frequency" -> "Frequency", "Dyn 2 Threshold" -> "Threshold". */
juce::String stripBandPrefix (const juce::String& name)
{
    const auto words = juce::StringArray::fromTokens (name, " ", "");
    if (words.size() >= 3 && (words[0] == "Band" || words[0] == "Dyn") && words[1].containsOnly ("0123456789"))
        return name.fromFirstOccurrenceOf (words[1] + " ", false, false);
    return name;
}
} // namespace

std::vector<int> ParamGrid::idsForGroup (const juce::String& group)
{
    std::vector<int> ids;
    const auto& table = flub::param::layout();
    for (int id = 0; id < static_cast<int> (table.size()); ++id)
        if (group == juce::String (table[static_cast<size_t> (id)].group))
            ids.push_back (id);
    return ids;
}

ParamGrid::ParamGrid (ParameterBinder& b, const std::vector<int>& paramIds)
    : binder (b)
{
    for (const int id : paramIds)
    {
        const auto& info = ParamFormat::info (id);
        juce::String prefix;
        const int band = bandOfKey (info.key, prefix);
        const auto title = band < 0 ? juce::String() : (prefix == "eq." ? "Band " : "Dynamic band ") + juce::String (band + 1);

        auto it = std::find_if (sections.begin(), sections.end(), [&title] (const Section& s) { return s.title == title; });
        if (it == sections.end())
        {
            sections.push_back ({ title, {}, {} });
            it = std::prev (sections.end());
        }

        Cell cell;
        cell.paramId = id;
        const auto label = band < 0 ? juce::String (info.name) : stripBandPrefix (juce::String (info.name));

        if (info.unit == Unit::Toggle)
        {
            auto t = std::make_unique<juce::ToggleButton> (label);
            Style::set (*t, "switch");
            t->setTitle (juce::String (info.name));
            binder.bindToggle (*t, id);
            cell.width = juce::jlimit (96, 190, static_cast<int> (juce::GlyphArrangement::getStringWidth (Theme::font (13.0f), label)) + 52);
            cell.control = std::move (t);
        }
        else if (info.unit == Unit::Choice)
        {
            auto c = std::make_unique<juce::ComboBox>();
            c->setTitle (juce::String (info.name));
            binder.bindChoice (*c, id);
            cell.caption = label;
            cell.width = 126;
            cell.control = std::move (c);
        }
        else
        {
            auto k = std::make_unique<ParamKnob> (label, ParamKnob::Size::Small);
            binder.bindSlider (k->slider, id);
            cell.width = 80;
            cell.control = std::move (k);
        }
        addAndMakeVisible (*cell.control);
        it->cells.push_back (std::move (cell));
    }
}

ParamGrid::~ParamGrid()
{
    for (auto& s : sections)
    {
        for (auto& c : s.cells)
        {
            if (auto* knob = dynamic_cast<ParamKnob*> (c.control.get()))
                binder.unbind (knob->slider);
            else if (c.control != nullptr)
                binder.unbind (*c.control);
        }
    }
}

int ParamGrid::layoutSections (int width, bool apply)
{
    int y = 0;
    for (auto& s : sections)
    {
        if (s.title.isNotEmpty())
        {
            s.titleArea = { 0, y, width, 18 };
            y += 20;
        }
        int x = 0;
        for (auto& c : s.cells)
        {
            if (x > 0 && x + c.width > width)
            {
                x = 0;
                y += kRowHeight + kGap;
            }
            if (apply)
            {
                // Knobs fill the cell; combo boxes / switches sit on the knobs' centre line.
                if (dynamic_cast<ParamKnob*> (c.control.get()) != nullptr)
                    c.control->setBounds (x, y, c.width, kRowHeight);
                else
                    c.control->setBounds (x, y + 32, c.width - 6, 26);
            }
            x += c.width + kGap;
        }
        y += kRowHeight + 10;
    }
    return y;
}

int ParamGrid::getHeightForWidth (int width) const
{
    return const_cast<ParamGrid*> (this)->layoutSections (width, false);
}

void ParamGrid::resized()
{
    layoutSections (getWidth(), true);
}

void ParamGrid::paint (juce::Graphics& g)
{
    for (size_t i = 0; i < sections.size(); ++i)
    {
        const auto& s = sections[i];
        if (s.title.isNotEmpty())
        {
            auto r = s.titleArea.toFloat();
            if (i > 0)
            {
                g.setColour (Palette::border);
                g.fillRect (r.getX(), r.getY() - 5.0f, r.getWidth(), 1.0f);
            }
            Theme::drawCaption (g, s.title.toUpperCase(), r, Palette::muted);
        }
        for (const auto& c : s.cells)
        {
            if (c.caption.isNotEmpty() && c.control != nullptr)
            {
                g.setColour (Palette::muted);
                g.setFont (Theme::font (11.5f));
                g.drawText (c.caption, c.control->getBounds().translated (0, -18).withHeight (16).toFloat(), juce::Justification::centredLeft,
                            true);
            }
        }
    }
}
} // namespace flub::app::ui
