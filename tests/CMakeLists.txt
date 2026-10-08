# daisycola tests.
#
# The TAPE tests compile the CHOMPI TAPE firmware against daisycola. They need
# DAISYCOLA_CHOMPI_DIR (found automatically when CHOMPI is checked out next to daisycola).

if(DAISYCOLA_CHOMPI_DIR AND EXISTS "${DAISYCOLA_CHOMPI_DIR}/firmware/chompi-tape/code/src")
    add_subdirectory(tape)
else()
    message(WARNING "daisycola: CHOMPI not found, TAPE tests are skipped")
endif()
