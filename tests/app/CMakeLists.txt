# Flubsound Pro - app-level tests (flub_app_tests).
#
# Added by app/CMakeLists.txt when FLUB_BUILD_APP and FLUB_BUILD_APP_TESTS are
# ON. The desktop app's own sources (everything but Main.cpp, plus the
# platform services when present) are compiled into this console executable
# with the FlubsoundPro target's compile definitions, include directories,
# link libraries and per-source warning flags, so the tests exercise the code
# the app runs. No audio device, window or display is needed: the device
# callback is driven with a fake juce::AudioIODevice, per-app routing with a
# fake platform::AppAudioRouter, and the views are used without a peer.
#
# Run: flub_app_tests [substring-filter]   (or ctest -R flub_app_tests)

set(FLUB_APP_DIR "${PROJECT_SOURCE_DIR}/app")

set(FLUB_APP_TEST_APP_SOURCES ${FLUB_APP_SOURCES})
list(REMOVE_ITEM FLUB_APP_TEST_APP_SOURCES Source/Main.cpp)
list(TRANSFORM FLUB_APP_TEST_APP_SOURCES PREPEND "${FLUB_APP_DIR}/")

file(GLOB FLUB_APP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

juce_add_console_app(flub_app_tests PRODUCT_NAME "flub_app_tests")
target_sources(flub_app_tests PRIVATE ${FLUB_APP_TEST_SOURCES} ${FLUB_APP_TEST_APP_SOURCES} ${FLUB_PLATFORM_SOURCES})

get_target_property(FLUB_APP_TEST_DEFINITIONS FlubsoundPro COMPILE_DEFINITIONS)
get_target_property(FLUB_APP_TEST_INCLUDES FlubsoundPro INCLUDE_DIRECTORIES)
get_target_property(FLUB_APP_TEST_LIBRARIES FlubsoundPro LINK_LIBRARIES)

# JUCE_MODAL_LOOPS_PERMITTED: the tests pump the message loop
# (MessageManager::runDispatchLoopUntil) to receive AsyncUpdater callbacks.
target_compile_definitions(flub_app_tests PRIVATE ${FLUB_APP_TEST_DEFINITIONS} JUCE_MODAL_LOOPS_PERMITTED=1)
target_include_directories(flub_app_tests PRIVATE ${FLUB_APP_TEST_INCLUDES} "${PROJECT_SOURCE_DIR}/tests")
# CMAKE_DL_LIBS: dlsym for the lock counter in AppTestMain.cpp (Linux).
target_link_libraries(flub_app_tests PRIVATE ${FLUB_APP_TEST_LIBRARIES} ${CMAKE_DL_LIBS})

# The app's and the platform's sources keep the warning flags their owners
# set in app/ (source properties are per directory); JUCE's module sources,
# also compiled into this target, get none.
foreach(source IN LISTS FLUB_APP_TEST_APP_SOURCES FLUB_PLATFORM_SOURCES)
    get_source_file_property(options "${source}" DIRECTORY "${FLUB_APP_DIR}" COMPILE_OPTIONS)
    if(options)
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "${options}")
    endif()
endforeach()

# The tests themselves: the core's strict set.
if(MSVC)
    set(FLUB_APP_TEST_WARNINGS /W4 /permissive- /utf-8)
    if(FLUB_WARNINGS_AS_ERRORS)
        list(APPEND FLUB_APP_TEST_WARNINGS /WX)
    endif()
else()
    set(FLUB_APP_TEST_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(FLUB_WARNINGS_AS_ERRORS)
        list(APPEND FLUB_APP_TEST_WARNINGS -Werror)
    endif()
endif()
set_source_files_properties(${FLUB_APP_TEST_SOURCES} PROPERTIES COMPILE_OPTIONS "${FLUB_APP_TEST_WARNINGS}")

add_test(NAME flub_app_tests COMMAND flub_app_tests)
