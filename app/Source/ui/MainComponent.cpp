#include "MainComponent.h"

#include "Theme.h"

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
constexpr const char* kPrefMeterPalette = "ui.meterPalette";
constexpr const char* kPrefAnalyzer = "ui.analyzer";
} // namespace

MainComponent::MainComponent (EngineController& c)
    : controller (c),
      header (c),
      deviceBanner (c),
      routing (c),
      boost (c),
      analyzer ([this] { return &controller.getSelectedParams(); }),
      rack (c)
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
    if (! juce::JUCEApplicationBase::getCommandLineParameterArray().contains ("--screenshot"))
        tooltips = std::make_unique<juce::TooltipWindow> (this, 650);

    for (auto* child : std::initializer_list<juce::Component*> { &header, &routing, &boost, &analyzer, &rack, &levels, &loudness, &history })
        addAndMakeVisible (child);

    // ---- Wiring ----
    header.onSettingsRequested = [this] { openSettings(); };
    addChildComponent (deviceBanner);
    deviceBanner.onDetailsRequested = [this] { openSettings(); };
    deviceBanner.refresh();
    levels.onResetRequested = [this] { requestLoudnessReset(); };
    loudness.onResetRequested = [this] { requestLoudnessReset(); };
    loudness.setTooltip ("Click the integrated loudness to reset it (also resets the true-peak hold)");
    rack.onLayoutModeChanged = [this] { resized(); };
    rack.onEqBandSelected = [this] (int band) { analyzer.getEqEditor().selectBand (band); };
    analyzer.getEqEditor().onBandSelected = [this] (int band) { rack.setSelectedEqBand (band); };
    analyzer.onOptionsChanged = [this] (const AnalyzerPanel::Options&) { saveUiPreferences(); };

    feed.addSink ([this] (AnalyzerFeed::Stream stream, const float* samples, int n)
                  {
                      const bool post = stream == AnalyzerFeed::Stream::Post;
                      analyzer.getAnalyzer().push (post, samples, n);
                      if (post)
                          history.push (samples, n);
                  });

    stripSignature = currentStripSignature();
    loadUiPreferences();
    applyMode (controller.getMode());
    controller.addListener (this);

    vblank = std::make_unique<juce::VBlankAttachment> (this, [this] (double timestamp) { frame (timestamp); });
    setSize (1280, 820);
}

MainComponent::~MainComponent()
{
    // The settings window talks to the controller: close it while that exists.
    if (settingsWindow != nullptr)
        delete settingsWindow.getComponent();
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
    const auto tokens = juce::StringArray::fromTokens (props.getValue (kPrefAnalyzer, "1,1,1,1,12"), ",", {});
    if (tokens.size() == 5)
    {
        o.showPre = tokens[0] != "0";
        o.showPost = tokens[1] != "0";
        o.tilt = tokens[2] != "0";
        o.peakHold = tokens[3] != "0";
        o.eqRangeDb = static_cast<float> (juce::jlimit (6, 24, tokens[4].getIntValue()));
    }
    analyzer.setOptions (o);
}

void MainComponent::saveUiPreferences()
{
    auto& props = controller.getSettings().getPropertiesFile();
    props.setValue (kPrefMeterPalette, lookAndFeel().getMeterPalette() == MeterPalette::ColourBlindSafe ? 1 : 0);
    const auto& o = analyzer.getOptions();
    props.setValue (kPrefAnalyzer, juce::String (o.showPre ? 1 : 0) + "," + juce::String (o.showPost ? 1 : 0) + "," + juce::String (o.tilt ? 1 : 0) + ","
                                       + juce::String (o.peakHold ? 1 : 0) + "," + juce::String (juce::roundToInt (o.eqRangeDb)));
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

    const double sampleRate = controller.getHost().getSampleRate();
    analyzer.getAnalyzer().setSampleRate (sampleRate);
    analyzer.getEqEditor().setSampleRate (sampleRate);
    history.setSampleRate (sampleRate);

    feed.pull (chain.taps());
    analyzer.getAnalyzer().advance (dt);
    analyzer.getEqEditor().refresh();
    analyzer.getEqEditor().setDynamicEqState (snapshot.dynEqGainDb, mode);
    history.setLoudness (snapshot.shortTermLufs);
    history.advance();
    levels.update (snapshot, dt);
    loudness.update (snapshot, dt);
    boost.setGovernorScale (snapshot.governorScale);
    routing.updateMeters (dt);
    header.animate (dt);

    ++frameCounter;
    if (frameCounter % 4 == 0)
        rack.updateFromEngine();
    if (frameCounter % 15 == 0)
        header.updateStatus();
}

void MainComponent::resetAnalysis()
{
    auto& chain = controller.getChain (controller.getSelectedStrip());
    AnalyzerFeed::discard (chain.taps());
    analyzer.getAnalyzer().reset();
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
            break;
        case Change::Engine:
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
            header.refresh();
            routing.setSelectedStrip (controller.getSelectedStrip());
            resetAnalysis();
            break;
        case Change::MasterEnable:
        case Change::Parameters:
            header.refresh();
            refreshDeviceBanner(); // the suggested preset follows the mode
            break;
        case Change::Device:
        case Change::Settings:
            header.updateStatus();
            routing.refreshRouting();
            refreshDeviceBanner();
            break;
        case Change::Routing:
            routing.refreshRouting();
            break;
    }
}

void MainComponent::refreshDeviceBanner()
{
    if (deviceBanner.refresh())
        resized();
}

juce::String MainComponent::currentStripSignature() const
{
    juce::String s;
    for (int i = 0; i < controller.getNumStrips(); ++i)
        s << controller.getStripName (i) << ':' << controller.getStripChannels (i) << ';';
    return s;
}

void MainComponent::openSettings()
{
    if (settingsWindow != nullptr)
    {
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
            safe->repaint();
        },
        lookAndFeel().getMeterPalette());
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && rack.hasExpandedCard())
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
    auto r = getLocalBounds();
    header.setBounds (r.removeFromTop (56));
    r.reduce (12, 12);
    const int gap = 10;
    const int w = getWidth();

    if (deviceBanner.shouldShow())
    {
        deviceBanner.setBounds (r.removeFromTop (DeviceAdviceBanner::kHeight));
        r.removeFromTop (gap);
    }

    history.setBounds (r.removeFromBottom (juce::jlimit (76, 128, r.getHeight() / 8)));
    r.removeFromBottom (gap);

    routing.setBounds (r.removeFromLeft (juce::jlimit (228, 300, juce::roundToInt (w * 0.17))));
    r.removeFromLeft (gap);

    auto right = r.removeFromRight (juce::jlimit (252, 320, juce::roundToInt (w * 0.19)));
    r.removeFromRight (gap);
    // The loudness panel's content has a fixed height (~340 px): on tall
    // windows the level meters take the spare height instead of leaving the
    // loudness panel half empty.
    constexpr int kLoudnessNeeds = 400;
    const int levelsH = juce::jlimit (196, 560, juce::jmax (juce::roundToInt (right.getHeight() * 0.40), right.getHeight() - kLoudnessNeeds - gap));
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
}
} // namespace flub::app::ui
