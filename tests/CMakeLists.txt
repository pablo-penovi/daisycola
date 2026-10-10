# daisycola tests.
#
# Unit tests always build. The TAPE tests compile the CHOMPI TAPE firmware against daisycola; they
# need DAISYCOLA_CHOMPI_DIR (found automatically when CHOMPI is checked out next to daisycola).

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

set(have_chompi OFF)
if(DAISYCOLA_CHOMPI_DIR AND EXISTS "${DAISYCOLA_CHOMPI_DIR}/firmware/chompi-tape/code/src")
    set(have_chompi ON)
    set(TAPE_CARD_DIR "${DAISYCOLA_CHOMPI_DIR}/firmware/card-profiles/tape-2.0")
else()
    message(WARNING "daisycola: CHOMPI not found, tests that need TAPE are skipped")
    set(TAPE_CARD_DIR "")
endif()

add_executable(daisycola_unit
    unit/i2c_test.cpp
    unit/ff_folder_test.cpp
    unit/ff_size_test.cpp
    unit/ws2812_test.cpp
)
target_link_libraries(daisycola_unit PRIVATE daisycola GTest::gtest_main)
target_compile_definitions(daisycola_unit PRIVATE DAISYCOLA_TAPE_CARD_DIR="${TAPE_CARD_DIR}")
gtest_discover_tests(daisycola_unit DISCOVERY_MODE PRE_TEST)

# Firmware-thread tests. ctest runs each test in its own process, which they need: firmware linked
# into a program runs once per process.
add_executable(daisycola_mcu mcu/firmware_test.cpp)
target_link_libraries(daisycola_mcu PRIVATE daisycola GTest::gtest_main)
gtest_discover_tests(daisycola_mcu DISCOVERY_MODE PRE_TEST)

# Power cycles: firmware built as libraries, loaded through daisycola_host.
daisycola_add_firmware(cycle_counter_fw SOURCES cycle/counter_firmware.cpp)
daisycola_add_firmware(cycle_spin_fw SOURCES cycle/spin_firmware.cpp)
add_executable(daisycola_cycle cycle/power_cycle_test.cpp)
target_link_libraries(daisycola_cycle PRIVATE daisycola_host GTest::gtest_main)
target_compile_definitions(daisycola_cycle PRIVATE
    COUNTER_FW="$<TARGET_FILE:cycle_counter_fw>"
    SPIN_FW="$<TARGET_FILE:cycle_spin_fw>")
add_dependencies(daisycola_cycle cycle_counter_fw cycle_spin_fw)
gtest_discover_tests(daisycola_cycle DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)

if(have_chompi)
    add_subdirectory(tape)
endif()
