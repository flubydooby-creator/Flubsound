// App-level tests: CPU-overload watchdog, device xruns and per-app capture
// FIFO statistics in the UI (R1.5).
//
// * OverloadWatchdog is the pure decision logic (no JUCE, no threads, no
//   clock): it is fed load / glitch-counter samples directly, one per poll.
// * EngineController::updateOverloadWatchdog is the poll its 2 Hz timer makes
//   with getStatus(); the tests call it with chosen statuses (no timers, no
//   waiting) and check the Change::Device notification and the header text.
// * Device xruns: a fake juce::AudioIODeviceType is installed in the host's
//   own AudioDeviceManager and opened through AudioEngineHost::openDevice, so
//   the count travels the real path getStatus() -> HeaderBar::formatCpuReadout.
// * Capture statistics: a fake ProcessLoopbackCapture feeds a strip through
//   its DriftCompensatedFifo while the device callback is driven block by
//   block (a capture gap = underrun, a device stall = overflow), then
//   EngineController::getCaptureStreams -> SettingsDialog::describeCaptureStreams.
//
// Set FLUB_APP_TEST_SNAPSHOT_DIR to also write PNGs of the header and of
// Settings > Processing in these states (visual check only, nothing asserted).
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "engine/OverloadWatchdog.h"
#include "ui/FlubLookAndFeel.h"
#include "ui/HeaderBar.h"
#include "ui/SettingsDialog.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

using namespace flub::app;
using Event = OverloadWatchdog::Event;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

OverloadWatchdog::Sample sample (double load, int64_t glitches = 0)
{
    OverloadWatchdog::Sample s;
    s.running = true;
    s.load = load;
    s.glitchCount = glitches;
    return s;
}

/** Feeds `polls` identical samples; returns how many transitions they caused. */
int feed (OverloadWatchdog& w, const OverloadWatchdog::Sample& s, int polls)
{
    int transitions = 0;
    for (int i = 0; i < polls; ++i)
        transitions += w.update (s) != Event::None ? 1 : 0;
    return transitions;
}

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    return o;
}

/** An open device of the fake type below; its xrun count is set by the test. */
class XrunDevice final : public juce::AudioIODevice
{
public:
    explicit XrunDevice (const std::atomic<int>& count) : juce::AudioIODevice ("Fake Xrun Device", "Fake xrun type"), xruns (count) {}

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
    juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double, int) override
    {
        activeIns = ins;
        activeOuts = outs;
        opened = true;
        return {};
    }
    void close() override { opened = false; }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* cb) override
    {
        // As JUCE's backends do; the test never runs audio through it.
        callback = cb;
        if (callback != nullptr)
            callback->audioDeviceAboutToStart (this);
    }
    void stop() override
    {
        if (auto* cb = std::exchange (callback, nullptr))
            cb->audioDeviceStopped();
    }
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return kRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOuts; }
    juce::BigInteger getActiveInputChannels() const override { return activeIns; }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }
    int getXRunCount() const noexcept override { return xruns.load(); }

private:
    const std::atomic<int>& xruns;
    juce::AudioIODeviceCallback* callback = nullptr;
    juce::BigInteger activeIns, activeOuts;
    bool opened = false;
};

class XrunDeviceType final : public juce::AudioIODeviceType
{
public:
    explicit XrunDeviceType (const std::atomic<int>& count) : juce::AudioIODeviceType ("Fake xrun type"), xruns (count) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool) const override { return { "Fake Xrun Device" }; }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice* device, bool) const override { return device != nullptr ? 0 : -1; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice (const juce::String&, const juce::String&) override { return new XrunDevice (xruns); }

private:
    const std::atomic<int>& xruns;
};

/** A device for AudioEngineHost::audioDeviceAboutToStart only (not in the
    device manager), as in test_app_realtime.cpp. */
class PlainDevice final : public juce::AudioIODevice
{
public:
    PlainDevice() : juce::AudioIODevice ("Fake Output", "Fake") {}

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return kRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger (0x3); }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return 0; }
};

/** Stands in for a platform::ProcessLoopbackCapture; the test delivers the
    packets (a stereo sine) itself. */
class FakeCapture final : public flub::platform::ProcessLoopbackCapture
{
public:
    bool isSupported() const override { return true; }

    bool start (uint32_t, bool, double, int numChannels, FrameCallback frameCallback, std::string&) override
    {
        channels = numChannels;
        callback = std::move (frameCallback);
        running = true;
        return true;
    }

    void stop() override { running = false; }
    bool isRunning() const override { return running; }

    void deliver (int numFrames)
    {
        packet.resize (static_cast<size_t> (numFrames * channels));
        for (int i = 0; i < numFrames; ++i, ++frame)
            for (int c = 0; c < channels; ++c)
                packet[static_cast<size_t> (i * channels + c)] =
                    0.25f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * static_cast<double> (frame) / kRate));
        if (running && callback != nullptr)
            callback (packet.data(), numFrames, channels);
    }

private:
    FrameCallback callback;
    std::vector<float> packet;
    int channels = 2;
    int64_t frame = 0;
    bool running = false;
};

struct ChangeCounter final : EngineController::Listener
{
    void engineControllerChanged (EngineController::Change change) override
    {
        if (change == EngineController::Change::Device)
            ++device;
    }
    int device = 0;
};

EngineStatus runningStatus (double load, int xruns, int glitches)
{
    EngineStatus st;
    st.deviceOpen = true;
    st.running = true;
    st.deviceName = "Fake Xrun Device";
    st.deviceTypeName = "Fake xrun type";
    st.cpuLoad = load;
    st.xruns = xruns;
    st.glitches = glitches;
    return st;
}

/** Writes `component` to $FLUB_APP_TEST_SNAPSHOT_DIR/<name>.png if that is set. */
void snapshotIfRequested (juce::Component& component, const char* name)
{
    const auto* dir = std::getenv ("FLUB_APP_TEST_SNAPSHOT_DIR");
    if (dir == nullptr || *dir == 0)
        return;
    const auto file = juce::File (juce::String::fromUTF8 (dir)).getChildFile (juce::String (name) + ".png");
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f);
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    if (! out.openedOk() || ! png.writeImageToStream (image, out))
        std::cerr << "    could not write " << file.getFullPathName() << "\n";
}

/** Renders the header and Settings > Processing (only with FLUB_APP_TEST_SNAPSHOT_DIR). */
void snapshotUi (EngineController& controller, const char* prefix)
{
    if (std::getenv ("FLUB_APP_TEST_SNAPSHOT_DIR") == nullptr)
        return;
    ui::FlubLookAndFeel lnf;
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
    {
        ui::HeaderBar header (controller);
        header.setSize (1440, 56);
        header.updateStatus();
        snapshotIfRequested (header, (juce::String (prefix) + "-header").toRawUTF8());
        header.setSize (1100, 56);
        header.updateStatus();
        snapshotIfRequested (header, (juce::String (prefix) + "-header-compact").toRawUTF8());

        ui::HotkeyHooks hooks;
        hooks.isSupported = [] { return false; };
        hooks.getFailures = [] { return juce::StringArray(); };
        hooks.reRegister = [] {};
        ui::SettingsDialog settings (controller, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
        settings.showPage (ui::SettingsDialog::Page::Processing);
        settings.setSize (ui::SettingsDialog::kMinWidth, ui::SettingsDialog::kMinHeight);
        snapshotIfRequested (settings, (juce::String (prefix) + "-settings-processing-min").toRawUTF8());
        settings.setSize (780, 700);
        snapshotIfRequested (settings, (juce::String (prefix) + "-settings-processing").toRawUTF8());
    }
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}
} // namespace

// =============================================================================
// OverloadWatchdog (pure)
// =============================================================================
TEST_CASE ("App: OverloadWatchdog enters after 4 polls at >= 90 % load and recovers after 10 calm polls")
{
    OverloadWatchdog w;
    CHECK (! w.isOverloaded());

    // Three hot polls are not yet sustained.
    CHECK (feed (w, sample (0.95), 3) == 0);
    CHECK (! w.isOverloaded());
    CHECK (w.getState().hotStreak == 3);

    // The fourth is.
    CHECK (w.update (sample (0.97)) == Event::OverloadStarted);
    CHECK (w.isOverloaded());
    CHECK (w.getState().episodes == 1);
    CHECK_NEAR (w.getState().peakLoad, 0.97, 1e-9);

    // Still hot: stays overloaded, no repeated event; the peak follows.
    CHECK (feed (w, sample (0.99), 5) == 0);
    CHECK (w.isOverloaded());
    CHECK_NEAR (w.getState().peakLoad, 0.99, 1e-9);

    // Nine calm polls are not enough ...
    CHECK (feed (w, sample (0.5), 9) == 0);
    CHECK (w.isOverloaded());
    // ... the tenth clears it.
    CHECK (w.update (sample (0.5)) == Event::OverloadCleared);
    CHECK (! w.isOverloaded());
    CHECK (w.getState().episodes == 1); // the session count survives recovery

    // A second episode is counted as such.
    CHECK (feed (w, sample (0.93), 4) == 1);
    CHECK (w.getState().episodes == 2);

    // reset() forgets everything.
    w.reset();
    CHECK (! w.isOverloaded());
    CHECK (w.getState().episodes == 0);
}

TEST_CASE ("App: OverloadWatchdog hysteresis: a load hovering around the thresholds does not flap")
{
    OverloadWatchdog w;

    // Hovering around the entry threshold: never 4 hot polls in a row.
    int transitions = 0;
    for (int i = 0; i < 200; ++i)
        transitions += w.update (sample (i % 4 == 3 ? 0.85 : 0.95)) != Event::None ? 1 : 0;
    CHECK (transitions == 0);
    CHECK (! w.isOverloaded());

    // Once overloaded, a load between the exit (75 %) and entry (90 %)
    // thresholds holds the state; so do calm stretches shorter than 10 polls.
    CHECK (feed (w, sample (0.95), 4) == 1);
    transitions = 0;
    for (int i = 0; i < 200; ++i)
        transitions += w.update (sample (i % 2 == 0 ? 0.8 : 0.88)) != Event::None ? 1 : 0;
    for (int i = 0; i < 200; ++i)
        transitions += w.update (sample (i % 9 == 8 ? 0.8 : 0.3)) != Event::None ? 1 : 0; // a warm poll every 9th
    CHECK (transitions == 0);
    CHECK (w.isOverloaded());
    CHECK (w.getState().episodes == 1);

    // Exactly at the exit threshold is not calm; just below it is.
    CHECK (feed (w, sample (0.75), 20) == 0);
    CHECK (w.isOverloaded());
    CHECK (feed (w, sample (0.7499), 10) == 1);
    CHECK (! w.isOverloaded());

    // Out-of-range loads are clamped, not trusted.
    CHECK (feed (w, sample (-1.0), 5) == 0);
    CHECK (w.getState().lastLoad == 0.0);
}

TEST_CASE ("App: OverloadWatchdog counts xrun bursts, re-bases a restarted counter and clears when the device stops")
{
    OverloadWatchdog w;

    // A lone glitch now and then (low load) is not an overload.
    int64_t count = 0;
    for (int i = 0; i < 100; ++i)
    {
        if (i % 12 == 0)
            ++count;
        CHECK (w.update (sample (0.3, count)) == Event::None);
    }
    CHECK (! w.isOverloaded());
    CHECK (w.getState().glitches == 8); // the first sample only sets the baseline
    CHECK (feed (w, sample (0.3, count), 10) == 0);

    // Three within 10 polls is, even at a low load.
    count += 1;
    CHECK (w.update (sample (0.3, count)) == Event::None);
    count += 2;
    CHECK (w.update (sample (0.3, count)) == Event::OverloadStarted);
    CHECK (w.getState().episodeGlitches == 3);

    // A glitch resets the calm streak, so recovery needs 10 glitch-free polls.
    CHECK (feed (w, sample (0.3, count), 8) == 0);
    count += 1;
    CHECK (w.update (sample (0.3, count)) == Event::None);
    CHECK (feed (w, sample (0.3, count), 9) == 0);
    CHECK (w.isOverloaded());
    CHECK (w.getState().episodeGlitches == 4);
    CHECK (w.update (sample (0.3, count)) == Event::OverloadCleared);

    // The glitches that started the episode do not immediately start another.
    CHECK (w.update (sample (0.3, count)) == Event::None);
    CHECK (! w.isOverloaded());

    // The device restarted and its counter began again at 1: one new glitch,
    // not a negative or a huge jump.
    const auto before = w.getState().glitches;
    CHECK (w.update (sample (0.3, 1)) == Event::None);
    CHECK (w.getState().glitches == before + 1);

    // A counter that is not reported (-1) contributes nothing.
    CHECK (feed (w, sample (0.3, -1), 20) == 0);
    CHECK (w.getState().glitches == before + 1);

    // No running device clears an overload and re-bases the counter.
    CHECK (feed (w, sample (0.95, 1), 4) == 1);
    OverloadWatchdog::Sample stopped;
    CHECK (w.update (stopped) == Event::OverloadCleared);
    CHECK (w.update (stopped) == Event::None);
    CHECK (w.update (sample (0.2, 5000)) == Event::None); // new baseline, not 4900 glitches
    CHECK (w.getState().glitches == before + 1);
}

// =============================================================================
// EngineController + header / settings text
// =============================================================================
TEST_CASE ("App: EngineController overload watchdog notifies Change::Device and the header readout turns into OVERLOAD")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    ChangeCounter changes;
    controller.addListener (&changes);

    // Offline: nothing to watch, and the readout says so.
    const EngineStatus offline;
    controller.updateOverloadWatchdog (offline);
    CHECK (changes.device == 0);
    auto readout = ui::HeaderBar::formatCpuReadout (offline, controller.getOverloadState());
    CHECK (readout.caption == "DEVICE");
    CHECK (readout.value == "offline");
    CHECK (ui::SettingsDialog::describeCpuLine (offline, controller.getOverloadState()) == "CPU  -  no audio device open");

    // Normal load, a device without its own xrun count (-1).
    auto status = runningStatus (0.42, -1, 0);
    controller.updateOverloadWatchdog (status);
    readout = ui::HeaderBar::formatCpuReadout (status, controller.getOverloadState());
    CHECK (readout.caption == "CPU");
    CHECK (readout.value == "42%");
    CHECK (! readout.warn);
    CHECK (! readout.overload);
    CHECK (ui::SettingsDialog::describeCpuLine (status, controller.getOverloadState()) == "CPU 42 %  -  0 overloads this session");

    // Sustained 96 %: amber from the first poll, OVERLOAD from the fourth.
    status = runningStatus (0.96, -1, 0);
    for (int i = 0; i < 3; ++i)
        controller.updateOverloadWatchdog (status);
    readout = ui::HeaderBar::formatCpuReadout (status, controller.getOverloadState());
    CHECK (readout.warn);
    CHECK (! readout.overload);
    CHECK (changes.device == 0);

    controller.updateOverloadWatchdog (status);
    CHECK (changes.device == 1);
    CHECK (controller.getOverloadState().overloaded);
    readout = ui::HeaderBar::formatCpuReadout (status, controller.getOverloadState());
    CHECK (readout.overload);
    CHECK (readout.caption == "OVERLOAD");
    CHECK (readout.value == "96%");
    const auto tip = ui::HeaderBar::describeCpu (status, controller.getOverloadState());
    CHECK (tip.contains ("Overload:"));
    CHECK (tip.contains ("Low Latency profile"));
    CHECK (tip.contains ("peak 96 %"));
    CHECK (ui::SettingsDialog::describeCpuLine (status, controller.getOverloadState()) == "CPU 96 %  -  OVERLOAD now (peak 96 %)");
    snapshotUi (controller, "overload");

    // Recovery: ten calm polls, one more notification; the count remains.
    status = runningStatus (0.35, -1, 0);
    for (int i = 0; i < 10; ++i)
        controller.updateOverloadWatchdog (status);
    CHECK (changes.device == 2);
    CHECK (! controller.getOverloadState().overloaded);
    readout = ui::HeaderBar::formatCpuReadout (status, controller.getOverloadState());
    CHECK (readout.caption == "CPU");
    CHECK (! readout.overload);
    CHECK (ui::HeaderBar::describeCpu (status, controller.getOverloadState()).contains ("1 overload this session (the last peaked at 96 %)"));
    CHECK (ui::SettingsDialog::describeCpuLine (status, controller.getOverloadState()) == "CPU 35 %  -  1 overload this session");

    // The device going away also ends an overload.
    for (int i = 0; i < 4; ++i)
        controller.updateOverloadWatchdog (runningStatus (0.99, -1, 0));
    CHECK (changes.device == 3);
    controller.updateOverloadWatchdog (offline);
    CHECK (changes.device == 4);
    CHECK (! controller.getOverloadState().overloaded);
    CHECK (controller.getOverloadState().episodes == 2);

    controller.removeListener (&changes);
}

TEST_CASE ("App: device xruns travel AudioEngineHost::getStatus to the header readout and the overload watchdog")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& host = controller.getHost();

    std::atomic<int> xruns { -1 };
    host.getDeviceManager().addAudioDeviceType (std::make_unique<XrunDeviceType> (xruns));
    const auto error = host.openDevice (nullptr, 2, 2);
    REQUIRE (error.isEmpty());
    REQUIRE (host.getDeviceManager().getCurrentAudioDevice() != nullptr);

    // A device that does not report xruns: the readout shows the load only.
    auto status = controller.getStatus();
    REQUIRE (status.deviceOpen);
    CHECK (status.running);
    CHECK (status.deviceName == "Fake Xrun Device");
    CHECK (status.xruns == -1);
    CHECK (status.glitches == 0);
    auto readout = ui::HeaderBar::formatCpuReadout (status, controller.getOverloadState());
    CHECK (readout.caption == "CPU");
    CHECK (! readout.value.contains ("xr"));
    CHECK (! ui::HeaderBar::describeCpu (status, controller.getOverloadState()).contains ("xrun"));
    controller.updateOverloadWatchdog (status);

    // It reports 3: shown next to the CPU %, in the tooltip and in Settings.
    xruns = 3;
    status = controller.getStatus();
    CHECK (status.xruns == 3);
    CHECK (status.glitches >= 3); // + callbacks that overran their period (none ran here)
    readout = ui::HeaderBar::formatCpuReadout (status, controller.getOverloadState());
    const auto cpuPercent = juce::String (juce::roundToInt (status.cpuLoad * 100.0)) + "%";
    CHECK (readout.value == cpuPercent + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + "3 xr");
    CHECK (ui::HeaderBar::describeCpu (status, controller.getOverloadState()).contains ("3 xruns reported by the device"));
    CHECK (ui::SettingsDialog::describeCpuLine (status, controller.getOverloadState()).contains ("  -  3 xruns  -  "));

    // The watchdog sees the same counter: +3 since the -1 poll (3 glitches in
    // one poll) is a burst and starts an overload even at a low load.
    CHECK (! controller.getOverloadState().overloaded);
    controller.updateOverloadWatchdog (status);
    CHECK (controller.getOverloadState().overloaded);
    CHECK (controller.getOverloadState().glitches >= 3);
    snapshotUi (controller, "xruns");

    xruns = 1;
    status = controller.getStatus();
    CHECK (ui::HeaderBar::describeCpu (status, controller.getOverloadState()).contains ("1 xrun reported"));

    host.closeDevice();
    CHECK (! controller.getStatus().deviceOpen);
    controller.updateOverloadWatchdog (controller.getStatus());
    CHECK (! controller.getOverloadState().overloaded);
}

TEST_CASE ("App: capture FIFO stats text formats fill, drift, underruns, overflows and dropped frames")
{
    EngineController::CaptureStream discord;
    discord.appName = "Discord";
    discord.stripName = "Chat";
    discord.info.id = 0;
    discord.info.strip = 2;
    discord.info.processId = 1234;
    discord.info.running = true;
    discord.info.stats.underruns = 1;
    discord.info.stats.fillMs = 21.34f;
    discord.info.stats.targetMs = 20.0f;
    discord.info.stats.correctionPpm = 41.6f;
    discord.info.stats.streaming = true;

    EngineController::CaptureStream unknown;
    unknown.stripName = "Music";
    unknown.info.processId = 4242;
    unknown.info.running = true;
    unknown.info.stats.underruns = 2;
    unknown.info.stats.overflows = 1;
    unknown.info.stats.droppedFrames = 7200;
    unknown.info.stats.targetMs = 10.7f;
    unknown.info.stats.correctionPpm = -3.2f;
    unknown.info.stats.streaming = false;

    auto stopped = discord;
    stopped.info.running = false;
    stopped.info.stats.underruns = 0;
    stopped.info.stats.correctionPpm = 0.0f;

    CHECK (ui::SettingsDialog::describeCaptureStreams ({}).isEmpty());
    CHECK (ui::SettingsDialog::describeCaptureStreams ({ discord })
           == "Discord (Chat): fill 21.3 / 20.0 ms, drift +42 ppm  -  1 underrun, 0 overflows");
    CHECK (ui::SettingsDialog::describeCaptureStreams ({ discord, unknown, stopped })
           == "Discord (Chat): fill 21.3 / 20.0 ms, drift +42 ppm  -  1 underrun, 0 overflows\n"
              "Process 4242 (Music): priming  -  fill 0.0 / 10.7 ms, drift -3 ppm  -  2 underruns, 1 overflow, 7200 frames dropped\n"
              "Discord (Chat): stopped  -  fill 21.3 / 20.0 ms, drift 0 ppm  -  0 underruns, 0 overflows");
}

TEST_CASE ("App: per-app capture FIFO stats reach EngineController::getCaptureStreams and the Settings text (underrun, overflow)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& host = controller.getHost();

    FakeCapture* capture = nullptr;
    host.setCaptureFactory ([&capture]
                            {
                                auto c = std::make_unique<FakeCapture>();
                                capture = c.get();
                                return c;
                            });

    PlainDevice device;
    host.audioDeviceAboutToStart (&device); // message thread: configures synchronously
    REQUIRE (host.getBlockSize() == kBlock);

    CHECK (controller.getCaptureStreams().empty());

    juce::String error;
    const int captureId = host.startProcessCapture (1, 4242, error); // Music strip
    REQUIRE (captureId >= 0);
    REQUIRE (capture != nullptr);

    // The device thread: 10 ms packets paced by the device clock, a 40-block
    // capture gap (underrun, re-prime) and a 300 ms device stall (the capture
    // keeps delivering: overflow, the oldest frames dropped).
    std::thread deviceThread ([&]
    {
        std::array<std::vector<float>, 2> outputData { std::vector<float> (kBlock), std::vector<float> (kBlock) };
        std::array<float*, 2> outputs { outputData[0].data(), outputData[1].data() };
        const juce::AudioIODeviceCallbackContext context {};
        int64_t sample = 0, captured = 0;
        for (int b = 0; b < 700; ++b)
        {
            const bool gap = b >= 200 && b < 240;
            if (b == 450)
            {
                // The device stalls for 300 ms; the capture keeps delivering.
                for (int i = 0; i < 30; ++i)
                    capture->deliver (480);
                captured += 30 * 480;
                sample += 30 * 480;
            }
            while (! gap && captured < sample + 2 * kBlock)
            {
                capture->deliver (480);
                captured += 480;
            }
            if (gap)
                captured = sample;
            host.audioDeviceIOCallbackWithContext (nullptr, 0, outputs.data(), 2, kBlock, context);
            sample += kBlock;
        }
    });
    deviceThread.join();

    const auto captures = host.getCaptures();
    const auto streams = controller.getCaptureStreams();
    REQUIRE (captures.size() == 1);
    REQUIRE (streams.size() == 1);

    // The controller hands the host's numbers through unchanged.
    const auto& st = streams[0].info.stats;
    CHECK (streams[0].info.id == captureId);
    CHECK (streams[0].info.processId == 4242u);
    CHECK (streams[0].info.running);
    CHECK (streams[0].stripName == "Music");
    CHECK (streams[0].appName.isEmpty()); // no AppRouting session for this pid
    CHECK (st.underruns == captures[0].stats.underruns);
    CHECK (st.overflows == captures[0].stats.overflows);
    CHECK (st.droppedFrames == captures[0].stats.droppedFrames);
    CHECK (st.fillMs == captures[0].stats.fillMs);
    CHECK (st.correctionPpm == captures[0].stats.correctionPpm);

    // Both paths really ran.
    CHECK (st.underruns >= 1);
    CHECK (st.overflows >= 1);
    CHECK (st.droppedFrames > 0);
    CHECK (st.streaming);
    CHECK (st.targetMs > 0.0f);
    std::cerr << "    " << ui::SettingsDialog::describeCaptureStreams (streams) << "\n";

    // ... and are what Settings > Processing (and the header tooltip) show.
    const auto text = ui::SettingsDialog::describeCaptureStreams (streams);
    const auto count = [] (uint64_t n) { return juce::String (static_cast<juce::int64> (n)); };
    CHECK (text.startsWith ("Process 4242 (Music): fill " + juce::String (st.fillMs, 1) + " / " + juce::String (st.targetMs, 1) + " ms, drift "));
    CHECK (text.contains (juce::String (juce::roundToInt (st.correctionPpm)) + " ppm"));
    CHECK (text.contains (count (st.underruns) + (st.underruns == 1 ? " underrun," : " underruns,")));
    CHECK (text.contains (count (st.overflows) + (st.overflows == 1 ? " overflow," : " overflows,")));
    CHECK (text.endsWith (count (st.droppedFrames) + " frames dropped"));
    CHECK (! text.contains ("\n"));

    snapshotUi (controller, "capture");

    host.audioDeviceStopped();
    host.stopAllCaptures();
    CHECK (controller.getCaptureStreams().empty());
}
