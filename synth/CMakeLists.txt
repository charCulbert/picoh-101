# The PICOH-101 synth in C++ as a CLAP (and a WCLAP for the browser), its interface the panel
# from web/ in a webview.
#
#   cmake -B build && cmake --build build     the CLAP
#   tools/build-wclap                           the WCLAP
cmake_minimum_required(VERSION 3.28)

# macOS plugins here are universal (Apple silicon and Intel).
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(PICOH-101 VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # build/compile_commands.json, where clangd finds the downloaded headers
if(MSVC) # clap-wrapper and the VST3 SDK use the DLL runtime (/MD)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux") # VST3 modules link static libraries
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# This template's libraries (libs/), at a release: read its release notes before
# changing GIT_TAG. To build against a local checkout of it instead:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_CLAP_TEMPLATE=<checkout>
include(FetchContent)
FetchContent_Declare(clap_template
    GIT_REPOSITORY https://github.com/charCulbert/char-clap-template.git
    GIT_TAG v0.6.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR none) # only libs/: the template's own CMakeLists isn't for plugins
FetchContent_MakeAvailable(clap_template)
set(LIBS "${clap_template_SOURCE_DIR}/libs")
include(${LIBS}/core/core.cmake)
include(${LIBS}/webview/webview.cmake)

# Signalsmith's elliptic BLEP (header only, MIT), for DSP/picohDSP_EllipticBlep.h: the copy in
# elliptic-blep/, which the Pico's firmware builds too.
set(elliptic_blep_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/elliptic-blep")

# The parameter tables, from params/parameters.json: again whenever it changes.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../params/generate.py" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../params/parameters.json")

add_library(PICOH-101_impl STATIC plugin.cpp Synth.cpp SynthParameters.cpp)
target_include_directories(PICOH-101_impl PRIVATE ${elliptic_blep_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)
core_add_to(PICOH-101_impl)
webview_add_to(PICOH-101_impl)

# The CLAP only. VST3 and AUV2 would go in this list, and a standalone app in STANDALONE_CONFIGURATIONS.
set(formats CLAP)
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(formats WCLAP)
endif()

make_clapfirst_plugins(
    TARGET_NAME PICOH-101
    IMPL_TARGET PICOH-101_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    OUTPUT_NAME "PICOH-101"
    BUNDLE_IDENTIFIER "com.charlieculbert.picoh-101"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChCu"
    AUV2_SUBTYPE_CODE "P101"
    AUV2_INSTRUMENT_TYPE "aumu"
    PLUGIN_FORMATS ${formats}
    WINDOWS_FOLDER_VST3 TRUE # a folder, so its resources can travel inside it
    COPY_AFTER_BUILD FALSE)

# resources/page/ ships inside each plugin, with the panel it shares with the browser and Pico pages
# (web/): the host shows it, or a WebView does inside the host's window.
set(WEB "${CMAKE_CURRENT_SOURCE_DIR}/../web")
include(${WEB}/web.cmake) # the page's TypeScript, compiled into web/dist
webview_resources(resources PICOH-101 resources)
add_dependencies(PICOH-101_resources web_dist)
add_custom_command(TARGET PICOH-101_resources POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${WEB}/dist" "${resources}/page/dist"
    COMMAND ${CMAKE_COMMAND} -E copy "${WEB}/instrument.css" "${resources}/page/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${WEB}/fonts" "${resources}/page/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${WEB}/vendor/compost/src" "${resources}/page/vendor/compost/src"
    COMMAND ${CMAKE_COMMAND} -E copy "${WEB}/vendor/compost/LICENSE" "${resources}/page/vendor/compost/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/licenses" "${resources}/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md" "${resources}/"
    VERBATIM)
core_ship_resources(PICOH-101 "${resources}" DEPENDS PICOH-101_resources)
