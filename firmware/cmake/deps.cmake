# What the firmware builds with, pinned and fetched once into firmware/.deps/ (shared by every preset):
# the Pico SDK and the Arm GCC toolchain. Included before project(), since the toolchain decides the
# compiler. To use copies you already have: -DPICO_SDK_PATH=... -DPICO_TOOLCHAIN_PATH=...
set(PICO_SDK_TAG 2.2.0)
set(ARM_GCC_VERSION 14.2.rel1)

set(DEPS ${CMAKE_CURRENT_LIST_DIR}/../.deps)
include(FetchContent)

# The SDK, with only the submodules the firmware uses (USB, the Wi-Fi chip, its IP stack).
if (NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH ${DEPS}/pico-sdk-${PICO_SDK_TAG})
    if (NOT EXISTS ${PICO_SDK_PATH}/pico_sdk_init.cmake)
        message(STATUS "Fetching Pico SDK ${PICO_SDK_TAG}")
        FetchContent_Populate(pico_sdk QUIET
            GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk
            GIT_TAG ${PICO_SDK_TAG}
            GIT_SHALLOW TRUE
            GIT_SUBMODULES lib/tinyusb lib/cyw43-driver lib/lwip
            SOURCE_DIR ${PICO_SDK_PATH}
            SUBBUILD_DIR ${DEPS}/pico-sdk-subbuild)
    endif()
endif()
set(PICO_SDK_PATH ${PICO_SDK_PATH} CACHE PATH "The Pico SDK" FORCE)

# The toolchain: Arm's own build for this computer.
if (NOT PICO_TOOLCHAIN_PATH)
    if (CMAKE_HOST_APPLE)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(host darwin-${arch}) # darwin-arm64 or darwin-x86_64
        set(ext tar.xz)
    elseif (CMAKE_HOST_WIN32)
        set(host mingw-w64-x86_64)
        set(ext zip)
    else()
        execute_process(COMMAND uname -m OUTPUT_VARIABLE arch OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(host ${arch}) # x86_64 or aarch64
        set(ext tar.xz)
    endif()
    set(name arm-gnu-toolchain-${ARM_GCC_VERSION}-${host}-arm-none-eabi)
    set(PICO_TOOLCHAIN_PATH ${DEPS}/${name})
    if (NOT EXISTS ${PICO_TOOLCHAIN_PATH}/bin)
        message(STATUS "Fetching Arm GCC ${ARM_GCC_VERSION} (${host})")
        set(archive ${DEPS}/${name}.${ext})
        file(DOWNLOAD https://developer.arm.com/-/media/Files/downloads/gnu/${ARM_GCC_VERSION}/binrel/${name}.${ext}
             ${archive} STATUS status SHOW_PROGRESS)
        list(GET status 0 code)
        if (NOT code EQUAL 0)
            message(FATAL_ERROR "Downloading the toolchain failed: ${status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT ${archive} DESTINATION ${DEPS})
        file(REMOVE ${archive})
    endif()
endif()
set(PICO_TOOLCHAIN_PATH ${PICO_TOOLCHAIN_PATH} CACHE PATH "The Arm GCC toolchain" FORCE)

# picotool (for the uf2 and for flashing): the SDK builds it, here, once.
set(PICOTOOL_FETCH_FROM_GIT_PATH ${DEPS}/picotool)

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
