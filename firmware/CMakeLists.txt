# The Pico firmware: the synth (../synth) on core 1, MIDI, USB and the GUI on core 0. From firmware/:
#
#   cmake -B build && cmake --build build                                 Pico 2 W
#   cmake -B build-pico2 -DPICO_BOARD=pico2 && cmake --build build-pico2  Pico 2, no Wi-Fi
#   cmake -B build-pico -DPICO_BOARD=pico && cmake --build build-pico     Pico (RP2040), no Wi-Fi
#   cmake --build build --target flash                                    build, then load it onto the board
#   cmake -B build-trace -DTRACE=ON                                       for tools/trace.py (profiling only)
#
# The first configure fetches the pinned SDK and toolchain (cmake/deps.cmake). Output: <build dir>/synth.uf2
cmake_minimum_required(VERSION 3.25)

set(PICO_BOARD pico2_w CACHE STRING "Board")
include(cmake/deps.cmake)
project(picoh101_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
pico_sdk_init()

# ---- Options ---------------------------------------------------------------

# Wi-Fi only on boards with the radio.
if (PICO_CYW43_SUPPORTED)
    set(web_default ON)
else()
    set(web_default OFF)
endif()
option(WEB "Serve the GUI over Wi-Fi" ${web_default})
option(TRACE "For profiling only: cycle stamps of each block and the synth's passes, for tools/trace.py" OFF)
set(WIFI_SSID "" CACHE STRING "Network to join; leave empty for the synth to make its own")
set(WIFI_PASSWORD "" CACHE STRING "Password of WIFI_SSID")
set(AP_PASSWORD "picosynth" CACHE STRING "Password of the network the synth makes (8 characters or more)")

# ---- Generated files ---------------------------------------------------------

set(SYNTH ${CMAKE_CURRENT_LIST_DIR}/../synth)
set(WEB_DIR ${CMAKE_CURRENT_LIST_DIR}/../web)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN})
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The parameter tables, from ../params/parameters.json (committed; regenerated on every configure).
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../params/generate.py COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../params/parameters.json)

# The GUI page, compiled (../web/web.cmake) and embedded (tools/embed.py follows its imports): served over
# Wi-Fi, and to the app over USB.
include(${WEB_DIR}/web.cmake)
file(GLOB_RECURSE WEB_SOURCES CONFIGURE_DEPENDS ${WEB_DIR}/*.html ${WEB_DIR}/*.css ${WEB_DIR}/fonts/*
     ${WEB_DIR}/vendor/compost/src/*.js)
add_custom_command(
    OUTPUT ${GEN}/web_files.generated.h
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/tools/embed.py ${GEN}/web_files.generated.h ${WEB_DIR}
    DEPENDS ${WEB_SOURCES} ${WEB_DIST_FILES} tools/embed.py)

# ---- The firmware ------------------------------------------------------------

add_executable(synth
    main.cpp
    synth_core.cpp
    usb.cpp
    usb_descriptors.c
    din_midi.cpp
    ${GEN}/web_files.generated.h
    ${SYNTH}/Synth.cpp
    ${SYNTH}/SynthParameters.cpp)
pico_generate_pio_header(synth ${CMAKE_CURRENT_LIST_DIR}/i2s.pio)
target_include_directories(synth PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../protocol ${GEN} ${SYNTH}
                                         ${SYNTH}/elliptic-blep)
target_link_libraries(synth PRIVATE
    pico_stdlib pico_multicore pico_unique_id
    hardware_dma hardware_pio hardware_uart
    tinyusb_device tinyusb_board)
target_compile_options(synth PRIVATE -O3 -ffast-math -Wall) # synth_core.cpp also turns on flush-to-zero
# The RP2040 has no FPU: the voice renders in fixed point (synth/DSP/picohDSP_FixedVoice.h), at 250 MHz.
if (PICO_RP2040)
    # Core 1 calls the SDK's soft float, divider, clz and 64-bit helpers: from RAM, not through the cache.
    target_compile_definitions(synth PRIVATE PICOH_FIXED_POINT=1 PICO_FLOAT_IN_RAM=1 PICO_DOUBLE_IN_RAM=1
                               PICO_DIVIDER_IN_RAM=1 PICO_BITS_IN_RAM=1 PICO_INT64_OPS_IN_RAM=1 PICO_MEM_IN_RAM=1)
    target_link_libraries(synth PRIVATE hardware_vreg)
    # A block for picotool link, so a universal UF2 can hold this and an RP2350 build (target universal).
    target_compile_definitions(synth PRIVATE PICO_CRT0_INCLUDE_PICOBIN_BLOCK=1)
    # Flash at clk_sys / 4 (62.5 MHz at 250), well inside the chip's rating; core 0 runs from it.
    pico_define_boot_stage2(synth_boot2 ${PICO_DEFAULT_BOOT_STAGE2_FILE})
    target_compile_definitions(synth_boot2 PRIVATE PICO_FLASH_SPI_CLKDIV=4)
    pico_set_boot_stage2(synth synth_boot2)
endif()

if (WEB)
    target_sources(synth PRIVATE wifi.cpp third_party/dhcpserver/dhcpserver.c)
    target_include_directories(synth PRIVATE third_party/dhcpserver)
    target_link_libraries(synth PRIVATE pico_cyw43_arch_lwip_poll pico_lwip_mdns)
    target_compile_definitions(synth PRIVATE HAVE_WEB=1 AP_PASSWORD="${AP_PASSWORD}")
    if (WIFI_SSID)
        target_compile_definitions(synth PRIVATE WIFI_SSID="${WIFI_SSID}" WIFI_PASSWORD="${WIFI_PASSWORD}")
    endif()
endif()

if (TRACE)
    target_compile_definitions(synth PRIVATE PICOH_TRACE=1)
    set_source_files_properties(${SYNTH}/Synth.cpp PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/trace_dsp.h")
endif()

# The USB serial port is the GUI's (usb.cpp); logs go there as "# " lines.
pico_enable_stdio_usb(synth 0)
pico_enable_stdio_uart(synth 0)

# ---- The synth runs from RAM -------------------------------------------------
# The flash cache is 16 KB and shared with core 0; the synth's code alone is bigger, and from flash the
# load was 46% with late blocks. So the SDK's own linker script, with the synth and what it calls added
# to what it already keeps out of flash (libgcc, libm, memcpy).

if (PICO_RP2040)
    set(chip rp2040)
else()
    set(chip rp2350)
endif()
file(READ ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/${chip}/memmap_default.ld memmap)
# Core 1's render loop must not touch flash at all, so a save can write flash while the synth plays;
# tools/core1-in-ram.py fails the build if anything it reaches is left there.
set(in_ram "*Synth.cpp.o *SynthParameters.cpp.o *synth_core.cpp.o *queue.c.o")
foreach(o memcpy memmove memset)
    string(APPEND in_ram " *libc.a:*${o}.o *libg.a:*${o}.o")
endforeach()
foreach(o double_aeabi_dcp.S double_conv_m33.S double_sci_m33.S double_math.c float_math.c float_sci_m33_vfp.S) # the SDK's float and double helpers
    string(APPEND in_ram " *${o}.o")
endforeach()
if (PICO_RP2040) # its soft float, and the atomics the M0+ lacks
    string(APPEND in_ram " *float_math.c.o *double_math.c.o *atomic.c.o")
endif()
string(REPLACE "*libm.a:)" "*libm.a: ${in_ram})" memmap "${memmap}")
file(WRITE ${GEN}/memmap_picoh.ld "${memmap}")
pico_set_linker_script(synth ${GEN}/memmap_picoh.ld)

# Before the UF2 is made, so a failing build leaves none to flash.
add_custom_command(TARGET synth POST_BUILD
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_LIST_DIR}/tools/core1-in-ram.py ${CMAKE_OBJDUMP} $<TARGET_FILE:synth>
    VERBATIM)
pico_add_extra_outputs(synth)

# A UF2 for the Pico and the Pico 2 (tools/universal-uf2), from a Pico 2 build and a Pico one in PICO_BUILD:
#   cmake --build build-pico2 --target universal      <build dir>/PICOH-101.uf2
if (NOT PICO_RP2040 AND NOT WEB)
    set(PICO_BUILD ${CMAKE_CURRENT_LIST_DIR}/build-pico CACHE PATH "The Pico (RP2040) build for the universal UF2")
    add_custom_target(universal
        COMMAND ${CMAKE_CURRENT_LIST_DIR}/tools/universal-uf2 $<TARGET_FILE:picotool> ${CMAKE_CURRENT_BINARY_DIR}/PICOH-101.uf2
                ${PICO_BUILD}/synth.bin $<TARGET_FILE_DIR:synth>/synth.bin
        DEPENDS synth
        VERBATIM)
endif()

# Flash: reboot the board into its bootloader (its serial port opened at 1200 baud), then load.
add_custom_target(flash
    COMMAND ${CMAKE_CURRENT_LIST_DIR}/tools/flash $<TARGET_FILE:picotool> $<TARGET_FILE_DIR:synth>/synth.uf2
    DEPENDS synth
    USES_TERMINAL)
