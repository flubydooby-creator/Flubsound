#include "MainComponent.h"

#include "Theme.h"
#include "shell/MainWindow.h"

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
constexpr const char* kPrefMeterPalette = "ui.meterPalette";
constexpr const char* kPrefAnalyzer = "ui.analyzer";
constexpr const char* kPrefVisualiserWindow = "ui.visualiserWindow";
} // namespace

MainComponent::MainComponent (EngineController& c)
    : controller (c),
      header (c),
      deviceError (c),
      deviceBanner (c),
      routing (c),
      boost (c),
      analyzer ([this] { return &controller.getSelectedParams(); }),
      rack (c),
      simple (c)
{
    setTitle ("Flubsound Pro");
    setOpaque (true);
    setWantsKeyboardFocus (true);

    // One FlubLookAndFeel for the whole UI: normally the application default
    // (so dialogs, tooltips and the tray menu match); a private one otherwise.
    if (dynamic_cast<FlubLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()) == nullptr)
    {
        ownLookAndFeel = std::make_unique<FlubLookAndFeel>();
        setLookAndFeel (ownLookAndFeel.get());
    }

    // Headless screenshots must not capture a tooltip for wherever the
    // virtual display happens to park the mouse pointer.
    screenshotRun = juce::JUCEApplicationBase::getCommandLineParameterArray().contains ("--screenshot");
    if (! screenshotRun)
        tooltips = std::make_unique<juce::TooltipWindow> (this, 650);

    for (auto* child : std::initializer_list<juce::Component*> { &header, &routing, &boost, &analyzer, &rack, &levels, &loudness, &history })
        addAndMakeVisible (child);

    // ---- Wiring ----
    header.onSettingsRequested = [this] { openSettings(); };
    header.onViewToggleRequested = [this] { setView (view == View::Simple ? View::Advanced : View::Simple); };
    addChildComponent (simple);
    simple.onAdvancedRequested = [this] { setView (View::Advanced); };
    simple.onOutputSettingsRequested = [this] { openSettings (SettingsDialog::Page::Audio); };
    simple.onCorrectionRequested = [this] { openSettings (SettingsDialog::Page::Correction); };
    header.onExportRequested = [this] { openExport(); };
    header.onBlindTestRequested = [this] { openBlindTest(); };
    header.onRoutingRequested = [this] { setRoutingDrawerOpen (! routingDrawer); };
    // The rack's ears are matched with the header's estimator and switch (docs/11 E37).
    rack.setEstimatorProvider ([this] { return header.getLoudnessEstimator(); });
    header.onComparisonMatchedChanged = [this] (bool matched) { rack.getListenMatch().setEnabled (matched); };
    addChildComponent (deviceError);
    deviceError.onChooseOutput = [this] { openSettings (SettingsDialog::Page::Audio); };
    deviceError.onOpenSoundSettings = [this] { controller.getRouting().openSystemRoutingSettings(); };
    deviceError.refresh();
    addChildComponent (deviceBanner);
    deviceBanner.onDetailsRequested = [this] { openSettings (SettingsDialog::Page::Audio); }; // the full guidance is on the Audio page
    deviceBanner.refresh();
    addChildComponent (notices);
    notices.onVisibilityChanged = [this] { resized(); };
    if (const auto& recovery = controller.getSettings().getRecovery(); recovery.wasDamaged())
        notices.post (NoticeBar::recoveryNotice (recovery));
    takePresetNotices(); // warnings of the presets restored at start-up
    levels.onResetRequested = [this] { requestLoudnessReset(); };
    loudness.onResetRequested = [this] { requestLoudnessReset(); };
    loudness.hearingSource = [this] { return LoudnessPanel::hearingReadoutOf (controller); }; // docs/11 E32 (c): the dose row
    loudness.setTooltip ("Click the integrated loudness to reset it (also resets the true-peak hold).\n"
                         "IN>OUT: short-term loudness out minus in. LIM: share of about the last 10 s with the limiter more than 1 dB down. "
                         "PRE: the automatic preamp (off unless Automatic Preamp is on).\n"
                         "Harmonics: what the bass harmonics and the air exciter add on purpose (not counted against the distortion budget).");
    rack.onLayoutModeChanged = [this] { resized(); };
    rack.onEqBandSelected = [this] (int band) { analyzer.getEqEditor().selectBand (band); };
    analyzer.getEqEditor().onBandSelected = [this] (int band) { rack.setSelectedEqBand (band); };
    analyzer.onOptionsChanged = [this] (const AnalyzerPanel::Options&) { saveUiPreferences(); };
    analyzer.onPopOutRequested = [this] { openVisualiserWindow(); };
    analyzer.popOutBlockedReason = [this] { return controller.isTournamentActive() ? juce::String ("off in Tournament mode") : juce::String(); };

    feed.addSink ([this] (AnalyzerFeed::Stream stream, const float* samples, int n)
                  {
                      const bool post = stream == AnalyzerFeed::Stream::Post;
                      analyzer.getAnalyzer().push (post, samples, n);
                      if (post)
                          history.push (samples, n);
                      else
                          analyzer.getVisualisers().pushPre (samples, n);
                  });
    feed.setSideSink ([this] (const float* samples, int n) { analyzer.getAnalyzer().pushSide (samples, n); });
    // The visualisers get the post tap as aligned mid / side pairs (AnalyzerTaps::postStereo).
    feed.setStereoSink ([this] (const float* mid, const float* side, int n) { analyzer.getVisualisers().pushPost (mid, side, n); });

    stripSignature = currentStripSignature();
    loadUiPreferences();
    setView (controller.getSettings().getMainView(), false);
    simple.refreshDevice (deviceBanner.shouldShow());
    applyMode (controller.getMode());
    controller.addListener (this);

    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestamp) { frame (timestamp); });
    setSize (1280, 820);
}

MainComponent::~MainComponent()
{
    abx.reset(); // puts the bank back that played before a running blind test
    closeVisualiserWindow();
    // The settings and export windows talk to the controller: close them
    // while that exists (closing the export window aborts a running export).
    if (settingsWindow != nullptr)
        delete settingsWindow.getComponent();
    if (exportWindow != nullptr)
        delete exportWindow.getComponent();
    vblank.reset();
    controller.removeListener (this);
    setLookAndFeel (nullptr);
}

FlubLookAndFeel& MainComponent::lookAndFeel()
{
    if (ownLookAndFeel == nullptr)
    {
        if (auto* shared = dynamic_cast<FlubLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()))
            return *shared;
        // The application default changed to something else: use a private one.
        ownLookAndFeel = std::make_unique<FlubLookAndFeel>();
        ownLookAndFeel->setAccent (Theme::accentForMode (mode));
        setLookAndFeel (ownLookAndFeel.get());
    }
    return *ownLookAndFeel;
}

// =============================================================================
// Preferences (UI only; stored next to the application settings)
// =============================================================================
void MainComponent::loadUiPreferences()
{
    auto& props = controller.getSettings().getPropertiesFile();
    lookAndFeel().setMeterPalette (props.getIntValue (kPrefMeterPalette, 0) == 1 ? MeterPalette::ColourBlindSafe : MeterPalette::Standard);

    AnalyzerPanel::Options o;
    if (! AnalyzerPanel::Options::fromString (props.getValue (kPrefAnalyzer, o.toString()), o))
        o = {};
    analyzer.setOptions (o);
}

void MainComponent::saveUiPreferences()
{
    auto& props = controller.getSettings().getPropertiesFile();
    props.setValue (kPrefMeterPalette, lookAndFeel().getMeterPalette() == MeterPalette::ColourBlindSafe ? 1 : 0);
    props.setValue (kPrefAnalyzer, analyzer.getOptions().toString());
}

// =============================================================================
// Visualiser window (docs/06 §6.4.2)
// =============================================================================
void MainComponent::openVisualiserWindow (const juce::String& viewId)
{
    if (controller.isTournamentActive())
        return;
    if (visualiserWindow != nullptr)
    {
        if (viewId.isNotEmpty())
            visualiserWindow->setView (viewId);
        visualiserWindow->toFront (true);
        return;
    }
    vis::VisualiserWindow::State state;
    if (! vis::VisualiserWindow::State::fromString (controller.getSettings().getPropertiesFile().getValue (kPrefVisualiserWindow), state))
        state.view = analyzer.getMainVisualiser() != nullptr ? analyzer.getOptions().visualiser : juce::String (vis::VisualiserWindow::kDefaultView);
    if (viewId.isNotEmpty())
        state.view = viewId;

    visualiserWindow = std::make_unique<vis::VisualiserWindow> (state.view);
    auto& window = *visualiserWindow;
    window.setLookAndFeel (&lookAndFeel());
    // The window's own view is fed with the panel's views (the host's extra view).
    analyzer.getVisualisers().setExtra (window.getView());
    window.onViewChanged = [this] (vis::Visualiser* v) { analyzer.getVisualisers().setExtra (v); };
    window.onCloseRequested = [this]
    {
        // Deleted asynchronously: this runs inside the window's own button handler.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
                                         {
                                             if (safe != nullptr)
                                                 safe->closeVisualiserWindow();
                                         });
    };
    window.applyState (state);
    window.onStateChanged = [this] { saveVisualiserWindowState(); };
    window.setVisible (true);
    window.toFront (true);
    saveVisualiserWindowState();
}

void MainComponent::closeVisualiserWindow()
{
    if (visualiserWindow == nullptr)
        return;
    saveVisualiserWindowState();
    analyzer.getVisualisers().setExtra (nullptr);
    visualiserWindow->onStateChanged = nullptr;
    visualiserWindow->onViewChanged = nullptr;
    visualiserWindow->setLookAndFeel (nullptr);
    visualiserWindow.reset();
}

void MainComponent::saveVisualiserWindowState()
{
    if (visualiserWindow != nullptr)
        controller.getSettings().getPropertiesFile().setValue (kPrefVisualiserWindow, visualiserWindow->getState().toString());
}

// =============================================================================
// View (docs/11 E39)
// =============================================================================
std::vector<juce::Component*> MainComponent::getAdvancedOnlyComponents()
{
    return { &routing, &analyzer, &rack, &levels, &loudness, &history };
}

void MainComponent::openBlindTest()
{
    if (abx != nullptr)
        return;
    rack.releaseListening();
    abx = std::make_unique<AbxPanel> (controller, controller.getSelectedStrip(), header.getComparison().isEnabled());
    abx->onClose = [this]
    {
        // Closed asynchronously: onClose runs inside the panel's own handlers.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
                                         {
                                             if (safe != nullptr)
                                                 safe->closeBlindTest();
                                         });
    };
    addAndMakeVisible (*abx);
    abx->setBounds (getLocalBounds());
    abx->toFront (true);
    if (isShowing())
        abx->grabKeyboardFocus();
}

void MainComponent::closeBlindTest()
{
    abx.reset();
    header.refresh();
}

void MainComponent::setRoutingDrawerOpen (bool open)
{
    routingDrawer = open;
    resized();
}

void MainComponent::setView (View newView, bool persist)
{
    const bool focusInside = hasKeyboardFocus (true);
    view = newView;
    const bool isSimple = view == View::Simple;
    if (isSimple && rack.hasExpandedCard())
        rack.collapse();
    for (auto* c : getAdvancedOnlyComponents())
        c->setVisible (! isSimple);
    simple.setVisible (isSimple);
    boost.setLayout (isSimple ? BoostPanel::Layout::Simple : BoostPanel::Layout::Standard);
    header.setSimpleView (isSimple);
    if (! isSimple)
        rack.updateFromEngine(); // not refreshed while hidden
    if (persist)
        controller.getSettings().setMainView (view);
    resized();
    repaint();
    // The button that switched may be hidden now: keep the focus on the header's.
    if (focusInside && isShowing())
        header.getViewButton().grabKeyboardFocus();
}

// =============================================================================
// Frame update
// =============================================================================
void MainComponent::frame (double timestampSeconds)
{
    const double dt = lastFrameTime < 0.0 ? 1.0 / 60.0 : juce::jlimit (0.0, 0.1, timestampSeconds - lastFrameTime);
    lastFrameTime = timestampSeconds;

    // Re-fetched every frame: the chain objects are re-created on reconfiguration.
    const int strip = controller.getSelectedStrip();
    const auto generation = controller.getEngineGeneration();
    if (strip != lastStrip || generation != lastGeneration)
    {
        lastStrip = strip;
        lastGeneration = generation;
        resetAnalysis();
    }
    auto& chain = controller.getChain (strip);

    const auto currentMode = controller.getMode (strip);
    if (! modeKnown || currentMode != mode)
        applyMode (currentMode);

    snapshot.read (chain.meters());
    snapshot.masterGainReductionDb = controller.getMasterGainReductionDb();
    snapshot.active = controller.isStripActive (strip);
    snapshot.autoPreampDb = chain.getAutoPreampDb();
    snapshot.predictedBoostDb = chain.getPredictedBoostDb();

    const double sampleRate = controller.getHost().getSampleRate();
    analyzer.getAnalyzer().setSampleRate (sampleRate);
    analyzer.getEqEditor().setSampleRate (sampleRate);
    history.setSampleRate (sampleRate);

    // The taps are drained in both views; the Simple view skips the
    // analyser's FFTs and the rack (both hidden) and feeds its own meter.
    const bool isSimple = view == View::Simple;
    feed.pull (chain.taps());
    // The visualiser window reads the analyser too: it runs while the window is open.
    const bool analyse = ! isSimple || visualiserWindow != nullptr;
    if (analyse)
        analyzer.getAnalyzer().advance (dt);
    if (! isSimple)
    {
        analyzer.getEqEditor().refresh();
        analyzer.getEqEditor().setDynamicEqState (snapshot.dynEqGainDb, mode);
    }
    if (analyse)
        analyzer.advanceVisualisers (snapshot, dt, sampleRate);
    history.setLoudness (snapshot.shortTermLufs);
    history.advance();
    levels.update (snapshot, dt);
    loudness.update (snapshot, dt);
    boost.update (snapshot);
    if (isSimple)
        simple.update (snapshot, dt);
    routing.updateMeters (dt);
    header.animate (dt);

    ++frameCounter;
    if (frameCounter % 4 == 0 && ! isSimple)
        rack.updateFromEngine();
    if (frameCounter % 15 == 0)
    {
        header.updateStatus();
        if (visualiserWindow != nullptr && controller.isTournamentActive())
            closeVisualiserWindow(); // docs/11 E55: no extra windows while Tournament mode is on
    }
}

void MainComponent::resetAnalysis()
{
    auto& chain = controller.getChain (controller.getSelectedStrip());
    AnalyzerFeed::discard (chain.taps());
    analyzer.getAnalyzer().reset();
    analyzer.getVisualisers().reset();
    history.reset();
    levels.reset();
    loudness.reset();
    analyzer.getEqEditor().refresh (true);
}

void MainComponent::requestLoudnessReset()
{
    controller.getChain (controller.getSelectedStrip()).meters().resetLoudnessRequest.store (true, std::memory_order_release);
}

void MainComponent::applyMode (ModeValue newMode)
{
    mode = newMode;
    modeKnown = true;
    lookAndFeel().setAccent (Theme::accentForMode (mode));
    boost.setMode (mode);
    header.refresh();
    sendLookAndFeelChange();
    if (visualiserWindow != nullptr)
        visualiserWindow->sendLookAndFeelChange();
    repaint();
}

// =============================================================================
// Controller events
// =============================================================================
void MainComponent::engineControllerChanged (EngineController::Change change)
{
    using Change = EngineController::Change;
    switch (change)
    {
        case Change::Preset:
            header.refreshPresets();
            refreshDeviceBanner(); // the "Use <preset>" offer hides once it is loaded
            takePresetNotices();
            break;
        case Change::Engine:
            rack.releaseListening(); // the re-created chains start without auditions
            // Device restarts re-create the chains but usually keep the strips.
            if (const auto signature = currentStripSignature(); signature != stripSignature)
            {
                stripSignature = signature;
                header.rebuildStrips();
                routing.rebuildStrips();
            }
            resetAnalysis();
            header.refresh();
            header.updateStatus();
            refreshDeviceBanner();
            break;
        case Change::SelectedStrip:
            rack.releaseListening();
            header.refresh();
            routing.setSelectedStrip (controller.getSelectedStrip());
            resetAnalysis();
            refreshDeviceBanner(); // the advice follows the selected strip's mode
            break;
        case Change::MasterEnable:
        case Change::Parameters:
            header.refresh();
            refreshDeviceBanner(); // the suggested preset follows the mode
            break;
        case Change::Device:
        case Change::Settings:
            if (controller.isTournamentActive())
                closeVisualiserWindow(); // docs/11 E55
            header.updateStatus();
            routing.refreshRouting();
            refreshDeviceBanner();
            // The latency prompt goes once the profile is the suggested one.
            if (shownSuggestion.has_value() && controller.getLatencyProfile() == shownSuggestion->suggested)
            {
                shownSuggestion.reset();
                notices.dismiss (NoticeBar::kLatencyKey);
            }
            break;
        case Change::Routing:
            routing.refreshRouting();
            break;
    }
}

void MainComponent::refreshDeviceBanner()
{
    const bool errorChanged = deviceError.refresh();
    const bool adviceChanged = deviceBanner.refresh();
    simple.refreshDevice (deviceBanner.shouldShow());
    if (adviceChanged || errorChanged)
        resized();
}

void MainComponent::takePresetNotices()
{
    for (const auto& w : controller.takePresetWarnings())
        notices.post (NoticeBar::presetWarningsNotice (w));

    if (auto suggestion = controller.takeLatencySuggestion())
    {
        const auto suggested = suggestion->suggested;
        shownSuggestion = suggestion;
        notices.post (NoticeBar::latencyNotice (*suggestion, [this, suggested] { controller.setLatencyProfile (suggested); }));
    }
    else if (shownSuggestion.has_value() && controller.getCurrentPresetName() != shownSuggestion->presetName)
    {
        // Another preset was loaded since: the prompt no longer applies.
        shownSuggestion.reset();
        notices.dismiss (NoticeBar::kLatencyKey);
    }
}

juce::String MainComponent::currentStripSignature() const
{
    juce::String s;
    for (int i = 0; i < controller.getNumStrips(); ++i)
        s << controller.getStripName (i) << ':' << controller.getStripChannels (i) << ';';
    return s;
}

void MainComponent::openSettings (std::optional<SettingsDialog::Page> page)
{
    if (settingsWindow != nullptr)
    {
        if (auto* dialog = dynamic_cast<SettingsDialog*> (settingsWindow->getContentComponent()); dialog != nullptr && page.has_value())
            dialog->showPage (*page);
        settingsWindow->toFront (true);
        return;
    }
    settingsWindow = SettingsDialog::show (
        controller, this, hotkeyHooks,
        [safe = juce::Component::SafePointer<MainComponent> (this)] (MeterPalette palette)
        {
            if (safe == nullptr)
                return;
            safe->lookAndFeel().setMeterPalette (palette);
            safe->saveUiPreferences();
            safe->sendLookAndFeelChange(); // repaints; views that cache palette colours refresh them
        },
        lookAndFeel().getMeterPalette(), page.value_or (SettingsDialog::Page::Audio));
}

SettingsDialog* MainComponent::getSettingsDialog() const
{
    return settingsWindow != nullptr ? dynamic_cast<SettingsDialog*> (settingsWindow->getContentComponent()) : nullptr;
}

bool MainComponent::isSettingsPageShowing (SettingsDialog::Page page) const
{
    const auto* dialog = getSettingsDialog();
    return dialog != nullptr && dialog->isShowing() && dialog->getCurrentPage() == page;
}

bool MainComponent::announceHotkeyFailures (HotkeyManager& hotkeys)
{
    if (hotkeys.getFailureList().empty())
        notices.dismiss (NoticeBar::kHotkeysKey); // fixed (or switched off): the notice has nothing left to say
    const auto fresh = hotkeys.takeUnannouncedFailures();
    if (fresh.empty() || isSettingsPageShowing (SettingsDialog::Page::Hotkeys))
        return false;
    notices.post (NoticeBar::hotkeyNotice (fresh,
                                           [safe = juce::Component::SafePointer<MainComponent> (this)]
                                           {
                                               if (safe != nullptr)
                                                   safe->openSettingsPage (SettingsDialog::Page::Hotkeys);
                                           }));
    return true;
}

void MainComponent::openExport()
{
    if (exportWindow != nullptr)
    {
        exportWindow->toFront (true);
        return;
    }
    exportWindow = ExportDialog::show (controller, this);
}

void MainComponent::parentHierarchyChanged()
{
    // The window sets its own limits after adding this content: register the
    // design minimum once that has happened, so the UI scale can re-fit it.
    if (screenshotRun || findParentComponentOfClass<juce::ResizableWindow>() == nullptr)
        return;
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
                                     {
                                         if (safe == nullptr)
                                             return;
                                         if (auto* window = safe->findParentComponentOfClass<juce::ResizableWindow>())
                                             Theme::setMinimumWindowSize (*window, { MainWindow::kMinWidth, MainWindow::kMinHeight });
                                     });
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && view == View::Advanced && routingDrawer)
    {
        setRoutingDrawerOpen (false);
        return true;
    }
    if (key == juce::KeyPress::escapeKey && view == View::Advanced && rack.hasExpandedCard())
    {
        rack.collapse();
        return true;
    }
    return false;
}

// =============================================================================
// Layout
// =============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
}

void MainComponent::resized()
{
    if (abx != nullptr)
        abx->setBounds (getLocalBounds());
    auto r = getLocalBounds();
    header.setBounds (r.removeFromTop (56));
    r.reduce (12, 12);
    const int gap = 10;
    const int w = getWidth();
    const bool narrow = w < HeaderBar::kNarrowWidth, shortWindow = getHeight() < 700;

    if (deviceError.shouldShow())
    {
        deviceError.setBounds (r.removeFromTop (DeviceErrorBanner::kHeight));
        r.removeFromTop (gap);
    }
    if (deviceBanner.shouldShow())
    {
        deviceBanner.setBounds (r.removeFromTop (DeviceAdviceBanner::kHeight));
        r.removeFromTop (gap);
    }
    if (notices.shouldShow())
    {
        notices.setBounds (r.removeFromTop (DeviceAdviceBanner::kHeight));
        r.removeFromTop (gap);
    }

    if (view == View::Simple)
    {
        layoutSimple (r);
        return;
    }

    // Short windows: no waveform history (docs/11 E39).
    history.setVisible (! shortWindow);
    if (! shortWindow)
    {
        history.setBounds (r.removeFromBottom (juce::jlimit (76, 128, r.getHeight() / 8)));
        r.removeFromBottom (gap);
    }

    // Narrow windows: the routing panel is a drawer over the analyser.
    const auto content = r;
    routing.setVisible (! narrow || routingDrawer);
    if (! narrow)
    {
        routingDrawer = false;
        routing.setBounds (r.removeFromLeft (juce::jlimit (228, 300, juce::roundToInt (w * 0.17))));
        r.removeFromLeft (gap);
    }

    auto right = r.removeFromRight (narrow ? juce::jlimit (220, 252, juce::roundToInt (w * 0.23)) : juce::jlimit (252, 320, juce::roundToInt (w * 0.19)));
    r.removeFromRight (gap);
    // The loudness panel's content has a fixed height (~420 px with its
    // PROTECTION section): on tall windows the level meters take the spare
    // height instead of leaving the loudness panel half empty.
    constexpr int kLoudnessNeeds = 480;
    const int levelsH = juce::jlimit (196, 560, juce::jmax (juce::roundToInt (right.getHeight() * 0.36), right.getHeight() - kLoudnessNeeds - gap));
    levels.setBounds (right.removeFromTop (levelsH));
    right.removeFromTop (gap);
    loudness.setBounds (right);

    boost.setBounds (r.removeFromTop (juce::jlimit (150, 212, juce::roundToInt (r.getHeight() * 0.27))));
    r.removeFromTop (gap);

    // The module rack gets most of the space while a module is expanded.
    const int rackH = rack.hasExpandedCard() ? juce::roundToInt (r.getHeight() * 0.64)
                                             : juce::jlimit (150, 200, juce::roundToInt (r.getHeight() * 0.36));
    rack.setBounds (r.removeFromBottom (rackH));
    r.removeFromBottom (gap);
    analyzer.setBounds (r);

    if (narrow && routingDrawer)
    {
        routing.setBounds (content.withWidth (juce::jmin (280, content.getWidth())));
        routing.toFront (false);
    }
}

void MainComponent::layoutSimple (juce::Rectangle<int> r)
{
    // The Boost panel over the status row, at most 1180 px wide; spare
    // height is split above and below.
    const int gap = 10;
    r = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 1180), r.getHeight());
    const int statusH = juce::jlimit (200, 260, juce::roundToInt (r.getHeight() * 0.40));
    const int boostH = juce::jlimit (150, 380, r.getHeight() - statusH - gap);
    const int spare = r.getHeight() - statusH - boostH - gap;
    if (spare > 0)
        r.removeFromTop (spare / 2);
    boost.setBounds (r.removeFromTop (boostH));
    r.removeFromTop (gap);
    simple.setBounds (r.removeFromTop (statusH));
}
} // namespace flub::app::ui
