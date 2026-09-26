#include "ScreenshotDriver.h"

#include <algorithm>

namespace flub::app
{
using namespace flub::param;

bool ScreenshotDriver::parseCommandLine (const juce::StringArray& args, Options& options, juce::String& error)
{
    const int index = args.indexOf ("--screenshot");
    if (index < 0)
        return false;

    if (index + 1 >= args.size() || args[index + 1].startsWith ("--"))
    {
        error = "--screenshot needs an output file, e.g. --screenshot out.png";
        return false;
    }
    options.output = juce::File::getCurrentWorkingDirectory().getChildFile (args[index + 1].unquoted());

    const int modeIndex = args.indexOf ("--mode");
    if (modeIndex >= 0)
    {
        const auto mode = args[modeIndex + 1].toLowerCase();
        if (mode != "music" && mode != "gaming")
        {
            error = "--mode must be 'music' or 'gaming'";
            return false;
        }
        options.gamingMode = mode == "gaming";
    }

    const int sizeIndex = args.indexOf ("--size");
    if (sizeIndex >= 0)
    {
        const auto size = args[sizeIndex + 1].toLowerCase();
        const int w = size.upToFirstOccurrenceOf ("x", false, false).getIntValue();
        const int h = size.fromFirstOccurrenceOf ("x", false, false).getIntValue();
        if (w < 64 || h < 64 || w > 8192 || h > 8192)
        {
            error = "--size must look like 1280x820";
            return false;
        }
        options.width = w;
        options.height = h;
    }

    const int secondsIndex = args.indexOf ("--seconds");
    if (secondsIndex >= 0)
        options.seconds = std::clamp (args[secondsIndex + 1].getDoubleValue(), 0.2, 60.0);

    const int scaleIndex = args.indexOf ("--scale");
    if (scaleIndex >= 0)
        options.scale = std::clamp (args[scaleIndex + 1].getFloatValue(), 0.5f, 4.0f);

    const int deviceIndex = args.indexOf ("--device");
    if (deviceIndex >= 0)
    {
        if (deviceIndex + 1 >= args.size() || args[deviceIndex + 1].startsWith ("--"))
        {
            error = "--device needs an output device name, e.g. --device \"Headphones (Stealth 700 Gen 2 MAX)\"";
            return false;
        }
        options.simulatedDevice = args[deviceIndex + 1].unquoted();
    }

    return true;
}

ScreenshotDriver::ScreenshotDriver (EngineController& c, juce::Component& t, Options o, Completion done)
    : controller (c), target (t), options (std::move (o)), onFinished (std::move (done))
{
}

ScreenshotDriver::~ScreenshotDriver()
{
    stopTimer();
}

void ScreenshotDriver::setUpScene()
{
    const int numStrips = controller.getNumStrips();
    int gameStrip = controller.findStrip ("Game");
    for (int i = 0; gameStrip < 0 && i < numStrips; ++i)
        if (controller.getStripChannels (i) >= 6)
            gameStrip = i;
    if (gameStrip < 0)
        gameStrip = 0;

    int musicStrip = controller.findStrip ("Music");
    for (int i = 0; musicStrip < 0 && i < numStrips; ++i)
        if (i != gameStrip)
            musicStrip = i;
    if (musicStrip < 0)
        musicStrip = gameStrip;

    const bool gaming = options.gamingMode;
    const int focus = gaming ? gameStrip : musicStrip;
    const auto wantedMode = gaming ? ModeValue::Gaming : ModeValue::Music;

    // A factory preset of the right mode is the most realistic starting point.
    for (const auto& preset : controller.getPresetManager().getFactoryPresets())
    {
        if (preset.mode == (gaming ? "Gaming" : "Music"))
        {
            juce::String error;
            controller.loadPreset (preset, focus, error);
            break;
        }
    }

    controller.setMode (wantedMode, focus);
    if (controller.getBoost (focus) < 0.45f)
        controller.setBoost (0.55f, focus);

    auto& store = controller.getParams (focus);
    static constexpr int macros[] = { Macro1, Macro2, Macro3, Macro4, Macro5 };
    static constexpr float macroValues[] = { 0.6f, 0.45f, 0.5f, 0.35f, 0.4f };
    bool anyMacro = false;
    for (const int m : macros)
        anyMacro = anyMacro || store.get (m) > 0.0f;
    if (! anyMacro)
        for (size_t i = 0; i < 5; ++i)
            store.set (macros[i], macroValues[i]);

    controller.setSelectedStrip (focus);
    if (options.simulatedDevice.isNotEmpty())
        controller.simulateOutputDevice (options.simulatedDevice, controller.getHost().getSampleRate(), 2);

    sampleRate = controller.getHost().getSampleRate();
    generator = std::make_unique<TestSignalGenerator> (sampleRate);
    if (gaming)
    {
        generator->setProgramme (gameStrip, TestSignalGenerator::Programme::Game71, 0.0f);
        if (musicStrip != gameStrip)
        {
            generator->setProgramme (musicStrip, TestSignalGenerator::Programme::Music, -12.0f);
            controller.setMode (ModeValue::Music, musicStrip);
            controller.setBoost (0.3f, musicStrip);
        }
    }
    else
    {
        generator->setProgramme (musicStrip, TestSignalGenerator::Programme::Music, 0.0f);
    }
}

void ScreenshotDriver::start()
{
    setUpScene();
    startMs = lastMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

void ScreenshotDriver::timerCallback()
{
    if (finished)
        return;

    // Real-time pacing: render exactly the audio that "would have played"
    // since the last tick (capped so a stalled message loop cannot explode).
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double elapsedMs = now - lastMs;
    lastMs = now;

    const auto samples = static_cast<int> (std::clamp (elapsedMs * 0.001 * sampleRate, 0.0, 0.1 * sampleRate));
    if (samples > 0)
    {
        controller.renderOffline (*generator, samples);
        renderedSamples += samples;
    }

    const auto wanted = static_cast<int64_t> (options.seconds * sampleRate * 0.9);
    if (now - startMs >= options.seconds * 1000.0 && renderedSamples >= wanted)
        finish();
}

void ScreenshotDriver::finish()
{
    finished = true;
    stopTimer();

    const auto image = target.createComponentSnapshot (target.getLocalBounds(), true, options.scale);
    bool ok = false;

    if (image.isValid())
    {
        options.output.getParentDirectory().createDirectory();
        options.output.deleteFile();
        juce::FileOutputStream stream (options.output);
        juce::PNGImageFormat png;
        ok = stream.openedOk() && png.writeImageToStream (image, stream);
        stream.flush();
    }

    const auto message = ok ? "Screenshot written: " + options.output.getFullPathName() + " (" + juce::String (image.getWidth()) + "x"
                                  + juce::String (image.getHeight()) + ")"
                            : "Could not write the screenshot to " + options.output.getFullPathName();

    // May destroy this object (the application quits): nothing after this.
    if (onFinished != nullptr)
        onFinished (ok, message);
}
} // namespace flub::app
