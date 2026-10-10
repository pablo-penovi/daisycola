cmake_minimum_required(VERSION 3.20)
project(daisycola LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Where libDaisy (and, for tests, the CHOMPI firmware) live -----------------------------------

set(DAISYCOLA_CHOMPI_DIR "" CACHE PATH
    "CHOMPI checkout. Used for the TAPE tests and as the default libDaisy fork.")
if(NOT DAISYCOLA_CHOMPI_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../CHOMPI/firmware")
    get_filename_component(DAISYCOLA_CHOMPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CHOMPI" REALPATH)
endif()

set(DAISYCOLA_LIBDAISY_DIR "" CACHE PATH "libDaisy tree to build against (the CHOMPI fork for now)")
if(NOT DAISYCOLA_LIBDAISY_DIR AND DAISYCOLA_CHOMPI_DIR)
    set(DAISYCOLA_LIBDAISY_DIR "${DAISYCOLA_CHOMPI_DIR}/firmware/chompi-tape/code/libs/libDaisy")
endif()
if(NOT EXISTS "${DAISYCOLA_LIBDAISY_DIR}/src/daisy.h")
    message(FATAL_ERROR
        "daisycola: set DAISYCOLA_LIBDAISY_DIR to a libDaisy tree "
        "(or DAISYCOLA_CHOMPI_DIR to a CHOMPI checkout)")
endif()

if(PROJECT_IS_TOP_LEVEL)
    set(daisycola_tests_default ON)
else()
    set(daisycola_tests_default OFF)
endif()
option(DAISYCOLA_BUILD_TESTS "Build the daisycola tests" ${daisycola_tests_default})
set(DAISYCOLA_SANITIZER "" CACHE STRING "Build with a sanitizer: thread, address or empty")

if(DAISYCOLA_SANITIZER)
    add_compile_options(-fsanitize=${DAISYCOLA_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${DAISYCOLA_SANITIZER})
endif()

set(LIBDAISY ${DAISYCOLA_LIBDAISY_DIR})
set(FATFS ${LIBDAISY}/Middlewares/Third_Party/FatFs/src)
set(DAISYCOLA_DIR ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)

# --- Sources -------------------------------------------------------------------------------------

set(daisycola_sources
    # libDaisy bodies backed by the virtual board
    src/daisy_seed.cpp
    src/dev/sdram.cpp
    src/hal.cpp
    src/hid/audio.cpp
    src/hid/usb_midi.cpp
    src/per/gpio.cpp
    src/per/i2c.cpp
    src/per/sai.cpp
    src/per/sdmmc.cpp
    src/per/tim.cpp
    src/per/tim_channel.cpp
    src/per/uart.cpp
    src/sys/dma.cpp
    src/sys/fatfs.cpp
    src/sys/ff_folder.cpp
    src/sys/system.cpp
    # virtual board and MCU
    src/board/board.cpp
    src/board/midi.cpp
    src/board/sd_card.cpp
    src/mcu/vmcu.cpp
    src/host.cpp

    # libDaisy sources that are not hardware-specific, compiled as they are
    ${LIBDAISY}/src/hid/midi.cpp
    ${LIBDAISY}/src/hid/midi_parser.cpp
    ${LIBDAISY}/src/hid/switch.cpp
    ${LIBDAISY}/src/ui/UI.cpp
)

# What firmware and host code need to compile against daisycola. daisycola's headers come before
# libDaisy's, so a header daisycola provides replaces the original. FatFs's directory supplies the
# FatFs types (ff.h, ffconf.h); FatFs itself isn't built, sys/ff_folder.cpp implements its API.
add_library(daisycola_headers INTERFACE)
target_include_directories(daisycola_headers BEFORE INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/compat/daisysp
    ${LIBDAISY}/src
    ${LIBDAISY}/src/sys
    ${FATFS}
)
# Like libDaisy's Makefile, every file sees the (replacement) device header. Every file also gets
# FatFs's integer types at their required widths (see include/daisycola/ff_integer.h).
target_compile_options(daisycola_headers INTERFACE
    "SHELL:-include daisycola/ff_integer.h"
    "SHELL:-include stm32h7xx.h")
target_compile_features(daisycola_headers INTERFACE cxx_std_17)
target_link_libraries(daisycola_headers INTERFACE Threads::Threads rt)

function(daisycola_internal target)
    target_link_libraries(${target} PUBLIC daisycola_headers)
    target_include_directories(${target} PRIVATE ${DAISYCOLA_DIR}/src)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS ON C_EXTENSIONS ON)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
endfunction()

# --- Firmware libraries --------------------------------------------------------------------------
#
# A firmware library holds the firmware, the libDaisy sources and its own copy of daisycola, so
# that unloading it and loading it again starts everything from scratch (host.h, PowerCycle).
# Everything in it is compiled position-independent with hidden symbols, and without GNU unique
# symbols, which would make dlclose a no-op.

# Prepares a static library that goes into a firmware library (DaisySP, for instance).
function(daisycola_firmware_library target)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique>)
    endif()
endfunction()

# daisycola_add_firmware(<name> SOURCES <file>...)
#
# Builds the firmware sources into a loadable library, lib<name>.so, for daisycola_host's
# LoadFirmware. The firmware's main is renamed for daisycola; add the firmware's include
# directories and libraries to <name> with the usual target_ commands.
function(daisycola_add_firmware name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES")
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "daisycola_add_firmware(${name}): no SOURCES")
    endif()
    add_library(${name} MODULE ${arg_SOURCES} $<TARGET_OBJECTS:daisycola_fw_entry>)
    target_compile_definitions(${name} PRIVATE main=daisycola_firmware_main)
    target_link_libraries(${name} PRIVATE daisycola_fw)
    daisycola_firmware_library(${name})
    if(NOT DAISYCOLA_SANITIZER) # a sanitizer's runtime lives in the executable
        target_link_options(${name} PRIVATE -Wl,--no-undefined)
    endif()
endfunction()

# --- The libraries -------------------------------------------------------------------------------

# Firmware linked into the host program: one run per process.
add_library(daisycola STATIC ${daisycola_sources})
daisycola_internal(daisycola)

# daisycola inside a firmware library, and the library's entry point.
add_library(daisycola_fw STATIC ${daisycola_sources})
daisycola_internal(daisycola_fw)
daisycola_firmware_library(daisycola_fw)
add_library(daisycola_fw_entry OBJECT src/firmware_api.cpp)
daisycola_internal(daisycola_fw_entry)
daisycola_firmware_library(daisycola_fw_entry)

# The host side of a firmware library: host.h, forwarded to whichever library is loaded.
add_library(daisycola_host STATIC src/host_loader.cpp)
daisycola_internal(daisycola_host)
target_link_libraries(daisycola_host PUBLIC ${CMAKE_DL_LIBS})

# --- Tests ---------------------------------------------------------------------------------------

if(DAISYCOLA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
