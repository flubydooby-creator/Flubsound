# Flubsound Pro - the ASIO device type of the Windows desktop app (R1.2,
# docs/02 §4.1 and §6, docs/04 §4.3, docs/08 D10). OFF by default.
#
# Steinberg's ASIO SDK is not in this repository. Since SDK 2.3.4
# (2025-10-15) it is dual-licensed: the proprietary Steinberg ASIO Licence
# (the SDK must not be redistributed, and a signed Steinberg ASIO SDK Licence
# Agreement is needed before publishing software built with it) or the GNU
# GPL version 3. Which one applies to a published Flubsound build is the
# owner's decision (docs/02 §6); this file never decides it:
#
#   -DFLUB_ASIO=ON -DFLUB_ASIO_SDK_DIR=<folder>
#       an SDK the developer downloaded from steinberg.net and accepted
#       (either licence). <folder> holds common/iasiodrv.h (the archive's
#       ASIOSDK folder; the folder above it works too).
#   -DFLUB_ASIO=ON -DFLUB_ASIO_FETCH=ON
#       CMake downloads the official 2.3.4 archive from Steinberg at
#       configure time (SHA-256 pinned below; a different file is refused)
#       and uses it under its GPLv3 option. A binary built this way may only
#       be given to others under GPLv3-compatible terms (with JUCE's free
#       licence: AGPLv3, source included). For local tests and CI's
#       compile check; never packaged.
#   -DFLUB_ASIO_SDK_DIR=<folder> alone (older build scripts): as the first.
#
# Sets FLUB_ASIO_ENABLED (TRUE / FALSE), FLUB_ASIO_INCLUDE_DIR (the SDK's
# common folder) and FLUB_ASIO_ORIGIN ("folder" or "fetched (GPLv3)").

option(FLUB_ASIO "Windows app: build the ASIO device type (needs the Steinberg ASIO SDK: FLUB_ASIO_SDK_DIR or FLUB_ASIO_FETCH)" OFF)
set(FLUB_ASIO_SDK_DIR "" CACHE PATH "Windows app: Steinberg ASIO SDK folder (holds common/iasiodrv.h); enables the ASIO device type")
option(FLUB_ASIO_FETCH "Windows app, with FLUB_ASIO: download the official ASIO SDK 2.3.4 at configure time and use it under its GPLv3 option" OFF)

# The official archive (Steinberg's download server, linked from
# steinberg.net/developers) and its SHA-256. The hash is the one MSYS2's
# mingw-w64-asiosdk 2.3.4-1 package pins for the same URL.
set(FLUB_ASIO_SDK_URL "https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip")
set(FLUB_ASIO_SDK_SHA256 "d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca")

set(FLUB_ASIO_ENABLED FALSE)
set(FLUB_ASIO_INCLUDE_DIR "")
set(FLUB_ASIO_ORIGIN "")

if(NOT WIN32)
    if(FLUB_ASIO OR FLUB_ASIO_SDK_DIR OR FLUB_ASIO_FETCH)
        message(WARNING "Flubsound: ASIO exists on Windows only; FLUB_ASIO / FLUB_ASIO_SDK_DIR / FLUB_ASIO_FETCH are ignored here")
    endif()
    return()
endif()

if(NOT FLUB_ASIO AND NOT FLUB_ASIO_SDK_DIR)
    if(FLUB_ASIO_FETCH)
        message(WARNING "Flubsound: FLUB_ASIO_FETCH does nothing without -DFLUB_ASIO=ON")
    endif()
    return()
endif()

if(FLUB_ASIO_SDK_DIR)
    # The ASIOSDK folder itself, or the folder it was unzipped into.
    if(EXISTS "${FLUB_ASIO_SDK_DIR}/common/iasiodrv.h")
        set(FLUB_ASIO_INCLUDE_DIR "${FLUB_ASIO_SDK_DIR}/common")
    elseif(EXISTS "${FLUB_ASIO_SDK_DIR}/ASIOSDK/common/iasiodrv.h")
        set(FLUB_ASIO_INCLUDE_DIR "${FLUB_ASIO_SDK_DIR}/ASIOSDK/common")
    else()
        message(FATAL_ERROR "Flubsound: FLUB_ASIO_SDK_DIR (${FLUB_ASIO_SDK_DIR}) is not an ASIO SDK: no common/iasiodrv.h in it "
                            "or in its ASIOSDK folder. Point it at the unzipped SDK's ASIOSDK folder.")
    endif()
    set(FLUB_ASIO_ORIGIN "folder")
elseif(FLUB_ASIO_FETCH)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW) # extracted files get the extraction time
    endif()
    # SOURCE_SUBDIR names a folder the archive does not have, so
    # FetchContent_MakeAvailable only downloads, checks and unpacks it.
    FetchContent_Declare(flub_asiosdk
        URL "${FLUB_ASIO_SDK_URL}"
        URL_HASH SHA256=${FLUB_ASIO_SDK_SHA256}
        SOURCE_SUBDIR flub-no-cmake-project)
    FetchContent_MakeAvailable(flub_asiosdk)
    if(EXISTS "${flub_asiosdk_SOURCE_DIR}/common/iasiodrv.h")
        set(FLUB_ASIO_INCLUDE_DIR "${flub_asiosdk_SOURCE_DIR}/common")
    elseif(EXISTS "${flub_asiosdk_SOURCE_DIR}/ASIOSDK/common/iasiodrv.h")
        set(FLUB_ASIO_INCLUDE_DIR "${flub_asiosdk_SOURCE_DIR}/ASIOSDK/common")
    else()
        message(FATAL_ERROR "Flubsound: the downloaded ASIO SDK has no common/iasiodrv.h (${flub_asiosdk_SOURCE_DIR})")
    endif()
    set(FLUB_ASIO_ORIGIN "fetched (GPLv3)")
    message(STATUS "Flubsound: ASIO SDK 2.3.4 downloaded from Steinberg and used under its GPLv3 option: "
                   "give this build to others only under GPLv3-compatible terms (docs/02 section 6)")
else()
    message(FATAL_ERROR "Flubsound: -DFLUB_ASIO=ON needs the Steinberg ASIO SDK, which is not in this repository:\n"
                        "  * download it yourself from https://www.steinberg.net/developers/ (accepting its licence) and pass "
                        "-DFLUB_ASIO_SDK_DIR=<the unzipped ASIOSDK folder>, or\n"
                        "  * pass -DFLUB_ASIO_FETCH=ON to download the official 2.3.4 archive and use it under GPLv3 "
                        "(a build you give to others must then be GPLv3-compatible; docs/02 section 6).")
endif()

set(FLUB_ASIO_ENABLED TRUE)
message(STATUS "Flubsound: ASIO device type enabled (SDK: ${FLUB_ASIO_ORIGIN}, ${FLUB_ASIO_INCLUDE_DIR})")
