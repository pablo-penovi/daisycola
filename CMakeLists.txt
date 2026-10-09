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

# --- The library ---------------------------------------------------------------------------------

add_library(daisycola STATIC
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
    src/sys/ff_host.c
    src/sys/system.cpp
    # virtual board and MCU
    src/board/board.cpp
    src/board/sd_card.cpp
    src/mcu/vmcu.cpp
    src/host.cpp

    # libDaisy sources that are not hardware-specific, compiled as they are
    ${LIBDAISY}/src/hid/midi.cpp
    ${LIBDAISY}/src/hid/midi_parser.cpp
    ${LIBDAISY}/src/hid/switch.cpp
    ${LIBDAISY}/src/ui/UI.cpp

    # FatFs (ff.c is compiled through src/sys/ff_host.c)
    ${FATFS}/diskio.c
    ${FATFS}/ff_gen_drv.c
    ${FATFS}/option/ccsbcs.c
)

# daisycola's headers come before libDaisy's, so a header daisycola provides replaces the original.
target_include_directories(daisycola BEFORE PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/compat/daisysp
    ${LIBDAISY}/src
    ${LIBDAISY}/src/sys
    ${FATFS}
)
target_include_directories(daisycola PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Like libDaisy's Makefile, every file sees the (replacement) device header. Every file also gets
# FatFs's integer types at their required widths (see include/daisycola/ff_integer.h).
target_compile_options(daisycola PUBLIC
    "SHELL:-include daisycola/ff_integer.h"
    "SHELL:-include stm32h7xx.h")
target_compile_features(daisycola PUBLIC cxx_std_17)
set_target_properties(daisycola PROPERTIES CXX_EXTENSIONS ON C_EXTENSIONS ON)

target_compile_options(daisycola PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)

find_package(Threads REQUIRED)
target_link_libraries(daisycola PUBLIC Threads::Threads rt)

# --- Tests ---------------------------------------------------------------------------------------

if(DAISYCOLA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
