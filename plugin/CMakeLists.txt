# Flubsound FX - VST3 / AU / Standalone plug-in around flub::ProcessingChain.
#
# Built when FLUB_BUILD_PLUGIN=ON (the root CMakeLists includes cmake/FlubJuce.cmake).
# Formats: VST3 + Standalone everywhere, AU on macOS. Plug-in binaries end up in
# <build>/plugin/FlubsoundFX_artefacts/<config>/{VST3,Standalone,AU}.
#
# NOTE on PLUGIN_CODE: JUCE's CMake docs recommend exactly one upper-case
# letter for AU (GarageBand 10.3 wants "Xxxx"). "FlFx" is the agreed code;
# changing it after release breaks session recall in every host, so decide
# before the first public build.

set(FLUB_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND FLUB_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(FlubsoundFX
    PRODUCT_NAME "Flubsound FX"
    PLUGIN_NAME "Flubsound FX"
    COMPANY_NAME "Flubsound"
    COMPANY_COPYRIGHT "Copyright (c) 2026 Flubes and Claude (co-authors)"
    BUNDLE_ID com.flubsound.fx
    VERSION ${PROJECT_VERSION}
    DESCRIPTION "Real-time enhancement for music and games: EQ, bass, clarity, space, loudness"
    PLUGIN_MANUFACTURER_CODE Flub
    PLUGIN_CODE FlFx
    FORMATS ${FLUB_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Dynamics EQ Spatial Mastering
    AU_MAIN_TYPE kAudioUnitType_Effect
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    # Standalone on macOS records from an input device.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Flubsound FX (standalone) processes audio from the selected input device.")

set(FLUB_PLUGIN_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_sources(FlubsoundFX PRIVATE
    ${FLUB_PLUGIN_SOURCES}
    Source/PluginProcessor.h
    Source/PluginEditor.h)
target_include_directories(FlubsoundFX PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

# PUBLIC so the per-format wrapper targets (VST3, AU, Standalone) see them too.
target_compile_definitions(FlubsoundFX PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

if(UNIX AND NOT APPLE)
    # Standalone wrapper: ALSA + JACK device types.
    target_compile_definitions(FlubsoundFX PUBLIC JUCE_ALSA=1 JUCE_JACK=1)
    # JUCE 9 needs XInput2 (libxi-dev) for multi-touch; not needed here.
    include(CheckIncludeFile)
    check_include_file("X11/extensions/XInput2.h" FLUB_PLUGIN_HAVE_XINPUT2)
    if(NOT FLUB_PLUGIN_HAVE_XINPUT2)
        target_compile_definitions(FlubsoundFX PUBLIC JUCE_USE_XINPUT=0)
    endif()
endif()

target_link_libraries(FlubsoundFX
    PRIVATE
        flub::core
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags)

# Warnings: strict on our own translation units only (the JUCE module sources
# compiled into this target are not ours to fix).
if(MSVC)
    set(FLUB_PLUGIN_WARNINGS /W4 /permissive- /utf-8)
else()
    set(FLUB_PLUGIN_WARNINGS -Wall -Wextra -Wshadow -Wno-sign-conversion)
endif()
set_source_files_properties(${FLUB_PLUGIN_SOURCES} PROPERTIES COMPILE_OPTIONS "${FLUB_PLUGIN_WARNINGS}")
