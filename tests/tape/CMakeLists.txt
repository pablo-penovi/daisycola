# Builds the TAPE firmware, unchanged, against daisycola.

set(TAPE ${DAISYCOLA_CHOMPI_DIR}/firmware/chompi-tape/code)

file(GLOB_RECURSE tape_daisysp_sources CONFIGURE_DEPENDS ${TAPE}/libs/DaisySP/Source/*.cpp)
add_library(tape_daisysp STATIC ${tape_daisysp_sources})
target_include_directories(tape_daisysp PUBLIC ${TAPE}/libs/DaisySP/Source)
foreach(dir Control Drums Dynamics Effects Filters Noise PhysicalModeling Synthesis Utility)
    target_include_directories(tape_daisysp PRIVATE ${TAPE}/libs/DaisySP/Source/${dir})
endforeach()
target_compile_options(tape_daisysp PRIVATE -w)

add_library(tape_firmware STATIC
    ${TAPE}/src/chompi_main.cpp
    ${TAPE}/src/encoder.cpp
    ${TAPE}/src/FileStreamingManager.cpp
    ${TAPE}/libs/coreJSON/source/core_json.c
)
target_include_directories(tape_firmware PUBLIC ${TAPE}/src ${TAPE}/libs/coreJSON/source/include)
target_compile_definitions(tape_firmware PRIVATE main=tape_main)
target_compile_options(tape_firmware PRIVATE -w)
target_link_libraries(tape_firmware PUBLIC daisycola tape_daisysp)

# Phase 1: TAPE and daisycola link with no undefined symbols.
add_executable(tape_link tape_link.cpp)
target_link_libraries(tape_link PRIVATE tape_firmware)
add_test(NAME tape_link COMMAND tape_link)

# Phase 3: peripheral models driven by TAPE's own code, single-threaded.
add_executable(tape_peripherals peripherals_test.cpp ${TAPE}/src/encoder.cpp)
target_include_directories(tape_peripherals PRIVATE ${TAPE}/src)
target_link_libraries(tape_peripherals PRIVATE daisycola GTest::gtest_main)
gtest_discover_tests(tape_peripherals DISCOVERY_MODE PRE_TEST)

# Phase 4: TAPE boots headless on the firmware thread.
add_executable(tape_boot boot_test.cpp)
target_link_libraries(tape_boot PRIVATE tape_firmware GTest::gtest_main)
target_compile_definitions(tape_boot PRIVATE DAISYCOLA_TAPE_CARD_DIR="${TAPE_CARD_DIR}")
gtest_discover_tests(tape_boot DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 180)

# TAPE's sample reader before its file opens (include/ff.h).
add_executable(tape_sample_reader sample_reader_test.cpp)
target_link_libraries(tape_sample_reader PRIVATE tape_firmware GTest::gtest_main)
target_compile_options(tape_sample_reader PRIVATE -w)
gtest_discover_tests(tape_sample_reader DISCOVERY_MODE PRE_TEST)

# TAPE as a firmware library, power-cycled in one process. DaisySP goes in with it, built the
# same way. tape_probe.cpp lets the test read TAPE's boot flags.
add_library(tape_daisysp_fw STATIC ${tape_daisysp_sources})
target_include_directories(tape_daisysp_fw PUBLIC ${TAPE}/libs/DaisySP/Source)
foreach(dir Control Drums Dynamics Effects Filters Noise PhysicalModeling Synthesis Utility)
    target_include_directories(tape_daisysp_fw PRIVATE ${TAPE}/libs/DaisySP/Source/${dir})
endforeach()
target_compile_options(tape_daisysp_fw PRIVATE -w)
daisycola_firmware_library(tape_daisysp_fw)

daisycola_add_firmware(tape_fw SOURCES
    ${TAPE}/src/chompi_main.cpp
    ${TAPE}/src/encoder.cpp
    ${TAPE}/src/FileStreamingManager.cpp
    ${TAPE}/libs/coreJSON/source/core_json.c
    tape_probe.cpp
)
target_include_directories(tape_fw PRIVATE ${TAPE}/src ${TAPE}/libs/coreJSON/source/include)
target_compile_options(tape_fw PRIVATE -w)
target_link_libraries(tape_fw PRIVATE tape_daisysp_fw)

add_executable(tape_cycle power_cycle_test.cpp)
target_link_libraries(tape_cycle PRIVATE daisycola_host GTest::gtest_main)
target_compile_definitions(tape_cycle PRIVATE
    TAPE_FW="$<TARGET_FILE:tape_fw>"
    DAISYCOLA_TAPE_CARD_DIR="${TAPE_CARD_DIR}")
add_dependencies(tape_cycle tape_fw)
gtest_discover_tests(tape_cycle DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 900)
