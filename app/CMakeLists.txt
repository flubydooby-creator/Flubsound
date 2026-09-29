# Flubsound Pro - desktop application (JUCE shell + engine host).
#
# Layout of app/Source:
#   Main.cpp, FlubsoundApplication.*  JUCEApplication, command line, lifetime
#   shell/       MainWindow, tray icon, global hotkeys, headless screenshot driver
#   diagnostics/ rotating log, crash reports, engine event monitor, the
#                diagnostics zip (docs/11 E54)
#   engine/      AudioEngineHost (device callback), DriftCompensatedFifo,
#                EngineController (the facade the UI talks to), AppRouting,
#                TestSignalGenerator (synthetic programme audio)
#   presets/     PresetManager (factory presets from BinaryData + user folder)
#   settings/    AppSettings (juce::PropertiesFile)
#   platform/    PlatformServices.h (fixed interface), PlatformBridge.* (guards
#                the optional platform implementation), PlatformServices_<os>.*
#                (owned by the platform engineer)
#   export/      ExportJob (background batch render of audio files through
#                the CLI's offline renderer) and ExportDialog (its UI)
#   ui/          the GUI: MainComponent (layout + frame loop), FlubLookAndFeel,
#                ParameterBinding, HeaderBar, SettingsDialog, BoostPanel,
#                AnalyzerPanel (SpectrumAnalyzer + EqCurveEditor), LevelMeters,
#                LoudnessPanel, WaveformHistory, ModuleRack / ModuleCard /
#                ParamGrid, RoutingPanel, shared Theme / Widgets / ParamKnob
#
# Options:
#   FLUB_ASIO_SDK_DIR        Steinberg ASIO SDK root (enables JUCE_ASIO on Windows)
#   FLUB_FACTORY_PRESET_DIR  folder whose *.json files are embedded as factory presets
#   FLUB_BUILD_APP_TESTS     also build flub_app_tests (tests/app, see the end of this file)

set(FLUB_ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK root folder (Windows, optional; enables the ASIO device type)")
set(FLUB_FACTORY_PRESET_DIR "${PROJECT_SOURCE_DIR}/presets/factory" CACHE PATH "Folder with factory preset *.json files to embed")

juce_add_gui_app(FlubsoundPro
    PRODUCT_NAME "Flubsound Pro"
    COMPANY_NAME "Flubsound"
    COMPANY_COPYRIGHT "Copyright (c) 2026 Flubes and Claude (co-authors)"
    BUNDLE_ID com.flubsound.pro
    VERSION ${PROJECT_VERSION}
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    # macOS: capturing a virtual loopback device (BlackHole / Flubsound driver)
    # is an audio *input* and therefore needs the microphone permission.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Flubsound Pro processes audio captured from its virtual output devices.")

# ---------------------------------------------------------------------------
# Sources owned by the app shell / engine host
# ---------------------------------------------------------------------------
set(FLUB_APP_SOURCES
    Source/Main.cpp
    Source/FlubsoundApplication.cpp
    Source/diagnostics/CrashHandler.cpp
    Source/diagnostics/DiagnosticLog.cpp
    Source/diagnostics/DiagnosticsBundle.cpp
    Source/diagnostics/DiagnosticsMonitor.cpp
    Source/diagnostics/DiagnosticsSession.cpp
    Source/engine/AudioEngineHost.cpp
    Source/engine/DriftCompensatedFifo.cpp
    Source/engine/EngineController.cpp
    Source/engine/AppRouting.cpp
    Source/engine/TestSignalGenerator.cpp
    Source/export/ExportDialog.cpp
    Source/export/ExportJob.cpp
    Source/presets/PresetManager.cpp
    Source/settings/AppSettings.cpp
    Source/platform/PlatformBridge.cpp
    Source/shell/MainWindow.cpp
    Source/shell/TrayIcon.cpp
    Source/shell/HotkeyManager.cpp
    Source/shell/ScreenshotDriver.cpp
    Source/ui/AbxPanel.cpp
    Source/ui/AnalyzerFeed.cpp
    Source/ui/AnalyzerPanel.cpp
    Source/ui/BoostPanel.cpp
    Source/ui/Comparison.cpp
    Source/ui/DeviceAdviceBanner.cpp
    Source/ui/EqCurveEditor.cpp
    Source/ui/FlubLookAndFeel.cpp
    Source/ui/HeaderBar.cpp
    Source/ui/LevelMeters.cpp
    Source/ui/LoudnessPanel.cpp
    Source/ui/MainComponent.cpp
    Source/ui/MeterSnapshot.cpp
    Source/ui/ModuleCard.cpp
    Source/ui/ModuleRack.cpp
    Source/ui/NoticeBanners.cpp
    Source/ui/ParameterBinding.cpp
    Source/ui/ParamGrid.cpp
    Source/ui/ParamHints.cpp
    Source/ui/ParamKnob.cpp
    Source/ui/PresetAudition.cpp
    Source/ui/PresetBrowser.cpp
    Source/ui/QuickControls.cpp
    Source/ui/RoutingPanel.cpp
    Source/ui/SettingsDialog.cpp
    Source/ui/SimpleStatusPanel.cpp
    Source/ui/SpectrumAnalyzer.cpp
    Source/ui/Theme.cpp
    Source/ui/WaveformHistory.cpp
    Source/ui/Widgets.cpp
    # The CLI's offline renderer (plain C++ on flub::core, no main.cpp): the
    # Export / batch dialog renders files exactly like `flubsound-cli process`.
    ../tools/flubsound-cli/Analysis.cpp
    ../tools/flubsound-cli/OfflineRenderer.cpp)

target_sources(FlubsoundPro PRIVATE ${FLUB_APP_SOURCES})
target_include_directories(FlubsoundPro PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    "${PROJECT_SOURCE_DIR}/tools/flubsound-cli")

# ---------------------------------------------------------------------------
# Platform services (implemented by the platform engineer). The app builds
# with or without them: when the OS implementation is missing,
# FLUB_HAS_PLATFORM_SERVICES=0 and PlatformBridge reports every service as
# unavailable (no global hotkeys, no per-app routing / capture).
# ---------------------------------------------------------------------------
set(FLUB_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/platform")
set(FLUB_PLATFORM_SOURCES)

if(WIN32)
    set(FLUB_PLATFORM_OS_SOURCE "${FLUB_PLATFORM_DIR}/PlatformServices_win.cpp")
elseif(APPLE)
    set(FLUB_PLATFORM_OS_SOURCE "${FLUB_PLATFORM_DIR}/PlatformServices_mac.mm")
else()
    set(FLUB_PLATFORM_OS_SOURCE "${FLUB_PLATFORM_DIR}/PlatformServices_linux.cpp")
endif()

if(EXISTS "${FLUB_PLATFORM_OS_SOURCE}")
    list(APPEND FLUB_PLATFORM_SOURCES "${FLUB_PLATFORM_OS_SOURCE}")
    if(EXISTS "${FLUB_PLATFORM_DIR}/PlatformServices_common.cpp")
        list(APPEND FLUB_PLATFORM_SOURCES "${FLUB_PLATFORM_DIR}/PlatformServices_common.cpp")
    endif()
    set(FLUB_HAS_PLATFORM_SERVICES 1)
else()
    message(STATUS "Flubsound: ${FLUB_PLATFORM_OS_SOURCE} not found - building without platform services")
    set(FLUB_HAS_PLATFORM_SERVICES 0)
endif()

target_sources(FlubsoundPro PRIVATE ${FLUB_PLATFORM_SOURCES})

# Re-run the configure step when a platform source appears or disappears.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FLUB_PLATFORM_DIR}")

# Build settings owned by the platform engineer (link libraries, options such
# as FLUB_ENABLE_UNDOCUMENTED_ROUTING, warning flags for their sources).
set(FLUB_PLATFORM_CMAKE "${FLUB_PLATFORM_DIR}/PlatformServices.cmake")
include("${FLUB_PLATFORM_CMAKE}" OPTIONAL)

# Fallback link dependencies when that file is absent.
if(FLUB_HAS_PLATFORM_SERVICES AND NOT EXISTS "${FLUB_PLATFORM_CMAKE}")
    if(WIN32)
        # COM (ole32), hotkey window (user32), ms-settings launch (shell32,
        # shlwapi), OS build query (version), MMCSS (avrt),
        # ActivateAudioInterfaceAsync (mmdevapi). MSVC also gets them from the
        # #pragma comment(lib) lines; MinGW needs them here.
        target_link_libraries(FlubsoundPro PRIVATE ole32 user32 shell32 shlwapi version avrt mmdevapi)
    elseif(APPLE)
        # Carbon RegisterEventHotKey (global hotkeys), AppKit (settings links).
        target_link_libraries(FlubsoundPro PRIVATE "-framework Carbon" "-framework AppKit")
    endif()
endif()

# ---------------------------------------------------------------------------
# Factory presets -> BinaryData (only when the folder has presets)
# ---------------------------------------------------------------------------
file(GLOB FLUB_FACTORY_PRESETS CONFIGURE_DEPENDS "${FLUB_FACTORY_PRESET_DIR}/*.json")
list(SORT FLUB_FACTORY_PRESETS)
list(LENGTH FLUB_FACTORY_PRESETS FLUB_NUM_FACTORY_PRESETS)

if(FLUB_NUM_FACTORY_PRESETS GREATER 0)
    juce_add_binary_data(FlubsoundPresets
        NAMESPACE FlubsoundPresetData
        HEADER_NAME FlubsoundPresetData.h
        SOURCES ${FLUB_FACTORY_PRESETS})
    target_link_libraries(FlubsoundPro PRIVATE FlubsoundPresets)
    set(FLUB_HAS_FACTORY_PRESETS 1)
    message(STATUS "Flubsound: embedding ${FLUB_NUM_FACTORY_PRESETS} factory preset(s)")
else()
    set(FLUB_HAS_FACTORY_PRESETS 0)
    message(STATUS "Flubsound: no factory presets found in ${FLUB_FACTORY_PRESET_DIR}")
endif()

# ---------------------------------------------------------------------------
# JUCE configuration
# ---------------------------------------------------------------------------
target_compile_definitions(FlubsoundPro PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:FlubsoundPro,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:FlubsoundPro,JUCE_VERSION>"
    FLUB_HAS_PLATFORM_SERVICES=${FLUB_HAS_PLATFORM_SERVICES}
    FLUB_HAS_FACTORY_PRESETS=${FLUB_HAS_FACTORY_PRESETS})

if(WIN32)
    # WASAPI shared / exclusive / low-latency are JUCE device types; the
    # legacy DirectSound backend only adds confusing duplicate devices.
    target_compile_definitions(FlubsoundPro PRIVATE JUCE_WASAPI=1 JUCE_DIRECTSOUND=0)
    if(FLUB_ASIO_SDK_DIR)
        if(NOT EXISTS "${FLUB_ASIO_SDK_DIR}/common/iasiodrv.h")
            message(WARNING "FLUB_ASIO_SDK_DIR does not look like an ASIO SDK (missing common/iasiodrv.h)")
        endif()
        target_compile_definitions(FlubsoundPro PRIVATE JUCE_ASIO=1)
        target_include_directories(FlubsoundPro PRIVATE "${FLUB_ASIO_SDK_DIR}/common")
    else()
        target_compile_definitions(FlubsoundPro PRIVATE JUCE_ASIO=0)
    endif()
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(FlubsoundPro PRIVATE JUCE_ALSA=1 JUCE_JACK=1)
    # JUCE 9 uses XInput2 for multi-touch; the app does not need it, so only
    # enable it when libxi-dev is installed.
    include(CheckIncludeFile)
    check_include_file("X11/extensions/XInput2.h" FLUB_HAVE_XINPUT2)
    if(NOT FLUB_HAVE_XINPUT2)
        target_compile_definitions(FlubsoundPro PRIVATE JUCE_USE_XINPUT=0)
    endif()
endif()

target_link_libraries(FlubsoundPro
    PRIVATE
        flub::core
        juce::juce_audio_formats # Export / batch: WAV, AIFF, FLAC, Ogg Vorbis, MP3 readers; FLAC writer
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags)

# Warnings: strict on our own translation units only (the JUCE module sources
# are compiled inside this target too and are not ours to fix).
if(MSVC)
    set(FLUB_APP_WARNINGS /W4 /permissive- /utf-8)
else()
    set(FLUB_APP_WARNINGS -Wall -Wextra -Wshadow -Wno-sign-conversion)
endif()
set_source_files_properties(${FLUB_APP_SOURCES} PROPERTIES COMPILE_OPTIONS "${FLUB_APP_WARNINGS}")

# ---------------------------------------------------------------------------
# App-level tests: tests/app/CMakeLists.txt builds flub_app_tests from these
# sources (all but Main.cpp) with this target's compile definitions, include
# directories, link libraries and per-source flags, so the tests run
# the code the app runs. The FlubsoundPro target itself is unchanged.
# ---------------------------------------------------------------------------
if(FLUB_BUILD_APP_TESTS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tests/app" "${PROJECT_BINARY_DIR}/tests/app")
endif()
