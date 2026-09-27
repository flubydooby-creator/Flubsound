#include "ExportDialog.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <array>

namespace flub::app::ui
{
namespace
{
constexpr int kRowHeight = 28;
constexpr int kCaptionWidth = 64;
constexpr int kCurrentStripItem = 1;
constexpr int kFirstPresetItem = 100;

enum Column
{
    FileColumn = 1,
    StatusColumn,
    InLufsColumn,
    OutLufsColumn,
    OutPeakColumn,
    DetailsColumn
};

void drawSectionTitle (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    Theme::drawCaption (g, title.toUpperCase(), area.toFloat(), Palette::muted);
    g.setColour (Palette::border);
    g.fillRect (area.getX(), area.getBottom() - 1, area.getWidth(), 1);
}

juce::Colour statusColour (ExportItem::Status status)
{
    switch (status)
    {
        case ExportItem::Status::Done: return Palette::green;
        case ExportItem::Status::Failed: return Palette::red;
        case ExportItem::Status::Running: return Palette::teal;
        case ExportItem::Status::Skipped:
        case ExportItem::Status::Cancelled: return Palette::faint.brighter (0.3f);
        case ExportItem::Status::Queued: break;
    }
    return Palette::muted;
}

const std::array<ExportFormat, 5> kFormats { ExportFormat::WavFloat32, ExportFormat::WavPcm24, ExportFormat::WavPcm16, ExportFormat::Flac24,
                                             ExportFormat::Flac16 };
} // namespace

ExportDialog::ExportDialog (EngineController& c)
    : controller (c)
{
    job.addChangeListener (this);
    controller.addListener (this);

    // ---- Input ----
    addFiles.onClick = [this] { chooseFiles(); };
    addFolder.onClick = [this] { chooseFolder (false); };
    clearInputs.onClick = [this] { setInputs ({}); };
    Style::set (recursive, "switch");
    recursive.setTooltip ("Also process the files in the sub-folders of every added folder (their folder tree is kept in the output folder)");
    for (auto* b : std::initializer_list<juce::Component*> { &addFiles, &addFolder, &clearInputs, &recursive })
        addAndMakeVisible (b);

    // ---- Output ----
    outputPath.setColour (juce::Label::textColourId, Palette::text.withAlpha (0.88f));
    outputPath.setColour (juce::Label::backgroundColourId, Palette::well);
    outputPath.setColour (juce::Label::outlineColourId, Palette::border);
    outputPath.setFont (Theme::font (12.5f));
    outputPath.setMinimumHorizontalScale (0.6f);
    chooseOutput.onClick = [this] { chooseFolder (true); };
    for (size_t i = 0; i < kFormats.size(); ++i)
        formatBox.addItem (ExportJob::describeFormat (kFormats[i]), static_cast<int> (i) + 1);
    formatBox.setSelectedId (1, juce::dontSendNotification);
    formatBox.setTooltip ("WAV: float32 is bit-exact; PCM formats are TPDF dithered (identical to flubsound-cli). FLAC is dithered the same way");
    sourceBox.setTooltip ("The processing settings: the selected strip's current settings (taken when you press Start) or a preset");
    for (auto* b : std::initializer_list<juce::Component*> { &outputPath, &chooseOutput, &formatBox, &sourceBox })
        addAndMakeVisible (b);
    rebuildSources();

    // ---- Loudness ----
    Style::set (targetOn, "switch");
    Style::set (ceilingOn, "switch");
    targetOn.setTooltip ("Render, measure the integrated loudness (EBU R128) and re-render with the maximizer drive / output gain "
                         "moved until the file is within 0.3 LU of the target");
    ceilingOn.setTooltip ("True-peak ceiling of the maximizer (enabled when needed); the file is trimmed if the limiter overshoots");
    auto setUpSlider = [] (juce::Slider& s, double min, double max, double step, double value, const juce::String& unit)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 86, 22);
        s.setRange (min, max, step);
        s.setValue (value, juce::dontSendNotification);
        s.textFromValueFunction = [unit] (double v) { return juce::String (v, 1) + " " + unit; };
        s.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("-+.0123456789").getDoubleValue(); };
        s.updateText();
    };
    setUpSlider (targetSlider, -60.0, -1.0, 0.5, -14.0, "LUFS");
    const auto& ceiling = flub::param::layout()[static_cast<size_t> (flub::param::MaxCeilingDb)];
    setUpSlider (ceilingSlider, ceiling.minValue, ceiling.maxValue, 0.1, -1.0, "dBTP");
    targetOn.onClick = [this] { updateControls(); };
    ceilingOn.onClick = [this] { updateControls(); };
    for (auto* b : std::initializer_list<juce::Component*> { &targetOn, &ceilingOn, &targetSlider, &ceilingSlider })
        addAndMakeVisible (b);

    // ---- Results ----
    table.setModel (this);
    table.setRowHeight (22);
    table.setColour (juce::ListBox::backgroundColourId, Palette::well);
    table.setColour (juce::ListBox::outlineColourId, Palette::border);
    table.setOutlineThickness (1);
    auto& header = table.getHeader();
    const int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;
    header.addColumn ("File", FileColumn, 190, 80, -1, columnFlags);
    header.addColumn ("Status", StatusColumn, 74, 60, -1, columnFlags);
    header.addColumn ("In LUFS", InLufsColumn, 62, 50, -1, columnFlags);
    header.addColumn ("Out LUFS", OutLufsColumn, 66, 50, -1, columnFlags);
    header.addColumn ("Out dBTP", OutPeakColumn, 66, 50, -1, columnFlags);
    header.addColumn ("Details", DetailsColumn, 220, 80, -1, columnFlags);
    header.setStretchToFitActive (true);
    addAndMakeVisible (table);

    // ---- Actions ----
    progressBar.setPercentageDisplay (false);
    revealButton.onClick = [this] { revealSelected(); };
    revealButton.setTooltip ("Show the selected file (or the output folder) in the file manager");
    cancelButton.onClick = [this]
    {
        job.cancel();
        statusText = "Stopping after the current file...";
        statusIsError = false;
        repaint();
    };
    startButton.onClick = [this] { startExport(); };
    for (auto* b : std::initializer_list<juce::Component*> { &progressBar, &revealButton, &cancelButton, &startButton })
        addAndMakeVisible (b);

    updateInputSummary();
    updateControls();
    setSize (kMinWidth, kMinHeight);
}

ExportDialog::~ExportDialog()
{
    controller.removeListener (this);
    job.removeChangeListener (this);
    table.setModel (nullptr);
}

juce::DialogWindow* ExportDialog::show (EngineController& controller, juce::Component* parent)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new ExportDialog (controller));
    options.dialogTitle = "Flubsound Pro - Export / batch process";
    options.dialogBackgroundColour = Palette::background;
    options.componentToCentreAround = parent;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    auto* window = options.launchAsync();
    if (window != nullptr)
        window->setResizeLimits (kMinWidth, kMinHeight, 1600, 1200);
    return window;
}

// =============================================================================
// Settings
// =============================================================================
void ExportDialog::setInputs (const juce::Array<juce::File>& newInputs)
{
    inputs = newInputs;
    updateInputSummary();
    updateControls();
}

void ExportDialog::setOutputFolder (const juce::File& folder)
{
    outputFolder = folder;
    outputPath.setText (folder == juce::File() ? juce::String ("(choose an output folder)") : folder.getFullPathName(), juce::dontSendNotification);
    updateControls();
}

void ExportDialog::setFormat (ExportFormat format)
{
    for (size_t i = 0; i < kFormats.size(); ++i)
        if (kFormats[i] == format)
            formatBox.setSelectedId (static_cast<int> (i) + 1, juce::dontSendNotification);
}

void ExportDialog::setRecursive (bool shouldRecurse)
{
    recursive.setToggleState (shouldRecurse, juce::dontSendNotification);
}

void ExportDialog::setParameterSource (const juce::String& presetId)
{
    int id = kCurrentStripItem;
    for (size_t i = 0; i < presetIds.size(); ++i)
        if (presetIds[i] == presetId)
            id = kFirstPresetItem + static_cast<int> (i);
    sourceBox.setSelectedId (id, juce::dontSendNotification);
}

void ExportDialog::setLoudnessTarget (std::optional<float> lufs)
{
    targetOn.setToggleState (lufs.has_value(), juce::dontSendNotification);
    if (lufs)
        targetSlider.setValue (*lufs, juce::dontSendNotification);
    updateControls();
}

void ExportDialog::setCeiling (std::optional<float> dbtp)
{
    ceilingOn.setToggleState (dbtp.has_value(), juce::dontSendNotification);
    if (dbtp)
        ceilingSlider.setValue (*dbtp, juce::dontSendNotification);
    updateControls();
}

bool ExportDialog::buildSettings (ExportSettings& s, juce::String& error) const
{
    s = ExportSettings();
    s.inputs = inputs;
    s.recursive = recursive.getToggleState();
    s.outputFolder = outputFolder;
    s.format = kFormats[static_cast<size_t> (juce::jlimit (1, static_cast<int> (kFormats.size()), formatBox.getSelectedId()) - 1)];
    if (targetOn.getToggleState())
        s.targetLufs = static_cast<float> (targetSlider.getValue());
    if (ceilingOn.getToggleState())
        s.ceilingDb = static_cast<float> (ceilingSlider.getValue());

    const int source = sourceBox.getSelectedId();
    if (source >= kFirstPresetItem && source - kFirstPresetItem < static_cast<int> (presetIds.size()))
    {
        const auto& presets = controller.getPresetManager();
        const auto* preset = presets.findById (presetIds[static_cast<size_t> (source - kFirstPresetItem)]);
        if (preset == nullptr)
        {
            error = "The selected preset no longer exists.";
            return false;
        }
        if (! ExportJob::presetValues (presets, *preset, s.values, error))
            return false;
    }
    else
    {
        // The strip the UI edits, as heard right now (its active A/B bank).
        s.values = ExportJob::snapshotStrip (controller.getParams (controller.getSelectedStrip()));
    }
    return ExportJob::validate (s, error);
}

bool ExportDialog::startExport()
{
    ExportSettings s;
    juce::String error;
    const bool ok = buildSettings (s, error) && job.start (std::move (s), error);
    statusText = ok ? juce::String ("Scanning...") : error;
    statusIsError = ! ok;
    if (ok)
        rows.clear();
    table.updateContent();
    updateControls();
    repaint();
    return ok;
}

void ExportDialog::rebuildSources()
{
    const int selected = sourceBox.getSelectedId() - kFirstPresetItem;
    const auto previous = selected >= 0 && selected < static_cast<int> (presetIds.size()) ? presetIds[static_cast<size_t> (selected)] : juce::String();
    sourceBox.clear (juce::dontSendNotification);
    presetIds.clear();
    sourceBox.addItem ("Current strip settings (" + controller.getStripName (controller.getSelectedStrip()) + ")", kCurrentStripItem);

    juce::String section;
    for (const auto& p : controller.getPresetManager().getPresets())
    {
        const auto heading = (p.isFactory ? "Factory - " : "User - ") + p.category;
        if (heading != section)
        {
            sourceBox.addSectionHeading (heading);
            section = heading;
        }
        presetIds.push_back (p.id);
        sourceBox.addItem (p.name, kFirstPresetItem + static_cast<int> (presetIds.size()) - 1);
    }
    sourceBox.setSelectedId (kCurrentStripItem, juce::dontSendNotification);
    if (previous.isNotEmpty())
        setParameterSource (previous);
}

void ExportDialog::engineControllerChanged (EngineController::Change change)
{
    if (change == EngineController::Change::SelectedStrip || change == EngineController::Change::Preset
        || change == EngineController::Change::Engine)
        rebuildSources();
}

// =============================================================================
// Choosers
// =============================================================================
void ExportDialog::chooseFiles()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Add audio files", juce::File(), ExportJob::inputWildcard (job.getFormatManager()));
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [safe = juce::Component::SafePointer<ExportDialog> (this)] (const juce::FileChooser& chooser)
                              {
                                  if (safe == nullptr)
                                      return;
                                  auto list = safe->inputs;
                                  for (const auto& f : chooser.getResults())
                                      list.addIfNotAlreadyThere (f);
                                  safe->setInputs (list);
                              });
}

void ExportDialog::chooseFolder (bool forOutput)
{
    fileChooser = std::make_unique<juce::FileChooser> (forOutput ? "Output folder" : "Add a folder of audio files", forOutput ? outputFolder : juce::File());
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<ExportDialog> (this), forOutput] (const juce::FileChooser& chooser)
                              {
                                  const auto folder = chooser.getResult();
                                  if (safe == nullptr || folder == juce::File())
                                      return;
                                  if (forOutput)
                                  {
                                      safe->setOutputFolder (folder);
                                  }
                                  else
                                  {
                                      auto list = safe->inputs;
                                      list.addIfNotAlreadyThere (folder);
                                      safe->setInputs (list);
                                  }
                              });
}

void ExportDialog::updateInputSummary()
{
    int files = 0, folders = 0;
    for (const auto& f : inputs)
        (f.isDirectory() ? folders : files)++;
    if (inputs.isEmpty())
    {
        inputSummary = "No input yet: add audio files (WAV, AIFF, FLAC, Ogg"
                       + juce::String (job.getFormatManager().findFormatForFileExtension (".mp3") != nullptr ? ", MP3" : "")
                       + ") or a folder.";
    }
    else
    {
        juce::StringArray parts;
        if (files > 0)
            parts.add (juce::String (files) + (files == 1 ? " file" : " files"));
        if (folders > 0)
            parts.add (juce::String (folders) + (folders == 1 ? " folder" : " folders"));
        juce::StringArray names;
        for (const auto& f : inputs)
            names.add (f.getFileName());
        inputSummary = parts.joinIntoString (", ") + ":  " + names.joinIntoString (", ");
    }
    repaint (summaryArea);
}

void ExportDialog::updateControls()
{
    const bool busy = job.isRunning();
    for (auto* c : std::initializer_list<juce::Component*> { &addFiles, &addFolder, &clearInputs, &recursive, &chooseOutput, &formatBox, &sourceBox,
                                                             &targetOn, &ceilingOn })
        c->setEnabled (! busy);
    targetSlider.setEnabled (! busy && targetOn.getToggleState());
    ceilingSlider.setEnabled (! busy && ceilingOn.getToggleState());
    startButton.setEnabled (! busy && ! inputs.isEmpty() && outputFolder != juce::File());
    cancelButton.setEnabled (busy);
    revealButton.setEnabled (outputFolder != juce::File());
}

void ExportDialog::revealSelected()
{
    const int row = table.getSelectedRow();
    if (row >= 0 && row < static_cast<int> (rows.size()) && rows[static_cast<size_t> (row)].output.existsAsFile())
    {
        rows[static_cast<size_t> (row)].output.revealToUser();
        return;
    }
    if (outputFolder.isDirectory())
        outputFolder.revealToUser();
}

// =============================================================================
// Job progress
// =============================================================================
void ExportDialog::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rows = job.getItems();
    const auto p = job.getProgress();
    progressValue = p.total > 0 ? static_cast<double> (p.finished) / static_cast<double> (p.total) : (p.completed ? 1.0 : 0.0);
    progressBar.setTextToDisplay (juce::String (p.finished) + " / " + juce::String (p.total) + (p.total == 1 ? " file" : " files"));

    statusIsError = p.error.isNotEmpty() || p.failed > 0;
    if (p.error.isNotEmpty())
        statusText = p.error;
    else if (p.running)
        statusText = p.current >= 0 && p.current < static_cast<int> (rows.size()) ? "Rendering " + rows[static_cast<size_t> (p.current)].displayName
                                                                                 : juce::String ("Scanning...");
    else if (p.completed)
        statusText = juce::String (p.finished - p.failed) + " done" + (p.failed > 0 ? ", " + juce::String (p.failed) + " failed" : juce::String())
                     + (p.skipped > 0 ? ", " + juce::String (p.skipped) + " skipped" : juce::String())
                     + (p.cancelled ? ", cancelled" : juce::String());
    if (p.error.isEmpty() && p.notes.size() > 0)
        statusText << "  -  " << p.notes.joinIntoString ("; ");

    table.updateContent();
    table.repaint();
    updateControls();
    repaint (statusArea);
}

int ExportDialog::getNumRows()
{
    return static_cast<int> (rows.size());
}

void ExportDialog::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    if (selected)
        g.fillAll (Palette::panelHover);
    else if (row % 2 == 1)
        g.fillAll (Palette::panel.withAlpha (0.5f));
}

juce::String ExportDialog::cellText (const ExportItem& item, int column) const
{
    const bool done = item.status == ExportItem::Status::Done;
    const bool measured = done || item.status == ExportItem::Status::Failed;
    switch (column)
    {
        case FileColumn: return item.displayName;
        case StatusColumn: return ExportJob::statusText (item.status);
        case InLufsColumn: return measured && item.numFrames > 0 ? Theme::formatLufs (item.inLufs) : juce::String();
        case OutLufsColumn: return done ? Theme::formatLufs (item.outLufs) : juce::String();
        case OutPeakColumn: return done ? Theme::formatDb (item.outTruePeakDbtp, 2) : juce::String();
        case DetailsColumn:
        {
            if (item.error.isNotEmpty())
                return item.error;
            juce::StringArray parts;
            if (item.inputFormat.isNotEmpty())
                parts.add (item.inputFormat);
            parts.addArray (item.notes);
            return parts.joinIntoString ("; ");
        }
        default: break;
    }
    return {};
}

void ExportDialog::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    if (row < 0 || row >= static_cast<int> (rows.size()))
        return;
    const auto& item = rows[static_cast<size_t> (row)];
    const bool numeric = column == InLufsColumn || column == OutLufsColumn || column == OutPeakColumn;
    g.setFont (numeric ? Theme::numeric (12.0f, false) : Theme::font (12.0f));
    juce::Colour colour = Palette::text.withAlpha (0.88f);
    if (column == StatusColumn)
        colour = statusColour (item.status);
    else if (column == DetailsColumn)
        colour = item.status == ExportItem::Status::Failed ? Palette::red : Palette::muted;
    g.setColour (colour);
    g.drawText (cellText (item, column), 6, 0, width - 10, height,
                numeric ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
}

void ExportDialog::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int> (rows.size()) && rows[static_cast<size_t> (row)].output.existsAsFile())
        rows[static_cast<size_t> (row)].output.revealToUser();
}

juce::String ExportDialog::getCellTooltip (int row, int column)
{
    if (row < 0 || row >= static_cast<int> (rows.size()))
        return {};
    const auto& item = rows[static_cast<size_t> (row)];
    if (column == FileColumn)
        return item.input.getFullPathName() + (item.output != juce::File() ? "\n-> " + item.output.getFullPathName() : juce::String());
    return cellText (item, column);
}

// =============================================================================
// Layout
// =============================================================================
void ExportDialog::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    drawSectionTitle (g, inputTitle, "Input");
    drawSectionTitle (g, outputTitle, "Output");
    drawSectionTitle (g, loudnessTitle, "Loudness");

    g.setFont (Theme::font (12.0f));
    g.setColour (Palette::muted);
    g.drawFittedText (inputSummary, summaryArea, juce::Justification::centredLeft, 1, 0.8f);

    g.setFont (Theme::font (12.5f));
    g.setColour (Palette::text.withAlpha (0.88f));
    g.drawText ("Folder", folderCaption, juce::Justification::centredLeft, true);
    g.drawText ("Format", formatCaption, juce::Justification::centredLeft, true);
    g.drawText ("Settings", sourceCaption, juce::Justification::centredLeft, true);

    g.setFont (Theme::font (12.0f));
    g.setColour (statusIsError ? Palette::red : Palette::muted);
    g.drawFittedText (statusText, statusArea, juce::Justification::centredLeft, 1, 0.8f);
}

void ExportDialog::resized()
{
    auto r = getLocalBounds().reduced (16, 12);
    const int gap = 8;

    // Input
    inputTitle = r.removeFromTop (22);
    r.removeFromTop (gap);
    {
        auto row = r.removeFromTop (kRowHeight);
        addFiles.setBounds (row.removeFromLeft (104));
        row.removeFromLeft (gap);
        addFolder.setBounds (row.removeFromLeft (110));
        row.removeFromLeft (gap);
        clearInputs.setBounds (row.removeFromLeft (64));
        row.removeFromLeft (2 * gap);
        recursive.setBounds (row.removeFromLeft (juce::jmin (row.getWidth(), 200)));
    }
    r.removeFromTop (4);
    summaryArea = r.removeFromTop (20);
    r.removeFromTop (gap);

    // Output
    outputTitle = r.removeFromTop (22);
    r.removeFromTop (gap);
    {
        auto row = r.removeFromTop (kRowHeight);
        folderCaption = row.removeFromLeft (kCaptionWidth);
        chooseOutput.setBounds (row.removeFromRight (96));
        row.removeFromRight (gap);
        outputPath.setBounds (row);
    }
    r.removeFromTop (gap);
    {
        auto row = r.removeFromTop (kRowHeight);
        auto left = row.removeFromLeft (row.getWidth() / 2 - gap);
        formatCaption = left.removeFromLeft (kCaptionWidth);
        formatBox.setBounds (left);
        row.removeFromLeft (2 * gap);
        sourceCaption = row.removeFromLeft (kCaptionWidth);
        sourceBox.setBounds (row);
    }
    r.removeFromTop (gap);

    // Loudness
    loudnessTitle = r.removeFromTop (22);
    r.removeFromTop (gap);
    {
        auto row = r.removeFromTop (kRowHeight);
        auto left = row.removeFromLeft (row.getWidth() / 2 - gap);
        targetOn.setBounds (left.removeFromLeft (150));
        targetSlider.setBounds (left);
        row.removeFromLeft (2 * gap);
        ceilingOn.setBounds (row.removeFromLeft (100));
        ceilingSlider.setBounds (row);
    }
    r.removeFromTop (12);

    // Actions (bottom), status line, table (the rest)
    {
        auto row = r.removeFromBottom (kRowHeight);
        startButton.setBounds (row.removeFromRight (84));
        row.removeFromRight (gap);
        cancelButton.setBounds (row.removeFromRight (84));
        row.removeFromRight (gap);
        revealButton.setBounds (row.removeFromRight (128));
        row.removeFromRight (2 * gap);
        progressBar.setBounds (row);
    }
    r.removeFromBottom (4);
    statusArea = r.removeFromBottom (20);
    r.removeFromBottom (4);
    table.setBounds (r);
}
} // namespace flub::app::ui
