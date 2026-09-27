// Flubsound Pro - the "Export / batch process" dialog (preset menu >
// "Export / batch process audio files...").
//
//   INPUT       [Add files...] [Add folder...] [Clear]   (o) Include sub-folders
//               what was added (files / folders)
//   OUTPUT      Folder [path ...........................] [Choose...]
//               Format [WAV 32-bit float v]   Settings [Current strip (Game) v]
//   LOUDNESS    (o) Loudness target [-14.0 LUFS]   (o) Ceiling [-1.0 dBTP]
//   +------------------------------------------------------------------+
//   | File | Status | In LUFS | Out LUFS | Out dBTP | Details           |
//   +------------------------------------------------------------------+
//   [progress 3 / 10 files ..........]  [Reveal in folder] [Cancel] [Start]
//
// Settings: "Current strip" snapshots the SELECTED strip's active A/B bank
// when Start is pressed (ExportJob::snapshotStrip; Bypass All is ignored),
// or any factory / user preset (ExportJob::presetValues). The live engine
// keeps playing: the export renders on its own chains, on ExportJob's worker
// thread (never the message or the audio thread). The table follows the job
// (ChangeListener): status per file, loudness in / out, true peak, error or
// renderer notes; double-click a finished row (or select it and press
// Reveal) to show the file. Cancel stops after the file being rendered;
// closing the dialog aborts the render in progress (its partial file is
// discarded). Message thread only.
#pragma once

#include "ExportJob.h"
#include "engine/EngineController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace flub::app::ui
{
class ExportDialog final : public juce::Component,
                           private juce::ChangeListener,
                           private juce::TableListBoxModel,
                           private EngineController::Listener
{
public:
    /** Smallest size at which everything fits (the table shrinks first). */
    static constexpr int kMinWidth = 720, kMinHeight = 560;

    explicit ExportDialog (EngineController& controller);
    ~ExportDialog() override;

    /** Opens the dialog (non-modal, owns its content). The caller should keep
        the returned window (SafePointer) and delete it before the controller. */
    static juce::DialogWindow* show (EngineController& controller, juce::Component* parent);

    // ---- Headless access (tests, scripted use) ----------------------------------
    void setInputs (const juce::Array<juce::File>& inputs);
    void setOutputFolder (const juce::File& folder);
    void setFormat (ExportFormat format);
    void setRecursive (bool shouldRecurse);
    /** "" = the current strip; otherwise a preset id ("factory:..." / "user:..."). */
    void setParameterSource (const juce::String& presetId);
    void setLoudnessTarget (std::optional<float> lufs);
    void setCeiling (std::optional<float> dbtp);
    /** The job the Start button would run, with the parameter snapshot taken
        now. False with a message when the settings are incomplete. */
    bool buildSettings (ExportSettings& settings, juce::String& error) const;
    /** Presses Start. */
    bool startExport();
    ExportJob& getJob() noexcept { return job; }
    juce::String getStatusText() const { return statusText; }

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void engineControllerChanged (EngineController::Change change) override;

    int getNumRows() override;
    void paintRowBackground (juce::Graphics& g, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics& g, int row, int column, int width, int height, bool selected) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent& e) override;
    juce::String getCellTooltip (int row, int column) override;

    void chooseFiles();
    void chooseFolder (bool forOutput);
    void rebuildSources();
    void updateInputSummary();
    void updateControls();
    void revealSelected();
    juce::String cellText (const ExportItem& item, int column) const;

    EngineController& controller;
    ExportJob job;

    juce::Array<juce::File> inputs;
    juce::File outputFolder;
    std::vector<ExportItem> rows;
    std::vector<juce::String> presetIds; // source combo item id - kFirstPresetItem -> preset id
    juce::String inputSummary, statusText;
    bool statusIsError = false;
    double progressValue = 0.0;

    juce::TextButton addFiles { "Add files..." }, addFolder { "Add folder..." }, clearInputs { "Clear" };
    juce::ToggleButton recursive { "Include sub-folders" };
    juce::Label outputPath;
    juce::TextButton chooseOutput { "Choose..." };
    juce::ComboBox formatBox, sourceBox;
    juce::ToggleButton targetOn { "Loudness target" }, ceilingOn { "Ceiling" };
    juce::Slider targetSlider, ceilingSlider;
    juce::TableListBox table;
    juce::ProgressBar progressBar { progressValue };
    juce::TextButton revealButton { "Reveal in folder" }, cancelButton { "Cancel" }, startButton { "Start" };
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::Rectangle<int> inputTitle, outputTitle, loudnessTitle, summaryArea, folderCaption, formatCaption, sourceCaption, statusArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportDialog)
};
} // namespace flub::app::ui
