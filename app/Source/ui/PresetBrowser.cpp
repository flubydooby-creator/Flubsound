#include "PresetBrowser.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
const juce::String kDot { juce::CharPointer_UTF8 (" \xc2\xb7 ") };
constexpr int kScopeGroup = 0x9b01, kModeGroup = 0x9b02;
constexpr int kStarWidth = 26;
constexpr int kMaxTagChips = 10;

/** Lower-case words of `text`: runs of letters and digits. */
juce::StringArray words (const juce::String& text)
{
    juce::StringArray out;
    juce::String word;
    const auto lower = text.toLowerCase();
    for (auto p = lower.getCharPointer(); ! p.isEmpty();)
    {
        const auto c = p.getAndAdvance();
        if (juce::CharacterFunctions::isLetterOrDigit (c))
        {
            word += c;
        }
        else if (word.isNotEmpty())
        {
            out.add (word);
            word.clear();
        }
    }
    if (word.isNotEmpty())
        out.add (word);
    return out;
}

bool anyWordStartsWith (const juce::StringArray& fieldWords, const juce::String& term)
{
    // "footsteps" also finds "footstep"; a single-letter stem finds nothing extra.
    const auto stem = term.length() > 3 && term.endsWithChar ('s') ? term.dropLastCharacters (1) : juce::String();
    for (const auto& w : fieldWords)
        if (w.startsWith (term) || (stem.isNotEmpty() && w.startsWith (stem)))
            return true;
    return false;
}

void drawStar (juce::Graphics& g, juce::Rectangle<float> area, bool filled, juce::Colour colour)
{
    juce::Path star;
    const float r = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    star.addStar (area.getCentre(), 5, r * 0.45f, r);
    g.setColour (colour);
    if (filled)
        g.fillPath (star);
    else
        g.strokePath (star, juce::PathStrokeType (1.2f));
}
} // namespace

// =============================================================================
// The model
// =============================================================================
juce::StringArray PresetBrowser::searchTerms (const juce::String& query)
{
    static const juce::StringArray filler { "an", "and", "for", "of", "the", "to", "with", "my", "me", "in", "on", "it", "is", "or" };
    juce::StringArray terms;
    for (const auto& w : words (query))
    {
        const bool hasDigit = w.containsAnyOf ("0123456789");
        if ((w.length() >= 2 || hasDigit) && ! filler.contains (w))
            terms.addIfNotAlreadyThere (w);
    }
    return terms;
}

PresetBrowser::Match PresetBrowser::match (const PresetInfo& preset, const juce::StringArray& terms)
{
    struct Field
    {
        juce::StringArray words;
        int weight;
    };
    const Field fields[] = { { words (preset.name), 6 },   { words (preset.tags.joinIntoString (" ")), 4 },
                             { words (preset.category), 3 }, { words (preset.mode), 2 },
                             { words (preset.author), 1 },   { words (preset.description), 1 } };
    Match m;
    for (const auto& term : terms)
    {
        int best = 0;
        for (const auto& f : fields)
            if (f.weight > best && anyWordStartsWith (f.words, term))
                best = f.weight;
        if (best > 0)
        {
            ++m.terms;
            m.score += best;
        }
    }
    return m;
}

std::vector<int> PresetBrowser::filterPresets (const std::vector<PresetInfo>& presets, const Filter& filter, const Context& context)
{
    const auto terms = searchTerms (filter.query);
    struct Row
    {
        int index;
        Match m;
    };
    std::vector<Row> rows;
    for (int i = 0; i < static_cast<int> (presets.size()); ++i)
    {
        const auto& p = presets[static_cast<size_t> (i)];
        if ((filter.scope == Scope::Favourites && ! context.favourites.contains (p.id))
            || (filter.scope == Scope::Recent && ! context.recent.contains (p.id))
            || (filter.scope == Scope::Recommended && ! context.recommended.contains (p.id)))
            continue;
        if (filter.mode.isNotEmpty() && ! p.mode.equalsIgnoreCase (filter.mode))
            continue;
        if (filter.category.isNotEmpty() && ! p.category.equalsIgnoreCase (filter.category))
            continue;
        bool tagsOk = true;
        for (const auto& t : filter.tags)
            tagsOk = tagsOk && p.tags.contains (t, true);
        if (! tagsOk)
            continue;
        Match m;
        if (! terms.isEmpty())
        {
            m = match (p, terms);
            if (m.terms == 0)
                continue;
        }
        rows.push_back ({ i, m });
    }

    if (! terms.isEmpty())
        std::stable_sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b)
                          { return a.m.terms != b.m.terms ? a.m.terms > b.m.terms : a.m.score > b.m.score; });
    else if (filter.scope == Scope::Recent)
        std::stable_sort (rows.begin(), rows.end(), [&] (const Row& a, const Row& b)
                          { return context.recent.indexOf (presets[static_cast<size_t> (a.index)].id)
                                   < context.recent.indexOf (presets[static_cast<size_t> (b.index)].id); });

    std::vector<int> out;
    out.reserve (rows.size());
    for (const auto& r : rows)
        out.push_back (r.index);
    return out;
}

juce::StringArray PresetBrowser::recommendedFor (const std::vector<PresetInfo>& presets, const juce::String& suggestedPreset,
                                                 flub::device::Connection connection)
{
    juce::StringArray ids;
    if (suggestedPreset.isNotEmpty())
        for (const auto& p : presets)
            if (p.name.equalsIgnoreCase (suggestedPreset))
            {
                ids.add (p.id);
                break;
            }
    if (connection == flub::device::Connection::Bluetooth || connection == flub::device::Connection::BluetoothHandsFree)
        for (const auto& p : presets)
            if (p.tags.contains ("bluetooth", true))
                ids.addIfNotAlreadyThere (p.id);
    return ids;
}

juce::StringArray PresetBrowser::commonTags (const std::vector<PresetInfo>& presets, int maxTags)
{
    std::map<juce::String, int> counts;
    for (const auto& p : presets)
    {
        juce::StringArray seen; // a preset counts once per tag
        for (const auto& t : p.tags)
            if (const auto tag = t.trim().toLowerCase(); tag.isNotEmpty() && seen.addIfNotAlreadyThere (tag))
                ++counts[tag];
    }
    std::vector<std::pair<juce::String, int>> sorted (counts.begin(), counts.end()); // alphabetical
    std::stable_sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.second > b.second; });
    juce::StringArray out;
    for (const auto& entry : sorted)
        if (out.size() < maxTags)
            out.add (entry.first);
    return out;
}

juce::String PresetBrowser::describeLoudness (float originalGainLu, float presetGainLu)
{
    const float d = presetGainLu - originalGainLu;
    if (! std::isfinite (d))
        return "Loudness could not be measured";
    if (std::abs (d) < 0.5f)
        return "About as loud as your current sound";
    return juce::String (std::abs (d), 1) + (d > 0.0f ? " LU louder than your current sound" : " LU quieter than your current sound");
}

// =============================================================================
// List
// =============================================================================
class PresetBrowser::ListModel final : public juce::ListBoxModel
{
public:
    explicit ListModel (PresetBrowser& b) : owner (b) {}

    int getNumRows() override { return static_cast<int> (owner.shown.size()) + 1; }

    const PresetInfo* presetAt (int row) const
    {
        if (row <= 0 || row > static_cast<int> (owner.shown.size()))
            return nullptr;
        return &owner.presets[static_cast<size_t> (owner.shown[static_cast<size_t> (row - 1)])];
    }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        auto r = juce::Rectangle<int> (width, height).reduced (4, 2);
        if (selected)
        {
            g.setColour (Palette::tabOn);
            g.fillRoundedRectangle (r.toFloat(), Theme::kControlRadius);
            g.setColour (Theme::accent (owner));
            g.fillRoundedRectangle (r.toFloat().removeFromLeft (3.0f), 1.5f);
        }
        auto text = r.reduced (6, 3);
        auto star = text.removeFromLeft (kStarWidth - 6);
        text.removeFromLeft (6);

        const auto* p = presetAt (row);
        juce::String title, sub;
        if (p == nullptr)
        {
            drawIcon (g, Icons::ear(), star.toFloat().withSizeKeepingCentre (16.0f, 16.0f), Palette::muted);
            title = "Current sound";
            const auto name = owner.controller.getPresetManager().findById (owner.audition.getPresetIdAtBegin());
            sub = (name != nullptr ? name->name : juce::String ("Default settings")) + kDot + "what you had when the browser opened";
        }
        else
        {
            const bool fav = owner.context.favourites.contains (p->id);
            drawStar (g, star.toFloat().withSizeKeepingCentre (14.0f, 14.0f), fav, fav ? Palette::amber : Palette::faint);
            title = p->name;
            juce::StringArray parts { p->category };
            parts.addIfNotAlreadyThere (p->mode); // a Music preset's category is Music
            for (int i = 0; i < juce::jmin (3, p->tags.size()); ++i)
                parts.add (p->tags[i]);
            sub = parts.joinIntoString (kDot);

            // Pills on the right: what is loaded now, what suits the output, user presets.
            auto pills = text;
            const auto pill = [&g, &pills] (const juce::String& label, juce::Colour colour)
            {
                g.setFont (Theme::caption (10.0f));
                const int w = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label)) + 14;
                Theme::drawPill (g, pills.removeFromRight (w).removeFromTop (18).toFloat(), label, colour);
                pills.removeFromRight (4);
            };
            if (p->id == owner.audition.getPresetIdAtBegin())
                pill ("LOADED", Palette::muted);
            if (owner.context.recommended.contains (p->id))
                pill ("FOR YOUR OUTPUT", Theme::accent (owner));
            if (! p->isFactory)
                pill ("USER", Palette::muted);
            text.setRight (juce::jmax (text.getX() + 60, pills.getRight() - 4));
        }

        g.setColour (Palette::text);
        g.setFont (Theme::font (13.5f, true));
        g.drawText (title, text.removeFromTop (text.getHeight() / 2 + 1), juce::Justification::bottomLeft, true);
        g.setColour (Palette::muted);
        g.setFont (Theme::font (11.5f));
        g.drawText (sub, text, juce::Justification::topLeft, true);
    }

    void selectedRowsChanged (int lastRowSelected) override { owner.rowSelected (lastRowSelected); }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (const auto* p = presetAt (row); p != nullptr && e.x < kStarWidth + 4)
            owner.toggleFavourite (p->id);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row >= 0)
            owner.loadSelected();
    }

    void returnKeyPressed (int) override { owner.loadSelected(); }

    juce::String getTooltipForRow (int row) override
    {
        const auto* p = presetAt (row);
        return p != nullptr ? p->description : juce::String ("The sound you had when the browser opened: select it to compare");
    }

    juce::String getNameForRow (int row) override
    {
        const auto* p = presetAt (row);
        return p != nullptr ? p->name + ", " + p->category + ", " + p->mode : juce::String ("Current sound");
    }

private:
    PresetBrowser& owner;
};

// =============================================================================
// Details
// =============================================================================
class PresetBrowser::DetailPane final : public juce::Component
{
public:
    explicit DetailPane (PresetBrowser& b) : owner (b) { setTitle ("Preset details"); }

    void paint (juce::Graphics& g) override
    {
        Theme::drawPanel (g, getLocalBounds().toFloat());
        auto r = getLocalBounds().reduced (14, 12);
        for (const auto& line : owner.detailLines())
        {
            if (r.getHeight() < 14)
                break;
            using Kind = DetailLine::Kind;
            switch (line.kind)
            {
                case Kind::Title:
                    g.setColour (Palette::text);
                    g.setFont (Theme::font (18.0f, true));
                    g.drawText (line.text, r.removeFromTop (26), juce::Justification::centredLeft, true);
                    break;
                case Kind::Badge:
                {
                    g.setFont (Theme::caption (10.5f));
                    const int w = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), line.text)) + 16;
                    Theme::drawPill (g, r.removeFromTop (20).removeFromLeft (juce::jmin (w, r.getWidth())).toFloat(), line.text,
                                     Theme::accent (*this));
                    r.removeFromTop (4);
                    break;
                }
                case Kind::Meta:
                case Kind::Muted:
                case Kind::Body:
                case Kind::Warning:
                case Kind::Loudness:
                {
                    const bool body = line.kind == Kind::Body;
                    const auto colour = line.kind == Kind::Warning ? Palette::amber
                                        : body || line.kind == Kind::Loudness ? Palette::text.withAlpha (0.9f)
                                                                              : Palette::muted;
                    juce::AttributedString s;
                    s.setWordWrap (juce::AttributedString::byWord);
                    s.append (line.text, Theme::font (body ? 13.5f : 12.5f, line.kind == Kind::Loudness), colour);
                    juce::TextLayout layout;
                    layout.createLayout (s, static_cast<float> (r.getWidth()));
                    const int h = juce::jmin (r.getHeight(), static_cast<int> (std::ceil (layout.getHeight())));
                    layout.draw (g, r.removeFromTop (h).toFloat());
                    r.removeFromTop (line.kind == Kind::Meta ? 6 : 8);
                    break;
                }
            }
        }
    }

private:
    PresetBrowser& owner;
};

// =============================================================================
// The component
// =============================================================================
PresetBrowser::PresetBrowser (EngineController& c, std::shared_ptr<PresetLoudnessEstimator> e)
    : controller (c), estimator (std::move (e)), audition (c)
{
    jassert (estimator != nullptr);
    setTitle ("Preset browser");
    setWantsKeyboardFocus (true);
    auto& settings = controller.getSettings();
    previewEnabled = settings.getPresetPreview();
    matchEnabled = settings.getPresetPreviewMatched();

    search.setTitle ("Search presets");
    search.setTextToShowWhenEmpty ("Search names, tags and descriptions (e.g. late night quiet)", Palette::faint);
    search.setFont (Theme::font (14.0f));
    search.setIndents (10, 7);
    search.onTextChange = [this] { setQuery (search.getText()); };
    search.onReturnKey = [this]
    {
        if (isCurrentSoundSelected() && ! shown.empty())
            selectPreset (presets[static_cast<size_t> (shown.front())].id);
        loadSelected();
    };
    search.onEscapeKey = [this] { cancel(); };
    search.onNavigationKey = [this] (const juce::KeyPress& key)
    {
        if (! (key.isKeyCode (juce::KeyPress::downKey) || key.isKeyCode (juce::KeyPress::upKey)))
            return false;
        const int rows = static_cast<int> (shown.size()) + 1;
        const int row = std::clamp (list.getSelectedRow() + (key.isKeyCode (juce::KeyPress::downKey) ? 1 : -1), 0, rows - 1);
        list.selectRow (row);
        return true;
    };
    addAndMakeVisible (search);

    Style::set (previewButton, "chip");
    Style::set (matchButton, "chip");
    Style::describe (previewButton, "Preview", "Play the selected preset while browsing (Cancel puts your sound back)");
    Style::describe (matchButton, "Match loudness",
                     "Turn the louder of the preview and your current sound down to the other, so the comparison is not won by volume");
    for (auto* b : { &previewButton, &matchButton })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (*b);
    }
    previewButton.onClick = [this] { setPreviewEnabled (previewButton.getToggleState()); };
    matchButton.onClick = [this] { setMatchEnabled (matchButton.getToggleState()); };

    const std::pair<juce::TextButton*, Scope> scopes[] = {
        { &scopeAll, Scope::All }, { &scopeFavourites, Scope::Favourites }, { &scopeRecent, Scope::Recent }, { &scopeRecommended, Scope::Recommended }
    };
    for (const auto& [b, scope] : scopes)
    {
        Style::set (*b, "tab");
        b->setRadioGroupId (kScopeGroup, juce::dontSendNotification);
        b->setClickingTogglesState (true);
        b->onClick = [this, s = scope] { setScope (s); };
        addAndMakeVisible (*b);
    }
    scopeAll.setToggleState (true, juce::dontSendNotification);
    scopeFavourites.setTooltip ("Presets you marked with the star");
    scopeRecent.setTooltip ("Presets you picked recently, newest first");

    const std::pair<juce::TextButton*, const char*> modes[] = { { &modeAny, "" }, { &modeMusic, "Music" }, { &modeGaming, "Gaming" } };
    for (const auto& [b, mode] : modes)
    {
        Style::set (*b, "chip");
        b->setRadioGroupId (kModeGroup, juce::dontSendNotification);
        b->setClickingTogglesState (true);
        b->onClick = [this, m = juce::String (mode)]
        {
            filter.mode = m;
            applyFilter();
        };
        addAndMakeVisible (*b);
    }
    modeAny.setToggleState (true, juce::dontSendNotification);
    modeAny.setTooltip ("Presets of either mode");

    categoryBox.setTitle ("Category");
    categoryBox.setTooltip ("Show one category");
    categoryBox.onChange = [this]
    {
        filter.category = categoryBox.getSelectedId() > 1 ? categoryBox.getText() : juce::String();
        applyFilter();
    };
    addAndMakeVisible (categoryBox);

    model = std::make_unique<ListModel> (*this);
    list.setModel (model.get());
    list.setTitle ("Presets");
    list.setRowHeight (42);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setColour (juce::ListBox::outlineColourId, Palette::border);
    list.setOutlineThickness (1);
    addAndMakeVisible (list);

    details = std::make_unique<DetailPane> (*this);
    addAndMakeVisible (*details);

    Style::describe (favouriteButton, "Favourite", "Mark the selected preset with a star (Favourites)");
    favouriteButton.onClick = [this]
    {
        if (const auto* p = getSelectedPreset())
            toggleFavourite (p->id);
    };
    Style::describe (cancelButton, "Cancel", "Close and put back the sound you had (Escape)");
    cancelButton.onClick = [this] { cancel(); };
    Style::describe (loadButton, "Load", "Load the selected preset into the strip (Enter or a double click)");
    loadButton.onClick = [this] { loadSelected(); };
    for (auto* b : { &favouriteButton, &cancelButton, &loadButton })
        addAndMakeVisible (*b);

    estimator->onEstimate = [this]
    {
        updateMatch();
        details->repaint();
        repaint (statusArea);
    };

    reloadPresets();
    audition.begin (controller.getSelectedStrip());
    if (const auto level = liveProgrammeLevel())
    {
        levelLufs = *level;
        levelMeasured = true;
    }
    applyFilter();
    list.selectRow (0, false, true);
    rowSelected (0);
    requestEstimates();
    controller.addListener (this);
    startTimerHz (2);
    setSize (780, 520);
}

PresetBrowser::~PresetBrowser()
{
    stopTimer();
    controller.removeListener (this);
    if (estimator != nullptr)
        estimator->onEstimate = nullptr;
    list.setModel (nullptr);
    audition.cancel();
}

// =============================================================================
// State
// =============================================================================
void PresetBrowser::reloadPresets()
{
    presets = controller.getPresetManager().getPresets();
    valuesCache.clear();
    auto& settings = controller.getSettings();
    context.favourites = settings.getFavouritePresets();
    context.recent = settings.getRecentPresets();
    const auto& advice = controller.getDeviceAdvice();
    context.recommended = recommendedFor (presets, juce::String::fromUTF8 (advice.suggestedPreset.c_str()), controller.getDeviceConnection());

    deviceLabel = controller.getDeviceProfileName();
    if (deviceLabel.isEmpty())
        deviceLabel = controller.getOutputDeviceName();
    const auto shortLabel = deviceLabel.length() > 22 ? deviceLabel.substring (0, 21).trimEnd() + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"))
                                                      : deviceLabel;
    scopeRecommended.setButtonText (deviceLabel.isNotEmpty() ? "For " + shortLabel : juce::String ("For your output"));
    scopeRecommended.setTooltip ("Presets suggested for " + (deviceLabel.isNotEmpty() ? deviceLabel : juce::String ("the output device")));
    scopeRecommended.setVisible (! context.recommended.isEmpty());
    if (context.recommended.isEmpty() && filter.scope == Scope::Recommended)
    {
        filter.scope = Scope::All;
        scopeAll.setToggleState (true, juce::dontSendNotification);
    }

    const auto category = filter.category;
    categoryBox.clear (juce::dontSendNotification);
    categoryBox.addItem ("All categories", 1);
    const auto categories = controller.getPresetManager().getCategories();
    for (int i = 0; i < categories.size(); ++i)
        categoryBox.addItem (categories[i], i + 2);
    const int index = categories.indexOf (category);
    categoryBox.setSelectedId (index >= 0 ? index + 2 : 1, juce::dontSendNotification);
    if (index < 0)
        filter.category = {};

    rebuildTagChips();
    resized();
}

void PresetBrowser::rebuildTagChips()
{
    for (auto& chip : tagChips)
        removeChildComponent (chip.get());
    tagChips.clear();
    for (const auto& tag : commonTags (presets, kMaxTagChips))
    {
        auto chip = std::make_unique<juce::TextButton> (tag);
        Style::set (*chip, "chip");
        chip->setClickingTogglesState (true);
        chip->setToggleState (filter.tags.contains (tag, true), juce::dontSendNotification);
        chip->setTooltip ("Only presets tagged \"" + tag + "\"");
        chip->onClick = [this, raw = chip.get(), tag]
        {
            if (raw->getToggleState())
                filter.tags.addIfNotAlreadyThere (tag);
            else
                filter.tags.removeString (tag, true);
            applyFilter();
        };
        addAndMakeVisible (*chip);
        tagChips.push_back (std::move (chip));
    }
}

void PresetBrowser::setQuery (const juce::String& query)
{
    if (search.getText() != query)
        search.setText (query, false);
    filter.query = query;
    applyFilter();
}

void PresetBrowser::setScope (Scope scope)
{
    filter.scope = scope;
    if (scope == Scope::Favourites || scope == Scope::Recent)
    {
        auto& settings = controller.getSettings();
        context.favourites = settings.getFavouritePresets();
        context.recent = settings.getRecentPresets();
    }
    auto* button = scope == Scope::Favourites ? &scopeFavourites : scope == Scope::Recent ? &scopeRecent : scope == Scope::Recommended ? &scopeRecommended : &scopeAll;
    button->setToggleState (true, juce::dontSendNotification);
    applyFilter();
}

void PresetBrowser::applyFilter()
{
    shown = filterPresets (presets, filter, context);
    list.updateContent();
    // Keep the selection where it is still listed (it keeps playing either way).
    int row = selectedId.isEmpty() ? 0 : -1;
    for (size_t i = 0; row < 0 && i < shown.size(); ++i)
        if (presets[static_cast<size_t> (shown[i])].id == selectedId)
            row = static_cast<int> (i) + 1;
    if (row >= 0)
        list.selectRow (row, true, true);
    else
        list.deselectAllRows();
    list.repaint();
    refreshButtons();
    repaint (statusArea);
}

std::vector<const PresetInfo*> PresetBrowser::getShownPresets() const
{
    std::vector<const PresetInfo*> out;
    for (const int i : shown)
        out.push_back (&presets[static_cast<size_t> (i)]);
    return out;
}

const PresetInfo* PresetBrowser::findPreset (const juce::String& id) const
{
    for (const auto& p : presets)
        if (p.id == id)
            return &p;
    return nullptr;
}

const PresetInfo* PresetBrowser::getSelectedPreset() const { return selectedId.isEmpty() ? nullptr : findPreset (selectedId); }

bool PresetBrowser::selectPreset (const juce::String& presetId)
{
    if (presetId.isEmpty())
    {
        list.selectRow (0);
        return true;
    }
    for (size_t i = 0; i < shown.size(); ++i)
        if (presets[static_cast<size_t> (shown[i])].id == presetId)
        {
            list.selectRow (static_cast<int> (i) + 1);
            return true;
        }
    return false;
}

void PresetBrowser::rowSelected (int row)
{
    if (row < 0 || closing)
        return; // a deselection (the filter hid the selection) keeps it
    const juce::String id = row == 0 ? juce::String() : presets[static_cast<size_t> (shown[static_cast<size_t> (row - 1)])].id;
    const bool changed = id != selectedId;
    selectedId = id;

    selectedWarnings.clear();
    selectedError.clear();
    if (const auto* p = getSelectedPreset())
    {
        if (audition.valuesFor (*p, &selectedWarnings).empty())
            selectedError = "This preset could not be read.";
        estimator->request (valuesFor (*p), levelLufs, true);
    }
    if (changed || audition.getPreviewId() != selectedId)
        playSelection();
    updateMatch();
    refreshButtons();
    details->repaint();
    repaint (statusArea);
}

void PresetBrowser::playSelection()
{
    if (! previewEnabled || closing)
        return;
    if (! audition.isActive())
        audition.begin (controller.getSelectedStrip());
    if (const auto* p = getSelectedPreset())
    {
        juce::String error;
        if (audition.preview (*p, error))
            lastPreviewedId = p->id;
        else
            selectedError = error;
    }
    else
    {
        audition.previewOriginal();
    }
}

void PresetBrowser::setPreviewEnabled (bool enabled)
{
    previewEnabled = enabled;
    controller.getSettings().setPresetPreview (enabled);
    if (enabled)
        playSelection();
    else
        audition.cancel(); // back to the sound the browser opened with
    updateMatch();
    refreshButtons();
    repaint (statusArea);
}

void PresetBrowser::setMatchEnabled (bool enabled)
{
    matchEnabled = enabled;
    controller.getSettings().setPresetPreviewMatched (enabled);
    updateMatch();
    refreshButtons();
    repaint (statusArea);
}

void PresetBrowser::toggleFavourite (const juce::String& presetId)
{
    auto& settings = controller.getSettings();
    settings.setFavouritePreset (presetId, ! settings.isFavouritePreset (presetId));
    context.favourites = settings.getFavouritePresets();
    if (filter.scope == Scope::Favourites)
        applyFilter();
    list.repaint();
    refreshButtons();
}

void PresetBrowser::refreshButtons()
{
    previewButton.setToggleState (previewEnabled, juce::dontSendNotification);
    matchButton.setToggleState (matchEnabled, juce::dontSendNotification);
    matchButton.setEnabled (previewEnabled);
    const auto* p = getSelectedPreset();
    favouriteButton.setEnabled (p != nullptr);
    favouriteButton.setButtonText (p != nullptr && context.favourites.contains (p->id) ? "Unfavourite" : "Favourite");
    loadButton.setButtonText (p != nullptr ? "Load" : "Keep current");
    loadButton.setEnabled (p == nullptr || selectedError.isEmpty());
}

// =============================================================================
// Loudness matching
// =============================================================================
const std::vector<float>& PresetBrowser::valuesFor (const PresetInfo& preset) const
{
    auto it = valuesCache.find (preset.id);
    if (it == valuesCache.end())
        it = valuesCache.emplace (preset.id, audition.valuesFor (preset)).first;
    return it->second;
}

std::optional<float> PresetBrowser::estimateFor (const PresetInfo& preset) const
{
    const auto& values = valuesFor (preset);
    if (values.empty())
        return {};
    return estimator->find (values, levelLufs);
}

std::optional<float> PresetBrowser::originalEstimate() const
{
    if (audition.getOriginalValues().empty())
        return {};
    return estimator->find (audition.getOriginalValues(), levelLufs);
}

void PresetBrowser::requestEstimates()
{
    if (! audition.getOriginalValues().empty())
        estimator->request (audition.getOriginalValues(), levelLufs, true);
    if (const auto* p = getSelectedPreset())
        estimator->request (valuesFor (*p), levelLufs, true);
    for (const auto& p : presets)
        if (const auto& values = valuesFor (p); ! values.empty())
            estimator->request (values, levelLufs);
}

std::optional<float> PresetBrowser::liveProgrammeLevel() const
{
    // The strip's input as it arrives: the chain's input loudness is read
    // after the input gain and AutoLevel, so both are taken off again.
    const int strip = audition.isActive() ? audition.getStrip() : controller.getSelectedStrip();
    if (strip < 0 || strip >= controller.getNumStrips())
        return {};
    auto& meters = controller.getChain (strip).meters();
    const float in = meters.inShortTermLufs.load (std::memory_order_relaxed);
    if (! std::isfinite (in) || in <= -60.0f)
        return {};
    const auto& playing = audition.getPlayingValues();
    const float inputGain = playing.empty() ? controller.getParams (strip).get (InputGainDb) : playing[static_cast<size_t> (InputGainDb)];
    return in - inputGain - meters.autoLevelGainDb.load (std::memory_order_relaxed);
}

void PresetBrowser::timerCallback()
{
    // Nothing played when the browser opened: estimate again at the level of
    // the first programme that arrives (once, so the trims stay put).
    if (levelMeasured || closing)
        return;
    if (const auto level = liveProgrammeLevel())
    {
        levelLufs = *level;
        levelMeasured = true;
        requestEstimates();
        updateMatch();
        details->repaint();
        repaint (statusArea);
    }
}

void PresetBrowser::updateMatch()
{
    if (! audition.isActive() || closing)
        return;
    float trim = 0.0f;
    if (previewEnabled && matchEnabled)
    {
        const auto original = originalEstimate();
        const auto* other = findPreset (audition.getPreviewId().isNotEmpty() ? audition.getPreviewId() : lastPreviewedId);
        const auto preset = other != nullptr ? estimateFor (*other) : std::optional<float>();
        if (original.has_value() && preset.has_value())
        {
            const auto trims = PresetAudition::matchTrims (*original, *preset);
            trim = audition.getPreviewId().isNotEmpty() ? trims.previewDb : trims.originalDb;
        }
    }
    if (trim != audition.getTrimDb())
        audition.setTrimDb (trim);
}

// =============================================================================
// Load / cancel
// =============================================================================
bool PresetBrowser::loadSelected()
{
    const auto* p = getSelectedPreset();
    if (p == nullptr)
    {
        cancel(); // "Keep current"
        return true;
    }
    const auto preset = *p; // the load rebuilds the list
    closing = true;
    juce::String error;
    if (! audition.commit (preset, error))
    {
        closing = false;
        selectedError = error;
        details->repaint();
        return false;
    }
    controller.getSettings().addRecentPreset (preset.id);
    close();
    return true;
}

void PresetBrowser::cancel()
{
    closing = true;
    audition.cancel();
    close();
}

void PresetBrowser::close()
{
    if (onClose != nullptr)
        onClose();
}

void PresetBrowser::engineControllerChanged (EngineController::Change change)
{
    if (closing)
        return;
    switch (change)
    {
        case EngineController::Change::Preset:
            // Someone else loaded a preset into the strip (an automatic
            // profile, a hotkey): it stays; the preview ends without restoring.
            if (audition.isActive() && controller.getCurrentPresetId (audition.getStrip()) != audition.getPresetIdAtBegin())
                audition.abandon();
            reloadPresets();
            applyFilter();
            break;
        case EngineController::Change::SelectedStrip:
        case EngineController::Change::Engine:
            audition.cancel(); // the next selection previews on the strip now selected
            break;
        case EngineController::Change::Device:
            reloadPresets();
            applyFilter();
            break;
        case EngineController::Change::MasterEnable:
        case EngineController::Change::Parameters:
        case EngineController::Change::Routing:
        case EngineController::Change::Settings:
            break;
    }
}

// =============================================================================
// Text
// =============================================================================
std::vector<PresetBrowser::DetailLine> PresetBrowser::detailLines() const
{
    using Kind = DetailLine::Kind;
    std::vector<DetailLine> lines;
    const auto original = originalEstimate();
    const auto* p = getSelectedPreset();

    if (p == nullptr)
    {
        lines.push_back ({ Kind::Title, "Current sound" });
        const auto* loaded = controller.getPresetManager().findById (audition.getPresetIdAtBegin());
        lines.push_back ({ Kind::Meta, (loaded != nullptr ? loaded->name : juce::String ("Default settings"))
                                           + kDot + controller.getStripName (controller.getSelectedStrip()) + " strip" });
        lines.push_back ({ Kind::Body, "What you had when the browser opened. Move between this row and a preset to compare them; "
                                       "Cancel or Escape keeps it." });
        if (const auto* other = findPreset (lastPreviewedId); other != nullptr && previewEnabled && matchEnabled)
            if (const auto e = estimateFor (*other); original.has_value() && e.has_value() && std::isfinite (*e) && std::isfinite (*original))
                lines.push_back ({ Kind::Loudness, describeLoudness (*e, *original).replace ("your current sound", other->name)
                                                       + (audition.getPreviewId().isEmpty() && audition.getTrimDb() < -0.05f
                                                              ? " (turned down " + juce::String (-audition.getTrimDb(), 1) + " dB to match)"
                                                              : juce::String()) });
        return lines;
    }

    lines.push_back ({ Kind::Title, p->name });
    juce::StringArray meta { p->isFactory ? "Factory" : "User", p->mode };
    meta.addIfNotAlreadyThere (p->category);
    if (p->author.isNotEmpty())
        meta.add ("by " + p->author);
    lines.push_back ({ Kind::Meta, meta.joinIntoString (kDot) });
    if (context.recommended.contains (p->id))
        lines.push_back ({ Kind::Badge, "SUGGESTED FOR " + (deviceLabel.isNotEmpty() ? deviceLabel : juce::String ("YOUR OUTPUT")).toUpperCase() });
    if (p->description.isNotEmpty())
        lines.push_back ({ Kind::Body, p->description });
    if (! p->tags.isEmpty())
        lines.push_back ({ Kind::Muted, "Tags: " + p->tags.joinIntoString (", ") });
    if (p->suggestedLatencyProfile.has_value())
    {
        const auto made = *p->suggestedLatencyProfile;
        const auto running = static_cast<LatencyProfileValue> (juce::roundToInt (audition.getOriginalValues().empty()
                                                                                     ? 1.0f
                                                                                     : audition.getOriginalValues()[static_cast<size_t> (LatencyProfile)]));
        const auto name = [] (LatencyProfileValue v)
        { return v == LatencyProfileValue::Quality ? "Quality" : v == LatencyProfileValue::LowLatency ? "Low Latency" : "Balanced"; };
        lines.push_back ({ Kind::Muted, juce::String ("Made for the ") + name (made) + " latency profile"
                                            + (made != running ? juce::String (" (the engine runs ") + name (running) + "; loading never changes it)"
                                                               : juce::String()) });
    }
    if (selectedError.isNotEmpty())
        lines.push_back ({ Kind::Warning, selectedError });
    for (const auto& w : selectedWarnings)
        lines.push_back ({ Kind::Warning, "Warning: " + w });

    const auto e = estimateFor (*p);
    juce::String loudness;
    if (! e.has_value() || ! original.has_value())
        loudness = "Measuring loudness against your current sound...";
    else
        loudness = describeLoudness (*original, *e);
    if (audition.getPreviewId() == p->id && audition.getTrimDb() < -0.05f)
        loudness << " (preview turned down " << juce::String (-audition.getTrimDb(), 1) << " dB to match)";
    else if (audition.getPreviewId() == p->id && previewEnabled && ! matchEnabled)
        loudness << " (not matched)";
    lines.push_back ({ Kind::Loudness, loudness });
    return lines;
}

juce::StringArray PresetBrowser::getDetailLines() const
{
    juce::StringArray out;
    for (const auto& l : detailLines())
        out.add (l.text);
    return out;
}

juce::String PresetBrowser::getStatusText() const
{
    juce::String s = juce::String (static_cast<int> (shown.size())) + " of " + juce::String (static_cast<int> (presets.size())) + " presets";
    if (! previewEnabled)
        s << kDot << "preview off";
    else if (audition.getTrimDb() < -0.05f)
        s << kDot << "matched " << Theme::formatSignedDb (audition.getTrimDb()) << " dB";
    else if (! matchEnabled)
        s << kDot << "not loudness matched";
    if (estimator->getQueuedCount() > 0)
        s << kDot << "measuring loudness";
    return s;
}

// =============================================================================
// Layout and painting
// =============================================================================
bool PresetBrowser::keyPressed (const juce::KeyPress& key)
{
    if (key.isKeyCode (juce::KeyPress::escapeKey))
    {
        cancel();
        return true;
    }
    return false;
}

void PresetBrowser::paint (juce::Graphics& g)
{
    Theme::drawPanel (g, getLocalBounds().toFloat());
    Theme::drawCaption (g, "PRESETS", getLocalBounds().reduced (16, 10).removeFromTop (18).toFloat());
    g.setFont (Theme::font (12.5f));
    g.setColour (Palette::muted);
    g.drawFittedText (getStatusText(), statusArea, juce::Justification::centredLeft, 1, 0.9f);
}

void PresetBrowser::resized()
{
    auto r = getLocalBounds().reduced (16, 10);
    r.removeFromTop (24); // caption

    auto top = r.removeFromTop (32);
    matchButton.setBounds (top.removeFromRight (132).reduced (0, 3));
    top.removeFromRight (6);
    previewButton.setBounds (top.removeFromRight (82).reduced (0, 3));
    top.removeFromRight (10);
    search.setBounds (top);
    r.removeFromTop (8);

    auto filters = r.removeFromTop (28);
    const auto place = [&filters] (juce::Component& c, int w, int gap)
    {
        c.setBounds (filters.removeFromLeft (juce::jmin (w, filters.getWidth())));
        filters.removeFromLeft (gap);
    };
    place (scopeAll, 48, 2);
    place (scopeFavourites, 88, 2);
    place (scopeRecent, 66, 2);
    if (scopeRecommended.isVisible())
    {
        const int w = juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (13.0f, true), scopeRecommended.getButtonText())) + 22;
        place (scopeRecommended, juce::jlimit (90, 190, w), 2);
    }
    filters.removeFromLeft (10);
    categoryBox.setBounds (filters.removeFromRight (juce::jlimit (0, 170, filters.getWidth() - 3 * 62)));
    filters.removeFromRight (8);
    for (auto* b : { &modeAny, &modeMusic, &modeGaming })
    {
        b->setBounds (filters.removeFromLeft (juce::jmin (62, filters.getWidth())).reduced (0, 2));
        filters.removeFromLeft (4);
    }
    r.removeFromTop (6);

    auto tags = r.removeFromTop (26);
    for (auto& chip : tagChips)
    {
        const int w = juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f, true), chip->getButtonText())) + 22;
        const bool fits = w <= tags.getWidth();
        chip->setVisible (fits);
        if (fits)
        {
            chip->setBounds (tags.removeFromLeft (w).reduced (0, 2));
            tags.removeFromLeft (4);
        }
        else
        {
            tags.setWidth (0);
        }
    }
    r.removeFromTop (8);

    auto bottom = r.removeFromBottom (32);
    loadButton.setBounds (bottom.removeFromRight (104));
    bottom.removeFromRight (6);
    cancelButton.setBounds (bottom.removeFromRight (84));
    bottom.removeFromRight (6);
    favouriteButton.setBounds (bottom.removeFromRight (104));
    bottom.removeFromRight (10);
    statusArea = bottom;
    r.removeFromBottom (10);

    list.setBounds (r.removeFromLeft (r.getWidth() * 46 / 100));
    r.removeFromLeft (10);
    details->setBounds (r);
}

// =============================================================================
// Overlay
// =============================================================================
PresetBrowserOverlay::PresetBrowserOverlay (EngineController& controller, std::shared_ptr<PresetLoudnessEstimator> estimator, int inset)
    : browser (controller, std::move (estimator)), topInset (inset)
{
    setTitle ("Preset browser");
    addAndMakeVisible (browser);
    browser.onClose = [this]
    {
        if (onClose != nullptr)
            onClose();
    };
}

juce::Rectangle<int> PresetBrowserOverlay::browserBounds (juce::Rectangle<int> area, int inset)
{
    const int w = juce::jmin (820, area.getWidth() - 32);
    const int h = juce::jmin (540, area.getHeight() - inset - 24);
    return { area.getCentreX() - w / 2, area.getY() + inset + 8, juce::jmax (0, w), juce::jmax (0, h) };
}

void PresetBrowserOverlay::paint (juce::Graphics& g)
{
    g.setColour (Palette::background.withAlpha (0.6f));
    g.fillRect (getLocalBounds().withTrimmedTop (topInset));
}

void PresetBrowserOverlay::resized() { browser.setBounds (browserBounds (getLocalBounds(), topInset)); }

void PresetBrowserOverlay::parentSizeChanged()
{
    if (auto* parent = getParentComponent())
        setBounds (parent->getLocalBounds());
}

void PresetBrowserOverlay::mouseDown (const juce::MouseEvent&) { browser.cancel(); }
} // namespace flub::app::ui
