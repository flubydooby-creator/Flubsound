// Flubsound Pro - the preset browser (docs/11 E40), opened from the header's
// preset box (a click, or Space / Enter on it) or the preset menu.
//
//   +-- Presets -------------------------------------------------------------+
//   | [search name, tags, description...       ] [Preview] [Match loudness]  |
//   | [All] [Favourites] [Recent] [For <headset>]  [Any] [Music] [Gaming] [v] |
//   | [night] [footsteps] [bass] [voice] ... (the most common tags)          |
//   | +- list ---------------------+  +- details ---------------------------+ |
//   | | Current sound: Signature   |  | Late Night Low Volume               | |
//   | | * Late Night Low Volume    |  | Factory . Music . by ...            | |
//   | |   Music . night, low-vol.. |  | For listening quietly without ...   | |
//   | | ...                        |  | Tags, latency profile, warnings,    | |
//   | +----------------------------+  | loudness against the current sound  | |
//   |  3 of 25 presets . matched -2.1 dB          [Favourite] [Cancel] [Load] |
//   +------------------------------------------------------------------------+
//
// * Search: every word of the query is looked for (as a word prefix) in the
//   name, tags, category, mode, author and description; presets matching
//   more words come first, then those matching in more prominent fields
//   (filterPresets). "late night quiet" finds Late Night Low Volume first.
// * Filters: scope (all, favourites, recent - newest first -, and
//   "For <headset>": the preset the output's device profile suggests for the
//   strip's mode and, on Bluetooth, the presets tagged bluetooth), mode,
//   category and tag chips (every selected tag must be present).
// * Favourites (the star, or the Favourite button) and recents are kept in
//   AppSettings by preset id (the uuid, docs/11 E52).
// * The first row is always the current sound, so a preview can be compared
//   with it by moving between the two rows.
// * Preview (on by default): the selected row plays on the selected strip
//   (PresetAudition: plain preview in the active bank); Load (Enter, a double
//   click) loads it through the controller, Cancel (Escape, a click outside)
//   restores the sound the browser opened with. With Match loudness (on by
//   default) the louder of the two sides is turned down to the quieter
//   (PresetAudition::matchTrims) from the chain gains PresetLoudnessEstimator
//   renders in the background: a louder preset no longer wins the comparison
//   by being louder (docs/11 E37).
// * The details show the description, tags, author, the latency profile the
//   preset was made for, the reader's warnings (unknown keys, clamped values)
//   and the loudness against the current sound.
// Message thread only.
#pragma once

#include "PresetAudition.h"
#include "Widgets.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace flub::app::ui
{
class PresetBrowser final : public juce::Component, private EngineController::Listener, private juce::Timer
{
public:
    // ---- The model (pure; tested) -----------------------------------------------------------
    enum class Scope { All, Favourites, Recent, Recommended };
    struct Filter
    {
        Scope scope = Scope::All;
        juce::String query;
        juce::String mode;     // "Music" / "Gaming"; empty: any
        juce::String category; // empty: any
        juce::StringArray tags; // every one must be present
    };
    struct Context
    {
        juce::StringArray favourites, recent, recommended; // preset ids (recent: newest first)
    };
    /** The lower-case words of a query (letters and digits; words shorter than
        2 characters and a few filler words dropped). */
    static juce::StringArray searchTerms (const juce::String& query);
    /** How well `preset` matches `terms`: the number of terms found (as a
        prefix of a word) and a score, the sum over those terms of the most
        prominent field each was found in (name 6, tag 4, category 3, mode 2,
        author and description 1). */
    struct Match
    {
        int terms = 0, score = 0;
    };
    static Match match (const PresetInfo& preset, const juce::StringArray& terms);
    /** Indices into `presets` in display order: filtered by scope, mode,
        category and tags; with a query, only presets matching at least one
        term, best first (terms, then score, then list order); Recent in
        recency order. */
    static std::vector<int> filterPresets (const std::vector<PresetInfo>& presets, const Filter& filter, const Context& context);
    /** Ids recommended for the output: the preset the device profile suggests
        (flub::device::Advice::suggestedPreset, by name) and, on a Bluetooth
        link, the presets tagged "bluetooth". */
    static juce::StringArray recommendedFor (const std::vector<PresetInfo>& presets, const juce::String& suggestedPreset,
                                             flub::device::Connection connection);
    /** The `maxTags` most common tags, most common first (ties: alphabetical). */
    static juce::StringArray commonTags (const std::vector<PresetInfo>& presets, int maxTags);
    /** "2.1 LU louder than your current sound", "1.4 LU quieter ...", "About
        as loud as your current sound" (|difference| < 0.5 LU). */
    static juce::String describeLoudness (float originalGainLu, float presetGainLu);

    // ---- The component ------------------------------------------------------------------------
    PresetBrowser (EngineController& controller, std::shared_ptr<PresetLoudnessEstimator> estimator);
    ~PresetBrowser() override; // cancels a preview that was not loaded

    /** Called when the browser is done (loaded, cancelled): the owner closes it. */
    std::function<void()> onClose;

    void setQuery (const juce::String& query);
    void setScope (Scope scope);
    Filter getFilter() const { return filter; }
    /** Presets shown, in order (excluding the current-sound row). */
    std::vector<const PresetInfo*> getShownPresets() const;
    /** Selects a row by preset id (empty: the current-sound row); previews it
        when preview is on. False if it is not shown. */
    bool selectPreset (const juce::String& presetId);
    /** The selected preset; nullptr while the current-sound row is selected.
        A selection the filter hides stays selected (and playing). */
    const PresetInfo* getSelectedPreset() const;
    bool isCurrentSoundSelected() const noexcept { return selectedId.isEmpty(); }

    void setPreviewEnabled (bool enabled);
    bool isPreviewEnabled() const noexcept { return previewEnabled; }
    void setMatchEnabled (bool enabled);
    bool isMatchEnabled() const noexcept { return matchEnabled; }

    /** Loads the selected preset (the controller's loadPreset) and closes. */
    bool loadSelected();
    /** Restores the sound the browser opened with and closes. */
    void cancel();
    void toggleFavourite (const juce::String& presetId);

    /** The input level the loudness estimates are made at (LUFS): the
        strip's programme as it arrives (its input loudness less the input
        gain and AutoLevel), measured when the browser opens or, if nothing
        plays then, when programme first arrives; kDefaultLevelLufs until then. */
    float getLevelLufs() const noexcept { return levelLufs; }
    PresetAudition& getAudition() noexcept { return audition; }
    PresetLoudnessEstimator& getEstimator() noexcept { return *estimator; }
    /** The detail pane's lines (for tests and accessibility). */
    juce::StringArray getDetailLines() const;
    /** The status line under the list ("3 of 25 presets . matched -2.1 dB"). */
    juce::String getStatusText() const;
    juce::TextEditor& getSearchBox() noexcept { return search; }
    juce::ListBox& getList() noexcept { return list; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    class ListModel;
    class DetailPane;
    /** The search field: Up / Down / Page keys move the list selection. */
    class SearchBox final : public juce::TextEditor
    {
    public:
        std::function<bool (const juce::KeyPress&)> onNavigationKey;
        bool keyPressed (const juce::KeyPress& key) override
        {
            if (onNavigationKey != nullptr && onNavigationKey (key))
                return true;
            return juce::TextEditor::keyPressed (key);
        }
    };
    struct DetailLine
    {
        enum class Kind { Title, Meta, Badge, Body, Muted, Warning, Loudness };
        Kind kind;
        juce::String text;
    };
    std::vector<DetailLine> detailLines() const;

    void engineControllerChanged (EngineController::Change change) override;
    void timerCallback() override;
    std::optional<float> liveProgrammeLevel() const;
    PresetLoudnessEstimator::Variant estimateVariant() const;
    void reloadPresets();
    void rebuildTagChips();
    void applyFilter();
    void rowSelected (int row);
    void playSelection();
    void updateMatch();
    void requestEstimates();
    void refreshButtons();
    void close();
    const PresetInfo* findPreset (const juce::String& id) const;
    /** The values `preset` plays with in this session (cached; empty on a read error). */
    const std::vector<float>& valuesFor (const PresetInfo& preset) const;
    std::optional<float> estimateFor (const PresetInfo& preset) const;
    std::optional<float> originalEstimate() const;

    EngineController& controller;
    std::shared_ptr<PresetLoudnessEstimator> estimator;
    int estimateListener = 0;
    PresetAudition audition;
    std::vector<PresetInfo> presets;
    Context context;
    Filter filter;
    std::vector<int> shown;      // indices into presets; list row r > 0 is shown[r - 1], row 0 the current sound
    juce::String selectedId;     // empty: the current sound
    juce::String lastPreviewedId; // the preset the current sound is compared with
    juce::StringArray selectedWarnings;
    juce::String selectedError;
    mutable std::map<juce::String, std::vector<float>> valuesCache;
    bool previewEnabled = true, matchEnabled = true, closing = false;
    float levelLufs = PresetLoudnessEstimator::kDefaultLevelLufs;
    bool levelMeasured = false;
    juce::String deviceLabel;

    SearchBox search;
    juce::TextButton previewButton { "Preview" }, matchButton { "Match loudness" };
    juce::TextButton scopeAll { "All" }, scopeFavourites { "Favourites" }, scopeRecent { "Recent" }, scopeRecommended { "For headset" };
    juce::TextButton modeAny { "Any" }, modeMusic { "Music" }, modeGaming { "Gaming" };
    juce::ComboBox categoryBox;
    std::vector<std::unique_ptr<juce::TextButton>> tagChips;
    std::unique_ptr<ListModel> model;
    juce::ListBox list;
    std::unique_ptr<DetailPane> details;
    juce::TextButton favouriteButton { "Favourite" }, cancelButton { "Cancel" }, loadButton { "Load" };
    juce::Rectangle<int> statusArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};

/** The browser as a popover over the main window: a scrim over the whole
    window (a click on it cancels) with the browser under the header. */
class PresetBrowserOverlay final : public juce::Component
{
public:
    PresetBrowserOverlay (EngineController& controller, std::shared_ptr<PresetLoudnessEstimator> estimator, int topInset);

    PresetBrowser& getBrowser() noexcept { return browser; }
    std::function<void()> onClose;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void parentSizeChanged() override;
    void mouseDown (const juce::MouseEvent& e) override;

    /** Browser bounds for a window of `area` (the overlay's size) below topInset. */
    static juce::Rectangle<int> browserBounds (juce::Rectangle<int> area, int topInset);

private:
    PresetBrowser browser;
    int topInset;
};
} // namespace flub::app::ui
