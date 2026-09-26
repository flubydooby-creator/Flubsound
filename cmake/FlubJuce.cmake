# Flubsound Pro - JUCE dependency.
#
# JUCE is dual-licensed (AGPLv3 / commercial JUCE 9 licence). A closed-source
# release of Flubsound needs a commercial JUCE licence - see docs/02-tech-stack.md.
#
# Offline / pinned builds: pass -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
# (standard CMake FetchContent override) to use a local checkout.

include(FetchContent)

set(FLUB_JUCE_VERSION "9.0.2" CACHE STRING "JUCE git tag to build against")

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${FLUB_JUCE_VERSION}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(JUCE)
